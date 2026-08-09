#include "button_handler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "BUTTON_HANDLER";

static button_config_t s_cfg;
static thermostat_dev_t *s_thermostat = NULL;

typedef struct {
    gpio_num_t pin;
    int64_t press_time_ms;  // 按下起始时刻
    bool long_pressed;      // 是否已触发长按或已消费
} button_state_t;

static button_state_t s_btn_power;
static button_state_t s_btn_func;

static int64_t s_combo_press_time_ms = 0;
static bool s_combo_triggered = false;

static uint8_t s_last_ra_level = 1;
static int64_t s_last_encoder_time_ms = 0;

static void init_single_btn(button_state_t *btn, gpio_num_t pin) {
    btn->pin = pin;
    btn->press_time_ms = 0;
    btn->long_pressed = false;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

esp_err_t button_handler_init(button_config_t *cfg, thermostat_dev_t *thermostat) {
    if (!cfg || !thermostat) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    s_thermostat = thermostat;

    init_single_btn(&s_btn_power, cfg->pin_power);
    init_single_btn(&s_btn_func, cfg->pin_func);

    // 配置 EC11 编码器 GPIO (KEY_RA, KEY_RB)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << cfg->pin_key_ra) | (1ULL << cfg->pin_key_rb),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    s_last_ra_level = gpio_get_level(cfg->pin_key_ra);
    s_last_encoder_time_ms = 0;

    s_combo_press_time_ms = 0;
    s_combo_triggered = false;

    return ESP_OK;
}

void button_handler_poll(void) {
    if (!s_thermostat) return;

    int64_t now_ms = esp_timer_get_time() / 1000;

    bool raw_pwr  = (gpio_get_level(s_cfg.pin_power) == 0);
    bool raw_func = (gpio_get_level(s_cfg.pin_func) == 0);

    // 1. 半步 EC11 旋转编码器双边沿 (上升沿+下降沿) 采样与方向检测
    // 适配每 1 个物理卡点产生 1 次 RA 电平跳变 (1->0 或 0->1) 的半步编码器
    uint8_t curr_ra = gpio_get_level(s_cfg.pin_key_ra);
    uint8_t curr_rb = gpio_get_level(s_cfg.pin_key_rb);

    int encoder_direction = 0; // +1: CW (顺时针加), -1: CCW (逆时针减), 0: None

    // KEY_RA 电平发生跳变 (上升沿或下降沿)
    if (curr_ra != s_last_ra_level) {
        if (now_ms - s_last_encoder_time_ms >= 20) { // 20ms 软件防抖
            if (curr_ra == curr_rb) {
                encoder_direction = 1;  // 顺时针 CW (+1℃)
            } else {
                encoder_direction = -1; // 逆时针 CCW (-1℃)
            }
            s_last_encoder_time_ms = now_ms;
        }
        s_last_ra_level = curr_ra;
    }

    // 旋转编码器触发响应
    if (encoder_direction != 0) {
        if (s_thermostat->mode == THERMOSTAT_MODE_STANDBY) {
            ESP_LOGI(TAG, "Encoder rotated -> Wake up from STANDBY");
            thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_ON);
        } else if (s_thermostat->mode == THERMOSTAT_MODE_ON) {
            if (encoder_direction > 0) {
                ESP_LOGI(TAG, "Encoder CW -> Target temp +1");
                thermostat_set_target_temperature(s_thermostat, s_thermostat->target_temp + 1.0f);
            } else {
                ESP_LOGI(TAG, "Encoder CCW -> Target temp -1");
                thermostat_set_target_temperature(s_thermostat, s_thermostat->target_temp - 1.0f);
            }
        }
    }

    // 2. 组合键检测 (FUNC GPIO18 + POWER GPIO19 同时长按 > 3 秒)
    if (raw_pwr && raw_func) {
        if (s_combo_press_time_ms == 0) {
            s_combo_press_time_ms = now_ms;
            s_combo_triggered = false;
        } else if (!s_combo_triggered && (now_ms - s_combo_press_time_ms >= 3000)) {
            s_combo_triggered = true;
            ESP_LOGI(TAG, "Combo key (FUNC + POWER) 3s hold -> Factory Reset");
            thermostat_factory_reset(s_thermostat);
        }
        // 组合键生效期间屏蔽单键触发
        s_btn_power.long_pressed = true;
        s_btn_func.long_pressed = true;
        return;
    } else {
        s_combo_press_time_ms = 0;
        if (s_combo_triggered) {
            if (!raw_pwr && !raw_func) {
                s_combo_triggered = false;
            }
            return;
        }
    }

    // 3. POWER 按键逻辑 (GPIO19)
    //    短按：待机/开机切换；长按 (>= 5s)：触发 Matter 重新配网模式
    if (raw_pwr) {
        if (s_btn_power.press_time_ms == 0) {
            s_btn_power.press_time_ms = now_ms;
            s_btn_power.long_pressed = false;
        } else if (!s_btn_power.long_pressed && (now_ms - s_btn_power.press_time_ms >= 5000)) {
            s_btn_power.long_pressed = true;
            ESP_LOGI(TAG, "Power button 5s hold -> Trigger Matter Pairing Mode");
            thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_PAIRING);
        }
    } else {
        if (s_btn_power.press_time_ms > 0) {
            int64_t duration = now_ms - s_btn_power.press_time_ms;
            if (!s_btn_power.long_pressed && duration >= 50 && duration < 5000) {
                if (s_thermostat->mode == THERMOSTAT_MODE_STANDBY) {
                    ESP_LOGI(TAG, "Power short press -> Wake up from STANDBY");
                    thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_ON);
                } else {
                    ESP_LOGI(TAG, "Power short press -> Enter STANDBY");
                    thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_STANDBY);
                }
            }
            s_btn_power.press_time_ms = 0;
            s_btn_power.long_pressed = false;
        }
    }

    // 4. FUNC 按键逻辑 (GPIO18)
    //    待机下短按唤醒设备
    if (raw_func) {
        if (s_btn_func.press_time_ms == 0) {
            s_btn_func.press_time_ms = now_ms;
            s_btn_func.long_pressed = false;
        }
    } else {
        if (s_btn_func.press_time_ms > 0) {
            int64_t duration = now_ms - s_btn_func.press_time_ms;
            if (!s_btn_func.long_pressed && duration >= 50) {
                if (s_thermostat->mode == THERMOSTAT_MODE_STANDBY) {
                    ESP_LOGI(TAG, "FUNC short press -> Wake up from STANDBY");
                    thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_ON);
                }
            }
            s_btn_func.press_time_ms = 0;
            s_btn_func.long_pressed = false;
        }
    }
}
