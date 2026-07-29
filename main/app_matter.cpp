#include "app_matter.h"
#include "esp_log.h"
#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <platform/CHIPDeviceLayer.h>

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *TAG = "APP_MATTER";
static uint16_t s_thermostat_endpoint_id = 0;

// Matter SystemMode 值定义
// 0x00 = Off (待机), 0x04 = Heat (加热运行)
#define THERMOSTAT_SYSTEM_MODE_OFF  0x00
#define THERMOSTAT_SYSTEM_MODE_HEAT 0x04

// 标记当前模式变化是否来自 Matter 回调，用于 button_poll_task
// 避免将本地变化回写 Matter 时产生冗余写入
bool g_mode_change_from_matter = false;

// Matter 属性变更回调函数
static esp_err_t app_matter_attribute_update_cb(attribute::callback_type_t type,
                                                 uint16_t endpoint_id,
                                                 uint32_t cluster_id,
                                                 uint32_t attribute_id,
                                                 esp_matter_attr_val_t *val,
                                                 void *priv_data) {
    // 仅处理 POST_UPDATE (属性已写入后) 阶段，避免重复响应
    if (type != attribute::POST_UPDATE) {
        return ESP_OK;
    }

    thermostat_dev_t *dev = (thermostat_dev_t *)priv_data;
    if (!dev || !val) return ESP_OK;

    if (cluster_id == Thermostat::Id) {
        // ---- 1. HomeKit 电源开关: SystemMode 属性 ----
        // Off (0x00) -> THERMOSTAT_MODE_STANDBY
        // Heat(0x04) -> THERMOSTAT_MODE_ON
        if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
            uint8_t system_mode = val->val.u8;
            ESP_LOGI(TAG, "Matter SystemMode changed to: 0x%02X (type=%d)", system_mode, val->type);

            // 标记变化来自 Matter，防止 button_poll_task 冗余回写
            g_mode_change_from_matter = true;

            if (system_mode == THERMOSTAT_SYSTEM_MODE_OFF) {
                thermostat_set_mode(dev, THERMOSTAT_MODE_STANDBY);
                ESP_LOGI(TAG, "Thermostat set to STANDBY (Heater OFF)");
            } else if (system_mode == THERMOSTAT_SYSTEM_MODE_HEAT) {
                thermostat_set_mode(dev, THERMOSTAT_MODE_ON);
                ESP_LOGI(TAG, "Thermostat set to ON (Heater control active)");
            } else {
                ESP_LOGW(TAG, "Unsupported SystemMode: 0x%02X, ignoring.", system_mode);
            }
        }

        // ---- 2. HomeKit 温度滑块: OccupiedHeatingSetpoint 属性 ----
        // Matter 单位: 0.01 °C，需转换为 float 摄氏度后传给温控逻辑
        if (attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
            if (val->type == ESP_MATTER_VAL_TYPE_INT16 || val->type == ESP_MATTER_VAL_TYPE_NULLABLE_INT16) {
                int16_t target_hundredths = val->val.i16;
                float target_temp = (float)target_hundredths / 100.0f;
                ESP_LOGI(TAG, "Matter Setpoint updated: %.2f C (raw: %d)", target_temp, target_hundredths);
                thermostat_set_target_temperature(dev, target_temp);
            } else {
                ESP_LOGW(TAG, "OccupiedHeatingSetpoint write with unexpected type: %d", val->type);
            }
        }
    }

    return ESP_OK;
}

