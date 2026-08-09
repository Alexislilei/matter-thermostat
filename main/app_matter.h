#ifndef APP_MATTER_H
#define APP_MATTER_H

#include "esp_err.h"
#include "thermostat_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 Matter 协议栈与 Endpoint Cluster 节点
 */
esp_err_t app_matter_init(thermostat_dev_t *dev);

/**
 * @brief 将实测温度与加热状态更新同步至 Matter Attributes
 */
void app_matter_update(const thermostat_dev_t *dev);

/**
 * @brief 由设备侧主动切换模式时（如按键）调用，将 SystemMode 同步至 Matter。
 *        使用 attribute::set_val 直接写入属性存储，不经过 Matter 写入验证路径。
 */
void app_matter_set_mode(thermostat_mode_t mode);

/**
 * @brief 由设备侧主动改变目标温度时（如按键加减温）调用，
 *        将 OccupiedHeatingSetpoint 同步至 Matter 属性层。
 *        使用 attribute::set_val 直接写入属性存储，不经过 Matter 写入验证路径。
 * @param target_temp_celsius 目标温度，单位摄氏度
 */
void app_matter_set_target_temperature(float target_temp_celsius);

/**
 * @brief 标记当前模式变化是否来自 Matter 回调，用于 button_poll_task
 *        避免将本地变化回写 Matter 时产生冗余写入。
 *        true  = 变化来自 Matter 回调，button_poll_task 跳过回写
 *        false = 变化来自本地按键，button_poll_task 执行回写
 */
extern bool g_mode_change_from_matter;

/**
 * @brief 检查配网是否已超时 (15 分钟)
 *        若超时，自动退出配网模式并触发配网失败灯效
 * @param dev 温控器设备指针
 * @return true 已超时并已处理，false 未超时或不在配网模式
 */
bool app_matter_check_commissioning_timeout(thermostat_dev_t *dev);

/**
 * @brief 执行 Matter 恢复出厂设置：清空 Matter 节点凭证
 *        调用后设备需重启以重新进入配网模式
 */
void app_matter_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif // APP_MATTER_H
