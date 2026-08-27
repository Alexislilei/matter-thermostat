#include <string.h>
#include "touch_driver.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "TOUCH_DRIVER";

// ---- NVS 校准参数持久化 ----
// 校准完成后将校准参数与"已校准"标记保存到 NVS，使后续上电无需重新校准。
#define TOUCH_NVS_NAMESPACE  "touch_calib"
#define TOUCH_NVS_KEY_CALIB  "calib"      // 校准参数 (blob)
#define TOUCH_NVS_KEY_FLAG   "calibrated" // 已校准标记 (u8, 1=已校准)

// ---- 屏幕分辨率 (与 LCD 竖屏一致) ----
#define TOUCH_LCD_W  240
#define TOUCH_LCD_H  320

// ---- XPT2046 控制字节 (单端模式, 12-bit) ----
// 控制字节格式: [S=1][A2 A1 A0][MODE=0][SER/DFR=1][PD1 PD0]
//   X 位置: A2A1A0 = 101 -> 0xD0
//   Y 位置: A2A1A0 = 001 -> 0x90
//   Z1    : A2A1A0 = 011 -> 0xB0
//   Z2    : A2A1A0 = 100 -> 0xC0
#define XPT2046_CMD_X   0xD0
#define XPT2046_CMD_Y   0x90
#define XPT2046_CMD_Z1  0xB0
#define XPT2046_CMD_Z2  0xC0

// ---- 压力阈值 ----
// 通过 Z1/Z2 计算压力值 (pressure = Z2 - Z1)，小于该阈值视为未按下
#define TOUCH_PRESSURE_THRESHOLD  60

// ---- 采样次数 (多次采样取平均，抑制噪声) ----
// 注意：触摸屏与 LCD 共用 SPI 总线，采样次数越多，单次读取占用的 SPI 事务越多，
// 越容易干扰 LCD 帧刷新。为降低对共享总线的占用，此处采样次数设为 1。
#define TOUCH_SAMPLE_COUNT  1

// ---- 触摸屏 SPI 设备句柄 ----
static spi_device_handle_t s_spi_dev = NULL;
static touch_calibration_t s_calib;
static gpio_num_t s_irq_gpio = GPIO_NUM_NC;   // 触摸屏中断引脚 (用于快速按下检测)
static gpio_num_t s_cs_gpio   = GPIO_NUM_NC;  // 触摸屏片选引脚 (手动控制)
static gpio_num_t s_lcd_cs_gpio = GPIO_NUM_NC; // LCD 片选引脚 (用于在触摸 SPI 事务期间强制关闭 LCD)

