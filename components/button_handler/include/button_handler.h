#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "thermostat_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t pin_power;   // GPIO19 (POWER 按键)
    gpio_num_t pin_func;    // GPIO18 (FUNC 编码器附加按键)
    gpio_num_t pin_key_ra;  // GPIO21 (旋转编码器 A相 KEY_RA)
    gpio_num_t pin_key_rb;  // GPIO20 (旋转编码器 B相 KEY_RB)
} button_config_t;

/**
 * @brief 初始化 GPIO 按键、旋钮及去抖检测
 */
esp_err_t button_handler_init(button_config_t *cfg, thermostat_dev_t *thermostat);

/**
 * @brief 按键扫描与状态机轮询任务 (推荐 20ms~50ms 周期)
 */
void button_handler_poll(void);

#ifdef __cplusplus
}
#endif

#endif // BUTTON_HANDLER_H
