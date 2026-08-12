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

// ---- 瞬态灯效状态机 ----
// 用于播放一次性灯效序列（配网成功/失败、恢复出厂、解绑等）
// 灯效播放完毕后自动回到正常显示模式

typedef enum {
    EFFECT_PHASE_IDLE = 0,      // 无瞬态灯效
    EFFECT_PHASE_BLINK_ON,      // 快闪亮阶段 (200ms)
    EFFECT_PHASE_BLINK_OFF,     // 快闪灭阶段 (200ms)
    EFFECT_PHASE_SEQUENCE,      // 逐个点亮阶段 (恢复出厂 LED_2~6)
    EFFECT_PHASE_DONE,          // 灯效播放完毕
} effect_phase_t;

static struct {
    led_effect_type_t active_effect;  // 当前正在播放的灯效类型
    effect_phase_t phase;             // 当前阶段
    int64_t phase_start_ms;           // 当前阶段起始时间
    int blink_count;                  // 已完成的快闪次数
    int blink_total;                  // 快闪总次数
    int seq_index;                    // 顺序点亮当前索引
    rgb_color_t blink_color;          // 当前快闪颜色
    rgb_color_t blink_color2;         // 第二段快闪颜色 (用于解绑灯效)
    bool use_second_color;            // 是否使用第二段颜色
} s_effect_state = {
    .active_effect = LED_EFFECT_NONE,
    .phase = EFFECT_PHASE_IDLE,
    .phase_start_ms = 0,
    .blink_count = 0,
    .blink_total = 0,
    .seq_index = 0,
    .use_second_color = false,
};

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

