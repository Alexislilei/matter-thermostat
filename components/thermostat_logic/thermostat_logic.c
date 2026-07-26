#include "thermostat_logic.h"
#include "esp_log.h"

static const char *TAG = "THERMOSTAT_LOGIC";

esp_err_t thermostat_init(thermostat_dev_t *dev, gpio_num_t heater_gpio) {
    if (!dev) return ESP_ERR_INVALID_ARG;
    dev->heater_gpio = heater_gpio;
    dev->target_temp = 19;      // 默认设定 19 摄氏度
    dev->current_temp = 20.0f;
    dev->mode = THERMOSTAT_MODE_STANDBY;
    dev->is_heating = false;

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
    // 若进入待机模式，强行关闭加热器
    if (mode == THERMOSTAT_MODE_STANDBY) {
        dev->is_heating = false;
        gpio_set_level(dev->heater_gpio, 0);
        ESP_LOGI(TAG, "Entered STANDBY mode, heater disabled.");
    } else {
        ESP_LOGI(TAG, "Switched to mode: %d", mode);
    }
}

void thermostat_set_target_temperature(thermostat_dev_t *dev, int target) {
    if (!dev) return;
    if (target < 17) target = 17;
    if (target > 21) target = 21;
    dev->target_temp = target;
    ESP_LOGI(TAG, "Target temperature set to: %d C", dev->target_temp);
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
    float high_threshold = (float)dev->target_temp + 0.5f;
    float low_threshold = (float)dev->target_temp - 0.5f;

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
