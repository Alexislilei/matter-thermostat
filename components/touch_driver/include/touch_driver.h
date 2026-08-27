#ifndef TOUCH_DRIVER_H
#define TOUCH_DRIVER_H

#include "esp_err.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 触摸点结构体（已校准，单位：像素）
 */
typedef struct {
    int16_t x;          // 屏幕 X 坐标 (0 ~ 239)
    int16_t y;          // 屏幕 Y 坐标 (0 ~ 319)
    bool    touched;    // 是否检测到触摸按下
} touch_point_t;

/**
 * @brief 触摸屏校准参数
 *
 * XPT2046 返回的是 ADC 原始值（0~4095），需通过线性映射转换为屏幕像素坐标。
 * 由于屏幕为竖屏（宽 240，高 320），且触摸屏与 LCD 共用 SPI 总线，
 * 原始 ADC 的 X/Y 轴与屏幕 X/Y 轴可能互换或镜像，需根据实际硬件校准。
 *
 * 映射公式：
 *   screen_x = (raw_x - x_min) * (LCD_W - 1) / (x_max - x_min)
 *   screen_y = (raw_y - y_min) * (LCD_H - 1) / (y_max - y_min)
 *
 * 若方向/镜像不对，可通过 swap_xy / invert_x / invert_y 修正。
 */
typedef struct {
    int32_t x_min;      // X 轴 ADC 最小值（对应屏幕最左）
    int32_t x_max;      // X 轴 ADC 最大值（对应屏幕最右）
    int32_t y_min;      // Y 轴 ADC 最小值（对应屏幕最上）
    int32_t y_max;      // Y 轴 ADC 最大值（对应屏幕最下）
    bool    swap_xy;    // 是否交换 X/Y 轴
    bool    invert_x;   // 是否镜像 X 轴
    bool    invert_y;   // 是否镜像 Y 轴
} touch_calibration_t;

/**
 * @brief 触摸屏配置
 */
typedef struct {
    spi_host_device_t spi_host;     // 与 LCD 共用的 SPI 主机 (SPI2_HOST)
    gpio_num_t        cs_gpio;      // 触摸屏片选 (GPIO13)
    gpio_num_t        irq_gpio;     // 触摸屏中断 (GPIO23)
    gpio_num_t        lcd_cs_gpio;  // LCD 片选 (GPIO2)，用于在触摸 SPI 事务期间强制关闭 LCD
    touch_calibration_t calibration; // 校准参数
} touch_config_t;

/**
 * @brief 初始化 XPT2046 触摸屏驱动
 *
 * 注意：必须在 lcd_display_init() 之后调用，因为触摸屏与 LCD 共用
 * SPI2_HOST 总线（该总线由 lcd_display_init() 初始化）。
 * 本函数仅向该总线添加一个 SPI 设备（使用独立的 CS=GPIO13）。
 *
 * @param cfg 触摸屏配置
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t touch_driver_init(const touch_config_t *cfg);

/**
 * @brief 读取一个触摸点（含压力检测与坐标校准）
 *
 * 通过 XPT2046 读取 X/Y 原始 ADC 值及压力（Z1/Z2），
 * 判断是否真正按下（压力阈值），并将原始坐标校准为屏幕像素坐标。
 *
 * @param point 输出触摸点（x/y 为屏幕像素坐标，touched 表示是否按下）
 * @return esp_err_t ESP_OK 表示读取成功
 */
esp_err_t touch_driver_get_point(touch_point_t *point);

/**
 * @brief 获取触摸屏中断引脚电平（用于快速判断是否有触摸）
 * @return true 表示有触摸按下（IRQ 拉低）
 */
bool touch_driver_is_pressed(void);

/**
 * @brief 检查触摸屏是否已完成校准
 *
 * 校准完成后会将校准参数与"已校准"标记持久化到 NVS。
 * 每次上电时调用本函数判断是否需要重新校准。
 *
 * @return true 表示已校准（校准参数已保存到 NVS）
 */
bool touch_driver_is_calibrated(void);

/**
 * @brief 从 NVS 加载校准参数
 *
 * 若 NVS 中已保存校准参数且"已校准"标记存在，则加载成功。
 * 加载成功后，运行时校准参数会被更新为加载值。
 *
 * @param calib 输出校准参数（可为 NULL，仅用于判断是否已校准）
 * @return true 表示加载成功且已校准
 */
bool touch_driver_load_calibration(touch_calibration_t *calib);

/**
 * @brief 保存校准参数到 NVS 并标记为已校准
 *
 * 校准流程完成后调用，将校准参数持久化，并写入"已校准"标记，
 * 使后续上电无需重新校准。
 *
 * @param calib 要保存的校准参数
 * @return esp_err_t ESP_OK 表示保存成功
 */
esp_err_t touch_driver_save_calibration(const touch_calibration_t *calib);

/**
 * @brief 清除 NVS 中保存的触摸屏校准参数
 *
 * 调用后下次上电将重新执行交互式校准流程。
 * 用于校准参数错误时强制重新校准（如触摸屏安装位置变更）。
 *
 * @return esp_err_t ESP_OK 表示清除成功
 */
esp_err_t touch_driver_erase_calibration(void);

/**
 * @brief 设置运行时校准参数（不保存到 NVS）
 *
 * 用于校准过程中临时更新校准参数，或加载 NVS 参数后应用。
 *
 * @param calib 校准参数
 */
void touch_driver_set_calibration(const touch_calibration_t *calib);

/**
 * @brief 读取原始 ADC 坐标（未校准）
 *
 * 用于校准流程：读取 X/Y 原始 ADC 值及压力，判断是否有效按下。
 * 与 touch_driver_get_point() 不同，本函数返回原始 ADC 值而非屏幕像素坐标。
 *
 * @param raw_x 输出原始 X ADC 值 (0~4095)
 * @param raw_y 输出原始 Y ADC 值 (0~4095)
 * @return true 表示检测到有效触摸（压力足够且坐标有效）
 */
bool touch_driver_get_raw_point(uint16_t *raw_x, uint16_t *raw_y);

/**
 * @brief 根据 4 个角点的原始 ADC 采样计算校准参数
 *
 * 校准流程：用户依次触摸屏幕 4 个角（左上/右上/左下/右下），
 * 记录每个角点的原始 ADC 值，然后调用本函数自动计算校准参数。
 * 本函数会自动判断 X/Y 轴是否需要交换 (swap_xy) 及是否需要镜像
 * (invert_x / invert_y)，并计算各轴的最小/最大值。
 *
 * @param raw_tl 左上角原始采样 [raw_x, raw_y]
 * @param raw_tr 右上角原始采样 [raw_x, raw_y]
 * @param raw_bl 左下角原始采样 [raw_x, raw_y]
 * @param raw_br 右下角原始采样 [raw_x, raw_y]
 * @param calib  输出计算得到的校准参数
 * @return esp_err_t ESP_OK 表示计算成功
 */
esp_err_t touch_driver_calibrate_from_corners(
    const uint16_t raw_tl[2], const uint16_t raw_tr[2],
    const uint16_t raw_bl[2], const uint16_t raw_br[2],
    touch_calibration_t *calib);

#ifdef __cplusplus
}
#endif

#endif // TOUCH_DRIVER_H
