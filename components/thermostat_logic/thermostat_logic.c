#include "thermostat_logic.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "THERMOSTAT_LOGIC";

// ---- Sleep Timer 设定值 NVS 持久化 ----
// 需求：Sleep Timer 时长设置改动后记忆，下次上电读取记忆的设置。
// 首次开机默认 30 分钟。
#define SLEEP_TIMER_NVS_NAMESPACE "sleep_timer"
#define SLEEP_TIMER_NVS_KEY       "setting"   // 设定值 (i32, 分钟)
#define SLEEP_TIMER_DEFAULT_MIN   30          // 首次开机默认 30 分钟

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
    // 首次开机默认 30 分钟 (无 OFF 选项)。若 NVS 中已有记忆值，
    // 由 thermostat_sleep_timer_load() 在启动时覆盖。
    dev->sleep_timer_setting = SLEEP_TIMER_DEFAULT_MIN;
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

    // 清空 Sleep Timer 状态 (恢复默认 30 分钟)
    dev->sleep_timer_setting = SLEEP_TIMER_DEFAULT_MIN;
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

// 从 NVS 读取记忆的 Sleep Timer 设定值并应用到 dev->sleep_timer_setting
// 若 NVS 中无有效记录（首次开机），保持默认值 (30 分钟)。
esp_err_t thermostat_sleep_timer_load(thermostat_dev_t *dev) {
    if (!dev) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SLEEP_TIMER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // 命名空间不存在（首次开机）：保持默认值
        ESP_LOGI(TAG, "Sleep Timer NVS namespace not found, using default %d min",
                 SLEEP_TIMER_DEFAULT_MIN);
        return ESP_OK;
    }

    int32_t setting = 0;
    err = nvs_get_i32(handle, SLEEP_TIMER_NVS_KEY, &setting);
    nvs_close(handle);

    if (err != ESP_OK) {
        // 键不存在（首次开机）：保持默认值
        ESP_LOGI(TAG, "Sleep Timer setting not found in NVS, using default %d min",
                 SLEEP_TIMER_DEFAULT_MIN);
        return ESP_OK;
    }

    // 校验设定值合法性：仅接受 {10, 30, 60, 90}，否则回退默认
    if (setting != 10 && setting != 30 && setting != 60 && setting != 90) {
        ESP_LOGW(TAG, "Invalid saved Sleep Timer setting %d, using default %d min",
                 (int)setting, SLEEP_TIMER_DEFAULT_MIN);
        return ESP_OK;
    }

    dev->sleep_timer_setting = (int)setting;
    ESP_LOGI(TAG, "Sleep Timer setting loaded from NVS: %d min", dev->sleep_timer_setting);
    return ESP_OK;
}

// 将 dev->sleep_timer_setting 保存到 NVS，实现"改动后记忆，下次上电读取"
esp_err_t thermostat_sleep_timer_save(const thermostat_dev_t *dev) {
    if (!dev) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SLEEP_TIMER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %d", SLEEP_TIMER_NVS_NAMESPACE, err);
        return err;
    }

    err = nvs_set_i32(handle, SLEEP_TIMER_NVS_KEY, (int32_t)dev->sleep_timer_setting);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Sleep Timer setting saved to NVS: %d min", dev->sleep_timer_setting);
    } else {
        ESP_LOGE(TAG, "Failed to save Sleep Timer setting to NVS: %d", err);
    }
    return err;
}
