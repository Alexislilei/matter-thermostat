#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "dht11.h"
#include "thermostat_logic.h"
#include "led_control.h"
#include "button_handler.h"
#include "app_matter.h"
#include "lcd_display.h"

static const char *TAG = "MAIN";

// 硬件引脚定义
#define GPIO_POWER_BTN      GPIO_NUM_19
#define GPIO_FUNC_BTN       GPIO_NUM_18
#define GPIO_KEY_RA         GPIO_NUM_21
#define GPIO_KEY_RB         GPIO_NUM_20
#define GPIO_HEATER_RELAY   GPIO_NUM_22
#define GPIO_DHT11_DATA     GPIO_NUM_4
#define GPIO_RGB_LED_STRIP  GPIO_NUM_8

// ---- LCD / 触摸屏引脚 (硬件均无外部上拉电阻，需启用内部上拉) ----
#define GPIO_LCD_BACKLIGHT  GPIO_NUM_0   // LCD 背光 (LCD LED)
#define GPIO_LCD_RESET      GPIO_NUM_1   // LCD 复位 (RESET)，需上拉至高电平才能退出复位
#define GPIO_LCD_CS         GPIO_NUM_2   // LCD 片选 (CS)，高电平为未选中
#define GPIO_LCD_DC         GPIO_NUM_3   // LCD 命令/数据 (DC)，高电平数据/低电平命令
#define GPIO_LCD_MISO       GPIO_NUM_10  // SPI MISO / T_DO (触摸屏数据返回)
#define GPIO_LCD_MOSI       GPIO_NUM_11  // SPI MOSI / T_DIN (LCD 与触摸屏共用)
#define GPIO_LCD_SCK        GPIO_NUM_12  // SPI SCK (LCD 与触摸屏共用)
#define GPIO_TOUCH_CS       GPIO_NUM_13  // 触摸屏片选 (XPT2046)
#define GPIO_TOUCH_IRQ      GPIO_NUM_23  // 触摸屏中断 (Touch IRQ)

static thermostat_dev_t s_thermostat;
static dht11_config_t s_dht11;

