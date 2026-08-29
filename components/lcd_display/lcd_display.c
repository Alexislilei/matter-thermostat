#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "lcd_display.h"
#include "touch_driver.h"

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

// ---- 温度刻度参数 ----
#define TEMP_MIN        15.0f
#define TEMP_MAX        25.0f
#define ARC_START_ANGLE 135
#define ARC_END_ANGLE   45
#define ARC_SWEEP_ANGLE 270.0f
#define DIAL_CENTER_X   120
#define DIAL_CENTER_Y   148

static lv_disp_t *s_disp = NULL;
static lv_indev_t *s_indev_touch = NULL;
static thermostat_dev_t *s_dev = NULL;

// ---- 主页面 UI 控件句柄 ----
static lv_obj_t *s_main_cont = NULL;          // 主页面主容器
static lv_obj_t *s_cont_top_bar = NULL;       // 顶部状态栏白底容器
static lv_obj_t *s_lbl_time = NULL;           // 顶部信息栏 (日期/时间)
static lv_obj_t *s_lbl_wifi = NULL;           // 顶部信息栏 (Wi-Fi 图标)
static lv_obj_t *s_lbl_heat_top = NULL;       // 顶部信息栏 (加热状态 "H"，位于 Wi-Fi 图标左侧)

static lv_obj_t *s_arc_current = NULL;        // 当前温度蓝色圆弧
static lv_obj_t *s_arc_target = NULL;         // 目标温度黄色调节圆弧 (可拖动)
static lv_obj_t *s_scale_dots[5];             // 5 个刻度圆点 (15, 17.5, 20, 22.5, 25)
static lv_obj_t *s_lbl_scale_min = NULL;      // "15°" 极值标注
static lv_obj_t *s_lbl_scale_max = NULL;      // "25°" 极值标注

static lv_obj_t *s_obj_inner_dial = NULL;     // 中央深色表盘圆盘
static lv_obj_t *s_cont_temp = NULL;          // 中央当前温度容器 (整数+小数)
static lv_obj_t *s_lbl_current_temp = NULL;   // 中央当前温度整数部分 (48 号字, 如 23)
static lv_obj_t *s_lbl_current_temp_dec = NULL;// 中央当前温度小数部分 (24 号字, 如 .5)
static lv_obj_t *s_lbl_current_title = NULL;  // "ROOM" 标签

static lv_obj_t *s_lbl_target_temp_val = NULL;// 黄色刻度线外侧目标温度读数 (如 22.5°)

static lv_obj_t *s_btn_heat = NULL;           // 底部左侧：加热状态卡片/按钮
static lv_obj_t *s_lbl_heat_title = NULL;     // "HEAT" 标题
static lv_obj_t *s_lbl_heat_status = NULL;    // "ON" / "OFF"

static lv_obj_t *s_btn_timer = NULL;          // 底部右侧：Sleep Timer 卡片/按钮
static lv_obj_t *s_lbl_timer_title = NULL;    // "SLEEP TIMER"
static lv_obj_t *s_lbl_timer_val = NULL;      // "OFF" 或倒计时 "28:30"

static lv_obj_t *s_lbl_standby = NULL;        // 待机页 STANDBY 文字
static lv_obj_t *s_lbl_standby_temp = NULL;   // 待机页当前温度整数部分文字
static lv_obj_t *s_lbl_standby_temp_dec = NULL;// 待机页当前温度小数部分文字

// ---- Sleep Timer 设置页控件 ----
static lv_obj_t *s_lbl_sleep_title;      // 页面标题 "SLEEP TIMER SETTING"
static lv_obj_t *s_sleep_opt_box[4];     // 4 个选项容器 (10 / 30 / 60 / 90 MIN)
static lv_obj_t *s_lbl_sleep_options[4]; // 4 个选项文字标签

#define SLEEP_OPT_CENTER_Y   182
#define SLEEP_OPT_SPACING    40
#define SLEEP_OPT_BOX_W      120
#define SLEEP_OPT_BOX_H      36
#define SLEEP_TITLE_BOTTOM   66
#define SLEEP_SCREEN_BOTTOM  302

// ---- 温度偏移 (Temp Offset) 设置页控件 ----
static lv_obj_t *s_lbl_offset_title;     // 页面标题 "TEMP OFFSET SETTING"
static lv_obj_t *s_lbl_offset_value;     // 大号偏移值显示 "CALIB: -1.5°C"
static lv_obj_t *s_lbl_offset_hint;      // 操作提示 "Rotate knob to adjust"

// ---- 触摸校准页面控件 ----
static lv_obj_t *s_calib_title;
static lv_obj_t *s_calib_hint;
static lv_obj_t *s_calib_target;
static bool      s_calib_active = false;
static touch_calib_step_t s_calib_step = TOUCH_CALIB_STEP_TL;

// 记录上次渲染状态，避免无变化时重复刷新
static float s_last_room_temp = -999.0f;
static float s_last_set_temp = -999.0f;
static bool  s_last_heating = false;
static int   s_last_timer_setting = -1;
static bool  s_last_timer_active = false;
static ui_page_t s_last_page = UI_PAGE_MAIN;
static thermostat_mode_t s_last_mode = THERMOSTAT_MODE_STANDBY;
static bool  s_last_wifi_connected = false;

// ---- 倒计时与 Wi-Fi 闪烁状态 ----
static int     s_last_timer_remaining_sec = -1;
static bool    s_blink_visible = true;
static int64_t s_last_blink_toggle_ms = 0;

static bool    s_wifi_blink_visible = true;
static int64_t s_last_wifi_blink_toggle_ms = 0;

// ---- 顶部时间显示状态 ----
static char s_last_time_str[32] = {0};

// 前向声明
static void update_target_indicator_pos(float target_temp);
static int timer_remaining_seconds(const thermostat_dev_t *dev);
static void ui_render(void);