extern "C" esp_err_t app_matter_init(thermostat_dev_t *dev) {
    ESP_LOGI(TAG, "Initializing Matter Stack for ESP32-C6 Thermostat...");

    // 1. 创建 Matter Node 节点
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_matter_attribute_update_cb, nullptr, dev);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return ESP_FAIL;
    }

    // 2. 创建标准 Thermostat Endpoint
    thermostat::config_t thermostat_config;
    // 初始实测温度 20.0°C (单位: 0.01°C)
    thermostat_config.thermostat.local_temperature = 2000;
    // Heating Only (控制序列 0x02 = Heating Only)
    thermostat_config.thermostat.control_sequence_of_operation = 0x02;
    // 初始 SystemMode: Heat (0x04)，与 main.c 中 thermostat_set_mode(ON) 保持一致
    thermostat_config.thermostat.system_mode = THERMOSTAT_SYSTEM_MODE_HEAT;
    // 默认设定温度 20.0°C
    thermostat_config.thermostat.features.heating.occupied_heating_setpoint = 2000;
    // 启用 Heating Feature
    thermostat_config.thermostat.feature_flags = cluster::thermostat::feature::heating::get_id();

    endpoint_t *endpoint = thermostat::create(node, &thermostat_config, ENDPOINT_FLAG_NONE, dev);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create Thermostat endpoint");
        return ESP_FAIL;
    }

    s_thermostat_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Thermostat Endpoint created successfully with ID: %d", s_thermostat_endpoint_id);

    // 3. 获取 Thermostat Cluster 引用，用于添加额外属性
    cluster_t *thermostat_cluster = cluster::get(endpoint, Thermostat::Id);
    if (!thermostat_cluster) {
        ESP_LOGE(TAG, "Failed to get Thermostat cluster");
        return ESP_FAIL;
    }

    // ---- 3a. 添加 HomeKit 要求的可选属性 ----
    // ThermostatRunningMode (0x001E): 只读 enum8，指示当前运行模式 (Heat/Off)
    // HomeKit 需要此属性才能显示温度控制界面
    attribute_t *attr_running_mode = cluster::thermostat::attribute::create_thermostat_running_mode(
        thermostat_cluster, THERMOSTAT_SYSTEM_MODE_HEAT);
    if (!attr_running_mode) {
        ESP_LOGW(TAG, "Failed to create ThermostatRunningMode attribute");
    }

    // PIHeatingDemand (0x0021): 只读 uint8 (0-100%)，指示当前加热需求百分比
    // HomeKit 使用此属性显示加热状态
    attribute_t *attr_pi_heat = cluster::thermostat::attribute::create_pi_heating_demand(
        thermostat_cluster, 0);
    if (!attr_pi_heat) {
        ESP_LOGW(TAG, "Failed to create PIHeatingDemand attribute");
    }

    // ThermostatRunningState (0x0022): 只读 bitmap16，指示 Heat/Fan/Cool 运行状态
    attribute_t *attr_running_state = cluster::thermostat::attribute::create_thermostat_running_state(
        thermostat_cluster, 0x0001); // bit0=Heat ON
    if (!attr_running_state) {
        ESP_LOGW(TAG, "Failed to create ThermostatRunningState attribute");
    }

    // ---- 3b. 创建温度范围限制属性（必须先创建，再设置值） ----
    // 这些属性在 SDK 的 thermostat::create() 中不会自动创建，
    // 必须手动创建后才能通过 attribute::set_val() 设置值
    attribute_t *attr_abs_min = cluster::thermostat::attribute::create_abs_min_heat_setpoint_limit(
        thermostat_cluster, 1500);
    if (!attr_abs_min) {
        ESP_LOGW(TAG, "Failed to create AbsMinHeatSetpointLimit attribute");
    }

    attribute_t *attr_abs_max = cluster::thermostat::attribute::create_abs_max_heat_setpoint_limit(
        thermostat_cluster, 2500);
    if (!attr_abs_max) {
        ESP_LOGW(TAG, "Failed to create AbsMaxHeatSetpointLimit attribute");
    }

    attribute_t *attr_min = cluster::thermostat::attribute::create_min_heat_setpoint_limit(
        thermostat_cluster, 1500);
    if (!attr_min) {
        ESP_LOGW(TAG, "Failed to create MinHeatSetpointLimit attribute");
    }

    attribute_t *attr_max = cluster::thermostat::attribute::create_max_heat_setpoint_limit(
        thermostat_cluster, 2500);
    if (!attr_max) {
        ESP_LOGW(TAG, "Failed to create MaxHeatSetpointLimit attribute");
    }

    ESP_LOGI(TAG, "Thermostat attributes created: RunningMode, PIHeatingDemand, RunningState, SetpointLimits");

    // 4. 启动 Matter 协议栈 (开启 BLE 广播与配网)
    esp_err_t err = esp_matter::start(nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter stack: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "Matter Stack started successfully. BLE commissioning active.");

    // 打印配对码，方便用手机扫码配网
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  Matter 配网信息 (Commissioning Codes)");
    ESP_LOGI(TAG, "============================================");
    PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    ESP_LOGI(TAG, "============================================");

    return ESP_OK;
}

