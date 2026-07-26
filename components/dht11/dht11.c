#include <string.h>
#include <stdlib.h>
#include "dht11.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

static const char *TAG = "DHT11";

// 辅助比较函数，用于中值滤波排序
static int compare_floats(const void *a, const void *b) {
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

esp_err_t dht11_init(dht11_config_t *config, gpio_num_t gpio_num) {
    if (!config) return ESP_ERR_INVALID_ARG;
    config->gpio_num = gpio_num;
    config->history_index = 0;
    config->history_count = 0;
    memset(config->temperature_history, 0, sizeof(config->temperature_history));

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io_conf);
}

esp_err_t dht11_read_raw(dht11_config_t *config, float *raw_temp) {
    if (!config || !raw_temp) return ESP_ERR_INVALID_ARG;

    uint8_t data[5] = {0};

    // 发送拉低起始信号（至少18ms）
    gpio_set_direction(config->gpio_num, GPIO_MODE_OUTPUT);
    gpio_set_level(config->gpio_num, 0);
    esp_rom_delay_us(20000);

    // 拉高 20-40us
    gpio_set_level(config->gpio_num, 1);
    esp_rom_delay_us(30);

    // 切换为输入模式等待 DHT11 响应
    gpio_set_direction(config->gpio_num, GPIO_MODE_INPUT);

    // 等待响应低电平
    uint32_t timeout = 0;
    while (gpio_get_level(config->gpio_num) == 1) {
        if (++timeout > 100) return ESP_ERR_TIMEOUT;
        esp_rom_delay_us(1);
    }

    // 等待响应高电平结束
    timeout = 0;
    while (gpio_get_level(config->gpio_num) == 0) {
        if (++timeout > 100) return ESP_ERR_TIMEOUT;
        esp_rom_delay_us(1);
    }
    timeout = 0;
    while (gpio_get_level(config->gpio_num) == 1) {
        if (++timeout > 100) return ESP_ERR_TIMEOUT;
        esp_rom_delay_us(1);
    }

    // 读取 40 bit 数据
    for (int i = 0; i < 40; i++) {
        // 等待低电平结束
        timeout = 0;
        while (gpio_get_level(config->gpio_num) == 0) {
            if (++timeout > 100) return ESP_ERR_TIMEOUT;
            esp_rom_delay_us(1);
        }

        // 高电平持续时间判断 bit 0 (26-28us) 或 bit 1 (70us)
        esp_rom_delay_us(35);
        if (gpio_get_level(config->gpio_num) == 1) {
            data[i / 8] |= (1 << (7 - (i % 8)));
            // 等待高电平结束
            timeout = 0;
            while (gpio_get_level(config->gpio_num) == 1) {
                if (++timeout > 100) return ESP_ERR_TIMEOUT;
                esp_rom_delay_us(1);
            }
        }
    }

    // 校验和验证
    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        ESP_LOGE(TAG, "DHT11 Checksum Error");
        return ESP_ERR_INVALID_CRC;
    }

    *raw_temp = (float)data[2] + ((float)data[3] * 0.1f);
    return ESP_OK;
}

esp_err_t dht11_read_filtered(dht11_config_t *config, float *filtered_temp) {
    if (!config || !filtered_temp) return ESP_ERR_INVALID_ARG;

    float raw_val = 0.0f;
    esp_err_t ret = dht11_read_raw(config, &raw_val);
    if (ret != ESP_OK) {
        // 若读取失败且已有历史记录，返回上一次滤波值
        if (config->history_count > 0) {
            float temp_buf[DHT11_FILTER_SIZE];
            memcpy(temp_buf, config->temperature_history, sizeof(float) * config->history_count);
            qsort(temp_buf, config->history_count, sizeof(float), compare_floats);
            *filtered_temp = temp_buf[config->history_count / 2];
            return ESP_OK;
        }
        return ret;
    }

    // 将采样数据存入环形缓冲区
    config->temperature_history[config->history_index] = raw_val;
    config->history_index = (config->history_index + 1) % DHT11_FILTER_SIZE;
    if (config->history_count < DHT11_FILTER_SIZE) {
        config->history_count++;
    }

    // 滑动中值滤波计算
    float temp_buf[DHT11_FILTER_SIZE];
    memcpy(temp_buf, config->temperature_history, sizeof(float) * config->history_count);
    qsort(temp_buf, config->history_count, sizeof(float), compare_floats);
    *filtered_temp = temp_buf[config->history_count / 2];

    return ESP_OK;
}
