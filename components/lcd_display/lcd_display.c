#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "lcd_display.h"

static const char *TAG = "LCD_DISPLAY";

// ---- 硬件引脚定义 (与 docs/01_requirements.md 一致) ----
#define LCD_PIN_SCK     GPIO_NUM_12
#define LCD_PIN_MOSI    GPIO_NUM_11
#define LCD_PIN_MISO    GPIO_NUM_10
#define LCD_PIN_CS      GPIO_NUM_2
#define LCD_PIN_DC      GPIO_NUM_3
#define LCD_PIN_RESET   GPIO_NUM_1
#define LCD_PIN_BL      GPIO_NUM_0

// ---- 屏幕参数 ----
#define LCD_H_RES       240   // 宽 (竖屏)
#define LCD_V_RES       320   // 高
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)  // 20MHz SPI 时钟

// ---- LVGL 显示缓冲 ----
#define LVGL_BUF_HEIGHT 40    // 部分刷新缓冲高度

static lv_disp_t *s_disp = NULL;
static thermostat_dev_t *s_dev = NULL;

// ---- UI 控件句柄 ----
static lv_obj_t *s_lbl_time;        // 顶部信息栏 (日期/时间)
static lv_obj_t *s_lbl_wifi;        // 顶部信息栏 (Wi-Fi 符号, 靠最右)
static lv_obj_t *s_lbl_room_temp;   // 当前室温大字
static lv_obj_t *s_lbl_room_label;  // "TEMP" 标签
static lv_obj_t *s_lbl_set_temp;    // 设定温度
static lv_obj_t *s_lbl_heat;        // 加热状态
static lv_obj_t *s_lbl_timer;       // 定时状态
static lv_obj_t *s_lbl_standby;     // 待机页 STANDBY 文字

// 记录上次渲染状态，避免无变化时重复刷新
static float s_last_room_temp = -999.0f;
static float s_last_set_temp = -999.0f;
static bool  s_last_heating = false;
static int   s_last_timer_setting = -1;
static bool  s_last_timer_active = false;
static ui_page_t s_last_page = UI_PAGE_MAIN;
static thermostat_mode_t s_last_mode = THERMOSTAT_MODE_STANDBY;

// 创建主页面控件
static void ui_create_main_page(void) {
    // 顶部信息栏 (0-30px)
    // 左侧：日期/时间 (原 14 号字放大 1.5 倍 -> 20 号字)
    s_lbl_time = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_lbl_time, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_time, lv_color_white(), 0);
    lv_obj_align(s_lbl_time, LV_ALIGN_TOP_LEFT, 8, 6);

    // 右侧：Wi-Fi 符号 (靠最右边放置, 与时间同字号)
    s_lbl_wifi = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_lbl_wifi, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_wifi, lv_color_white(), 0);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_TOP_RIGHT, -8, 6);

    // 当前室温大字 (居中, 30-180px 区域, 原 32 号字放大 2 倍 -> 48 号字)
    s_lbl_room_temp = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_lbl_room_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_lbl_room_temp, lv_color_white(), 0);
    lv_obj_align(s_lbl_room_temp, LV_ALIGN_CENTER, 0, -60);

    // "TEMP" 标签
    s_lbl_room_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_lbl_room_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_room_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(s_lbl_room_label, LV_ALIGN_CENTER, 0, -20);

    // 设定温度 (180-250px 区域, 原 20 号字放大 2 倍 -> 40 号字)
    s_lbl_set_temp = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_lbl_set_temp, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_lbl_set_temp, lv_color_hex(0x00BFFF), 0);
    lv_obj_align(s_lbl_set_temp, LV_ALIGN_CENTER, 0, 40);

    // 底部状态栏 (250-320px, 原 16 号字放大 1.5 倍 -> 24 号字)
    // 左侧：加热状态
    s_lbl_heat = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_lbl_heat, &lv_font_montserrat_24, 0);
    lv_obj_align(s_lbl_heat, LV_ALIGN_BOTTOM_LEFT, 12, -12);

    // 右侧：定时状态
    s_lbl_timer = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_lbl_timer, &lv_font_montserrat_24, 0);
    lv_obj_align(s_lbl_timer, LV_ALIGN_BOTTOM_RIGHT, -12, -12);

    // 待机页 STANDBY 文字 (默认隐藏)
    s_lbl_standby = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_lbl_standby, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_lbl_standby, lv_color_white(), 0);
    lv_obj_align(s_lbl_standby, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_flag(s_lbl_standby, LV_OBJ_FLAG_HIDDEN);
}

