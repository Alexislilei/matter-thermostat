#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_control.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_timer.h"

static led_strip_handle_t s_led_strip = NULL;

static const rgb_color_t COLOR_WHITE  = {100, 100, 100};
static const rgb_color_t COLOR_OFF    = {0, 0, 0};
static const rgb_color_t COLOR_YELLOW = {120, 120, 0};
static const rgb_color_t COLOR_ORANGE = {150, 60, 0};
static const rgb_color_t COLOR_RED    = {150, 0, 0};
static const rgb_color_t COLOR_PURPLE = {100, 0, 100};
static const rgb_color_t COLOR_GREEN  = {0, 120, 0};
static const rgb_color_t COLOR_BLUE   = {0, 0, 150};
static const rgb_color_t COLOR_CYAN   = {0, 120, 120};

esp_err_t led_control_init(gpio_num_t gpio_num) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio_num,
        .max_leds = LED_NUM_TOTAL,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip);
    if (ret == ESP_OK && s_led_strip) {
        led_strip_clear(s_led_strip);
    }
    return ret;
}

void led_control_post(void) {
    if (!s_led_strip) return;
    for (int i = 0; i < LED_NUM_TOTAL; i++) {
        led_strip_clear(s_led_strip);
        led_strip_set_pixel(s_led_strip, i, COLOR_WHITE.r, COLOR_WHITE.g, COLOR_WHITE.b);
        led_strip_refresh(s_led_strip);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    led_strip_clear(s_led_strip);
}

// 得到正弦呼吸系数 (0.0f - 1.0f)，period_ms 为呼吸周期
static float get_breathing_factor(uint32_t period_ms) {
    int64_t now_ms = esp_timer_get_time() / 1000;
    float phase = (float)(now_ms % period_ms) / (float)period_ms;
    return (sinf(phase * 2.0f * M_PI) + 1.0f) / 2.0f;
}

void led_control_update(const thermostat_dev_t *dev) {
    if (!s_led_strip || !dev) return;

    rgb_color_t leds[LED_NUM_TOTAL] = {COLOR_OFF};

    if (dev->mode == THERMOSTAT_MODE_PAIRING) {
        // 配网模式：LED_1 显示黄色呼吸灯，呼吸周期 2 秒
        float factor = get_breathing_factor(2000);
        leds[0].r = (uint8_t)(COLOR_YELLOW.r * factor);
        leds[0].g = (uint8_t)(COLOR_YELLOW.g * factor);
        leds[0].b = (uint8_t)(COLOR_YELLOW.b * factor);
    } 
    else if (dev->mode == THERMOSTAT_MODE_STANDBY) {
        // 待机模式：LED_2~6 熄灭，LED_1 根据实测温度以 2s 周期呼吸
        float factor = get_breathing_factor(2000);
        rgb_color_t base_color = COLOR_BLUE;
        int t = (int)roundf(dev->current_temp);
        if (t <= 17) base_color = COLOR_BLUE;
        else if (t == 18) base_color = COLOR_CYAN;
        else if (t == 19) base_color = COLOR_GREEN;
        else if (t == 20) base_color = COLOR_YELLOW;
        else if (t == 21) base_color = COLOR_ORANGE;
        else if (t == 22) base_color = COLOR_RED;
        else base_color = COLOR_PURPLE; // >= 23

        leds[0].r = (uint8_t)(base_color.r * factor);
        leds[0].g = (uint8_t)(base_color.g * factor);
        leds[0].b = (uint8_t)(base_color.b * factor);
    } 
    else if (dev->mode == THERMOSTAT_MODE_ON) {
        // 1. LED_1 加热/状态指示灯
        if (dev->is_heating) {
            // 循环 200ms 橙色 -> 200ms 红色 -> 200ms 紫色
            int64_t now_ms = esp_timer_get_time() / 1000;
            int step = (now_ms / 200) % 3;
            if (step == 0) leds[0] = COLOR_ORANGE;
            else if (step == 1) leds[0] = COLOR_RED;
            else leds[0] = COLOR_PURPLE;
        } else {
            leds[0] = COLOR_GREEN; // 未加热常绿
        }

        // 2. LED_2 ~ LED_6 温度阶梯指示灯
        // 映射规则：LED_2(<=17C), LED_3(18C), LED_4(19C), LED_5(20C), LED_6(>=21C)
        float cur = dev->current_temp;
        int target = dev->target_temp;

        for (int i = 1; i <= 5; i++) {
            // i=1 (LED_2, threshold 17), i=2 (LED_3, 18), i=3 (LED_4, 19), i=4 (LED_5, 20), i=5 (LED_6, 21)
            int led_temp = 16 + i;

            if (led_temp == target) {
                // 设定温度对应的指示灯
                if (cur < (float)target) {
                    // 紫色呼吸灯 (周期 1s)
                    float factor = get_breathing_factor(1000);
                    leds[i].r = (uint8_t)(COLOR_PURPLE.r * factor);
                    leds[i].g = (uint8_t)(COLOR_PURPLE.g * factor);
                    leds[i].b = (uint8_t)(COLOR_PURPLE.b * factor);
                } else {
                    // 实测 >= 设定：交替显示绿色和紫色，各 1s
                    int64_t now_ms = esp_timer_get_time() / 1000;
                    if ((now_ms / 1000) % 2 == 0) {
                        leds[i] = COLOR_GREEN;
                    } else {
                        leds[i] = COLOR_PURPLE;
                    }
                }
            } else if ((float)led_temp < cur) {
                // 小于当前实测温度：常亮绿色
                leds[i] = COLOR_GREEN;
            } else {
                // 大于当前实测且非设定：熄灭
                leds[i] = COLOR_OFF;
            }
        }
    }

    // 刷新 RGB 数组到硬件
    for (int i = 0; i < LED_NUM_TOTAL; i++) {
        led_strip_set_pixel(s_led_strip, i, leds[i].r, leds[i].g, leds[i].b);
    }
    led_strip_refresh(s_led_strip);
}
