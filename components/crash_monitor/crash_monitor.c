/**
 * @file crash_monitor.c
 * @brief 崩溃监控组件实现。
 *
 * 核心思路（适配 ESP-IDF v5.1，不依赖私有 panic handler API）：
 *  - 复位原因：由 IDF 框架通过 esp_reset_reason() 提供（框架内部已将
 *    panic/WDT/brownout 等复位原因写入 RTC 寄存器，跨复位保留）。
 *  - 崩溃时间点：使用 RTC_NOINIT 内存周期性保存"最近一次运行时长快照"。
 *    崩溃发生时，最后一次快照即为崩溃发生的大致时间点（误差 <= 心跳间隔）。
 *  - 启动时读取复位原因 + 快照，打印醒目横幅，让开发者无需紧盯串口即可
 *    得知"上一次是否崩溃、发生在何时、原因是什么"。
 *  - 历史记录：将每次启动的复位原因持久化到 NVS（Flash），保留最近若干次
 *    记录。即使开发者在崩溃后拔插 USB 重新连接串口（导致本次上电复位原因
 *    被覆盖为 POWERON），也能从 NVS 历史中追溯真实的崩溃原因。
 *
 * 说明：
 *  - 若要获取崩溃的精确调用栈(backtrace)，请配合启用 Core Dump 到 Flash
 *    (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)，崩溃后可用
 *    `idf.py coredump-info` 解码。
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "crash_monitor.h"

static const char *TAG = "CRASH_MONITOR";

/* ------------------------------------------------------------------ */
/* RTC_NOINIT 运行时长快照                                             */
/* ------------------------------------------------------------------ */

// 魔数：校验 RTC_NOINIT 中的快照是否有效（防止读到随机值）
#define CRASH_MONITOR_MAGIC       0x4D4F4E49u   // "MONI"
#define CRASH_MONITOR_MAGIC_CLEAR 0x00000000u

// 快照结构（保存在 RTC_NOINIT，跨软复位保留）
typedef struct {
    uint32_t magic;           // 校验魔数
    uint64_t last_uptime_ms;  // 最近一次记录的运行时长（毫秒）
    uint32_t reserved;        // 保留
} uptime_snapshot_t;

// 注意：RTC_NOINIT 变量必须放在 .noinit 段，且不能有初始化器。
__NOINIT_ATTR static uptime_snapshot_t s_uptime_snapshot;

// 心跳节流：记录上次心跳打印时间（毫秒）
static uint64_t s_last_heartbeat_ms = 0;

/* ------------------------------------------------------------------ */
/* NVS 历史记录                                                        */
/* ------------------------------------------------------------------ */

#define CRASH_MON_NVS_NS       "crash_mon"   // NVS 命名空间
#define CRASH_MON_KEY_COUNT    "boot_count"  // 启动序号键
#define CRASH_MON_KEY_HIST     "history"     // 历史记录键（blob）
#define CRASH_MON_HIST_MAX     8             // 保留最近 8 次启动记录

// 单条启动记录
typedef struct {
    uint32_t boot_index;      // 启动序号（从 1 开始递增）
    uint8_t  reset_reason;    // esp_reset_reason_t
    uint8_t  reserved[3];     // 对齐保留
    uint64_t uptime_ms;       // 该次启动时记录的运行时长（崩溃快照，0 表示未知）
} crash_boot_record_t;

// 历史记录数组（按时间先后排列，最新在末尾）
static crash_boot_record_t s_history[CRASH_MON_HIST_MAX];

/* ------------------------------------------------------------------ */
/* 复位原因字符串映射                                                  */
/* ------------------------------------------------------------------ */

static const char *reset_reason_to_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_UNKNOWN:    return "UNKNOWN";
    case ESP_RST_POWERON:    return "POWERON (上电)";
    case ESP_RST_EXT:        return "EXT (外部复位引脚)";
    case ESP_RST_SW:         return "SW (软件 esp_restart)";
    case ESP_RST_PANIC:      return "PANIC (异常/崩溃)";
    case ESP_RST_INT_WDT:    return "INT_WDT (中断看门狗超时)";
    case ESP_RST_TASK_WDT:   return "TASK_WDT (任务看门狗超时)";
    case ESP_RST_WDT:        return "WDT (其他看门狗)";
    case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP (深度睡眠唤醒)";
    case ESP_RST_BROWNOUT:   return "BROWNOUT (欠压复位)";
    case ESP_RST_SDIO:       return "SDIO";
    default:                 return "?";
    }
}

/**
 * @brief 判断某复位原因是否属于"异常/崩溃"类（需要告警提示）。
 */
