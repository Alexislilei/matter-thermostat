#include "button_handler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/pulse_cnt.h"

static const char *TAG = "BUTTON_HANDLER";

static button_config_t s_cfg;
static thermostat_dev_t *s_thermostat = NULL;

typedef struct {
    gpio_num_t pin;
    int64_t press_time_ms;     // 按下起始时刻
    bool pairing_triggered;     // 是否已触发 >5s 配网模式
    bool reset_triggered;       // 是否已触发 >15s 恢复出厂设置
} button_state_t;

static button_state_t s_btn_power;
static button_state_t s_btn_func;
static button_state_t s_btn_sleep;

// ---- EC11 旋转编码器 PCNT 硬件正交解码 ----
// 使用 ESP32-C6 的 PCNT (Pulse Counter) 外设在硬件层面进行标准双通道正交解码，
// 不依赖 CPU 轮询，因此快速旋转时也不会漏掉任何卡点 (解决轮询丢步问题)。
// 双通道正交模式要求 (RA, RB) 按合法格雷码序列变化，机械抖动导致的非法跳变
// 会被硬件自动忽略，从根本上避免单通道方案因抖动产生的方向误判 (解决方向翻转问题)。
//   通道0: 边沿=RA, 电平=RB
//   通道1: 边沿=RB, 电平=RA
// 半步编码器 (每卡点 1 次 RA 跳变) 下，每卡点计数 ±1，方向与旋转方向一致。
static pcnt_unit_handle_t s_pcnt_unit = NULL;
static pcnt_channel_handle_t s_pcnt_chan0 = NULL;
static pcnt_channel_handle_t s_pcnt_chan1 = NULL;

// 半步编码器 (每卡点 = 2 次正交边沿 = 2 个 PCNT 计数) 的余数累加器。
// 用于将 PCNT 计数折算为卡点数 (每 2 计数 = 1 卡点)，并保留跨轮询的余数，
// 避免快速旋转时因整数除法截断而漏掉卡点。
static int s_enc_remainder = 0;

