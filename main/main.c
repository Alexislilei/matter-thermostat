#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "dht11.h"
#include "thermostat_logic.h"
#include "led_control.h"
#include "button_handler.h"
#include "app_matter.h"

static const char *TAG = "MAIN";

// 硬件引脚定义
#define GPIO_POWER_BTN      GPIO_NUM_19
#define GPIO_TEMP_DOWN_BTN  GPIO_NUM_20
#define GPIO_TEMP_UP_BTN    GPIO_NUM_21
#define GPIO_HEATER_RELAY   GPIO_NUM_22
#define GPIO_DHT11_DATA     GPIO_NUM_4
#define GPIO_RGB_LED_STRIP  GPIO_NUM_8

static thermostat_dev_t s_thermostat;
static dht11_config_t s_dht11;

// 1. 温度采集与控制任务 (2 秒周期)
static void temp_control_task(void *pvParameters) {
    float temp_val = 0.0f;

    while (1) {
        esp_err_t err = dht11_read_filtered(&s_dht11, &temp_val);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Read Filtered Temperature: %.1f C", temp_val);
            thermostat_update_temperature(&s_thermostat, temp_val);
            app_matter_update(&s_thermostat);
        } else {
            ESP_LOGW(TAG, "Failed to read temperature from DHT11: %d", err);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// 2. LED 阵列灯效刷新任务 (50ms 刷新率)
static void led_ui_task(void *pvParameters) {
    while (1) {
        led_control_update(&s_thermostat);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// 3. 按键扫描与状态轮询任务 (20ms 周期)
static void button_poll_task(void *pvParameters) {
    while (1) {
        button_handler_poll();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== Starting ESP32-C6 Matter Thermostat ===");

    // 1. NVS 初始化
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化核心逻辑与加热器 GPIO
    ESP_ERROR_CHECK(thermostat_init(&s_thermostat, GPIO_HEATER_RELAY));

    // 3. 初始化 DHT11 传感器
    ESP_ERROR_CHECK(dht11_init(&s_dht11, GPIO_DHT11_DATA));

    // 4. 初始化 RGB LED 阵列并运行 POST 自检
    ESP_ERROR_CHECK(led_control_init(GPIO_RGB_LED_STRIP));
    led_control_post();

    // 5. 初始化按键驱动
    button_config_t btn_cfg = {
        .pin_power = GPIO_POWER_BTN,
        .pin_temp_down = GPIO_TEMP_DOWN_BTN,
        .pin_temp_up = GPIO_TEMP_UP_BTN,
    };
    ESP_ERROR_CHECK(button_handler_init(&btn_cfg, &s_thermostat));

    // 6. 初始化 Matter 协议栈
    ESP_ERROR_CHECK(app_matter_init(&s_thermostat));

    // 默认为开机模式
    thermostat_set_mode(&s_thermostat, THERMOSTAT_MODE_ON);

    // 7. 创建后台并发 Task
    xTaskCreate(temp_control_task, "temp_control_task", 3072, NULL, 5, NULL);
    xTaskCreate(led_ui_task, "led_ui_task", 3072, NULL, 4, NULL);
    xTaskCreate(button_poll_task, "button_poll_task", 2048, NULL, 6, NULL);

    ESP_LOGI(TAG, "Initialization complete. Thermostat tasks running.");
}
