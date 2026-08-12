#include "thermostat_logic.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "THERMOSTAT_LOGIC";

esp_err_t thermostat_init(thermostat_dev_t *dev, gpio_num_t heater_gpio) {
    if (!dev) return ESP_ERR_INVALID_ARG;
    dev->heater_gpio = heater_gpio;
    dev->target_temp = 20.0f;   // 默认设定 20.0 摄氏度
    dev->current_temp = 20.0f;
    dev->mode = THERMOSTAT_MODE_STANDBY;
    dev->is_heating = false;
    dev->pending_led_effect = LED_EFFECT_NONE;
    dev->pairing_start_time_ms = 0;

    // UI 页面管理初始化
    dev->current_page = UI_PAGE_MAIN;
    dev->last_input_time_ms = 0;

    // Sleep Timer 初始化
    dev->sleep_timer_setting = 0;
    dev->sleep_timer_active = false;
    dev->sleep_timer_start_ms = 0;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << heater_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    gpio_set_level(dev->heater_gpio, 0);
    return ret;
}

void thermostat_set_mode(thermostat_dev_t *dev, thermostat_mode_t mode) {
    if (!dev) return;
    dev->mode = mode;

    // 记录配网开始时间，用于 15 分钟超时检测
    if (mode == THERMOSTAT_MODE_PAIRING) {
        dev->pairing_start_time_ms = esp_timer_get_time() / 1000;
        ESP_LOGI(TAG, "Entered PAIRING mode, 15-minute timeout started.");
    } else {
        dev->pairing_start_time_ms = 0;
    }

    // 若进入待机模式，强行关闭加热器
    if (mode == THERMOSTAT_MODE_STANDBY) {
        dev->is_heating = false;
        gpio_set_level(dev->heater_gpio, 0);
        ESP_LOGI(TAG, "Entered STANDBY mode, heater disabled.");
    } else {
        ESP_LOGI(TAG, "Switched to mode: %d", mode);
    }
}

void thermostat_set_target_temperature(thermostat_dev_t *dev, float target) {
    if (!dev) return;
    if (target < 15.0f) target = 15.0f;
    if (target > 25.0f) target = 25.0f;
    dev->target_temp = target;
    ESP_LOGI(TAG, "Target temperature set to: %.2f C", dev->target_temp);
}

void thermostat_update_temperature(thermostat_dev_t *dev, float new_temp) {
    if (!dev) return;
    dev->current_temp = new_temp;

    // 待机模式或配网模式下不启动加热器
    if (dev->mode != THERMOSTAT_MODE_ON) {
        if (dev->is_heating) {
            dev->is_heating = false;
            gpio_set_level(dev->heater_gpio, 0);
        }
        return;
    }

    // 迟滞温控逻辑 (Hysteresis Control ±0.5℃)
    float high_threshold = dev->target_temp + 0.5f;
    float low_threshold  = dev->target_temp - 0.5f;

    if (dev->is_heating) {
        // 升温过程中：当实测温度上升至 设定温度 + 0.5 ℃ 时，关闭加热
        if (dev->current_temp >= high_threshold) {
            dev->is_heating = false;
            gpio_set_level(dev->heater_gpio, 0);
            ESP_LOGI(TAG, "Temp reached %.2f >= %.2f C, Turning OFF Heater", dev->current_temp, high_threshold);
        }
    } else {
        // 降温过程中：当实测温度下降至 设定温度 - 0.5 ℃ 时，开启加热
        if (dev->current_temp <= low_threshold) {
            dev->is_heating = true;
            gpio_set_level(dev->heater_gpio, 1);
            ESP_LOGI(TAG, "Temp dropped to %.2f <= %.2f C, Turning ON Heater", dev->current_temp, low_threshold);
        }
    }
}

void thermostat_factory_reset(thermostat_dev_t *dev) {
    if (!dev) return;

    ESP_LOGI(TAG, "=== FACTORY RESET TRIGGERED ===");

    // 关闭加热器
    dev->is_heating = false;
    gpio_set_level(dev->heater_gpio, 0);

    // 恢复默认状态
    dev->target_temp = 20.0f;
    dev->current_temp = 20.0f;
    dev->mode = THERMOSTAT_MODE_STANDBY;
    dev->pairing_start_time_ms = 0;

    // 清空 UI 页面状态
    dev->current_page = UI_PAGE_MAIN;
    dev->last_input_time_ms = 0;

    // 清空 Sleep Timer 状态
    dev->sleep_timer_setting = 0;
    dev->sleep_timer_active = false;
    dev->sleep_timer_start_ms = 0;

    // 请求播放恢复出厂灯效
    dev->pending_led_effect = LED_EFFECT_FACTORY_RESET;

    ESP_LOGI(TAG, "Factory reset complete. LED effect queued, reboot pending.");
}

void thermostat_sleep_timer_tick(thermostat_dev_t *dev) {
    if (!dev || !dev->sleep_timer_active || dev->sleep_timer_start_ms == 0) return;

    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t elapsed_ms = now_ms - dev->sleep_timer_start_ms;
    int64_t target_ms  = (int64_t)dev->sleep_timer_setting * 60LL * 1000LL;

    if (elapsed_ms >= target_ms) {
        ESP_LOGI(TAG, "Sleep Timer expired (%d min) -> Entering STANDBY", dev->sleep_timer_setting);
        dev->sleep_timer_active   = false;
        dev->sleep_timer_start_ms = 0;
        thermostat_set_mode(dev, THERMOSTAT_MODE_STANDBY);
    }
}
