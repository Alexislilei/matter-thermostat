#include "app_matter.h"
#include "esp_log.h"

static const char *TAG = "APP_MATTER";

esp_err_t app_matter_init(thermostat_dev_t *dev) {
    ESP_LOGI(TAG, "Initializing Matter Stack for ESP32-C6 Thermostat...");
    // 此处可接入 Matter SDK 初始化流程及 Endpoint 创建 (Thermostat Cluster)
    return ESP_OK;
}

void app_matter_update(const thermostat_dev_t *dev) {
    if (!dev) return;
    // 定期将本地实测温度与 Target Temp 属性点同步至 Matter Cluster Attributes
}