extern "C" void app_matter_update(const thermostat_dev_t *dev) {
    if (!dev || s_thermostat_endpoint_id == 0) return;

    // 将 float 温度转换为 Matter 规范的 0.01°C 整数格式 (如 20.5°C -> 2050)
    int16_t local_temp_hundredths  = (int16_t)(dev->current_temp * 100.0f);

    // 同步当前实测温度 (LocalTemperature 为设备上报属性，attribute::update 合法)
    esp_matter_attr_val_t val_local = esp_matter_nullable_int16(local_temp_hundredths);
    attribute::update(s_thermostat_endpoint_id, Thermostat::Id,
                      Thermostat::Attributes::LocalTemperature::Id, &val_local);

    // 注意: OccupiedHeatingSetpoint 不由设备周期性推送。
    // 该属性应由控制器 (HomeKit) 写入，设备仅当用户本地按键改变目标温度时，
    // 通过 app_matter_set_target_temperature() 单次同步。
    // 如果设备每 2 秒主动更新 OccupiedHeatingSetpoint，HomeKit 会认为设备
    // 控制着设定值，从而不显示温度调节滑块。

    // 同步 ThermostatRunningMode: 根据当前模式设置
    // 0x00 = Off, 0x01 = Cool, 0x02 = Cool (不再使用), 0x03 = Heat
    uint8_t running_mode = (dev->mode == THERMOSTAT_MODE_ON) ? 0x03 : 0x00; // Heat or Off
    esp_matter_attr_val_t val_running_mode = esp_matter_enum8(running_mode);
    attribute::update(s_thermostat_endpoint_id, Thermostat::Id,
                      Thermostat::Attributes::ThermostatRunningMode::Id, &val_running_mode);

    // 同步 PIHeatingDemand: 根据加热状态计算百分比
    // 如果加热开启且温度低于目标，按温差比例计算需求
    uint8_t pi_demand = 0;
    if (dev->mode == THERMOSTAT_MODE_ON && dev->is_heating) {
        float temp_diff = dev->target_temp - dev->current_temp;
        if (temp_diff > 0.0f) {
            // 温差每 5°C 对应 100% 需求，上限 100%
            pi_demand = (uint8_t)(temp_diff * 20.0f);
            if (pi_demand > 100) pi_demand = 100;
        }
        // 即使温差很小，只要加热器在工作，至少给 10%
        if (pi_demand < 10) pi_demand = 10;
    }
    esp_matter_attr_val_t val_pi_demand = esp_matter_uint8(pi_demand);
    attribute::update(s_thermostat_endpoint_id, Thermostat::Id,
                      Thermostat::Attributes::PIHeatingDemand::Id, &val_pi_demand);

    // 同步 ThermostatRunningState: bitmap16
    // bit0=Heat, bit1=Cool, bit2= Fan, bit3=Heat Stage 2, ...
    uint16_t running_state = 0;
    if (dev->mode == THERMOSTAT_MODE_ON && dev->is_heating) {
        running_state |= 0x0001; // Heat ON
    }
    esp_matter_attr_val_t val_running_state = esp_matter_bitmap16(running_state);
    attribute::update(s_thermostat_endpoint_id, Thermostat::Id,
                      Thermostat::Attributes::ThermostatRunningState::Id, &val_running_state);

    // 注意: SystemMode 是由控制器 (HomeKit) 写入的属性，不在此处周期性推送。
    // 模式切换时通过 app_matter_set_mode() 单次同步。
}

