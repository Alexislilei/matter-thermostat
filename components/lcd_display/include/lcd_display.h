#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "esp_err.h"
#include "thermostat_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 ILI9341 LCD (通过 esp_lcd + SPI) 并启动 LVGL 渲染
 *
 * 硬件引脚定义 (与 docs/01_requirements.md 一致)：
 *   - SPI SCK  : GPIO12
 *   - SPI MOSI : GPIO11
 *   - SPI MISO : GPIO10
 *   - LCD CS   : GPIO2
 *   - LCD DC   : GPIO3
 *   - LCD RESET: GPIO1
 *   - LCD 背光 : GPIO0
 *
 * @param dev 温控器设备状态指针，用于 UI 渲染
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t lcd_display_init(thermostat_dev_t *dev);

/**
 * @brief 刷新 UI 显示内容（根据当前温控器状态渲染对应页面）
 *
 * 建议由主循环或独立任务周期性调用（如每 100ms）。
 * 内部会调用 lv_timer_handler() 处理 LVGL 事件。
 *
 * @param dev 温控器设备状态指针
 */
void lcd_display_update(thermostat_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif // LCD_DISPLAY_H