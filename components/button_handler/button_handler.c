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
    bool last_state;        // 上一次引脚电平 (0=按下, 1=释放)
    int64_t press_time_ms;  // 按下起始时刻
    bool long_pressed;      // 是否已触发长按
} button_state_t;

static button_state_t s_btn_power;
static button_state_t s_btn_down;
static button_state_t s_btn_up;

static int64_t s_combo_press_time_ms = 0;
static bool s_combo_triggered = false;

static void init_single_btn(button_state_t *btn, gpio_num_t pin) {
    btn->pin = pin;
    btn->last_state = true;
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
    init_single_btn(&s_btn_down, cfg->pin_temp_down);
    init_single_btn(&s_btn_up, cfg->pin_temp_up);

    return ESP_OK;
}

void button_handler_poll(void) {
    if (!s_thermostat) return;

    int64_t now_ms = esp_timer_get_time() / 1000;

    bool raw_pwr = (gpio_get_level(s_btn_power.pin) == 0);
    bool raw_down = (gpio_get_level(s_btn_down.pin) == 0);
    bool raw_up = (gpio_get_level(s_btn_up.pin) == 0);

    // 1. 组合按键检测 (Temp Up GPIO21 + Temp Down GPIO20 同时长按 3 秒)
    if (raw_down && raw_up) {
        if (s_combo_press_time_ms == 0) {
            s_combo_press_time_ms = now_ms;
            s_combo_triggered = false;
        } else if (!s_combo_triggered && (now_ms - s_combo_press_time_ms >= 3000)) {
            s_combo_triggered = true;
            ESP_LOGI(TAG, "Combo key (GPIO20 + GPIO21) 3s hold -> Matter Pairing Mode");
            thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_PAIRING);
        }
        return; // 组合键生效期间屏蔽单键
    } else {
        s_combo_press_time_ms = 0;
        s_combo_triggered = false;
    }

    // 待机模式检测：任意单键按下/长按立即唤醒恢复开机模式
    if (s_thermostat->mode == THERMOSTAT_MODE_STANDBY) {
        if (raw_pwr || raw_down || raw_up) {
            ESP_LOGI(TAG, "Any key pressed -> Wake up from STANDBY");
            thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_ON);
            vTaskDelay(pdMS_TO_TICKS(200)); // 唤醒去抖
            return;
        }
    }

    // 2. 电源键逻辑 (GPIO19)
    if (raw_pwr) {
        if (s_btn_power.press_time_ms == 0) s_btn_power.press_time_ms = now_ms;
    } else {
        if (s_btn_power.press_time_ms > 0) {
            int64_t duration = now_ms - s_btn_power.press_time_ms;
            if (duration >= 50 && duration < 3000) {
                // 短按/长按：在“待机模式”与“开机模式”之间切换
                if (s_thermostat->mode == THERMOSTAT_MODE_ON) {
                    thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_STANDBY);
                } else {
                    thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_ON);
                }
            }
            s_btn_power.press_time_ms = 0;
        }
    }

    // 3. 温度加键逻辑 (GPIO21)
    if (raw_up) {
        if (s_btn_up.press_time_ms == 0) s_btn_up.press_time_ms = now_ms;
    } else {
        if (s_btn_up.press_time_ms > 0) {
            int64_t duration = now_ms - s_btn_up.press_time_ms;
            if (duration >= 50) {
                // 增加 1 摄氏度 (上限 21℃)
                thermostat_set_target_temperature(s_thermostat, s_thermostat->target_temp + 1);
            }
            s_btn_up.press_time_ms = 0;
        }
    }

    // 4. 温度减键逻辑 (GPIO20)
    if (raw_down) {
        if (s_btn_down.press_time_ms == 0) {
            s_btn_down.press_time_ms = now_ms;
            s_btn_down.long_pressed = false;
        } else if (!s_btn_down.long_pressed && (now_ms - s_btn_down.press_time_ms >= 1000)) {
            // 长按：设备进入待机模式
            s_btn_down.long_pressed = true;
            ESP_LOGI(TAG, "Temp Down long press -> Enter STANDBY");
            thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_STANDBY);
        }
    } else {
        if (s_btn_down.press_time_ms > 0) {
            int64_t duration = now_ms - s_btn_down.press_time_ms;
            if (!s_btn_down.long_pressed && duration >= 50) {
                // 短按：减少 1 摄氏度 (下限 17℃)
                thermostat_set_target_temperature(s_thermostat, s_thermostat->target_temp - 1);
            }
            s_btn_down.press_time_ms = 0;
            s_btn_down.long_pressed = false;
        }
    }
}
