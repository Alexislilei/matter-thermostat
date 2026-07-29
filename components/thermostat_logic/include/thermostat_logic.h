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

typedef struct {
    gpio_num_t heater_gpio;     // GPIO22
    float target_temp;          // 设定目标温度 (15.0 ~ 25.0 ℃)
    float current_temp;         // 当前滤波后的实测温度
    thermostat_mode_t mode;     // 系统运行模式
    bool is_heating;            // 当前加热器 Relay 控制状态 (HIGH/LOW)
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

#ifdef __cplusplus
}
#endif

#endif // THERMOSTAT_LOGIC_H
