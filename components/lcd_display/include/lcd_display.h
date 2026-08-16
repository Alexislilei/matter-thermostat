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

/**
 * @brief 触摸校准步骤
 */
typedef enum {
    TOUCH_CALIB_STEP_TL = 0,   // 左上角
    TOUCH_CALIB_STEP_TR = 1,   // 右上角
    TOUCH_CALIB_STEP_BL = 2,   // 左下角
    TOUCH_CALIB_STEP_BR = 3,   // 右下角
    TOUCH_CALIB_STEP_DONE = 4, // 校准完成
} touch_calib_step_t;

/**
 * @brief 显示触摸校准页面
 *
 * 进入校准模式时调用，显示全屏校准页面（含标题与提示文字）。
 * 校准页面会覆盖主页面/待机页等所有普通 UI。
 */
void lcd_display_calib_show(void);

/**
 * @brief 隐藏触摸校准页面，恢复普通 UI 显示
 */
void lcd_display_calib_hide(void);

/**
 * @brief 设置当前校准步骤，更新校准页面上的目标位置与提示文字
 *
 * @param step 当前要触摸的角点
 */
void lcd_display_calib_set_step(touch_calib_step_t step);

#ifdef __cplusplus
}
#endif

#endif // LCD_DISPLAY_H