static void init_single_btn(button_state_t *btn, gpio_num_t pin) {
    btn->pin = pin;
    btn->press_time_ms = 0;
    btn->pairing_triggered = false;
    btn->reset_triggered = false;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

esp_err_t button_handler_init(button_config_t *cfg, thermostat_dev_t *thermostat) {
    if (!cfg || !thermostat) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    s_thermostat = thermostat;

    init_single_btn(&s_btn_power, cfg->pin_power);
    init_single_btn(&s_btn_func, cfg->pin_func);
    init_single_btn(&s_btn_sleep, cfg->pin_sleep);

    // 配置 EC11 编码器 GPIO (KEY_RA, KEY_RB) 为输入 + 内部上拉
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << cfg->pin_key_ra) | (1ULL << cfg->pin_key_rb),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // 初始化 PCNT 单元 (硬件脉冲计数，避免轮询丢步)
    pcnt_unit_config_t unit_config = {
        .high_limit = 100,   // 计数上限，防止溢出
        .low_limit  = -100,  // 计数下限
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &s_pcnt_unit));

    // 标准双通道正交解码:
    //   通道0: 边沿 = KEY_RA, 电平 = KEY_RB
    //   通道1: 边沿 = KEY_RB, 电平 = KEY_RA
    // 方向约定: 顺时针 (CW) 计数为正, 逆时针 (CCW) 计数为负。
    // 若实际接线方向相反，只需交换下面两个通道的 edge/level 引脚即可。
    pcnt_chan_config_t chan0_config = {
        .edge_gpio_num = cfg->pin_key_ra,
        .level_gpio_num = cfg->pin_key_rb,
    };
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt_unit, &chan0_config, &s_pcnt_chan0));

    pcnt_chan_config_t chan1_config = {
        .edge_gpio_num = cfg->pin_key_rb,
        .level_gpio_num = cfg->pin_key_ra,
    };
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt_unit, &chan1_config, &s_pcnt_chan1));

    // 通道0: RA 上升沿 -1, 下降沿 +1; 电平 RB=0 保持, RB=1 反转
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(s_pcnt_chan0,
                                                 PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                                 PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(s_pcnt_chan0,
                                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                  PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    // 通道1: RB 上升沿 +1, 下降沿 -1; 电平 RA=0 保持, RA=1 反转
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(s_pcnt_chan1,
                                                 PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                 PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(s_pcnt_chan1,
                                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                  PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt_unit));

    return ESP_OK;
}

void button_handler_poll(void) {
    if (!s_thermostat) return;

    int64_t now_ms = esp_timer_get_time() / 1000;

    // 非主页面 60 秒无操作超时：自动保存当前设置并返回主页面
    if (s_thermostat->mode == THERMOSTAT_MODE_ON &&
        s_thermostat->current_page != UI_PAGE_MAIN &&
        s_thermostat->last_input_time_ms > 0) {
        if (now_ms - s_thermostat->last_input_time_ms >= 60000) {
            ESP_LOGI(TAG, "60s timeout on non-main page -> Back to MAIN page");
            s_thermostat->current_page = UI_PAGE_MAIN;
            s_thermostat->last_input_time_ms = 0;
        }
    }

    bool raw_pwr  = (gpio_get_level(s_cfg.pin_power) == 0);
    bool raw_func = (gpio_get_level(s_cfg.pin_func) == 0);
    bool raw_sleep = (gpio_get_level(s_cfg.pin_sleep) == 0);

    // static bool last_raw_pwr = false;
    // static bool last_raw_func = false;
    // static bool last_raw_sleep = false;
    // static int64_t last_print_time = 0;
    // if (raw_pwr != last_raw_pwr || raw_func != last_raw_func || raw_sleep != last_raw_sleep || (now_ms - last_print_time >= 2000)) {
    //     ESP_LOGI(TAG, "[BUTTON DEBUG] Raw states: PWR(GPIO%d)=%d, FUNC(GPIO%d)=%d, SLEEP(GPIO%d)=%d (1=Pressed, 0=Released)", 
    //              s_cfg.pin_power, raw_pwr, s_cfg.pin_func, raw_func, s_cfg.pin_sleep, raw_sleep);
    //     last_raw_pwr = raw_pwr;
    //     last_raw_func = raw_func;
    //     last_raw_sleep = raw_sleep;
    //     last_print_time = now_ms;
    // }

    // 1. EC11 旋转编码器 PCNT 硬件正交解码
    //    读取 PCNT 计数值 (硬件层面已按方向累加，每边沿 ±1)，
    //    读取后立即清零，避免重复计数。快速旋转时一次轮询可能累计多个边沿。
    //    半步编码器每卡点 = 2 次正交边沿 = 2 个计数，故每 2 计数折算为 1 卡点。
    //    方向约定: 顺时针 (CW) 为正, 逆时针 (CCW) 为负。
    //    硬件配置中 CW 计数为负，此处取反以符合"CW=加温, CCW=降温"的约定。
    int encoder_direction = 0; // >0: CW 加温, <0: CCW 降温, 0: 无旋转 (绝对值 = 卡点数)
    int pcnt_count = 0;
    if (s_pcnt_unit != NULL) {
        ESP_ERROR_CHECK(pcnt_unit_get_count(s_pcnt_unit, &pcnt_count));
        if (pcnt_count != 0) {
            ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt_unit));
            // 取反使 CW 为正，并累加余数后折算为卡点数 (每 2 计数 = 1 卡点)
            s_enc_remainder += -pcnt_count;
            encoder_direction = s_enc_remainder / 2;
            s_enc_remainder %= 2;
        }
    }

    // 旋转编码器触发响应
    if (encoder_direction != 0) {
        // 记录操作时间，用于非主页面 60s 超时返回
        s_thermostat->last_input_time_ms = now_ms;

        if (s_thermostat->mode == THERMOSTAT_MODE_STANDBY) {
            ESP_LOGI(TAG, "Encoder rotated -> Wake up from STANDBY");
            thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_ON);
        } else if (s_thermostat->mode == THERMOSTAT_MODE_ON) {
            if (s_thermostat->current_page == UI_PAGE_SLEEP_TIMER) {
                // Sleep Timer 设置页面：选择睡眠时长 (10 -> 30 -> 60 -> 90 MIN)
                // 需求：顺时针旋转一格，列表向上移动一位 (选中项向后移动)，
                //      到最后一个 (90 MIN) 就停住，不做周期循环；逆时针反之。
                static const int sleep_options[] = {10, 30, 60, 90};
                const int num_options = sizeof(sleep_options) / sizeof(sleep_options[0]);

                int idx = 0;
                for (int i = 0; i < num_options; i++) {
                    if (s_thermostat->sleep_timer_setting == sleep_options[i]) {
                        idx = i;
                        break;
                    }
                }
                // 顺时针 +1，逆时针 -1，并在两端钳制 (不循环)
                int new_idx = idx + encoder_direction;
                if (new_idx < 0)      new_idx = 0;
                if (new_idx > num_options - 1) new_idx = num_options - 1;

                if (new_idx != idx) {
                    s_thermostat->sleep_timer_setting = sleep_options[new_idx];
                    ESP_LOGI(TAG, "Sleep Timer option selected: %d MIN",
                             s_thermostat->sleep_timer_setting);
                    // 改动后记忆：保存到 NVS，下次上电读取
                    thermostat_sleep_timer_save(s_thermostat);
                }
            } else {
                // 主页面：调整目标温度 (encoder_direction 绝对值 = 卡点数)
                // 每卡点改变 0.5℃ (需求变更: 由 1.0℃ 改为 0.5℃)
                float delta = (float)encoder_direction * 0.5f; // 正=加温, 负=降温
                ESP_LOGI(TAG, "Encoder rotate %d -> Target temp %+.1f",
                         encoder_direction, delta);
                thermostat_set_target_temperature(s_thermostat,
                                                  s_thermostat->target_temp + delta);
            }
        }
    }

    // 2. POWER 按键逻辑 (GPIO19)
    //    按下 < 1s：待机/开机切换；按住 > 5s：进入配对模式；按住 > 15s：恢复出厂设置
    if (raw_pwr) {
        if (s_btn_power.press_time_ms == 0) {
            s_btn_power.press_time_ms = now_ms;
            s_btn_power.pairing_triggered = false;
            s_btn_power.reset_triggered = false;
        } else {
            int64_t hold_time = now_ms - s_btn_power.press_time_ms;
            if (!s_btn_power.reset_triggered && hold_time >= 15000) {
                s_btn_power.reset_triggered = true;
                ESP_LOGI(TAG, "Power button 15s hold -> Trigger Factory Reset");
                thermostat_factory_reset(s_thermostat);
            } else if (!s_btn_power.pairing_triggered && !s_btn_power.reset_triggered && hold_time >= 5000) {
                s_btn_power.pairing_triggered = true;
                ESP_LOGI(TAG, "Power button 5s hold -> Trigger Matter Pairing Mode");
                thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_PAIRING);
            }
        }
    } else {
        if (s_btn_power.press_time_ms > 0) {
            int64_t duration = now_ms - s_btn_power.press_time_ms;
            if (!s_btn_power.pairing_triggered && !s_btn_power.reset_triggered && duration >= 50 && duration < 1000) {
                if (s_thermostat->mode == THERMOSTAT_MODE_STANDBY) {
                    ESP_LOGI(TAG, "Power short press (<1s) -> Wake up from STANDBY");
                    thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_ON);
                } else {
                    ESP_LOGI(TAG, "Power short press (<1s) -> Enter STANDBY");
                    thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_STANDBY);
                }
            }
            s_btn_power.press_time_ms = 0;
            s_btn_power.pairing_triggered = false;
            s_btn_power.reset_triggered = false;
        }
    }

    // 3. FUNC 按键逻辑 (GPIO15)
    //    待机下短按唤醒设备；开机下短按循环切换页面 (主页面 <-> Sleep Timer 设置页)
    if (raw_func) {
        if (s_btn_func.press_time_ms == 0) {
            s_btn_func.press_time_ms = now_ms;
        }
    } else {
        if (s_btn_func.press_time_ms > 0) {
            int64_t duration = now_ms - s_btn_func.press_time_ms;
            if (duration >= 50) {
                if (s_thermostat->mode == THERMOSTAT_MODE_STANDBY) {
                    ESP_LOGI(TAG, "FUNC short press -> Wake up from STANDBY");
                    thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_ON);
                } else if (s_thermostat->mode == THERMOSTAT_MODE_ON) {
                    // 记录操作时间，用于非主页面 60s 超时返回
                    s_thermostat->last_input_time_ms = now_ms;

                    if (s_thermostat->current_page == UI_PAGE_MAIN) {
                        // 主页面 -> Sleep Timer 设置页
                        s_thermostat->current_page = UI_PAGE_SLEEP_TIMER;
                        ESP_LOGI(TAG, "FUNC press -> Enter SLEEP TIMER SETTING page");
                    } else {
                        // Sleep Timer 设置页 -> 主页面
                        // 注意：FUNC 按键仅负责页面切换，不在此处启动倒计时。
                        // 触发 sleeper 启动的只有屏上右下角的触摸按键。
                        s_thermostat->current_page = UI_PAGE_MAIN;
                        ESP_LOGI(TAG, "FUNC press -> Back to MAIN page (countdown not started)");
                    }
                }
            }
            s_btn_func.press_time_ms = 0;
        }
    }

    // 4. SLEEP 按键逻辑 (GPIO18)
    //    待机下短按唤醒设备；开机下短按启动/关闭 Sleep Timer (等同于点击屏上右下角的触摸按键)
    if (raw_sleep) {
        if (s_btn_sleep.press_time_ms == 0) {
            s_btn_sleep.press_time_ms = now_ms;
        }
    } else {
        if (s_btn_sleep.press_time_ms > 0) {
            int64_t duration = now_ms - s_btn_sleep.press_time_ms;
            if (duration >= 50) {
                if (s_thermostat->mode == THERMOSTAT_MODE_STANDBY) {
                    ESP_LOGI(TAG, "SLEEP short press -> Wake up from STANDBY");
                    thermostat_set_mode(s_thermostat, THERMOSTAT_MODE_ON);
                } else if (s_thermostat->mode == THERMOSTAT_MODE_ON) {
                    // 记录操作时间，用于非主页面 60s 超时返回
                    s_thermostat->last_input_time_ms = now_ms;

                    if (s_thermostat->sleep_timer_active) {
                        s_thermostat->sleep_timer_active = false;
                        s_thermostat->sleep_timer_start_ms = 0;
                        ESP_LOGI(TAG, "Physical Sleep Timer button -> OFF");
                    } else {
                        s_thermostat->sleep_timer_active = true;
                        s_thermostat->sleep_timer_start_ms = now_ms;
                        ESP_LOGI(TAG, "Physical Sleep Timer button -> %d min countdown started", s_thermostat->sleep_timer_setting);
                    }
                }
            }
            s_btn_sleep.press_time_ms = 0;
        }
    }
}
