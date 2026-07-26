#ifndef DHT11_H
#define DHT11_H

#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DHT11_FILTER_SIZE 5

typedef struct {
    gpio_num_t gpio_num;
    float temperature_history[DHT11_FILTER_SIZE];
    uint8_t history_index;
    uint8_t history_count;
} dht11_config_t;

/**
 * @brief 初始化 DHT11 传感器及滤波缓冲区
 * 
 * @param config DHT11 配置结构体指针
 * @param gpio_num 硬件 GPIO 引脚
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t dht11_init(dht11_config_t *config, gpio_num_t gpio_num);

/**
 * @brief 读取一次 DHT11 温度原始数据
 * 
 * @param config DHT11 配置结构体指针
 * @param raw_temp 输出原始摄氏温度值
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t dht11_read_raw(dht11_config_t *config, float *raw_temp);

/**
 * @brief 读取经过滑动中值滤波后的温度数据
 * 
 * @param config DHT11 配置结构体指针
 * @param filtered_temp 输出滤波后的摄氏温度值
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t dht11_read_filtered(dht11_config_t *config, float *filtered_temp);

#ifdef __cplusplus
}
#endif

#endif // DHT11_H