// 触摸输入设备读取回调 (接入 LVGL)
static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    if (s_calib_active) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }
    touch_point_t pt;
    if (touch_driver_get_point(&pt) == ESP_OK && pt.touched) {
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// 目标温度圆弧拖动事件回调 (步长 0.5°C 吸附)
static void arc_target_event_cb(lv_event_t *e) {
    if (!s_dev || s_dev->mode != THERMOSTAT_MODE_ON) return;
    lv_obj_t *arc = lv_event_get_target(e);
    int val = lv_arc_get_value(arc); // 30 ~ 50 (代表 15.0 ~ 25.0)
    float new_target = (float)val * 0.5f;

    if (new_target < TEMP_MIN) new_target = TEMP_MIN;
    if (new_target > TEMP_MAX) new_target = TEMP_MAX;

    if (fabsf(new_target - s_dev->target_temp) >= 0.25f) {
        thermostat_set_target_temperature(s_dev, new_target);
        s_dev->last_input_time_ms = esp_timer_get_time() / 1000;
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f°", s_dev->target_temp);
        lv_label_set_text(s_lbl_target_temp_val, buf);
        update_target_indicator_pos(s_dev->target_temp);
    }
}

// 底部 Timer 按钮点击事件回调
static void btn_timer_click_cb(lv_event_t *e) {
    if (!s_dev || s_dev->mode != THERMOSTAT_MODE_ON) return;
    int64_t now_ms = esp_timer_get_time() / 1000;
    s_dev->last_input_time_ms = now_ms;

    if (s_dev->sleep_timer_active) {
        s_dev->sleep_timer_active = false;
        s_dev->sleep_timer_start_ms = 0;
        ESP_LOGI(TAG, "Touch Sleep Timer button -> OFF");
    } else {
        s_dev->sleep_timer_active = true;
        s_dev->sleep_timer_start_ms = now_ms;
        ESP_LOGI(TAG, "Touch Sleep Timer button -> %d min countdown started", s_dev->sleep_timer_setting);
    }
}

// 底部 Settings 按钮点击事件回调
// 行为与物理 FUNC 按键一致：开机下短按循环切换页面
// (主页面 -> Sleep Timer 设置页 -> 温度偏移设置页 -> 主页面)。
static void btn_settings_click_cb(lv_event_t *e) {
    if (!s_dev || s_dev->mode != THERMOSTAT_MODE_ON) return;
    s_dev->last_input_time_ms = esp_timer_get_time() / 1000;

    // 页面循环：主页面 -> Sleep Timer 设置页 -> 温度偏移设置页 -> 主页面
    // 离开温度偏移页时保存偏移量到 NVS (改动后记忆)。
    if (s_dev->current_page == UI_PAGE_MAIN) {
        s_dev->current_page = UI_PAGE_SLEEP_TIMER;
        ESP_LOGI(TAG, "Touch Settings button -> Enter SLEEP TIMER SETTING page");
    } else if (s_dev->current_page == UI_PAGE_SLEEP_TIMER) {
        s_dev->current_page = UI_PAGE_TEMP_OFFSET;
        ESP_LOGI(TAG, "Touch Settings button -> Enter TEMP OFFSET SETTING page");
    } else {
        // 温度偏移设置页 -> 主页面
        thermostat_temp_offset_save(s_dev);
        s_dev->current_page = UI_PAGE_MAIN;
        ESP_LOGI(TAG, "Touch Settings button -> Back to MAIN page (temp offset saved)");
    }
}

// 更新黄色刻度线外侧目标温度标签的绝对位置
static void update_target_indicator_pos(float target_temp) {
    if (!s_lbl_target_temp_val) return;
    if (target_temp < TEMP_MIN) target_temp = TEMP_MIN;
    if (target_temp > TEMP_MAX) target_temp = TEMP_MAX;

    float frac = (target_temp - TEMP_MIN) / (TEMP_MAX - TEMP_MIN);
    float angle_deg = (float)ARC_START_ANGLE + frac * ARC_SWEEP_ANGLE;
    float rad = angle_deg * (3.14159265f / 180.0f);

    // 刻度环半径为 76px，外侧读数标签中心半径设为 96px (留出适当间距)
    int r = 96;
    int cx = DIAL_CENTER_X + (int)(r * cosf(rad));
    int cy = DIAL_CENTER_Y + (int)(r * sinf(rad));

    // 限制在屏幕有效区域内 (Top bar 高度 26px，cy>=36 保证标签顶部 cy-8>=28 完全处于白底栏下方)
    if (cx < 24) cx = 24;
    if (cx > 216) cx = 216;
    if (cy < 36) cy = 36;
    if (cy > 244) cy = 244;

    lv_obj_align(s_lbl_target_temp_val, LV_ALIGN_TOP_LEFT, cx - 18, cy - 8);
}

// 创建主页面控件
static void ui_create_main_page(void) {
    // 主页面容器 (0,0 ~ 240,320)
    s_main_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_main_cont, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(s_main_cont, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_bg_opa(s_main_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_main_cont, 0, 0);
    lv_obj_set_style_radius(s_main_cont, 0, 0);
    lv_obj_set_style_pad_all(s_main_cont, 0, 0);
    lv_obj_align(s_main_cont, LV_ALIGN_TOP_LEFT, 0, 0);
    // 禁用滚动与溢出裁剪，避免子控件（底部按钮）触摸区域被截断
    lv_obj_clear_flag(s_main_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(s_main_cont, false, 0);

    // 1. 顶部状态栏 (Top Bar) - 白底黑字纯二维扁平设计 (挂载在屏幕根节点，主页/待机/睡眠设置页共用)
    s_cont_top_bar = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_cont_top_bar, LCD_H_RES, 26);
    lv_obj_set_style_bg_color(s_cont_top_bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_cont_top_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cont_top_bar, 0, 0);
    lv_obj_set_style_radius(s_cont_top_bar, 0, 0);
    lv_obj_set_style_pad_all(s_cont_top_bar, 0, 0);
    lv_obj_align(s_cont_top_bar, LV_ALIGN_TOP_MID, 0, 0);

    // 左侧：日期与时间 (Fri, Mar 11  19:45)
    s_lbl_time = lv_label_create(s_cont_top_bar);
    lv_obj_set_style_text_font(s_lbl_time, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_time, lv_color_black(), 0);
    lv_obj_align(s_lbl_time, LV_ALIGN_LEFT_MID, 8, 0);

    // 右侧：加热状态 "H" (位于 Wi-Fi 图标左侧)
    // 需求：不加热时显示黑色，加热时显示红色。
    s_lbl_heat_top = lv_label_create(s_cont_top_bar);
    lv_obj_set_style_text_font(s_lbl_heat_top, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_heat_top, lv_color_black(), 0);
    lv_label_set_text(s_lbl_heat_top, "H");
    lv_obj_align(s_lbl_heat_top, LV_ALIGN_RIGHT_MID, -30, 0);

    // 右侧：Wi-Fi 状态图标
    s_lbl_wifi = lv_label_create(s_cont_top_bar);
    lv_obj_set_style_text_font(s_lbl_wifi, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_wifi, lv_color_black(), 0);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_RIGHT_MID, -8, 0);

    // 2. 中央圆环温度区 (X: 120, Y: 148, R=84)
    // 2.1 刻度固定圆点 (5 个分度点: 15.0°, 17.5°, 20.0°, 22.5°, 25.0°) - 纯二维扁平方点，无圆角彩虹边缘
    static const float dot_temps[5] = {15.0f, 17.5f, 20.0f, 22.5f, 25.0f};
    for (int i = 0; i < 5; i++) {
        float frac = (dot_temps[i] - TEMP_MIN) / (TEMP_MAX - TEMP_MIN);
        float deg = (float)ARC_START_ANGLE + frac * ARC_SWEEP_ANGLE;
        float rad = deg * (3.14159265f / 180.0f);
        int dot_r = 76;
        int dx = DIAL_CENTER_X + (int)(dot_r * cosf(rad));
        int dy = DIAL_CENTER_Y + (int)(dot_r * sinf(rad));

        s_scale_dots[i] = lv_obj_create(s_main_cont);
        lv_obj_set_size(s_scale_dots[i], 5, 5);
        lv_obj_set_style_bg_color(s_scale_dots[i], (i == 0 || i == 4) ? lv_color_hex(0x94A3B8) : lv_color_hex(0x475569), 0);
        lv_obj_set_style_bg_opa(s_scale_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_scale_dots[i], 0, 0);
        lv_obj_set_style_border_width(s_scale_dots[i], 0, 0);
        lv_obj_set_style_pad_all(s_scale_dots[i], 0, 0);
        lv_obj_align(s_scale_dots[i], LV_ALIGN_TOP_LEFT, dx - 2, dy - 2);
        lv_obj_clear_flag(s_scale_dots[i], LV_OBJ_FLAG_CLICKABLE);
    }

    // 2.2 极值标注 ("15°" 和 "25°")
    s_lbl_scale_min = lv_label_create(s_main_cont);
    lv_obj_set_style_text_font(s_lbl_scale_min, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_scale_min, lv_color_white(), 0);
    lv_label_set_text(s_lbl_scale_min, "15°");
    lv_obj_align(s_lbl_scale_min, LV_ALIGN_TOP_LEFT, 34, 208);

    s_lbl_scale_max = lv_label_create(s_main_cont);
    lv_obj_set_style_text_font(s_lbl_scale_max, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_scale_max, lv_color_white(), 0);
    lv_label_set_text(s_lbl_scale_max, "25°");
    lv_obj_align(s_lbl_scale_max, LV_ALIGN_TOP_LEFT, 180, 208);

    // 2.3 当前温度蓝色圆弧 (不可拖动，只作显示)
    s_arc_current = lv_arc_create(s_main_cont);
    lv_obj_set_size(s_arc_current, 152, 152);
    lv_obj_align(s_arc_current, LV_ALIGN_CENTER, 0, -12);
    lv_arc_set_rotation(s_arc_current, 0);
    lv_arc_set_bg_angles(s_arc_current, ARC_START_ANGLE, ARC_END_ANGLE);
    lv_arc_set_angles(s_arc_current, ARC_START_ANGLE, ARC_START_ANGLE);
    lv_arc_set_range(s_arc_current, (int)(TEMP_MIN * 10), (int)(TEMP_MAX * 10));
    lv_arc_set_value(s_arc_current, (int)(20.0f * 10));

    // 样式：深灰蓝底轨，亮蓝指示弧，隐藏滑块
    lv_obj_set_style_arc_width(s_arc_current, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_current, lv_color_hex(0x18202E), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_arc_current, true, LV_PART_MAIN);

    lv_obj_set_style_arc_width(s_arc_current, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc_current, lv_color_hex(0x00B4FF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc_current, true, LV_PART_INDICATOR);

    // 隐藏 knob
    lv_obj_set_style_opa(s_arc_current, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_arc_current, 0, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc_current, LV_OBJ_FLAG_CLICKABLE);

    // 2.4 目标温度黄色调节圆弧 (可触摸拖动，吸附 0.5°C)
    // 范围 30 ~ 50 (对应 15.0 ~ 25.0, 步长 1 = 0.5°C)
    s_arc_target = lv_arc_create(s_main_cont);
    lv_obj_set_size(s_arc_target, 152, 152);
    lv_obj_align(s_arc_target, LV_ALIGN_CENTER, 0, -12);
    lv_arc_set_rotation(s_arc_target, 0);
    lv_arc_set_bg_angles(s_arc_target, ARC_START_ANGLE, ARC_END_ANGLE);
    lv_arc_set_range(s_arc_target, 30, 50);
    lv_arc_set_value(s_arc_target, 40); // 20.0°C

    // 底轨与指示弧全透明，仅保留黄色 Knob 作为刻度线指针
    lv_obj_set_style_arc_opa(s_arc_target, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arc_target, LV_OPA_TRANSP, LV_PART_INDICATOR);

    // 黄色刻度线 Knob 样式
    lv_obj_set_style_bg_color(s_arc_target, lv_color_hex(0xFFD700), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_arc_target, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_color(s_arc_target, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_border_width(s_arc_target, 2, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_arc_target, 4, LV_PART_KNOB);
    lv_obj_set_style_radius(s_arc_target, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_add_event_cb(s_arc_target, arc_target_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // 2.5 黄色刻度线外侧目标温度数值标签 (如 22.5°)
    s_lbl_target_temp_val = lv_label_create(s_main_cont);
    lv_obj_set_style_text_font(s_lbl_target_temp_val, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_target_temp_val, lv_color_hex(0xFFD700), 0);
    lv_label_set_text(s_lbl_target_temp_val, "20.0°");
    lv_obj_clear_flag(s_lbl_target_temp_val, LV_OBJ_FLAG_CLICKABLE);

    // 2.6 中央深色表盘圆盘 (同心圆)
    s_obj_inner_dial = lv_obj_create(s_main_cont);
    lv_obj_set_size(s_obj_inner_dial, 114, 114);
    lv_obj_align(s_obj_inner_dial, LV_ALIGN_CENTER, 0, -12);
    lv_obj_set_style_bg_color(s_obj_inner_dial, lv_color_hex(0x111722), 0);
    lv_obj_set_style_bg_opa(s_obj_inner_dial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_obj_inner_dial, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(s_obj_inner_dial, 3, 0);
    lv_obj_set_style_radius(s_obj_inner_dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_obj_inner_dial, 0, 0);
    lv_obj_clear_flag(s_obj_inner_dial, LV_OBJ_FLAG_CLICKABLE);

    // 2.7 中央当前温度 (整数部分 48 号字 + 小数部分 24 号字，共 3 位有效数字，如 23.5)
    // 因增加小数显示宽度，整体下移以在表盘内获得更宽的显示位置
    s_cont_temp = lv_obj_create(s_obj_inner_dial);
    lv_obj_set_size(s_cont_temp, 92, 48);
    lv_obj_set_style_bg_opa(s_cont_temp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cont_temp, 0, 0);
    lv_obj_set_style_pad_all(s_cont_temp, 0, 0);
    lv_obj_align(s_cont_temp, LV_ALIGN_CENTER, 0, 4);
    lv_obj_clear_flag(s_cont_temp, LV_OBJ_FLAG_CLICKABLE);

    // 整数部分 (如 23)
    s_lbl_current_temp = lv_label_create(s_cont_temp);
    lv_obj_set_style_text_font(s_lbl_current_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_lbl_current_temp, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_lbl_current_temp, "23");
    lv_obj_align(s_lbl_current_temp, LV_ALIGN_LEFT_MID, 0, 0);

    // 小数部分 (如 .5)，字号为整数部分的一半 (24 号字)，底部对齐以保持基线一致
    s_lbl_current_temp_dec = lv_label_create(s_cont_temp);
    lv_obj_set_style_text_font(s_lbl_current_temp_dec, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_current_temp_dec, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_lbl_current_temp_dec, ".5");
    lv_obj_align(s_lbl_current_temp_dec, LV_ALIGN_BOTTOM_LEFT, 56, 0);

    // 2.8 "ROOM" 标签 (纯白色，加粗/增大至 16 号字，提升对比度与清晰度)
    // 随温度整体下移，保持与温度之间的相对间距
    s_lbl_current_title = lv_label_create(s_obj_inner_dial);
    lv_obj_set_style_text_font(s_lbl_current_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_current_title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_lbl_current_title, "ROOM");
    lv_obj_align(s_lbl_current_title, LV_ALIGN_CENTER, 0, 34);

    // 3. 底部状态与操作栏 (Bottom Area) - 纯二维直角设计，彻底消除倒角线彩虹纹
    // 3.1 左侧：Settings 卡片/按钮 (点击行为与物理 FUNC 按键一致，循环切换页面)
    s_btn_heat = lv_btn_create(s_main_cont);
    lv_obj_set_size(s_btn_heat, 108, 62);
    lv_obj_align(s_btn_heat, LV_ALIGN_BOTTOM_LEFT, 8, -12);  // 上移避开触摸校准底部盲区
    lv_obj_set_style_bg_color(s_btn_heat, lv_color_hex(0x131A26), 0);
    lv_obj_set_style_border_color(s_btn_heat, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(s_btn_heat, 2, 0);
    lv_obj_set_style_radius(s_btn_heat, 0, 0); // 直角无倒角抗锯齿彩虹
    lv_obj_set_style_pad_all(s_btn_heat, 4, 0);
    lv_obj_add_event_cb(s_btn_heat, btn_settings_click_cb, LV_EVENT_CLICKED, NULL);

    s_lbl_heat_title = lv_label_create(s_btn_heat);
    lv_obj_set_style_text_font(s_lbl_heat_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_heat_title, lv_color_white(), 0);
    lv_label_set_text(s_lbl_heat_title, "SETTINGS");
    lv_obj_align(s_lbl_heat_title, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_clear_flag(s_lbl_heat_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_lbl_heat_title, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_lbl_heat_status = lv_label_create(s_btn_heat);
    lv_obj_set_style_text_font(s_lbl_heat_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_heat_status, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(s_lbl_heat_status, "MENU");
    lv_obj_align(s_lbl_heat_status, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_clear_flag(s_lbl_heat_status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_lbl_heat_status, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 3.2 右侧：Sleep Timer 卡片/按钮
    s_btn_timer = lv_btn_create(s_main_cont);
    lv_obj_set_size(s_btn_timer, 108, 62);
    lv_obj_align(s_btn_timer, LV_ALIGN_BOTTOM_RIGHT, -8, -12);  // 上移避开触摸校准底部盲区
    lv_obj_set_style_bg_color(s_btn_timer, lv_color_hex(0x131A26), 0);
    lv_obj_set_style_border_color(s_btn_timer, lv_color_hex(0x334155), 0);
    lv_obj_set_style_border_width(s_btn_timer, 2, 0);
    lv_obj_set_style_radius(s_btn_timer, 0, 0); // 直角无倒角抗锯齿彩虹
    lv_obj_set_style_pad_all(s_btn_timer, 4, 0);
    lv_obj_add_event_cb(s_btn_timer, btn_timer_click_cb, LV_EVENT_CLICKED, NULL);

    s_lbl_timer_title = lv_label_create(s_btn_timer);
    lv_obj_set_style_text_font(s_lbl_timer_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_timer_title, lv_color_white(), 0);
    lv_label_set_text(s_lbl_timer_title, "SLEEP TIMER");
    lv_obj_align(s_lbl_timer_title, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_clear_flag(s_lbl_timer_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_lbl_timer_title, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_lbl_timer_val = lv_label_create(s_btn_timer);
    lv_obj_set_style_text_font(s_lbl_timer_val, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_timer_val, lv_color_hex(0xE2E8F0), 0);
    lv_label_set_text(s_lbl_timer_val, "OFF");
    lv_obj_align(s_lbl_timer_val, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_clear_flag(s_lbl_timer_val, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_lbl_timer_val, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 4. 待机页控件
    // 待机页当前温度整数部分 (48 号字)
    s_lbl_standby_temp = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_lbl_standby_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_lbl_standby_temp, lv_color_white(), 0);
    lv_obj_align(s_lbl_standby_temp, LV_ALIGN_CENTER, -14, -20);
    lv_obj_add_flag(s_lbl_standby_temp, LV_OBJ_FLAG_HIDDEN);

    // 待机页当前温度小数部分 (24 号字，字号为整数部分的一半)
    s_lbl_standby_temp_dec = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_lbl_standby_temp_dec, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_lbl_standby_temp_dec, lv_color_white(), 0);
    lv_obj_align(s_lbl_standby_temp_dec, LV_ALIGN_CENTER, 20, -8);
    lv_obj_add_flag(s_lbl_standby_temp_dec, LV_OBJ_FLAG_HIDDEN);

    s_lbl_standby = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_lbl_standby, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_lbl_standby, lv_color_white(), 0);
    lv_obj_align(s_lbl_standby, LV_ALIGN_CENTER, 0, 45);
    lv_obj_add_flag(s_lbl_standby, LV_OBJ_FLAG_HIDDEN);
}

// 创建 Sleep Timer 设置页控件
static void ui_create_sleep_timer_page(void) {
    static const char *option_texts[4] = {"10 MIN", "30 MIN", "60 MIN", "90 MIN"};

    s_lbl_sleep_title = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_lbl_sleep_title, LCD_H_RES, 36);
    lv_obj_set_style_bg_color(s_lbl_sleep_title, lv_color_hex(0x0055AA), 0);
    lv_obj_set_style_bg_opa(s_lbl_sleep_title, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_lbl_sleep_title, 0, 0);
    lv_obj_set_style_radius(s_lbl_sleep_title, 0, 0);
    lv_obj_set_style_pad_all(s_lbl_sleep_title, 0, 0);
    lv_obj_align(s_lbl_sleep_title, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *title_lbl = lv_label_create(s_lbl_sleep_title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_black(), 0);
    lv_label_set_text(title_lbl, "SLEEP TIMER SETTING");
    lv_obj_center(title_lbl);

    for (int i = 0; i < 4; i++) {
        s_sleep_opt_box[i] = lv_obj_create(lv_scr_act());
        lv_obj_set_size(s_sleep_opt_box[i], SLEEP_OPT_BOX_W, SLEEP_OPT_BOX_H);
        lv_obj_set_style_bg_color(s_sleep_opt_box[i], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_sleep_opt_box[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_sleep_opt_box[i], 0, 0);
        lv_obj_set_style_radius(s_sleep_opt_box[i], 4, 0);
        lv_obj_set_style_pad_all(s_sleep_opt_box[i], 0, 0);

        s_lbl_sleep_options[i] = lv_label_create(s_sleep_opt_box[i]);
        lv_obj_set_style_text_font(s_lbl_sleep_options[i], &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(s_lbl_sleep_options[i], lv_color_hex(0x555555), 0);
        lv_label_set_text(s_lbl_sleep_options[i], option_texts[i]);
        lv_obj_center(s_lbl_sleep_options[i]);

        lv_obj_add_flag(s_sleep_opt_box[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// 创建温度偏移 (Temp Offset) 设置页控件
static void ui_create_temp_offset_page(void) {
    // 页面标题栏 (与 Sleep Timer 页风格一致，蓝色标题栏)
    s_lbl_offset_title = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_lbl_offset_title, LCD_H_RES, 36);
    lv_obj_set_style_bg_color(s_lbl_offset_title, lv_color_hex(0x0055AA), 0);
    lv_obj_set_style_bg_opa(s_lbl_offset_title, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_lbl_offset_title, 0, 0);
    lv_obj_set_style_radius(s_lbl_offset_title, 0, 0);
    lv_obj_set_style_pad_all(s_lbl_offset_title, 0, 0);
    lv_obj_align(s_lbl_offset_title, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *title_lbl = lv_label_create(s_lbl_offset_title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_black(), 0);
    lv_label_set_text(title_lbl, "TEMP OFFSET SETTING");
    lv_obj_center(title_lbl);

    // 大号偏移值显示 "CALIB: -1.5°C"
    s_lbl_offset_value = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_lbl_offset_value, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_lbl_offset_value, lv_color_white(), 0);
    lv_label_set_text(s_lbl_offset_value, "CALIB: 0.0°C");
    lv_obj_align(s_lbl_offset_value, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_flag(s_lbl_offset_value, LV_OBJ_FLAG_HIDDEN);

    // 操作提示文字
    s_lbl_offset_hint = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_lbl_offset_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_offset_hint, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(s_lbl_offset_hint, "Rotate knob to adjust");
    lv_obj_align(s_lbl_offset_hint, LV_ALIGN_CENTER, 0, 30);
    lv_obj_add_flag(s_lbl_offset_hint, LV_OBJ_FLAG_HIDDEN);
}

// 更新温度偏移 (Temp Offset) 设置页显示内容
static void ui_update_temp_offset_page(void) {
    char buf[32];
    // 需求：显示 "CALIB: X.X°C"，如 "CALIB: -1.5°C"
    snprintf(buf, sizeof(buf), "CALIB: %+.1f°C", s_dev->temp_offset);
    lv_label_set_text(s_lbl_offset_value, buf);
}

// 创建触摸校准页面控件
static void ui_create_calib_page(void) {
    s_calib_title = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_calib_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_calib_title, lv_color_white(), 0);
    lv_label_set_text(s_calib_title, "TOUCH CALIBRATION");
    lv_obj_align(s_calib_title, LV_ALIGN_CENTER, 0, -60);
    lv_obj_add_flag(s_calib_title, LV_OBJ_FLAG_HIDDEN);

    s_calib_hint = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(s_calib_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_calib_hint, lv_color_hex(0x00BFFF), 0);
    lv_label_set_text(s_calib_hint, "Touch the crosshair");
    lv_obj_align(s_calib_hint, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_calib_hint, LV_OBJ_FLAG_HIDDEN);

    s_calib_target = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_calib_target, 40, 40);
    lv_obj_set_style_bg_color(s_calib_target, lv_color_hex(0x00BFFF), 0);
    lv_obj_set_style_bg_opa(s_calib_target, LV_OPA_50, 0);
    lv_obj_set_style_border_color(s_calib_target, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_calib_target, 3, 0);
    lv_obj_set_style_radius(s_calib_target, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(s_calib_target, LV_OBJ_FLAG_HIDDEN);
}

static void ui_update_calib_page(void) {
    static const lv_coord_t target_pos[4][2] = {
        { 30,  20 },   // TL (靠近顶部边缘)
        { 210, 20 },   // TR (靠近顶部边缘)
        { 30,  305 },  // BL (靠近底部边缘，覆盖按钮下半部分)
        { 210, 305 },  // BR (靠近底部边缘，覆盖按钮下半部分)
    };
    static const char *hint_text[4] = {
        "Touch TOP-LEFT corner",
        "Touch TOP-RIGHT corner",
        "Touch BOTTOM-LEFT corner",
        "Touch BOTTOM-RIGHT corner",
    };

    if (s_calib_step >= TOUCH_CALIB_STEP_DONE) {
        lv_label_set_text(s_calib_hint, "Calibration done!");
        lv_obj_add_flag(s_calib_target, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    int idx = (int)s_calib_step;
    lv_obj_align(s_calib_target, LV_ALIGN_TOP_LEFT,
                 target_pos[idx][0] - 20, target_pos[idx][1] - 20);
    lv_obj_clear_flag(s_calib_target, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_calib_hint, hint_text[idx]);
}

static void ui_calib_hide_all_normal(void) {
    if (s_cont_top_bar) lv_obj_add_flag(s_cont_top_bar, LV_OBJ_FLAG_HIDDEN);
    if (s_main_cont) lv_obj_add_flag(s_main_cont, LV_OBJ_FLAG_HIDDEN);
    if (s_lbl_standby_temp) lv_obj_add_flag(s_lbl_standby_temp, LV_OBJ_FLAG_HIDDEN);
    if (s_lbl_standby_temp_dec) lv_obj_add_flag(s_lbl_standby_temp_dec, LV_OBJ_FLAG_HIDDEN);
    if (s_lbl_standby) lv_obj_add_flag(s_lbl_standby, LV_OBJ_FLAG_HIDDEN);
    if (s_lbl_sleep_title) lv_obj_add_flag(s_lbl_sleep_title, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 4; i++) {
        if (s_sleep_opt_box[i]) lv_obj_add_flag(s_sleep_opt_box[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_lbl_offset_title) lv_obj_add_flag(s_lbl_offset_title, LV_OBJ_FLAG_HIDDEN);
    if (s_lbl_offset_value) lv_obj_add_flag(s_lbl_offset_value, LV_OBJ_FLAG_HIDDEN);
    if (s_lbl_offset_hint) lv_obj_add_flag(s_lbl_offset_hint, LV_OBJ_FLAG_HIDDEN);
}

static void ui_calib_restore_all_normal(void) {
    ui_render();
}

void lcd_display_calib_show(void) {
    if (!s_disp) return;
    lvgl_port_lock(0);
    s_calib_active = true;
    ui_calib_hide_all_normal();
    lv_obj_clear_flag(s_calib_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_calib_hint, LV_OBJ_FLAG_HIDDEN);
    ui_update_calib_page();
    lvgl_port_unlock();
}

void lcd_display_calib_hide(void) {
    if (!s_disp) return;
    lvgl_port_lock(0);
    s_calib_active = false;
    lv_obj_add_flag(s_calib_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_calib_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_calib_target, LV_OBJ_FLAG_HIDDEN);
    ui_calib_restore_all_normal();
    lvgl_port_unlock();
}

void lcd_display_calib_set_step(touch_calib_step_t step) {
    if (!s_disp) return;
    lvgl_port_lock(0);
    s_calib_step = step;
    ui_update_calib_page();
    lvgl_port_unlock();
}

static void ui_update_sleep_timer_page(void) {
    static const int option_values[4] = {10, 30, 60, 90};

    int sel = 0;
    for (int i = 0; i < 4; i++) {
        if (s_dev->sleep_timer_setting == option_values[i]) {
            sel = i;
            break;
        }
    }

    for (int i = 0; i < 4; i++) {
        int offset = i - sel;
        lv_coord_t center_y = SLEEP_OPT_CENTER_Y + offset * SLEEP_OPT_SPACING;

        if (center_y < SLEEP_TITLE_BOTTOM || center_y > SLEEP_SCREEN_BOTTOM) {
            lv_obj_add_flag(s_sleep_opt_box[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_sleep_opt_box[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_sleep_opt_box[i], LV_ALIGN_CENTER, 0, center_y - SLEEP_OPT_CENTER_Y);

        if (i == sel) {
            lv_obj_set_style_bg_opa(s_sleep_opt_box[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_sleep_opt_box[i], lv_color_white(), 0);
            lv_obj_set_style_border_width(s_sleep_opt_box[i], 2, 0);
            lv_obj_set_style_border_color(s_sleep_opt_box[i], lv_color_white(), 0);
            lv_obj_set_style_text_color(s_lbl_sleep_options[i], lv_color_black(), 0);
        } else {
            lv_obj_set_style_bg_opa(s_sleep_opt_box[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_sleep_opt_box[i], lv_color_black(), 0);
            lv_obj_set_style_border_width(s_sleep_opt_box[i], 0, 0);
            lv_obj_set_style_text_color(s_lbl_sleep_options[i], lv_color_white(), 0);
        }
    }
}

static int timer_remaining_seconds(const thermostat_dev_t *dev) {
    if (!dev->sleep_timer_active || dev->sleep_timer_start_ms == 0) {
        return -1;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t elapsed_ms = now_ms - dev->sleep_timer_start_ms;
    int64_t target_ms = (int64_t)dev->sleep_timer_setting * 60LL * 1000LL;
    int64_t remaining_ms = target_ms - elapsed_ms;
    if (remaining_ms <= 0) return 0;
    return (int)((remaining_ms + 999) / 1000);
}

// 格式化本地时间为 "Fri, Mar 11  19:45"
static void format_local_time(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm timeinfo;
    if (now == (time_t)-1 || !localtime_r(&now, &timeinfo)) {
        snprintf(buf, len, "--, --- --  --:--");
        return;
    }

    if (timeinfo.tm_year + 1900 < 2024) {
        snprintf(buf, len, "Fri, Mar 11  19:45");
        return;
    }

    // 格式：Fri, Mar 11  19:45
    strftime(buf, len, "%a, %b %d  %H:%M", &timeinfo);
}

static void ui_update_wifi_symbol(void) {
    if (s_dev->wifi_connected) {
        lv_label_set_text(s_lbl_wifi, LV_SYMBOL_WIFI);
        lv_obj_clear_flag(s_lbl_wifi, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_lbl_wifi, LV_SYMBOL_WIFI);
        if (s_wifi_blink_visible) {
            lv_obj_clear_flag(s_lbl_wifi, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_lbl_wifi, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// 更新主页面显示内容
static void ui_update_main_page(void) {
    char buf[64];

    // 1. 顶部状态栏
    format_local_time(buf, sizeof(buf));
    lv_label_set_text(s_lbl_time, buf);
    ui_update_wifi_symbol();

    // 2. 中央当前温度 (3 位有效数字，整数部分 48 号字 + 小数部分 24 号字，如 23.5)
    int cur_int = (int)floorf(s_dev->current_temp);
    int cur_dec = (int)roundf((s_dev->current_temp - cur_int) * 10.0f);
    if (cur_dec >= 10) { cur_dec = 0; cur_int += 1; }
    snprintf(buf, sizeof(buf), "%d", cur_int);
    lv_label_set_text(s_lbl_current_temp, buf);
    snprintf(buf, sizeof(buf), ".%d", cur_dec);
    lv_label_set_text(s_lbl_current_temp_dec, buf);

    // 3. 当前温度蓝色圆弧进度更新 (15.0 ~ 25.0)
    int arc_val = (int)(s_dev->current_temp * 10.0f);
    if (arc_val < (int)(TEMP_MIN * 10)) arc_val = (int)(TEMP_MIN * 10);
    if (arc_val > (int)(TEMP_MAX * 10)) arc_val = (int)(TEMP_MAX * 10);
    lv_arc_set_value(s_arc_current, arc_val);

    // 4. 目标温度黄色圆弧与读数更新
    int target_step_val = (int)roundf(s_dev->target_temp * 2.0f);
    if (target_step_val < 30) target_step_val = 30;
    if (target_step_val > 50) target_step_val = 50;
    lv_arc_set_value(s_arc_target, target_step_val);

    snprintf(buf, sizeof(buf), "%.1f°", s_dev->target_temp);
    lv_label_set_text(s_lbl_target_temp_val, buf);
    update_target_indicator_pos(s_dev->target_temp);

    // 5. 顶部加热状态 "H" (位于 Wi-Fi 图标左侧)
    //    需求：不加热时显示黑色，加热时显示红色。
    if (s_dev->is_heating) {
        lv_obj_set_style_text_color(s_lbl_heat_top, lv_color_hex(0xFF0000), 0);
    } else {
        lv_obj_set_style_text_color(s_lbl_heat_top, lv_color_black(), 0);
    }

    // 6. Sleep Timer 卡片
    if (s_dev->sleep_timer_active && s_dev->sleep_timer_setting > 0) {
        int remaining = timer_remaining_seconds(s_dev);
        if (remaining > 0) {
            int mm = remaining / 60;
            int ss = remaining % 60;
            snprintf(buf, sizeof(buf), "%02d:%02d", mm, ss);
        } else {
            snprintf(buf, sizeof(buf), "00:00");
        }
        lv_label_set_text(s_lbl_timer_val, buf);
        lv_obj_set_style_text_color(s_lbl_timer_val, lv_color_hex(0x00FF7F), 0);
        lv_obj_set_style_border_color(s_btn_timer, lv_color_hex(0x00FF7F), 0);
    } else {
        snprintf(buf, sizeof(buf), "OFF");
        lv_label_set_text(s_lbl_timer_val, buf);
        lv_obj_set_style_text_color(s_lbl_timer_val, lv_color_hex(0x94A3B8), 0);
        lv_obj_set_style_border_color(s_btn_timer, lv_color_hex(0x334155), 0);
    }
}

// 更新待机页显示内容
static void ui_update_standby_page(void) {
    char buf[64];

    format_local_time(buf, sizeof(buf));
    lv_label_set_text(s_lbl_time, buf);
    ui_update_wifi_symbol();

    // 顶部加热状态 "H" (位于 Wi-Fi 图标左侧)
    // 需求：不加热时显示黑色，加热时显示红色。
    if (s_dev->is_heating) {
        lv_obj_set_style_text_color(s_lbl_heat_top, lv_color_hex(0xFF0000), 0);
    } else {
        lv_obj_set_style_text_color(s_lbl_heat_top, lv_color_black(), 0);
    }

    int cur_int = (int)floorf(s_dev->current_temp);
    int cur_dec = (int)roundf((s_dev->current_temp - cur_int) * 10.0f);
    if (cur_dec >= 10) { cur_dec = 0; cur_int += 1; }
    snprintf(buf, sizeof(buf), "%d", cur_int);
    lv_label_set_text(s_lbl_standby_temp, buf);
    snprintf(buf, sizeof(buf), ".%d", cur_dec);
    lv_label_set_text(s_lbl_standby_temp_dec, buf);

    lv_label_set_text(s_lbl_standby, "STANDBY");
}

// 状态分发渲染
static void ui_render(void) {
    if (!s_dev) return;

    if (s_calib_active) {
        return;
    }

    // 待机模式
    if (s_dev->mode == THERMOSTAT_MODE_STANDBY) {
        lv_obj_clear_flag(s_cont_top_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_standby_temp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_standby_temp_dec, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_standby, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_main_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_sleep_title, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 4; i++) {
            lv_obj_add_flag(s_sleep_opt_box[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(s_lbl_offset_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_offset_value, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_offset_hint, LV_OBJ_FLAG_HIDDEN);
        ui_update_standby_page();
        return;
    }

    // Sleep Timer 设置页
    if (s_dev->current_page == UI_PAGE_SLEEP_TIMER) {
        lv_obj_clear_flag(s_cont_top_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_standby_temp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_standby_temp_dec, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_standby, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_main_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_sleep_title, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 4; i++) {
            lv_obj_clear_flag(s_sleep_opt_box[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(s_lbl_offset_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_offset_value, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_offset_hint, LV_OBJ_FLAG_HIDDEN);
        ui_update_sleep_timer_page();
        return;
    }

    // 温度偏移 (Temp Offset) 设置页
    if (s_dev->current_page == UI_PAGE_TEMP_OFFSET) {
        lv_obj_clear_flag(s_cont_top_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_standby_temp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_standby_temp_dec, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_standby, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_main_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_sleep_title, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 4; i++) {
            lv_obj_add_flag(s_sleep_opt_box[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(s_lbl_offset_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_offset_value, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_offset_hint, LV_OBJ_FLAG_HIDDEN);
        ui_update_temp_offset_page();
        return;
    }

    // 开机主页面
    lv_obj_clear_flag(s_cont_top_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_standby_temp, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_standby_temp_dec, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_standby, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_main_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_sleep_title, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 4; i++) {
        lv_obj_add_flag(s_sleep_opt_box[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(s_lbl_offset_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_offset_value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_offset_hint, LV_OBJ_FLAG_HIDDEN);
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
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // ---- 4. 初始化 LVGL 端口 ----
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
            .mirror_x = true,
            .mirror_y = false,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "LVGL display port add failed");
        return ESP_FAIL;
    }

    // 关闭抗锯齿 (anti-aliasing)：本屏幕为 240x320 低分辨率 RGB565，
    // 抗锯齿会产生灰蒙蒙/断断续续的模糊边缘，反而让文字与圆环显得坑洼不平。
    // 关闭后文字与图形边缘更锐利清晰。
    s_disp->driver->antialiasing = 0;

    // ---- 6. 注册 LVGL 触摸输入设备 ----
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read_cb;
    s_indev_touch = lv_indev_drv_register(&indev_drv);

    // ---- 7. 创建 UI 控件 ----
    lvgl_port_lock(0);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0A0D14), 0);
    ui_create_main_page();
    ui_create_sleep_timer_page();
    ui_create_temp_offset_page();
    ui_create_calib_page();
    ui_render();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LCD display initialized with Thermometer UI: %dx%d", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

void lcd_display_update(thermostat_dev_t *dev) {
    if (!dev || !s_disp) return;
    s_dev = dev;

    int remaining_sec = timer_remaining_seconds(dev);

    bool changed = false;
    if (dev->current_temp != s_last_room_temp ||
        dev->target_temp != s_last_set_temp ||
        dev->is_heating != s_last_heating ||
        dev->sleep_timer_setting != s_last_timer_setting ||
        dev->sleep_timer_active != s_last_timer_active ||
        dev->current_page != s_last_page ||
        dev->mode != s_last_mode ||
        dev->wifi_connected != s_last_wifi_connected ||
        remaining_sec != s_last_timer_remaining_sec) {
        changed = true;
    }

    {
        char now_time[32];
        format_local_time(now_time, sizeof(now_time));
        if (strcmp(now_time, s_last_time_str) != 0) {
            strncpy(s_last_time_str, now_time, sizeof(s_last_time_str) - 1);
            s_last_time_str[sizeof(s_last_time_str) - 1] = '\0';
            changed = true;
        }
    }

    // 倒计时最后 10 秒闪烁
    if (remaining_sec > 0 && remaining_sec <= 10) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (s_last_blink_toggle_ms == 0) {
            s_last_blink_toggle_ms = now_ms;
            s_blink_visible = true;
        } else if (now_ms - s_last_blink_toggle_ms >= 500) {
            s_last_blink_toggle_ms = now_ms;
            s_blink_visible = !s_blink_visible;
            changed = true;
        }
    } else {
        s_last_blink_toggle_ms = 0;
        s_blink_visible = true;
    }

    // Wi-Fi 未连接时闪烁
    if (!dev->wifi_connected) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (s_last_wifi_blink_toggle_ms == 0) {
            s_last_wifi_blink_toggle_ms = now_ms;
            s_wifi_blink_visible = true;
        } else if (now_ms - s_last_wifi_blink_toggle_ms >= 500) {
            s_last_wifi_blink_toggle_ms = now_ms;
            s_wifi_blink_visible = !s_wifi_blink_visible;
            changed = true;
        }
    } else {
        s_last_wifi_blink_toggle_ms = 0;
        s_wifi_blink_visible = true;
    }

    if (changed) {
        s_last_room_temp = dev->current_temp;
        s_last_set_temp = dev->target_temp;
        s_last_heating = dev->is_heating;
        s_last_timer_setting = dev->sleep_timer_setting;
        s_last_timer_active = dev->sleep_timer_active;
        s_last_page = dev->current_page;
        s_last_mode = dev->mode;
        s_last_wifi_connected = dev->wifi_connected;
        s_last_timer_remaining_sec = remaining_sec;

        lvgl_port_lock(0);
        ui_render();
        lvgl_port_unlock();
    }
}