static bool is_crash_reason(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* NVS 历史记录读写                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief 初始化 NVS（带擦除重试），供历史记录使用。
 */
static esp_err_t nvs_init_retry(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS 分区已满或版本不匹配，擦除后重新初始化
        ESP_LOGW(TAG, "NVS partition full/version mismatch, erasing and re-initializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

/**
 * @brief 将本次启动记录写入 NVS 历史（环形保留最近 CRASH_MON_HIST_MAX 次）。
 *
 * @param reason    本次复位原因
 * @param uptime_ms 本次启动时记录的运行时长快照（0 表示未知）
 */
static esp_err_t crash_monitor_nvs_record(esp_reset_reason_t reason, uint64_t uptime_ms)
{
    esp_err_t err = nvs_init_retry();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(CRASH_MON_NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    // 1. 读取并递增启动序号
    uint32_t boot_count = 0;
    nvs_get_u32(handle, CRASH_MON_KEY_COUNT, &boot_count);
    boot_count++;

    // 2. 读取现有历史记录
    size_t hist_size = sizeof(s_history);
    err = nvs_get_blob(handle, CRASH_MON_KEY_HIST, s_history, &hist_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // 首次使用，清空历史
        memset(s_history, 0, sizeof(s_history));
        hist_size = sizeof(s_history);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(history) failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    // 3. 将历史整体前移一位（丢弃最旧记录），最新记录放到末尾
    memmove(&s_history[0], &s_history[1], sizeof(s_history) - sizeof(s_history[0]));
    memset(&s_history[CRASH_MON_HIST_MAX - 1], 0, sizeof(s_history[0]));
    s_history[CRASH_MON_HIST_MAX - 1].boot_index   = boot_count;
    s_history[CRASH_MON_HIST_MAX - 1].reset_reason = (uint8_t)reason;
    s_history[CRASH_MON_HIST_MAX - 1].uptime_ms    = uptime_ms;

    // 4. 写回 NVS
    err = nvs_set_blob(handle, CRASH_MON_KEY_HIST, s_history, sizeof(s_history));
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, CRASH_MON_KEY_COUNT, boot_count);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief 打印 NVS 中保存的最近若干次启动复位记录。
 */
esp_err_t crash_monitor_print_history(void)
{
    esp_err_t err = nvs_init_retry();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(CRASH_MON_NVS_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    memset(s_history, 0, sizeof(s_history));
    size_t hist_size = sizeof(s_history);
    err = nvs_get_blob(handle, CRASH_MON_KEY_HIST, s_history, &hist_size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No boot history recorded yet.");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(history) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "===== Boot History (most recent last) =====");
    for (int i = 0; i < CRASH_MON_HIST_MAX; i++) {
        if (s_history[i].boot_index == 0) {
            continue; // 未使用的槽位
        }
        esp_reset_reason_t r = (esp_reset_reason_t)s_history[i].reset_reason;
        ESP_LOGI(TAG, "  boot #%u : reason=%d (%s), uptime=%llu ms (%llu s)",
                 (unsigned)s_history[i].boot_index,
                 (int)r, reset_reason_to_str(r),
                 (unsigned long long)s_history[i].uptime_ms,
                 (unsigned long long)(s_history[i].uptime_ms / 1000));
    }
    ESP_LOGI(TAG, "===========================================");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 公共接口                                                            */
/* ------------------------------------------------------------------ */

uint64_t crash_monitor_get_uptime_ms(void)
{
    // esp_timer_get_time() 返回微秒，转换为毫秒
    return (uint64_t)(esp_timer_get_time() / 1000);
}

esp_err_t crash_monitor_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();

    // 1. 读取 RTC_NOINIT 中最近一次运行时长快照（崩溃发生的大致时间点）
    uint64_t last_uptime_ms = 0;
    if (s_uptime_snapshot.magic == CRASH_MONITOR_MAGIC) {
        last_uptime_ms = s_uptime_snapshot.last_uptime_ms;
    }

    // 2. 判断本次复位是否由异常/崩溃引起
    if (is_crash_reason(reason)) {
        ESP_LOGE(TAG, "============================================================");
        ESP_LOGE(TAG, "  *** 检测到系统异常复位/崩溃 ***");
        ESP_LOGE(TAG, "  复位原因 : %d (%s)", (int)reason, reset_reason_to_str(reason));
        if (last_uptime_ms > 0) {
            ESP_LOGE(TAG, "  崩溃时运行时长(约) : %llu ms (%llu s = %llu min)",
                     (unsigned long long)last_uptime_ms,
                     (unsigned long long)(last_uptime_ms / 1000),
                     (unsigned long long)(last_uptime_ms / 60000));
        } else {
            ESP_LOGE(TAG, "  崩溃时运行时长 : 未知（未记录到快照）");
        }
        ESP_LOGE(TAG, "  >>> 若已启用 Core Dump，请用 idf.py coredump-info 解码崩溃栈 <<<");
        ESP_LOGE(TAG, "============================================================");
    } else {
        // 正常启动（上电 / 软件重启 / 深度睡眠唤醒等）
        ESP_LOGI(TAG, "Normal boot. Reset reason: %d (%s)",
                 (int)reason, reset_reason_to_str(reason));
    }

    // 3. 将本次启动记录写入 NVS 历史（跨拔插 USB 保留，用于追溯真实崩溃原因）
    crash_monitor_nvs_record(reason, last_uptime_ms);

    // 4. 清除快照魔数，避免下次启动重复告警
    s_uptime_snapshot.magic = CRASH_MONITOR_MAGIC_CLEAR;

    return ESP_OK;
}

void crash_monitor_heartbeat(uint32_t interval_ms)
{
    uint64_t now_ms = crash_monitor_get_uptime_ms();

    // 1. 周期性更新 RTC_NOINIT 中的运行时长快照。
    //    崩溃发生时，最后一次快照即为崩溃发生的大致时间点。
    s_uptime_snapshot.magic = CRASH_MONITOR_MAGIC;
    s_uptime_snapshot.last_uptime_ms = now_ms;

    // 2. 节流打印心跳日志（便于在串口中定位崩溃前设备运行了多久）
    if ((now_ms - s_last_heartbeat_ms) >= interval_ms) {
        s_last_heartbeat_ms = now_ms;
        ESP_LOGI(TAG, "Heartbeat: uptime = %llu ms (%llu s)",
                 (unsigned long long)now_ms,
                 (unsigned long long)(now_ms / 1000));
    }
}