// ---- 瞬态灯效状态机处理 ----
// 返回 true 表示灯效仍在播放，false 表示已结束
static bool process_transient_effect(rgb_color_t leds[LED_NUM_TOTAL]) {
    if (s_effect_state.active_effect == LED_EFFECT_NONE) {
        return false;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    // 初始化灯效
    if (s_effect_state.phase == EFFECT_PHASE_IDLE) {
        s_effect_state.phase = EFFECT_PHASE_BLINK_ON;
        s_effect_state.phase_start_ms = now_ms;
        s_effect_state.blink_count = 0;
        s_effect_state.seq_index = 0;
        s_effect_state.use_second_color = false;

        // 根据灯效类型设置参数
        switch (s_effect_state.active_effect) {
        case LED_EFFECT_COMMISSIONING_OK:
            s_effect_state.blink_total = 2;
            s_effect_state.blink_color = COLOR_GREEN;
            break;
        case LED_EFFECT_COMMISSIONING_FAIL:
            s_effect_state.blink_total = 2;
            s_effect_state.blink_color = COLOR_RED;
            break;
        case LED_EFFECT_FACTORY_RESET:
            // 先 LED_1 白色两下，再 LED_2~6 依次闪一下
            s_effect_state.blink_total = 2;
            s_effect_state.blink_color = COLOR_WHITE;
            break;
        case LED_EFFECT_UNBIND:
            // 橙色一下 + 绿色一下
            s_effect_state.blink_total = 1;
            s_effect_state.blink_color = COLOR_ORANGE;
            s_effect_state.blink_color2 = COLOR_GREEN;
            s_effect_state.use_second_color = true;
            break;
        default:
            break;
        }
    }

    // 处理快闪阶段 (BLINK_ON / BLINK_OFF)
    if (s_effect_state.phase == EFFECT_PHASE_BLINK_ON ||
        s_effect_state.phase == EFFECT_PHASE_BLINK_OFF) {

        int64_t elapsed = now_ms - s_effect_state.phase_start_ms;

        if (s_effect_state.phase == EFFECT_PHASE_BLINK_ON && elapsed >= 200) {
            // 亮 200ms 结束，切换到灭
            s_effect_state.phase = EFFECT_PHASE_BLINK_OFF;
            s_effect_state.phase_start_ms = now_ms;
            // 灭阶段：LED_1 熄灭
            led_strip_set_pixel(s_led_strip, 0, 0, 0, 0);
            led_strip_refresh(s_led_strip);
            return true;
        }

        if (s_effect_state.phase == EFFECT_PHASE_BLINK_OFF && elapsed >= 200) {
            // 灭 200ms 结束，计数+1
            s_effect_state.blink_count++;

            if (s_effect_state.blink_count >= s_effect_state.blink_total) {
                // 当前颜色段完成
                if (s_effect_state.use_second_color) {
                    // 切换到第二段颜色
                    s_effect_state.use_second_color = false;
                    s_effect_state.blink_color = s_effect_state.blink_color2;
                    s_effect_state.blink_count = 0;
                    s_effect_state.blink_total = 1;
                    s_effect_state.phase = EFFECT_PHASE_BLINK_ON;
                    s_effect_state.phase_start_ms = now_ms;
                    led_strip_set_pixel(s_led_strip, 0,
                                        s_effect_state.blink_color.r,
                                        s_effect_state.blink_color.g,
                                        s_effect_state.blink_color.b);
                    led_strip_refresh(s_led_strip);
                    return true;
                }

                // 快闪阶段结束
                if (s_effect_state.active_effect == LED_EFFECT_FACTORY_RESET) {
                    // 进入顺序点亮阶段
                    s_effect_state.phase = EFFECT_PHASE_SEQUENCE;
                    s_effect_state.phase_start_ms = now_ms;
                    s_effect_state.seq_index = 1; // LED_2 (index 1)
                    // 先熄灭所有 LED
                    for (int i = 0; i < LED_NUM_TOTAL; i++) {
                        led_strip_set_pixel(s_led_strip, i, 0, 0, 0);
                    }
                    led_strip_refresh(s_led_strip);
                    return true;
                } else {
                    // 其他灯效结束
                    s_effect_state.phase = EFFECT_PHASE_DONE;
                    s_effect_state.active_effect = LED_EFFECT_NONE;
                    // 清除所有 LED
                    for (int i = 0; i < LED_NUM_TOTAL; i++) {
                        led_strip_set_pixel(s_led_strip, i, 0, 0, 0);
                    }
                    led_strip_refresh(s_led_strip);
                    return false;
                }
            }

            // 还有更多快闪，切换到亮
            s_effect_state.phase = EFFECT_PHASE_BLINK_ON;
            s_effect_state.phase_start_ms = now_ms;
            led_strip_set_pixel(s_led_strip, 0,
                                s_effect_state.blink_color.r,
                                s_effect_state.blink_color.g,
                                s_effect_state.blink_color.b);
            led_strip_refresh(s_led_strip);
            return true;
        }

        // 仍在当前阶段内
        if (s_effect_state.phase == EFFECT_PHASE_BLINK_ON) {
            led_strip_set_pixel(s_led_strip, 0,
                                s_effect_state.blink_color.r,
                                s_effect_state.blink_color.g,
                                s_effect_state.blink_color.b);
            led_strip_refresh(s_led_strip);
        }
        return true;
    }

    // 处理顺序点亮阶段 (仅 FACTORY_RESET)
    if (s_effect_state.phase == EFFECT_PHASE_SEQUENCE) {
        int64_t elapsed = now_ms - s_effect_state.phase_start_ms;

        if (elapsed >= 200) {
            // 当前 LED 亮 200ms 结束，熄灭它
            led_strip_set_pixel(s_led_strip, s_effect_state.seq_index, 0, 0, 0);
            led_strip_refresh(s_led_strip);

            s_effect_state.seq_index++;
            if (s_effect_state.seq_index >= LED_NUM_TOTAL) {
                // 所有 LED_2~6 都闪过了，灯效结束
                s_effect_state.phase = EFFECT_PHASE_DONE;
                s_effect_state.active_effect = LED_EFFECT_NONE;
                for (int i = 0; i < LED_NUM_TOTAL; i++) {
                    led_strip_set_pixel(s_led_strip, i, 0, 0, 0);
                }
                led_strip_refresh(s_led_strip);
                return false;
            }

            // 点亮下一个 LED
            s_effect_state.phase_start_ms = now_ms;
            led_strip_set_pixel(s_led_strip, s_effect_state.seq_index,
                                COLOR_WHITE.r, COLOR_WHITE.g, COLOR_WHITE.b);
            led_strip_refresh(s_led_strip);
            return true;
        }

        // 仍在当前 LED 亮阶段
        led_strip_set_pixel(s_led_strip, s_effect_state.seq_index,
                            COLOR_WHITE.r, COLOR_WHITE.g, COLOR_WHITE.b);
        led_strip_refresh(s_led_strip);
        return true;
    }

    return false;
}

// 检查是否有新的瞬态灯效请求，如果有则启动
static void check_pending_effect(const thermostat_dev_t *dev) {
    if (!dev) return;

    // 如果当前有灯效正在播放，不打断
    if (s_effect_state.active_effect != LED_EFFECT_NONE) {
        return;
    }

    // 记录上一次已启动的灯效类型，防止在 main.c 清零 pending_led_effect
    // 之前重复触发同一个灯效
    static led_effect_type_t s_last_started_effect = LED_EFFECT_NONE;

    if (dev->pending_led_effect != LED_EFFECT_NONE &&
        dev->pending_led_effect != s_last_started_effect) {
        s_last_started_effect = dev->pending_led_effect;
        s_effect_state.active_effect = dev->pending_led_effect;
        s_effect_state.phase = EFFECT_PHASE_IDLE;
    }

    // 当 pending_led_effect 被 main.c 清零后，重置 last_started 以允许
    // 未来再次触发同类型灯效
    if (dev->pending_led_effect == LED_EFFECT_NONE) {
        s_last_started_effect = LED_EFFECT_NONE;
    }
}

bool led_control_effect_finished(const thermostat_dev_t *dev) {
    (void)dev; // dev 参数保留用于未来扩展
    return (s_effect_state.active_effect == LED_EFFECT_NONE &&
            s_effect_state.phase == EFFECT_PHASE_IDLE);
}

// 根据当前 DHT11 实测温度，返回待机模式 LED_1 呼吸灯颜色
// 映射：<=17 蓝, 18 青, 19 绿, 20 黄, 21 橙, 22 红, >=23 紫
static rgb_color_t get_standby_color(float temp) {
    if (temp <= 17.0f) return COLOR_BLUE;
    if (temp <= 18.0f) return COLOR_CYAN;
    if (temp <= 19.0f) return COLOR_GREEN;
    if (temp <= 20.0f) return COLOR_YELLOW;
    if (temp <= 21.0f) return COLOR_ORANGE;
    if (temp <= 22.0f) return COLOR_RED;
    return COLOR_PURPLE;
}

void led_control_update(const thermostat_dev_t *dev) {
    if (!s_led_strip || !dev) return;

    // 检查是否有待处理的瞬态灯效
    check_pending_effect(dev);

    rgb_color_t leds[LED_NUM_TOTAL] = {COLOR_OFF};

    // 如果瞬态灯效正在播放，使用灯效覆盖
    if (process_transient_effect(leds)) {
        // 瞬态灯效已直接写入硬件，跳过正常模式渲染
        return;
    }

    // ---- 正常模式渲染 ----
    if (dev->mode == THERMOSTAT_MODE_PAIRING) {
        // 配网进行中：LED_1 显示黄色呼吸灯，呼吸周期为 2 秒
        float factor = get_breathing_factor(2000);
        leds[0].r = (uint8_t)(COLOR_YELLOW.r * factor);
        leds[0].g = (uint8_t)(COLOR_YELLOW.g * factor);
        leds[0].b = (uint8_t)(COLOR_YELLOW.b * factor);
        // LED_2 ~ LED_6 熄灭
    } else if (dev->mode == THERMOSTAT_MODE_ON) {
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
        int target = (int)dev->target_temp;

        for (int i = 1; i <= 5; i++) {
            int led_temp = 16 + i;
            if (led_temp == target) {
                // 设定温度对应的指示灯：显示紫色
                // 若实测温度 < 设定温度：显示紫色
                // 若实测温度 >= 设定温度：交替显示绿色和紫色，各切换保持 1 秒
                if (cur < dev->target_temp) {
                    leds[i] = COLOR_PURPLE;
                } else {
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

        // 开机模式所有 LED 亮度设定为 50%
        for (int i = 0; i < LED_NUM_TOTAL; i++) {
            leds[i].r = leds[i].r / 2;
            leds[i].g = leds[i].g / 2;
            leds[i].b = leds[i].b / 2;
        }
    } else if (dev->mode == THERMOSTAT_MODE_STANDBY) {
        // 待机模式：LED_1 根据实测温度以 2 秒周期呼吸显示对应颜色
        // LED_2 ~ LED_6 熄灭
        rgb_color_t color = get_standby_color(dev->current_temp);
        float factor = get_breathing_factor(2000);
        leds[0].r = (uint8_t)(color.r * factor);
        leds[0].g = (uint8_t)(color.g * factor);
        leds[0].b = (uint8_t)(color.b * factor);
    }

    // 刷新 RGB 数组到硬件
    for (int i = 0; i < LED_NUM_TOTAL; i++) {
        led_strip_set_pixel(s_led_strip, i, leds[i].r, leds[i].g, leds[i].b);
    }
    led_strip_refresh(s_led_strip);
}
