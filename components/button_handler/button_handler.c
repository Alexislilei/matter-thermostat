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
    int64_t press_time_ms;     // 按下起始时刻
    bool pairing_triggered;     // 是否已触发 >5s 配网模式
    bool reset_triggered;       // 是否已触发 >15s 恢复出厂设置
} button_state_t;

static button_state_t s_btn_power;
static button_state_t s_btn_func;

static uint8_t s_last_ra_level = 1;
static int64_t s_last_encoder_time_ms = 0;

static void init_single_btn(button_state_t *btn, gpio_num_t pin) {
    btn->pin = pin;
    btn->press_time_ms = 0;
    btn->pairing_triggered = false;
    btn->reset_triggered = false;

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

    return ESP_OK;
}

void button_handler_poll(void) {
    if (!s_thermostat) return;

    int64_t now_ms = esp_timer_get_time() / 1000;

    // 非主页面 60 秒无操作超时：自动保存当前设置并返回主页面
    if (s_thermostat->mode == THERMOSTAT_MODE_ON &&
        s_thermostat->current_page != UI_PAGE_MAIN &&
        s_thermostat->last_input_time_ms > 0) {
        if (now_ms - s_thermostat->last_input_time_ms >= 60000) {
            ESP_LOGI(TAG, "60s timeout on non-main page -> Back to MAIN page");
            s_thermostat->current_page = UI_PAGE_MAIN;
            s_thermostat->last_input_time_ms = 0;
        }
    }

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
        // 记录操作时间，用于非主页面 60s 超时返回
        s_thermostat->last_input_time_ms = now_ms;

        if (s_thermostat->mode == THERMOSTAT_MODE_STANDBY) {
            ESP_LOGI(TAG, "Encoder rotated -> Wake up from STANDBY");
            thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_ON);
        } else if (s_thermostat->mode == THERMOSTAT_MODE_ON) {
            if (s_thermostat->current_page == UI_PAGE_SLEEP_TIMER) {
                // Sleep Timer 设置页面：循环选择睡眠时间 (OFF -> 10 -> 30 -> 60 MIN)
                static const int sleep_options[] = {0, 10, 30, 60};
                const int num_options = sizeof(sleep_options) / sizeof(sleep_options[0]);

                int idx = 0;
                for (int i = 0; i < num_options; i++) {
                    if (s_thermostat->sleep_timer_setting == sleep_options[i]) {
                        idx = i;
                        break;
                    }
                }
                // 顺时针 +1，逆时针 -1，循环切换
                idx = (idx + encoder_direction + num_options) % num_options;
                s_thermostat->sleep_timer_setting = sleep_options[idx];
                ESP_LOGI(TAG, "Sleep Timer option selected: %d MIN", s_thermostat->sleep_timer_setting);
            } else {
                // 主页面：调整目标温度
                if (encoder_direction > 0) {
                    ESP_LOGI(TAG, "Encoder CW -> Target temp +1");
                    thermostat_set_target_temperature(s_thermostat, s_thermostat->target_temp + 1.0f);
                } else {
                    ESP_LOGI(TAG, "Encoder CCW -> Target temp -1");
                    thermostat_set_target_temperature(s_thermostat, s_thermostat->target_temp - 1.0f);
                }
            }
        }
    }

    // 2. POWER 按键逻辑 (GPIO19)
    //    按下 < 1s：待机/开机切换；按住 > 5s：进入配对模式；按住 > 15s：恢复出厂设置
    if (raw_pwr) {
        if (s_btn_power.press_time_ms == 0) {
            s_btn_power.press_time_ms = now_ms;
            s_btn_power.pairing_triggered = false;
            s_btn_power.reset_triggered = false;
        } else {
            int64_t hold_time = now_ms - s_btn_power.press_time_ms;
            if (!s_btn_power.reset_triggered && hold_time >= 15000) {
                s_btn_power.reset_triggered = true;
                ESP_LOGI(TAG, "Power button 15s hold -> Trigger Factory Reset");
                thermostat_factory_reset(s_thermostat);
            } else if (!s_btn_power.pairing_triggered && !s_btn_power.reset_triggered && hold_time >= 5000) {
                s_btn_power.pairing_triggered = true;
                ESP_LOGI(TAG, "Power button 5s hold -> Trigger Matter Pairing Mode");
                thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_PAIRING);
            }
        }
    } else {
        if (s_btn_power.press_time_ms > 0) {
            int64_t duration = now_ms - s_btn_power.press_time_ms;
            if (!s_btn_power.pairing_triggered && !s_btn_power.reset_triggered && duration >= 50 && duration < 1000) {
                if (s_thermostat->mode == THERMOSTAT_MODE_STANDBY) {
                    ESP_LOGI(TAG, "Power short press (<1s) -> Wake up from STANDBY");
                    thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_ON);
                } else {
                    ESP_LOGI(TAG, "Power short press (<1s) -> Enter STANDBY");
                    thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_STANDBY);
                }
            }
            s_btn_power.press_time_ms = 0;
            s_btn_power.pairing_triggered = false;
            s_btn_power.reset_triggered = false;
        }
    }

    // 3. FUNC 按键逻辑 (GPIO18)
    //    待机下短按唤醒设备；开机下短按循环切换页面 (主页面 <-> Sleep Timer 设置页)
    if (raw_func) {
        if (s_btn_func.press_time_ms == 0) {
            s_btn_func.press_time_ms = now_ms;
        }
    } else {
        if (s_btn_func.press_time_ms > 0) {
            int64_t duration = now_ms - s_btn_func.press_time_ms;
            if (duration >= 50) {
                if (s_thermostat->mode == THERMOSTAT_MODE_STANDBY) {
                    ESP_LOGI(TAG, "FUNC short press -> Wake up from STANDBY");
                    thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_ON);
                } else if (s_thermostat->mode == THERMOSTAT_MODE_ON) {
                    // 记录操作时间，用于非主页面 60s 超时返回
                    s_thermostat->last_input_time_ms = now_ms;

                    if (s_thermostat->current_page == UI_PAGE_MAIN) {
                        // 主页面 -> Sleep Timer 设置页
                        s_thermostat->current_page = UI_PAGE_SLEEP_TIMER;
                        ESP_LOGI(TAG, "FUNC press -> Enter SLEEP TIMER SETTING page");
                    } else {
                        // Sleep Timer 设置页 -> 主页面，确认选择并开始倒计时
                        s_thermostat->current_page = UI_PAGE_MAIN;
                        if (s_thermostat->sleep_timer_setting > 0) {
                            s_thermostat->sleep_timer_active = true;
                            s_thermostat->sleep_timer_start_ms = now_ms;
                            ESP_LOGI(TAG, "Sleep Timer confirmed: %d MIN countdown started",
                                     s_thermostat->sleep_timer_setting);
                        } else {
                            // OFF：取消定时
                            s_thermostat->sleep_timer_active = false;
                            s_thermostat->sleep_timer_start_ms = 0;
                            ESP_LOGI(TAG, "Sleep Timer set to OFF (disabled)");
                        }
                        ESP_LOGI(TAG, "FUNC press -> Back to MAIN page");
                    }
                }
            }
            s_btn_func.press_time_ms = 0;
        }
    }
}