// 读取 XPT2046 单个通道的 12-bit ADC 原始值
// 发送 1 字节控制命令，随后读取 2 字节数据（低 12 位有效）
static uint16_t read_channel(uint8_t cmd) {
    if (s_spi_dev == NULL) {
        return 0;
    }

    // 【关键修复】触摸屏与 LCD 共用 SPI2_HOST 总线。
    // 必须在整个触摸 SPI 事务（含手动 CS 操作）期间获取 SPI 总线锁，
    // 使触摸事务与 LCD 事务串行化，避免两者并发竞争破坏 SPI 数据。
    // 若不加锁，触摸事务与 LCD 事务（esp_lcd 内部会加锁）并发时，
    // XPT2046 可能收到被破坏的控制字节，进入 PENIRQ 禁用的掉电模式
    // （PD=11），导致 IRQ 引脚永久失效（触摸检测不到）。
    // spi_device_acquire_bus() 会阻塞等待总线空闲，确保独占访问。
    if (spi_device_acquire_bus(s_spi_dev, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to acquire SPI bus for touch read");
        return 0;
    }

    // 触摸屏与 LCD 共用 SPI2_HOST 总线。为保证操作触摸屏时 LCD 处于关闭状态，
    // 在触摸 SPI 事务期间强制将 LCD 片选 (GPIO2) 拉高（关闭 LCD），
    // 事务结束后再恢复。这样可避免共享总线上的触摸事务干扰 LCD 帧刷新。
    if (s_lcd_cs_gpio != GPIO_NUM_NC) {
        gpio_set_level(s_lcd_cs_gpio, 1);   // 强制关闭 LCD
    }

    // 手动拉低片选，选中触摸屏 (XPT2046 CS 低电平有效)
    if (s_cs_gpio != GPIO_NUM_NC) {
        gpio_set_level(s_cs_gpio, 0);
    }

    // 发送缓冲区：命令字节 + 2 个填充字节（用于读取返回数据）
    uint8_t tx_buf[3] = { cmd, 0x00, 0x00 };
    uint8_t rx_buf[3] = { 0x00, 0x00, 0x00 };

    spi_transaction_t t = {
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
        .length = 8 * 3,      // 24 bits
        .rxlength = 8 * 3,    // 24 bits
    };

    esp_err_t ret = spi_device_transmit(s_spi_dev, &t);

    // 手动拉高片选，释放触摸屏
    if (s_cs_gpio != GPIO_NUM_NC) {
        gpio_set_level(s_cs_gpio, 1);
    }

    // 触摸事务结束，恢复 LCD 片选（由 esp_lcd 驱动自行管理，此处仅释放强制关闭）
    if (s_lcd_cs_gpio != GPIO_NUM_NC) {
        gpio_set_level(s_lcd_cs_gpio, 0);   // 恢复 LCD（esp_lcd 驱动会在下次事务前再次拉低）
    }

    // 释放 SPI 总线锁，允许 LCD 事务继续
    spi_device_release_bus(s_spi_dev);

    if (ret != ESP_OK) {
        return 0;
    }

    // XPT2046 返回 12-bit 数据：第一个字节的高 4 位为 0，
    // 有效数据分布在 rx_buf[1] 与 rx_buf[2] 中。
    // 实际布局：rx_buf[1] 高 8 位 + rx_buf[2] 高 4 位 = 12 bit
    uint16_t value = ((uint16_t)rx_buf[1] << 4) | ((uint16_t)rx_buf[2] >> 4);
    return value & 0x0FFF;
}

// 多次采样取中值/平均，抑制触摸噪声
static uint16_t read_channel_filtered(uint8_t cmd) {
    uint32_t sum = 0;
    for (int i = 0; i < TOUCH_SAMPLE_COUNT; i++) {
        sum += read_channel(cmd);
    }
    return (uint16_t)(sum / TOUCH_SAMPLE_COUNT);
}

// 将原始 ADC 坐标校准为屏幕像素坐标
static void calibrate_point(int32_t raw_x, int32_t raw_y, int16_t *out_x, int16_t *out_y) {
    int32_t x = raw_x;
    int32_t y = raw_y;

    // 可选交换 X/Y 轴
    if (s_calib.swap_xy) {
        int32_t tmp = x;
        x = y;
        y = tmp;
    }

    // 线性映射到屏幕像素坐标
    int32_t sx, sy;
    if (s_calib.x_max > s_calib.x_min) {
        sx = (x - s_calib.x_min) * (TOUCH_LCD_W - 1) / (s_calib.x_max - s_calib.x_min);
    } else {
        sx = 0;
    }
    if (s_calib.y_max > s_calib.y_min) {
        sy = (y - s_calib.y_min) * (TOUCH_LCD_H - 1) / (s_calib.y_max - s_calib.y_min);
    } else {
        sy = 0;
    }

    // 可选镜像
    if (s_calib.invert_x) {
        sx = (TOUCH_LCD_W - 1) - sx;
    }
    if (s_calib.invert_y) {
        sy = (TOUCH_LCD_H - 1) - sy;
    }

    // 限制在屏幕范围内
    int16_t clamped_x = sx;
    int16_t clamped_y = sy;
    if (clamped_x < 0) clamped_x = 0;
    if (clamped_x > TOUCH_LCD_W - 1) clamped_x = TOUCH_LCD_W - 1;
    if (clamped_y < 0) clamped_y = 0;
    if (clamped_y > TOUCH_LCD_H - 1) clamped_y = TOUCH_LCD_H - 1;

    // ESP_LOGI(TAG, "[TOUCH DETAIL] raw(%d,%d) [calib: x_min=%d, x_max=%d, y_min=%d, y_max=%d, swap=%d, invX=%d, invY=%d] -> mapped(%ld,%ld) -> clamped(%d,%d)",
    //          (int)raw_x, (int)raw_y,
    //          (int)s_calib.x_min, (int)s_calib.x_max, (int)s_calib.y_min, (int)s_calib.y_max,
    //          (int)s_calib.swap_xy, (int)s_calib.invert_x, (int)s_calib.invert_y,
    //          (long)sx, (long)sy, (int)clamped_x, (int)clamped_y);

    *out_x = clamped_x;
    *out_y = clamped_y;
}

esp_err_t touch_driver_init(const touch_config_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;

    s_calib = cfg->calibration;
    s_irq_gpio = cfg->irq_gpio;
    s_cs_gpio = cfg->cs_gpio;
    s_lcd_cs_gpio = cfg->lcd_cs_gpio;

    // 配置触摸屏中断 (IRQ) 引脚为输入 + 内部上拉
    // （硬件无外部上拉，需启用内部上拉，防止悬空误触发）
    gpio_config_t irq_conf = {
        .pin_bit_mask = (1ULL << cfg->irq_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&irq_conf), TAG, "Touch IRQ GPIO config failed");

    // 配置触摸屏片选 (CS) 引脚为输出，空闲电平为高（未选中）。
    // 注意：此处不将 CS 交由 SPI 驱动自动控制（spics_io_num = -1），
    // 而是手动控制。原因：若将 CS 交给 SPI 驱动 (spics_io_num = GPIO13)，
    // spi_bus_add_device() 会把 GPIO13 重新配置为 SPI 片选输出，
    // 经实测会导致共享 SPI2_HOST 总线上的 LCD 显示异常（白屏/横线）。
    // 手动控制 CS 可避免该问题。
    // 硬件无外部上拉电阻，故启用内部上拉，防止 CS 悬空导致误触发。
    gpio_config_t cs_conf = {
        .pin_bit_mask = (1ULL << cfg->cs_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   // 内部上拉，防止悬空
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cs_conf), TAG, "Touch CS GPIO config failed");
    gpio_set_level(cfg->cs_gpio, 1);   // 空闲高电平，未选中

    // 向与 LCD 共用的 SPI 总线添加触摸屏设备
    // 注意：SPI2_HOST 总线已由 lcd_display_init() 初始化，此处仅添加设备。
    // XPT2046 使用 SPI Mode 0，时钟频率不宜过高（典型 1~2 MHz）。
    // spics_io_num = -1：不使用 SPI 驱动自动片选，改为手动控制 CS，
    // 以避免 spi_bus_add_device() 重新配置共享总线导致 LCD 显示异常。
    spi_device_interface_config_t dev_cfg = {
        .mode = 0,
        .clock_speed_hz = 2000000,      // 2 MHz
        .spics_io_num = -1,             // 手动控制片选，避免干扰共享总线
        .queue_size = 4,
        // 不使用 SPI_DEVICE_HALFDUPLEX：半双工下同一事务不能同时启用
        // MOSI 与 MISO 相位，会触发 "SPI half duplex mode is not supported"
        // 错误。改用全双工事务（与共享总线上 ILI9341 LCD 一致）。
        .flags = 0,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(cfg->spi_host, &dev_cfg, &s_spi_dev),
                        TAG, "Touch SPI device add failed");

    ESP_LOGI(TAG, "XPT2046 touch driver initialized (SPI host=%d, CS=%d, IRQ=%d)",
             (int)cfg->spi_host, (int)cfg->cs_gpio, (int)cfg->irq_gpio);
    return ESP_OK;
}

bool touch_driver_is_pressed(void) {
    // XPT2046 触摸屏 IRQ 引脚为低电平有效（标准行为）：
    //   未按下时读为高电平 (1)，按下时读为低电平 (0)。
    // 实测确认（结合打印逻辑）：未按下打印 IRQ=0，按下打印 IRQ=1，
    // 即物理电平未按下=1、按下=0，故此处用 == 0 判断按下。
    // 注意：此判断非常关键。若极性判断错误，会导致未按下时也被视为"已按下"，
    // 从而持续执行 SPI 事务，占用共享 SPI 总线并干扰 LCD 帧刷新（白屏/横线）。
    if (s_irq_gpio == GPIO_NUM_NC) {
        return false;
    }
    return (gpio_get_level(s_irq_gpio) == 0);
}

esp_err_t touch_driver_get_point(touch_point_t *point) {
    if (!point || s_spi_dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // 关键优化：仅当 IRQ 引脚检测到触摸按下时才进行 SPI 读取。
    // 原因：触摸屏与 LCD 共用 SPI2_HOST 总线，若每次轮询都执行多次 SPI 事务
    // （X/Y/Z1/Z2 各采样 3 次 = 12 次事务），会持续占用共享总线，
    // 干扰 LCD 的帧刷新，导致屏幕出现横条/白屏等显示异常。
    // XPT2046 的 IRQ 引脚（低电平有效）专用于笔按下检测，未按下时直接返回，
    // 从而避免对共享 SPI 总线的无谓占用。
    if (!touch_driver_is_pressed()) {
        point->touched = false;
        point->x = 0;
        point->y = 0;
        return ESP_OK;
    }

    // 读取 X / Y / Z1 / Z2 原始值
    uint16_t raw_x = read_channel_filtered(XPT2046_CMD_X);
    uint16_t raw_y = read_channel_filtered(XPT2046_CMD_Y);
    uint16_t z1 = read_channel_filtered(XPT2046_CMD_Z1);
    uint16_t z2 = read_channel_filtered(XPT2046_CMD_Z2);

    // 计算压力值：Z2 - Z1 越大表示按压力度越大
    int32_t pressure = (int32_t)z2 - (int32_t)z1;
    if (pressure < 0) pressure = -pressure;

    // ESP_LOGI(TAG, "[TOUCH DEBUG] SPI read: raw_x=%u, raw_y=%u, z1=%u, z2=%u, pressure=%ld",
    //          raw_x, raw_y, z1, z2, (long)pressure);

    // 压力过小或坐标无效（0 或 4095 边界）视为未按下
    if (pressure < TOUCH_PRESSURE_THRESHOLD ||
        raw_x == 0 || raw_x == 0x0FFF ||
        raw_y == 0 || raw_y == 0x0FFF) {
        // ESP_LOGW(TAG, "[TOUCH DEBUG] Reading ignored: pressure threshold=%d (actual=%ld) or invalid coords",
        //          TOUCH_PRESSURE_THRESHOLD, (long)pressure);
        point->touched = false;
        point->x = 0;
        point->y = 0;
        return ESP_OK;
    }

    // 校准为屏幕像素坐标
    calibrate_point(raw_x, raw_y, &point->x, &point->y);
    point->touched = true;

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// 触摸屏校准功能
// ---------------------------------------------------------------------------

// 检查触摸屏是否已完成校准（读取 NVS 中的"已校准"标记）
bool touch_driver_is_calibrated(void) {
    nvs_handle_t handle;
    if (nvs_open(TOUCH_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    uint8_t flag = 0;
    esp_err_t err = nvs_get_u8(handle, TOUCH_NVS_KEY_FLAG, &flag);
    nvs_close(handle);
    return (err == ESP_OK && flag == 1);
}

// 从 NVS 加载校准参数；若已校准则同时更新运行时校准参数
bool touch_driver_load_calibration(touch_calibration_t *calib) {
    nvs_handle_t handle;
    if (nvs_open(TOUCH_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    // 读取"已校准"标记
    uint8_t flag = 0;
    esp_err_t err = nvs_get_u8(handle, TOUCH_NVS_KEY_FLAG, &flag);
    if (err != ESP_OK || flag != 1) {
        nvs_close(handle);
        return false;
    }

    // 读取校准参数 (blob)
    touch_calibration_t loaded;
    size_t len = sizeof(loaded);
    err = nvs_get_blob(handle, TOUCH_NVS_KEY_CALIB, &loaded, &len);
    nvs_close(handle);
    if (err != ESP_OK || len != sizeof(loaded)) {
        ESP_LOGW(TAG, "NVS calibration blob missing/invalid, treating as uncalibrated");
        return false;
    }

    // 应用加载的校准参数到运行时
    s_calib = loaded;
    if (calib) {
        *calib = loaded;
    }
    ESP_LOGI(TAG, "Loaded calibration from NVS: x[%d,%d] y[%d,%d] swap=%d invX=%d invY=%d",
             (int)loaded.x_min, (int)loaded.x_max, (int)loaded.y_min, (int)loaded.y_max,
             (int)loaded.swap_xy, (int)loaded.invert_x, (int)loaded.invert_y);
    return true;
}

// 保存校准参数到 NVS 并标记为已校准
esp_err_t touch_driver_save_calibration(const touch_calibration_t *calib) {
    if (!calib) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(TOUCH_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %d", err);
        return err;
    }

    // 保存校准参数
    err = nvs_set_blob(handle, TOUCH_NVS_KEY_CALIB, calib, sizeof(*calib));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set calib blob failed: %d", err);
        nvs_close(handle);
        return err;
    }

    // 写入"已校准"标记
    uint8_t flag = 1;
    err = nvs_set_u8(handle, TOUCH_NVS_KEY_FLAG, flag);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set calibrated flag failed: %d", err);
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %d", err);
        return err;
    }

    // 更新运行时校准参数
    s_calib = *calib;
    ESP_LOGI(TAG, "Calibration saved to NVS: x[%d,%d] y[%d,%d] swap=%d invX=%d invY=%d",
             (int)calib->x_min, (int)calib->x_max, (int)calib->y_min, (int)calib->y_max,
             (int)calib->swap_xy, (int)calib->invert_x, (int)calib->invert_y);
    return ESP_OK;
}

// 清除 NVS 中的触摸屏校准参数，迫使下次上电重新校准
esp_err_t touch_driver_erase_calibration(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TOUCH_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed for erase: %d", err);
        return err;
    }

    nvs_erase_key(handle, TOUCH_NVS_KEY_FLAG);   // 忽略删除失败错误 (可能不存在)
    nvs_erase_key(handle, TOUCH_NVS_KEY_CALIB);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGW(TAG, "Touch calibration erased from NVS - will re-calibrate on next boot");
    } else {
        ESP_LOGE(TAG, "NVS commit failed during erase: %d", err);
    }
    return err;
}


// 设置运行时校准参数（不保存到 NVS）
void touch_driver_set_calibration(const touch_calibration_t *calib) {
    if (calib) {
        s_calib = *calib;
    }
}

// 读取原始 ADC 坐标（未校准），用于校准流程
// 返回 true 表示检测到有效触摸（压力足够且坐标有效）
bool touch_driver_get_raw_point(uint16_t *raw_x, uint16_t *raw_y) {
    if (!raw_x || !raw_y || s_spi_dev == NULL) {
        return false;
    }

    // 仅当 IRQ 检测到按下时才读取，避免无谓占用共享 SPI 总线
    if (!touch_driver_is_pressed()) {
        return false;
    }

    uint16_t rx = read_channel_filtered(XPT2046_CMD_X);
    uint16_t ry = read_channel_filtered(XPT2046_CMD_Y);
    uint16_t z1 = read_channel_filtered(XPT2046_CMD_Z1);
    uint16_t z2 = read_channel_filtered(XPT2046_CMD_Z2);

    int32_t pressure = (int32_t)z2 - (int32_t)z1;
    if (pressure < 0) pressure = -pressure;

    // 压力过小或坐标无效（0 或 4095 边界）视为无效触摸
    if (pressure < TOUCH_PRESSURE_THRESHOLD ||
        rx == 0 || rx == 0x0FFF ||
        ry == 0 || ry == 0x0FFF) {
        return false;
    }

    *raw_x = rx;
    *raw_y = ry;
    return true;
}

// 根据 4 个角点的原始 ADC 采样计算校准参数
// 自动判断 X/Y 轴是否需要交换 (swap_xy) 及是否需要镜像 (invert_x/invert_y)，
// 并计算各轴的最小/最大值。
esp_err_t touch_driver_calibrate_from_corners(
        const uint16_t raw_tl[2], const uint16_t raw_tr[2],
        const uint16_t raw_bl[2], const uint16_t raw_br[2],
        touch_calibration_t *calib) {
    if (!raw_tl || !raw_tr || !raw_bl || !raw_br || !calib) {
        return ESP_ERR_INVALID_ARG;
    }

    // 原始采样数组下标：0 = XPT2046 X 通道, 1 = XPT2046 Y 通道
    // 屏幕 4 个角：
    //   TL (0,0)     TR (239,0)
    //   BL (0,319)   BR (239,319)
    //
    // 沿屏幕 X 方向 (TL->TR)：哪个原始通道变化更大，就对应屏幕 X 轴。
    // 沿屏幕 Y 方向 (TL->BL)：哪个原始通道变化更大，就对应屏幕 Y 轴。
    int dX_r0 = abs((int)raw_tr[0] - (int)raw_tl[0]);
    int dX_r1 = abs((int)raw_tr[1] - (int)raw_tl[1]);
    int dY_r0 = abs((int)raw_bl[0] - (int)raw_tl[0]);
    int dY_r1 = abs((int)raw_bl[1] - (int)raw_tl[1]);

    // 判断屏幕 X 轴对应哪个原始通道
    bool x_is_r0 = (dX_r0 >= dX_r1);   // true: 屏幕 X 用通道0; false: 屏幕 X 用通道1
    // 判断屏幕 Y 轴对应哪个原始通道
    bool y_is_r0 = (dY_r0 >= dY_r1);   // true: 屏幕 Y 用通道0; false: 屏幕 Y 用通道1

    // swap_xy：若屏幕 X 轴对应原始通道1（即 X 轴与 Y 通道绑定），则需要交换
    calib->swap_xy = !x_is_r0;

    // 收集屏幕 X 轴在各角点的原始值
    int32_t x_tl = x_is_r0 ? raw_tl[0] : raw_tl[1];
    int32_t x_tr = x_is_r0 ? raw_tr[0] : raw_tr[1];
    int32_t x_bl = x_is_r0 ? raw_bl[0] : raw_bl[1];
    int32_t x_br = x_is_r0 ? raw_br[0] : raw_br[1];

    // 收集屏幕 Y 轴在各角点的原始值
    int32_t y_tl = y_is_r0 ? raw_tl[0] : raw_tl[1];
    int32_t y_tr = y_is_r0 ? raw_tr[0] : raw_tr[1];
    int32_t y_bl = y_is_r0 ? raw_bl[0] : raw_bl[1];
    int32_t y_br = y_is_r0 ? raw_br[0] : raw_br[1];

    // 计算各轴最小/最大值
    int32_t x_min = x_tl, x_max = x_tl;
    int32_t y_min = y_tl, y_max = y_tl;
    int32_t xs[4] = {x_tl, x_tr, x_bl, x_br};
    int32_t ys[4] = {y_tl, y_tr, y_bl, y_br};
    for (int i = 0; i < 4; i++) {
        if (xs[i] < x_min) x_min = xs[i];
        if (xs[i] > x_max) x_max = xs[i];
        if (ys[i] < y_min) y_min = ys[i];
        if (ys[i] > y_max) y_max = ys[i];
    }

    // 判断是否需要镜像：
    //   invert_x：屏幕 X 从左到右增大，若原始 X 值在右侧 (TR/BR) 反而小于左侧 (TL/BL)，则需镜像
    //   invert_y：屏幕 Y 从上到下增大，若原始 Y 值在下方 (BL/BR) 反而小于上方 (TL/TR)，则需镜像
    int32_t x_left_avg  = (x_tl + x_bl) / 2;
    int32_t x_right_avg = (x_tr + x_br) / 2;
    int32_t y_top_avg   = (y_tl + y_tr) / 2;
    int32_t y_bot_avg   = (y_bl + y_br) / 2;
    calib->invert_x = (x_right_avg < x_left_avg);
    calib->invert_y = (y_bot_avg < y_top_avg);

    // 防止除零：若某轴范围过小（< 50），说明采样异常，返回错误
    if ((x_max - x_min) < 50 || (y_max - y_min) < 50) {
        ESP_LOGE(TAG, "Calibration failed: axis range too small (x[%d,%d] y[%d,%d])",
                 (int)x_min, (int)x_max, (int)y_min, (int)y_max);
        return ESP_ERR_INVALID_RESPONSE;
    }

    calib->x_min = x_min;
    calib->x_max = x_max;
    calib->y_min = y_min;
    calib->y_max = y_max;

    ESP_LOGI(TAG, "Calibration computed: x[%d,%d] y[%d,%d] swap=%d invX=%d invY=%d",
             (int)calib->x_min, (int)calib->x_max, (int)calib->y_min, (int)calib->y_max,
             (int)calib->swap_xy, (int)calib->invert_x, (int)calib->invert_y);
    return ESP_OK;
}
