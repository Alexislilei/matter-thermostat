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
    float current_temp;             // 当前滤波后的实测温度
    thermostat_mode_t mode;         // 系统运行模式
    bool is_heating;                // 当前加热器 Relay 控制状态 (HIGH/LOW)

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
} thermostat_dev_t;

/**
 * @brief 初始化温控器核心结构体与加热继电器 GPIO
 */
esp_err_t thermostat_init(thermostat_dev_t *dev, gpio_num_t heater_gpio);

/**
 * @brief 运行迟滞温控控制算法 (Hysteresis Control: ±0.5℃)
 * 
 * @param dev 温控器结构体指针
 * @param new_temp 最新获取的滤波实测温度
 */
void thermostat_update_temperature(thermostat_dev_t *dev, float new_temp);

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

#ifdef __cplusplus
}
#endif

#endif // THERMOSTAT_LOGIC_H
