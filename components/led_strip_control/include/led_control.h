#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "thermostat_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LED_NUM_TOTAL 6

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_color_t;

/**
 * @brief 初始化 WS2812B RGB 指示灯阵列 (GPIO 8)
 */
esp_err_t led_control_init(gpio_num_t gpio_num);

/**
 * @brief 上电自检 (POST) 灯效
 * LED_1 至 LED_6 依次点亮 100ms 白色后熄灭
 */
void led_control_post(void);

/**
 * @brief 主灯效刷新引擎，由系统主任务定期调用 (建议 50ms~100ms 周期)
 * 
 * @param dev 当前温控器设备状态指针
 */
void led_control_update(const thermostat_dev_t *dev);

/**
 * @brief 检查瞬态灯效是否已播放完毕
 *        调用方在灯效播放完毕后可执行后续操作（如重启设备）
 * @param dev 设备状态指针
 * @return true 瞬态灯效已结束，false 仍在播放中
 */
bool led_control_effect_finished(const thermostat_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif // LED_CONTROL_H