// 1. 温度采集与控制任务 (2 秒周期)
//    同时负责配网超时检测和瞬态灯效完成后的后续处理
static void temp_control_task(void *pvParameters) {
    float temp_val = 0.0f;

    while (1) {
        // ---- 温度采集与温控 ----
        esp_err_t err = dht11_read_filtered(&s_dht11, &temp_val);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Read Filtered Temperature: %.1f C", temp_val);
            thermostat_update_temperature(&s_thermostat, temp_val);
            app_matter_update(&s_thermostat);
        } else {
            ESP_LOGW(TAG, "Failed to read temperature from DHT11: %d", err);
        }

        // ---- 配网超时检测 ----
        app_matter_check_commissioning_timeout(&s_thermostat);

        // ---- Sleep Timer 倒计时检测 ----
        // 倒计时结束后自动切换至待机模式并关闭加热
        thermostat_sleep_timer_tick(&s_thermostat);

        // ---- 瞬态 LED 灯效完成后的清理 ----
        // 当 led_control 播放完瞬态灯效后，清零 pending_led_effect 标记
        if (s_thermostat.pending_led_effect != LED_EFFECT_NONE &&
            led_control_effect_finished(&s_thermostat)) {

            led_effect_type_t finished_effect = s_thermostat.pending_led_effect;
            s_thermostat.pending_led_effect = LED_EFFECT_NONE;

            ESP_LOGI(TAG, "LED effect finished: %d", (int)finished_effect);

            // 恢复出厂灯效播放完毕后，执行 Matter 出厂重置并重启
            if (finished_effect == LED_EFFECT_FACTORY_RESET) {
                ESP_LOGI(TAG, "Factory reset LED effect complete. Clearing Matter credentials...");
                app_matter_factory_reset();
                // 短暂延迟确保日志输出
                vTaskDelay(pdMS_TO_TICKS(500));
                ESP_LOGI(TAG, "Rebooting device...");
                esp_restart();
            }
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

// 2.1 LCD 显示刷新任务 (100ms 周期)
// 根据温控器状态渲染主页面/待机页，并处理 LVGL 定时器
static void lcd_ui_task(void *pvParameters) {
    while (1) {
        lcd_display_update(&s_thermostat);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// 3. 按键扫描与状态轮询任务 (20ms 周期)
// 检测模式变化和目标温度变化，变化时同步至 Matter
static void button_poll_task(void *pvParameters) {
    thermostat_mode_t last_mode = s_thermostat.mode;
    float last_target_temp = s_thermostat.target_temp;

    while (1) {
        button_handler_poll();

        // 检测模式变化：按键改变模式时将 SystemMode 同步至 Matter
        if (s_thermostat.mode != last_mode) {
            last_mode = s_thermostat.mode;

            // 如果模式变化来自 Matter 回调 (HomeKit 写入 SystemMode)，
            // 则跳过回写，避免冗余写入和 err 258
            if (g_mode_change_from_matter) {
                g_mode_change_from_matter = false;
                ESP_LOGD(TAG, "Mode change from Matter, skip write-back");
            } else {
                app_matter_set_mode(last_mode);
            }
        }

        // 检测目标温度变化：按键加减温时将 OccupiedHeatingSetpoint 同步至 Matter
        // 注意：HomeKit 写入的温度变化在回调中已处理，此处仅同步本地按键变化
        if (s_thermostat.target_temp != last_target_temp) {
            last_target_temp = s_thermostat.target_temp;
            app_matter_set_target_temperature(last_target_temp);
        }

        // 延迟 1 Tick (10ms) 释放 CPU 资源，防止 IDLE 任务被饿死触发 Task Watchdog (WDT)
        vTaskDelay(pdMS_TO_TICKS(10));
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

    // 2. 初始化 LCD / 触摸屏 GPIO
    //    硬件上所有 TFT-LCD 与触摸屏引脚均未连接外部上拉电阻，
    //    需启用内部上拉，并将控制引脚 (RESET/CS/DC) 置为正确的空闲电平，
    //    否则 RESET 悬空为低会导致 LCD 一直处于复位状态而白屏。
    //
    //    a) 控制引脚 (RESET/CS/DC/背光)：配置为输出，内部上拉，置为正确电平
    gpio_config_t lcd_ctrl_conf = {
        .pin_bit_mask = (1ULL << GPIO_LCD_BACKLIGHT) |
                        (1ULL << GPIO_LCD_RESET) |
                        (1ULL << GPIO_LCD_CS) |
                        (1ULL << GPIO_LCD_DC),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   // 内部上拉，防止引脚悬空
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&lcd_ctrl_conf));

    // 背光置高点亮；RESET 置高退出复位；CS 置高未选中；DC 置高数据模式
    gpio_set_level(GPIO_LCD_BACKLIGHT, 1);
    gpio_set_level(GPIO_LCD_RESET, 1);
    gpio_set_level(GPIO_LCD_CS, 1);
    gpio_set_level(GPIO_LCD_DC, 1);
    ESP_LOGI(TAG, "LCD control pins configured: BL=HIGH, RESET=HIGH, CS=HIGH, DC=HIGH");

    //    b) SPI 数据引脚 (SCK/MOSI/MISO) 与触摸屏引脚 (Touch CS/IRQ)：
    //       配置为输入并启用内部上拉，防止悬空导致误触发
    gpio_config_t lcd_spi_conf = {
        .pin_bit_mask = (1ULL << GPIO_LCD_SCK) |
                        (1ULL << GPIO_LCD_MOSI) |
                        (1ULL << GPIO_LCD_MISO) |
                        (1ULL << GPIO_TOUCH_CS) |
                        (1ULL << GPIO_TOUCH_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   // 内部上拉，防止引脚悬空
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&lcd_spi_conf));
    ESP_LOGI(TAG, "LCD SPI & touch pins configured with internal pull-up");

    // 3. 初始化核心逻辑与加热器 GPIO
    ESP_ERROR_CHECK(thermostat_init(&s_thermostat, GPIO_HEATER_RELAY));

    // 4. 初始化 DHT11 传感器
    ESP_ERROR_CHECK(dht11_init(&s_dht11, GPIO_DHT11_DATA));

    // 5. 初始化 RGB LED 阵列并运行 POST 自检
    ESP_ERROR_CHECK(led_control_init(GPIO_RGB_LED_STRIP));
    led_control_post();

    // 6. 初始化按键及旋转编码器驱动
    button_config_t btn_cfg = {
        .pin_power = GPIO_POWER_BTN,
        .pin_func = GPIO_FUNC_BTN,
        .pin_key_ra = GPIO_KEY_RA,
        .pin_key_rb = GPIO_KEY_RB,
    };
    ESP_ERROR_CHECK(button_handler_init(&btn_cfg, &s_thermostat));

    // 7. 初始化 LCD 显示 (ILI9341 + LVGL)
    ESP_ERROR_CHECK(lcd_display_init(&s_thermostat));

    // 8. 初始化 Matter 协议栈
    ESP_ERROR_CHECK(app_matter_init(&s_thermostat));

    // 默认为开机模式
    thermostat_set_mode(&s_thermostat, THERMOSTAT_MODE_ON);

    // 9. 创建后台并发 Task
    xTaskCreate(temp_control_task, "temp_control_task", 4096, NULL, 5, NULL);
    xTaskCreate(led_ui_task,       "led_ui_task",       3072, NULL, 4, NULL);
    xTaskCreate(lcd_ui_task,       "lcd_ui_task",       4096, NULL, 4, NULL);
    xTaskCreate(button_poll_task,  "button_poll_task",  3072, NULL, 6, NULL);

    ESP_LOGI(TAG, "Initialization complete. Thermostat tasks running.");
}