// 更新主页面显示内容
static void ui_update_main_page(void) {
    char buf[64];

    // 顶部信息栏：左侧日期时间，右侧 Wi-Fi 符号 (靠最右)
    // 由于未接入 RTC/网络时间，此处显示固定占位时间，
    // Wi-Fi 状态使用 LVGL 内置符号 LV_SYMBOL_WIFI (U+F1EB)，
    // 该符号已包含在 lv_font_montserrat_20 字库中，无需额外加载字体。
    lv_label_set_text(s_lbl_time, "2026-08-11 10:30");
    lv_label_set_text(s_lbl_wifi, LV_SYMBOL_WIFI);

    // 当前室温 (保留 1 位小数)
    snprintf(buf, sizeof(buf), "%.1f C", s_dev->current_temp);
    lv_label_set_text(s_lbl_room_temp, buf);

    // 室温下方的 "TEMP" 标签
    lv_label_set_text(s_lbl_room_label, "TEMP");

    // 设定温度
    snprintf(buf, sizeof(buf), "SET: %.0f C", s_dev->target_temp);
    lv_label_set_text(s_lbl_set_temp, buf);

    // 加热状态
    if (s_dev->is_heating) {
        lv_label_set_text(s_lbl_heat, "HEAT");
        lv_obj_set_style_text_color(s_lbl_heat, lv_color_hex(0xFF0000), 0);
    } else {
        lv_label_set_text(s_lbl_heat, "OFF");
        lv_obj_set_style_text_color(s_lbl_heat, lv_color_hex(0x888888), 0);
    }

    // 定时状态
    if (s_dev->sleep_timer_active && s_dev->sleep_timer_setting > 0) {
        snprintf(buf, sizeof(buf), "TIMER: %dM", s_dev->sleep_timer_setting);
    } else {
        snprintf(buf, sizeof(buf), "TIMER: OFF");
    }
    lv_label_set_text(s_lbl_timer, buf);
}

// 更新待机页显示内容
static void ui_update_standby_page(void) {
    char buf[64];

    // 顶部信息栏 (Wi-Fi 状态使用 LVGL 内置符号 LV_SYMBOL_WIFI)
    lv_label_set_text(s_lbl_time, "2026-08-11 10:30");
    lv_label_set_text(s_lbl_wifi, LV_SYMBOL_WIFI);

    // 当前室温 (中号字)
    snprintf(buf, sizeof(buf), "%.1f C", s_dev->current_temp);
    lv_label_set_text(s_lbl_room_temp, buf);

    // STANDBY 大字
    lv_label_set_text(s_lbl_standby, "STANDBY");
}

// 根据当前状态切换页面显示
static void ui_render(void) {
    if (!s_dev) return;

    // 待机模式：显示待机页
    if (s_dev->mode == THERMOSTAT_MODE_STANDBY) {
        lv_obj_clear_flag(s_lbl_standby, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_set_temp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_heat, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_timer, LV_OBJ_FLAG_HIDDEN);
        // 待机页室温用中号字
        lv_obj_set_style_text_font(s_lbl_room_temp, &lv_font_montserrat_20, 0);
        ui_update_standby_page();
        return;
    }

    // 开机模式：显示主页面
    lv_obj_add_flag(s_lbl_standby, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_lbl_set_temp, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_lbl_heat, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_lbl_timer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(s_lbl_room_temp, &lv_font_montserrat_48, 0);
    ui_update_main_page();
}

esp_err_t lcd_display_init(thermostat_dev_t *dev) {
    if (!dev) return ESP_ERR_INVALID_ARG;
    s_dev = dev;

    // ---- 1. 初始化 SPI 总线 ----
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_PIN_SCK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = LCD_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    // ---- 2. 初始化 LCD 面板 IO (SPI) ----
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle),
                        TAG, "LCD panel IO init failed");

    // ---- 3. 初始化 ILI9341 面板 ----
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RESET,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel),
                        TAG, "ILI9341 panel init failed");

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    // 本面板无需反色；若开启反色会导致黑/白互换（黑底变白底）
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
    // 直接对面板硬件应用水平镜像，修正屏幕左右镜像。
    // 注意：disp_cfg.rotation.mirror_x 仅在 lv_disp_set_rotation() 触发
    // lvgl_port_update_callback() 时才会被应用到面板，而本代码从不调用该函数，
    // 因此必须在此处直接调用 esp_lcd_panel_mirror() 才能生效。
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // ---- 4. 初始化 LVGL 端口 (必须先于 lvgl_port_add_disp 调用) ----
    // 该函数会初始化 LVGL 内存池并创建 LVGL 定时器/任务，
    // 若未调用则 lvgl_port_add_disp 内部 lv_mem_alloc 会因内存池未初始化而崩溃
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port init failed");

    // ---- 5. 初始化 LVGL 显示端口 ----
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * LVGL_BUF_HEIGHT,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,   // 修正左右镜像
            .mirror_y = false,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "LVGL display port add failed");
        return ESP_FAIL;
    }

    // ---- 6. 创建 UI 控件 ----
    lvgl_port_lock(0);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0);
    ui_create_main_page();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LCD display initialized: %dx%d", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

void lcd_display_update(thermostat_dev_t *dev) {
    if (!dev || !s_disp) return;
    s_dev = dev;

    // 检测状态变化，仅在变化时刷新 UI
    bool changed = false;
    if (dev->current_temp != s_last_room_temp ||
        dev->target_temp != s_last_set_temp ||
        dev->is_heating != s_last_heating ||
        dev->sleep_timer_setting != s_last_timer_setting ||
        dev->sleep_timer_active != s_last_timer_active ||
        dev->current_page != s_last_page ||
        dev->mode != s_last_mode) {
        changed = true;
    }

    if (changed) {
        s_last_room_temp = dev->current_temp;
        s_last_set_temp = dev->target_temp;
        s_last_heating = dev->is_heating;
        s_last_timer_setting = dev->sleep_timer_setting;
        s_last_timer_active = dev->sleep_timer_active;
        s_last_page = dev->current_page;
        s_last_mode = dev->mode;

        lvgl_port_lock(0);
        ui_render();
        lvgl_port_unlock();
    }

    // 处理 LVGL 定时器任务 (tick 由 esp_lvgl_port 内部处理)
    lv_timer_handler();
}