/**
 * @file crash_monitor.h
 * @brief 崩溃监控组件：捕获崩溃时间与复位原因，并在下次启动时打印。
 *
 * 设计目标：
 *  1. 在系统崩溃/复位瞬间，将"崩溃时的运行时长(uptime)"与"复位原因"写入
 *     RTC_NOINIT 内存（该内存可跨软复位保留，且写入开销极小，可在 panic
 *     中断上下文中安全执行）。
 *  2. 下次上电启动时，读取 RTC_NOINIT 中的崩溃记录，并打印醒目的横幅，
 *     让开发者无需紧盯串口也能立即得知"上一次是否崩溃、发生在何时、原因是什么"。
 *  3. 提供心跳(heartbeat)接口，周期性打印运行时长，便于与崩溃时间点关联。
 *  4. 将每次启动的复位原因持久化到 NVS（Flash），保留最近若干次的历史记录。
 *     这样即使开发者在崩溃后拔插 USB 重新连接串口（导致本次上电复位原因被
 *     覆盖为 POWERON），也能从 NVS 历史中追溯真实的崩溃原因。
 *
 * 说明：
 *  - 本组件仅负责"时间点 + 复位原因"的捕获与展示。
 *  - 若要获取崩溃的精确调用栈(backtrace)，请配合启用 Core Dump 到 Flash
 *    (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)，崩溃后可用
 *    `idf.py coredump-info` 解码。
 *
 * 使用方式：
 *  - 在 app_main() 最早期调用 crash_monitor_init()（该函数内部会自行初始化
 *    NVS，无需在调用前手动初始化）。
 *  - 在任意周期任务中调用 crash_monitor_heartbeat()（可选）。
 *  - 需要查看历史复位记录时调用 crash_monitor_print_history()。
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化崩溃监控。
 *
 * 应在 app_main() 最早期调用。该函数会：
 *  - 读取 esp_reset_reason() 获取本次复位原因；
 *  - 检查 RTC_NOINIT 中是否残留上一次崩溃记录，若有则打印醒目横幅；
 *  - 自行初始化 NVS，并将本次复位原因写入 NVS 历史记录（跨拔插 USB 保留）。
 *
 * @return ESP_OK 成功；其他为失败。
 */
esp_err_t crash_monitor_init(void);

/**
 * @brief 心跳打印（可选）。
 *
 * 在周期任务中调用，周期性打印当前运行时长（uptime），
 * 便于在串口日志中定位崩溃发生前设备已运行了多久。
 *
 * @param interval_ms 心跳打印间隔（毫秒）。内部会做节流，仅在
 *                    距上次打印超过该间隔时才真正输出。
 */
void crash_monitor_heartbeat(uint32_t interval_ms);

/**
 * @brief 获取当前系统运行时长（毫秒）。
 *
 * 基于 esp_timer_get_time()，返回自启动以来的毫秒数。
 *
 * @return 运行时长（毫秒）。
 */
uint64_t crash_monitor_get_uptime_ms(void);

/**
 * @brief 打印 NVS 中保存的最近若干次启动复位记录。
 *
 * 用于追溯历史复位原因，即使开发者在崩溃后拔插 USB 重新连接串口，
 * 也能从历史记录中看到崩溃发生时的真实复位原因。
 *
 * @return ESP_OK 成功；其他为失败。
 */
esp_err_t crash_monitor_print_history(void);

#ifdef __cplusplus
}
#endif
