#ifndef APP_MATTER_H
#define APP_MATTER_H

#include "esp_err.h"
#include "thermostat_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 Matter 协议栈与 Endpoint Cluster 节点
 */
esp_err_t app_matter_init(thermostat_dev_t *dev);

/**
 * @brief 将实测温度与加热状态更新同步至 Matter Attributes
 */
void app_matter_update(const thermostat_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif // APP_MATTER_H