// 由设备侧主动改变目标温度时（如按键加减温）调用，将 OccupiedHeatingSetpoint 同步至 Matter。
// 使用 attribute::set_val 直接写入属性存储，不经过 Matter 写入验证路径。
// OccupiedHeatingSetpoint 在 Matter 规范中为 INT16 类型，使用 esp_matter_int16() 创建值。
extern "C" void app_matter_set_target_temperature(float target_temp_celsius) {
    if (s_thermostat_endpoint_id == 0) return;

    int16_t target_temp_hundredths = (int16_t)(target_temp_celsius * 100.0f);

    endpoint_t *endpoint = endpoint::get(node::get(), s_thermostat_endpoint_id);
    if (!endpoint) return;

    cluster_t *thermostat_cluster = cluster::get(endpoint, Thermostat::Id);
    if (!thermostat_cluster) return;

    attribute_t *attr_setpoint = attribute::get(thermostat_cluster,
                                                Thermostat::Attributes::OccupiedHeatingSetpoint::Id);
    if (!attr_setpoint) {
        ESP_LOGW(TAG, "OccupiedHeatingSetpoint attribute not found");
        return;
    }

    esp_matter_attr_val_t val = esp_matter_int16(target_temp_hundredths);
    esp_err_t err = attribute::set_val(attr_setpoint, &val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set OccupiedHeatingSetpoint to %d, err: %d", target_temp_hundredths, err);
    } else {
        ESP_LOGI(TAG, "OccupiedHeatingSetpoint synced to Matter: %.2f C (raw: %d)", target_temp_celsius, target_temp_hundredths);
    }
}

// 由设备侧主动切换模式时（如按键）调用，将 SystemMode 同步至 Matter 属性层。
// 使用 attribute::set_val 直接写入属性存储，不经过 Matter 写入验证路径。
// SystemMode 在 Matter 规范中为 ENUM8 类型，必须使用 esp_matter_enum8() 创建值，
// 否则 attribute::set_val 的类型检查 (get_val_type(attr) == val->type) 会失败，
// 返回 ESP_ERR_INVALID_ARG (err 258)。
extern "C" void app_matter_set_mode(thermostat_mode_t mode) {
    if (s_thermostat_endpoint_id == 0) return;

    uint8_t system_mode = (mode == THERMOSTAT_MODE_ON)
                          ? THERMOSTAT_SYSTEM_MODE_HEAT
                          : THERMOSTAT_SYSTEM_MODE_OFF;

    endpoint_t *endpoint = endpoint::get(node::get(), s_thermostat_endpoint_id);
    if (!endpoint) return;

    cluster_t *thermostat_cluster = cluster::get(endpoint, Thermostat::Id);
    if (!thermostat_cluster) return;

    attribute_t *attr_mode = attribute::get(thermostat_cluster,
                                            Thermostat::Attributes::SystemMode::Id);
    if (!attr_mode) {
        ESP_LOGW(TAG, "SystemMode attribute not found");
        return;
    }

    // SystemMode 是 ENUM8 类型，必须使用 esp_matter_enum8() 而非 esp_matter_uint8()
    esp_matter_attr_val_t val = esp_matter_enum8(system_mode);
    esp_err_t err = attribute::set_val(attr_mode, &val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set SystemMode to 0x%02X, err: %d", system_mode, err);
    } else {
        ESP_LOGI(TAG, "SystemMode synced to Matter: 0x%02X (%s)",
                 system_mode,
                 (mode == THERMOSTAT_MODE_ON) ? "Heat" : "Off");
    }
}
