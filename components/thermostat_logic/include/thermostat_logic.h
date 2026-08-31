#ifndef THERMOSTAT_LOGIC_H
#define THERMOSTAT_LOGIC_H

#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    THERMOSTAT_MODE_STANDBY = 0, // 待机模式
    THERMOSTAT_MODE_ON      = 1, // 开机运行模式
    THERMOSTAT_MODE_PAIRING = 2, // Matter 配网模式
} thermostat_mode_t;

/**
 * @brief UI 页面枚举
 */
typedef enum {
    UI_PAGE_MAIN         = 0, // 主页面
    UI_PAGE_SLEEP_TIMER  = 1, // Sleep Timer 设置页
    UI_PAGE_TEMP_OFFSET  = 2, // 温度偏移 (Temp Offset) 设置页
} ui_page_t;

/**
 * @brief 瞬态 LED 灯效类型枚举
 *        用于指示 led_control 当前需要播放的一次性灯效序列
 */
typedef enum {
    LED_EFFECT_NONE               = 0, // 无瞬态灯效，正常显示
    LED_EFFECT_COMMISSIONING_OK   = 1, // 配网成功：LED_1 快闪两下绿色
    LED_EFFECT_COMMISSIONING_FAIL = 2, // 配网失败/超时：LED_1 快闪两下红色
    LED_EFFECT_FACTORY_RESET      = 3, // 恢复出厂：LED_1 白色两下 + LED_2~6 依次闪一下
    LED_EFFECT_UNBIND             = 4, // 解除绑定：LED_1 橙色一下 + 绿色一下
} led_effect_type_t;

typedef struct {
    gpio_num_t heater_gpio;         // GPIO22
    float target_temp;              // 设定目标温度 (15.0 ~ 25.0 ℃)
    float raw_temp;                 // 传感器最新实测原始温度 (滤波后)
    float current_temp;             // 补偿后的有效显示与控制温度 (raw_temp + temp_offset)
    thermostat_mode_t mode;         // 系统运行模式
    bool is_heating;                // 当前加热器 Relay 控制状态 (HIGH/LOW)
 
    /**
     * @brief Wi-Fi 连接状态
     * true  = Wi-Fi STA 已连接并获得 IP (esp_netif_is_netif_up)
     * false = Wi-Fi 未连接/未配网
     * 由 main.c 周期性更新，供 LCD 顶部信息栏显示 Wi-Fi 符号状态
     */
    bool wifi_connected;

    /**
     * @brief 瞬态 LED 灯效请求
     * 由业务逻辑层设置，led_control 消费后自动清零
     */
    led_effect_type_t pending_led_effect;

    /**
     * @brief 配网模式开始时间戳 (毫秒)，用于 15 分钟超时检测
     * 0 表示未在配网模式或未开始计时
     */
    int64_t pairing_start_time_ms;

    // ---- UI 页面管理 ----
    ui_page_t current_page;       // 当前显示页面
    int64_t last_input_time_ms;   // 最后一次操作时间戳 (ms)，用于非主页面 60s 超时返回

    // ---- Sleep Timer 定时睡眠 ----
    // 设定值可选: 10, 30, 60, 90 分钟 (无 OFF 选项)。
    // 首次开机默认 30 分钟，改动后记忆 (NVS 持久化)，下次上电读取记忆值。
    int sleep_timer_setting;      // 设定值 (10, 30, 60, 90 分钟)
    bool sleep_timer_active;      // 是否正在倒计时
    int64_t sleep_timer_start_ms; // 倒计时开始时间戳 (ms)

    // ---- 温度偏移 (Temp Offset) 校准 ----
    // 用于校准温度传感器读数偏差，范围 [-2.0, +2.0] ℃，步进 0.5 ℃。
    // 首次开机默认 0.0 ℃，改动后记忆 (NVS 持久化)，下次上电读取记忆值。
    float temp_offset;            // 温度偏移量 (℃)
} thermostat_dev_t;

/**
 * @brief 初始化温控器核心结构体与加热继电器 GPIO
 */
esp_err_t thermostat_init(thermostat_dev_t *dev, gpio_num_t heater_gpio);

/**
 * @brief 运行迟滞温控控制算法 (Hysteresis Control: ±0.5℃)
 * 
 * @param dev 温控器结构体指针
 * @param raw_temp 最新获取的传感器滤波原始温度
 */
void thermostat_update_temperature(thermostat_dev_t *dev, float raw_temp);

/**
 * @brief 动态调整温度偏移量 ( Temp Offset ) 并立即重算 current_temp 及更新温控
 *
 * @param dev 温控器结构体指针
 * @param offset 新的温度偏移量 [-2.0, +2.0] ℃
 */
void thermostat_set_temp_offset(thermostat_dev_t *dev, float offset);

/**
 * @brief 调整目标设定温度（范围锁在 15.0 ~ 25.0 ℃）
 */
void thermostat_set_target_temperature(thermostat_dev_t *dev, float target);

/**
 * @brief 设置系统模式 (STANDBY / ON / PAIRING)
 */
void thermostat_set_mode(thermostat_dev_t *dev, thermostat_mode_t mode);

/**
 * @brief 执行恢复出厂设置：关闭加热器，清空状态，设置 LED 灯效
 *        调用方应在灯效播放完毕后重启设备
 */
void thermostat_factory_reset(thermostat_dev_t *dev);

/**
 * @brief Sleep Timer 倒计时检测，建议每 2 秒调用一次
 *        倒计时结束后自动切换至待机模式并停止定时器
 */
void thermostat_sleep_timer_tick(thermostat_dev_t *dev);

/**
 * @brief 从 NVS 读取记忆的 Sleep Timer 设定值并应用到 dev->sleep_timer_setting
 *
 * 若 NVS 中无有效记录（首次开机），则保持默认值 (30 分钟)。
 * 调用前需确保 NVS 已初始化 (nvs_flash_init)。
 *
 * @param dev 温控器设备状态指针
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t thermostat_sleep_timer_load(thermostat_dev_t *dev);

/**
 * @brief 将 dev->sleep_timer_setting 保存到 NVS，实现"改动后记忆，下次上电读取"
 *
 * 调用前需确保 NVS 已初始化 (nvs_flash_init)。
 *
 * @param dev 温控器设备状态指针
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t thermostat_sleep_timer_save(const thermostat_dev_t *dev);

/**
 * @brief 从 NVS 读取记忆的温度偏移值并应用到 dev->temp_offset
 *
 * 若 NVS 中无有效记录（首次开机），则保持默认值 (0.0 ℃)。
 * 调用前需确保 NVS 已初始化 (nvs_flash_init)。
 *
 * @param dev 温控器设备状态指针
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t thermostat_temp_offset_load(thermostat_dev_t *dev);

/**
 * @brief 将 dev->temp_offset 保存到 NVS，实现"改动后记忆，下次上电读取"
 *
 * 调用前需确保 NVS 已初始化 (nvs_flash_init)。
 *
 * @param dev 温控器设备状态指针
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t thermostat_temp_offset_save(const thermostat_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif // THERMOSTAT_LOGIC_H
