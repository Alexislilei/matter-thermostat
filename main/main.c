#include <stdio.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "dht11.h"
#include "thermostat_logic.h"
#include "led_control.h"
#include "button_handler.h"
#include "app_matter.h"
#include "lcd_display.h"
#include "touch_driver.h"
#include "crash_monitor.h"

static const char *TAG = "MAIN";

// ---- 时间同步配置 ----
// 目标时区：澳大利亚东部标准时间 (AEST/AEDT)，自动夏令时。
// 使用 IANA 时区名 "Australia/Sydney"，libc 会根据日期自动在
// AEST (UTC+10) 与 AEDT (UTC+11) 之间切换。
#define TIMEZONE_AUSTRALIA_SYDNEY "AEST-10AEDT,M10.1.0,M4.1.0/3"
// SNTP 服务器列表 (依次尝试)
// 前两个为域名（依赖 DNS 解析），后两个为直接 IP（可绕过 DNS 解析失败的问题，
// 适用于设备仅有链路本地 IPv6、无法解析公网域名，但能访问公网 IP 的场景）。
// 注意：CONFIG_LWIP_SNTP_MAX_SERVERS 需 >= 本数组长度 (当前为 4)。
static const char *SNTP_SERVERS[] = {
    "pool.ntp.org",
    "time.google.com",
    "216.239.35.0",   // time.google.com 的 NTP 服务器 IP
    "129.6.15.28",    // time.nist.gov 的 NTP 服务器 IP
};

// 初始化 SNTP 时间同步并设置时区
// 说明：ESP-IDF 的 newlib 支持 POSIX 时区字符串。为获得自动夏令时，
// 需同时满足：
//   1) 通过 setenv("TZ", ...) + tzset() 设置时区；
//   2) 通过 esp_sntp_setoperatingmode(SNTP_OPMODE_POLL) 使 SNTP 更新
//      系统时间时调用 settimeofday()，从而触发 tzset() 重新计算夏令时。
// 标记 SNTP 是否已初始化，避免重复初始化 (esp_sntp_init 重复调用会报错)
static bool s_sntp_started = false;

// 初始化 SNTP 时间同步并设置时区
// 说明：ESP-IDF 的 newlib 支持 POSIX 时区字符串。为获得自动夏令时，
// 需同时满足：
//   1) 通过 setenv("TZ", ...) + tzset() 设置时区；
//   2) 通过 esp_sntp_setoperatingmode(SNTP_OPMODE_POLL) 使 SNTP 更新
//      系统时间时调用 settimeofday()，从而触发 tzset() 重新计算夏令时。
// 该函数可安全地多次调用（内部有 s_sntp_started 保护），
// 以便在 Wi-Fi 获得 IP 后重新触发 SNTP 同步。
static void time_sync_init(void) {
    // 1. 设置时区为澳大利亚东部标准时间 (AEST/AEDT，自动夏令时)
    setenv("TZ", TIMEZONE_AUSTRALIA_SYDNEY, 1);
    tzset();

    // 2. 初始化 SNTP，从网络获取 UTC 时间
    //    仅在首次调用时执行初始化；后续调用仅重新触发同步，
    //    避免 esp_sntp_init() 重复调用导致错误。
    if (!s_sntp_started) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, SNTP_SERVERS[0]);
        esp_sntp_setservername(1, SNTP_SERVERS[1]);
        esp_sntp_setservername(2, SNTP_SERVERS[2]);
        esp_sntp_set_time_sync_notification_cb(NULL);
        esp_sntp_init();
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP time sync initialized (timezone: Australia/Sydney, AEST/AEDT auto DST)");
    } else {
        // 已初始化过：重新触发一次同步，确保在 Wi-Fi 就绪后能尽快获取时间
        esp_sntp_restart();
        ESP_LOGI(TAG, "SNTP time sync re-triggered after network ready");
    }
}

// Wi-Fi 事件处理：当设备获得 IP 地址 (STA_GOT_IP) 时触发 SNTP 时间同步。
// 根因：原代码在 app_main() 启动时（此时 Wi-Fi 尚未连接）就调用 time_sync_init()，
// 导致 SNTP 首次请求因无网络而失败；而 CONFIG_LWIP_SNTP_UPDATE_DELAY 默认 1 小时，
// 重试间隔过长，即使之后配网成功，时间也长时间停留在占位符。
// 通过监听 IP_EVENT_STA_GOT_IP，在 Wi-Fi 真正就绪后再触发 SNTP，即可正常同步时间。
static void wifi_got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Wi-Fi connected, got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    time_sync_init();
}

// 上次触发 SNTP 重新同步的时间戳 (ms)，用于节流，避免频繁重启 SNTP 干扰同步
static int64_t s_last_sntp_retry_ms = 0;

// 前向声明：NTP 故障诊断函数与自定义 NTP 同步函数（定义在下方），
// 均在 ensure_time_synced 中调用
static void run_ntp_diagnostics(void);
static void custom_ntp_sync(void);

// 周期性检查并确保 SNTP 时间同步完成。
// 作为 IP_EVENT_STA_GOT_IP 事件处理的兜底方案：某些情况下（如 Matter 内部
// 管理 Wi-Fi 连接、或事件未投递到默认事件循环），GOT_IP 事件可能不会触发
// 我们的处理器。此函数在 temp_control_task 中周期性调用，若检测到 SNTP
// 尚未同步且 Wi-Fi 已连接，则重新触发 SNTP 同步，确保时间最终能同步成功。
// 返回 true 表示时间已同步完成。
static bool ensure_time_synced(void) {
    // 已同步完成，无需处理
    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        return true;
    }

    // 检查 Wi-Fi STA 接口是否已连接并获得 IP
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_is_netif_up(sta)) {
        // 打印当前 SNTP 同步状态与 STA 接口 IP，便于诊断 NTP 失败原因
        esp_netif_ip_info_t ip_info;
        char ip_str[32] = "none";
        if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
        ESP_LOGI(TAG, "SNTP not synced (status=%d), STA IP=%s, re-triggering SNTP...",
                 (int)esp_sntp_get_sync_status(), ip_str);

        // Wi-Fi 已就绪但 SNTP 尚未同步：节流后重新触发同步
        // (至少间隔 15 秒，避免频繁重启 SNTP 干扰同步)
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - s_last_sntp_retry_ms >= 15000) {
            s_last_sntp_retry_ms = now_ms;
            time_sync_init();
        }

        // 周期性执行 NTP 故障诊断（内部节流 60 秒），定位 SNTP 无法同步的环节
        run_ntp_diagnostics();

        // 自定义 NTP 同步（绕过有问题的 lwIP SNTP 客户端）：
        // 诊断已证实原始 UDP NTP 请求可用，因此直接通过 socket 获取时间并
        // 调用 settimeofday() 设置系统时间，确保时间能最终同步成功。
        custom_ntp_sync();
    }
    return false;
}

// ---- NTP 故障诊断 ----
// 目的：当 SNTP 客户端始终无法同步（status 一直为 RESET）时，通过独立的
// 诊断手段定位失败环节。SNTP 客户端内部不打印具体错误，因此这里分别测试：
//   1) DNS 解析：能否将 "pool.ntp.org" / "time.google.com" 解析为 IP。
//   2) 原始 UDP NTP 请求：直接向 IP 服务器 (216.239.35.0) 发送 NTP 请求，
//      并等待响应，绕过 SNTP 客户端，验证 UDP 123 端口是否可达。
// 通过对比这两项结果，可判断失败原因是 DNS、UDP 端口被阻断，还是 SNTP 客户端本身。
// 该诊断仅在 SNTP 长时间未同步时周期性执行（节流 60 秒），避免刷屏。
static int64_t s_last_ntp_diag_ms = 0;

// 测试 DNS 解析是否正常
static void diag_test_dns(void) {
    const char *hosts[] = { "pool.ntp.org", "time.google.com" };
    for (int i = 0; i < 2; i++) {
        struct addrinfo hints;
        struct addrinfo *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        int rc = getaddrinfo(hosts[i], NULL, &hints, &res);
        if (rc == 0 && res) {
            char ipbuf[INET6_ADDRSTRLEN] = "?";
            if (res->ai_family == AF_INET) {
                struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
                inet_ntop(AF_INET, &sa->sin_addr, ipbuf, sizeof(ipbuf));
            } else if (res->ai_family == AF_INET6) {
                struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)res->ai_addr;
                inet_ntop(AF_INET6, &sa6->sin6_addr, ipbuf, sizeof(ipbuf));
            }
            ESP_LOGI(TAG, "[DIAG] DNS OK: %s -> %s", hosts[i], ipbuf);
            freeaddrinfo(res);
        } else {
            ESP_LOGE(TAG, "[DIAG] DNS FAIL: %s (rc=%d, errno=%d)", hosts[i], rc, errno);
        }
    }
}

// 直接向指定 IP 发送原始 NTP 请求并等待响应，验证 UDP 123 端口可达性
// 返回 true 表示收到有效 NTP 响应
static bool diag_raw_ntp_request(const char *ip_str) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "[DIAG] socket() failed: errno=%d", errno);
        return false;
    }

    // 构造 NTP 请求包 (48 字节)：首字节 0x23 = LI=0, VN=4, Mode=3 (client)
    uint8_t ntp_pkt[48];
    memset(ntp_pkt, 0, sizeof(ntp_pkt));
    ntp_pkt[0] = 0x23;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(123);
    dest.sin_addr.s_addr = inet_addr(ip_str);

    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int sent = sendto(sock, ntp_pkt, sizeof(ntp_pkt), 0,
                      (struct sockaddr *)&dest, sizeof(dest));
    if (sent < 0) {
        ESP_LOGE(TAG, "[DIAG] NTP sendto %s failed: errno=%d", ip_str, errno);
        close(sock);
        return false;
    }

    uint8_t rbuf[48];
    socklen_t addrlen = sizeof(dest);
    int recvd = recvfrom(sock, rbuf, sizeof(rbuf), 0,
                         (struct sockaddr *)&dest, &addrlen);
    close(sock);

    if (recvd < 0) {
        ESP_LOGE(TAG, "[DIAG] NTP recvfrom %s failed/timeout: errno=%d", ip_str, errno);
        return false;
    }
    ESP_LOGI(TAG, "[DIAG] NTP RESPONSE from %s: %d bytes (LI=%d, VN=%d, Mode=%d)",
             ip_str, recvd, (rbuf[0] >> 6) & 0x3, (rbuf[0] >> 3) & 0x7, rbuf[0] & 0x7);
    return true;
}

// 执行 NTP 故障诊断（节流 60 秒）
static void run_ntp_diagnostics(void) {
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_last_ntp_diag_ms < 60000) {
        return;
    }
    s_last_ntp_diag_ms = now_ms;

    ESP_LOGI(TAG, "[DIAG] === NTP 故障诊断开始 ===");
    diag_test_dns();
    diag_raw_ntp_request("216.239.35.0");   // time.google.com NTP IP
    diag_raw_ntp_request("129.6.15.28");    // time.nist.gov NTP IP
    ESP_LOGI(TAG, "[DIAG] === NTP 故障诊断结束 ===");
}

// ---- 自定义 NTP 时间同步（绕过 lwIP SNTP 客户端） ----
// 背景：诊断已证实 DNS 解析正常、原始 UDP NTP 请求能收到有效服务器响应
// (Mode=4)，但 lwIP 的 esp_sntp 客户端始终无法完成同步 (status 一直为 RESET)。
// 这说明问题出在 SNTP 客户端本身（很可能是启动时网络未就绪即调用
// esp_sntp_init()，之后 esp_sntp_restart() 无法正确恢复客户端状态）。
// 因此这里改用与诊断相同的、已验证可用的原始 UDP socket 方式直接获取
// NTP 时间并调用 settimeofday() 设置系统时间，彻底绕开有问题的 SNTP 客户端。
// 该函数可安全地多次调用，内部节流（至少间隔 10 秒）避免频繁请求。
static bool s_time_synced = false;   // 自定义时间同步是否已完成
static int64_t s_last_custom_sync_ms = 0;

// 向指定服务器发送 NTP 请求并解析响应中的发送时间戳，成功则设置系统时间
// 返回 true 表示成功获取并设置了系统时间
static bool custom_ntp_request(const char *ip_str) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "[NTP] socket() failed: errno=%d", errno);
        return false;
    }

    // 构造 NTP 请求包 (48 字节)：首字节 0x23 = LI=0, VN=4, Mode=3 (client)
    uint8_t ntp_pkt[48];
    memset(ntp_pkt, 0, sizeof(ntp_pkt));
    ntp_pkt[0] = 0x23;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(123);
    dest.sin_addr.s_addr = inet_addr(ip_str);

    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int sent = sendto(sock, ntp_pkt, sizeof(ntp_pkt), 0,
                      (struct sockaddr *)&dest, sizeof(dest));
    if (sent < 0) {
        ESP_LOGE(TAG, "[NTP] sendto %s failed: errno=%d", ip_str, errno);
        close(sock);
        return false;
    }

    uint8_t rbuf[48];
    socklen_t addrlen = sizeof(dest);
    int recvd = recvfrom(sock, rbuf, sizeof(rbuf), 0,
                         (struct sockaddr *)&dest, &addrlen);
    close(sock);

    if (recvd < 48) {
        ESP_LOGE(TAG, "[NTP] recvfrom %s failed/short: errno=%d, recvd=%d", ip_str, errno, recvd);
        return false;
    }

    // 校验响应模式：Mode 应为 4 (server)
    if ((rbuf[0] & 0x07) != 4) {
        ESP_LOGE(TAG, "[NTP] %s: unexpected mode %d", ip_str, rbuf[0] & 0x07);
        return false;
    }

    // 解析发送时间戳 (Transmit Timestamp)：位于偏移 40-47 字节，为 64 位 NTP 时间戳
    // 高 32 位为自 1900-01-01 起的秒数，低 32 位为小数部分
    uint32_t tx_sec = ((uint32_t)rbuf[40] << 24) | ((uint32_t)rbuf[41] << 16) |
                      ((uint32_t)rbuf[42] << 8) | (uint32_t)rbuf[43];

    // NTP 纪元 (1900-01-01) 与 Unix 纪元 (1970-01-01) 之间的秒数差
    const uint32_t NTP_TO_UNIX = 2208988800UL;

    // 防止异常值：若秒数小于 NTP_TO_UNIX 则明显无效
    if (tx_sec < NTP_TO_UNIX) {
        ESP_LOGE(TAG, "[NTP] %s: invalid transmit timestamp (sec=%" PRIu32 ")", ip_str, tx_sec);
        return false;
    }

    time_t unix_time = (time_t)(tx_sec - NTP_TO_UNIX);

    // 设置系统时间 (UTC)
    struct timeval tv_now;
    tv_now.tv_sec = unix_time;
    tv_now.tv_usec = 0;
    if (settimeofday(&tv_now, NULL) != 0) {
        ESP_LOGE(TAG, "[NTP] settimeofday failed: errno=%d", errno);
        return false;
    }

    s_time_synced = true;
    ESP_LOGI(TAG, "[NTP] Time synced via %s: %lld (UTC)", ip_str, (long long)unix_time);
    return true;
}

// 自定义 NTP 同步入口：依次尝试多个服务器，成功即返回
// 内部节流（至少间隔 10 秒），避免频繁请求
static void custom_ntp_sync(void) {
    if (s_time_synced) {
        return;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_last_custom_sync_ms < 10000) {
        return;
    }
    s_last_custom_sync_ms = now_ms;

    // 依次尝试：先 IP 直连（已验证可用），再域名（依赖 DNS）
    const char *servers[] = {
        "216.239.35.0",   // time.google.com NTP IP
        "129.6.15.28",    // time.nist.gov NTP IP
        "pool.ntp.org",
        "time.google.com",
    };
    for (int i = 0; i < 4; i++) {
        if (custom_ntp_request(servers[i])) {
            return;
        }
    }
    ESP_LOGW(TAG, "[NTP] All NTP servers failed, will retry later");
}

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

// ---- 触摸屏 Timer 按钮区域 (屏幕右下角, 底部状态栏右侧) ----
// 屏幕为竖屏 (宽 240, 高 320)。底部状态栏位于 y=250~320，
// 右侧为 TIMER 显示区域。触摸该区域视为按下 Timer 按钮。
#define TIMER_BTN_X_MIN     120
#define TIMER_BTN_X_MAX     240
#define TIMER_BTN_Y_MIN     250
#define TIMER_BTN_Y_MAX     320

// 说明：Timer 按钮 (右下角触摸) 启动倒计时时，使用当前记忆的 Sleep Timer
// 设定值 (dev->sleep_timer_setting，默认 30 分钟，可在 Sleep Timer 设置页调整)，
// 不再使用固定时长。

static thermostat_dev_t s_thermostat;
static dht11_config_t s_dht11;

// 1. 温度采集与控制任务 (2 秒周期)
//    同时负责配网超时检测和瞬态灯效完成后的后续处理
static void temp_control_task(void *pvParameters) {
    float temp_val = 0.0f;

    while (1) {
        // ---- 崩溃监控心跳：周期性记录运行时长快照，便于崩溃后定位时间点 ----
        crash_monitor_heartbeat(60000);   // 每 60 秒打印一次心跳日志

        // ---- SNTP 时间同步兜底 ----
        // 若 SNTP 尚未同步且 Wi-Fi 已连接，周期性重新触发同步（内部已节流）。
        ensure_time_synced();

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
        // 周期性更新 Wi-Fi 连接状态，供 LCD 顶部信息栏显示 Wi-Fi 符号
        // (已连接显示实心符号；未连接则闪烁符号)
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        s_thermostat.wifi_connected = (sta && esp_netif_is_netif_up(sta));

        lcd_display_update(&s_thermostat);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// 触摸校准状态标记：true 表示正在执行首次触摸校准。
// 校准期间触摸轮询任务 (touch_poll_task) 应跳过 Timer 按钮处理，
// 避免校准过程中的触摸被误判为按钮点击。
static volatile bool s_touch_calibrating = false;

// 等待一次有效的触摸按下，返回原始 ADC 坐标
// 返回 true 表示成功采集到有效触摸点
static bool calib_wait_for_press(uint16_t *raw_x, uint16_t *raw_y) {
    // 等待按下（带超时，避免无限阻塞）
    int timeout_ms = 0;
    const int PRESS_TIMEOUT_MS = 30000;   // 30 秒内未按下则超时
    while (timeout_ms < PRESS_TIMEOUT_MS) {
        if (touch_driver_get_raw_point(raw_x, raw_y)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout_ms += 50;
    }
    return false;
}

// 等待触摸释放（手指离开屏幕）
static void calib_wait_for_release(void) {
    // 等待释放（带超时，避免无限阻塞）
    int timeout_ms = 0;
    const int RELEASE_TIMEOUT_MS = 10000;  // 10 秒内未释放则继续
    while (timeout_ms < RELEASE_TIMEOUT_MS) {
        if (!touch_driver_is_pressed()) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        timeout_ms += 50;
    }
}

// 触摸校准任务：首次上电时执行交互式校准流程
// 流程：依次提示用户触摸屏幕 4 个角（左上/右上/左下/右下），
// 采集每个角点的原始 ADC 值，计算校准参数并保存到 NVS。
// 校准完成后自动退出（删除自身任务）。
static void touch_calibration_task(void *pvParameters) {
    ESP_LOGI(TAG, "=== Touch calibration started (first power-on) ===");

    // 标记正在校准，触摸轮询任务跳过 Timer 按钮处理
    s_touch_calibrating = true;

    // 显示校准页面
    lcd_display_calib_show();

    // 4 个角点的原始采样值 [raw_x, raw_y]
    uint16_t raw_tl[2], raw_tr[2], raw_bl[2], raw_br[2];
    bool ok = true;

    // 步骤 1：左上角
    lcd_display_calib_set_step(TOUCH_CALIB_STEP_TL);
    if (!calib_wait_for_press(&raw_tl[0], &raw_tl[1])) {
        ESP_LOGE(TAG, "Calibration timeout at TOP-LEFT corner");
        ok = false;
    }
    calib_wait_for_release();

    // 步骤 2：右上角
    if (ok) {
        lcd_display_calib_set_step(TOUCH_CALIB_STEP_TR);
        if (!calib_wait_for_press(&raw_tr[0], &raw_tr[1])) {
            ESP_LOGE(TAG, "Calibration timeout at TOP-RIGHT corner");
            ok = false;
        }
        calib_wait_for_release();
    }

    // 步骤 3：左下角
    if (ok) {
        lcd_display_calib_set_step(TOUCH_CALIB_STEP_BL);
        if (!calib_wait_for_press(&raw_bl[0], &raw_bl[1])) {
            ESP_LOGE(TAG, "Calibration timeout at BOTTOM-LEFT corner");
            ok = false;
        }
        calib_wait_for_release();
    }

    // 步骤 4：右下角
    if (ok) {
        lcd_display_calib_set_step(TOUCH_CALIB_STEP_BR);
        if (!calib_wait_for_press(&raw_br[0], &raw_br[1])) {
            ESP_LOGE(TAG, "Calibration timeout at BOTTOM-RIGHT corner");
            ok = false;
        }
        calib_wait_for_release();
    }

    if (ok) {
        // 计算校准参数
        touch_calibration_t calib;
        esp_err_t err = touch_driver_calibrate_from_corners(
                raw_tl, raw_tr, raw_bl, raw_br, &calib);
        if (err == ESP_OK) {
            // 保存到 NVS 并应用
            err = touch_driver_save_calibration(&calib);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Touch calibration completed and saved to NVS");
            } else {
                ESP_LOGE(TAG, "Failed to save calibration to NVS: %d", err);
                // 即使保存失败，也应用当前计算的校准参数
                touch_driver_set_calibration(&calib);
            }
        } else {
            ESP_LOGE(TAG, "Calibration computation failed: %d", err);
        }
    } else {
        ESP_LOGE(TAG, "Calibration aborted due to timeout");
    }

    // 校准完成：隐藏校准页面，恢复普通 UI
    lcd_display_calib_set_step(TOUCH_CALIB_STEP_DONE);
    vTaskDelay(pdMS_TO_TICKS(1000));   // 短暂显示完成提示
    lcd_display_calib_hide();

    // 清除校准标记，恢复触摸轮询任务
    s_touch_calibrating = false;

    ESP_LOGI(TAG, "=== Touch calibration finished ===");
    vTaskDelete(NULL);
}

// 2.2 触摸屏轮询任务 (200ms 周期)
// 读取 XPT2046 触摸点，检测右下角 Timer 按钮的"按下-释放"点击事件，
// 点击一次开启 30 分钟倒计时，再次点击关闭倒计时。
// 注意：触摸屏与 LCD 共用 SPI 总线，轮询周期不宜过短，否则会持续占用
// 共享总线、干扰 LCD 帧刷新（导致白屏/横线）。200ms 周期在响应速度与
// 总线占用之间取得平衡。
static void touch_poll_task(void *pvParameters) {
    // 触摸按下状态机：
    //   s_touch_btn_pressed = true 表示手指当前按在 Timer 按钮区域内
    //   当手指释放 (touched=false) 且之前按在按钮区域内时，触发一次点击
    bool btn_pressed = false;

    while (1) {
        // 校准期间跳过 Timer 按钮处理，避免校准触摸被误判为按钮点击
        if (s_touch_calibrating) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        touch_point_t point;
        if (touch_driver_get_point(&point) == ESP_OK) {
            // 判断触摸点是否落在 Timer 按钮区域内
            bool in_btn = point.touched &&
                          point.x >= TIMER_BTN_X_MIN && point.x <= TIMER_BTN_X_MAX &&
                          point.y >= TIMER_BTN_Y_MIN && point.y <= TIMER_BTN_Y_MAX;

            if (in_btn) {
                // 手指按在按钮区域内
                btn_pressed = true;
            } else if (btn_pressed && !point.touched) {
                // 手指已释放，且之前按在按钮区域内 -> 触发一次点击
                btn_pressed = false;

                // 仅在开机主页面下响应 Timer 按钮
                if (s_thermostat.mode == THERMOSTAT_MODE_ON &&
                    s_thermostat.current_page == UI_PAGE_MAIN) {

                    int64_t now_ms = esp_timer_get_time() / 1000;

                    if (s_thermostat.sleep_timer_active) {
                        // 倒计时进行中 -> 关闭倒计时
                        s_thermostat.sleep_timer_active = false;
                        s_thermostat.sleep_timer_start_ms = 0;
                        ESP_LOGI(TAG, "Touch Timer button -> Countdown OFF");
                    } else {
                        // 未倒计时 -> 使用当前记忆的 Sleep Timer 设定值启动倒计时
                        // 需求：触发 sleeper 启动的只有屏上右下角的触摸按键。
                        // 设定值由 Sleep Timer 设置页 (FUNC 进入) 通过编码器调整，
                        // 改动后记忆 (NVS)，此处直接使用 dev->sleep_timer_setting。
                        s_thermostat.sleep_timer_active = true;
                        s_thermostat.sleep_timer_start_ms = now_ms;
                        ESP_LOGI(TAG, "Touch Timer button -> %d min countdown started",
                                 s_thermostat.sleep_timer_setting);
                    }
                }
            } else if (!in_btn && point.touched) {
                // 手指按在按钮区域外，保持未按下状态
                btn_pressed = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
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

    // 0. 崩溃监控初始化：打印上次复位原因/崩溃时间点（需在最早阶段调用）
    ESP_ERROR_CHECK(crash_monitor_init());

    // 1. NVS 初始化
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 1.1 打印 NVS 中保存的历史复位记录（跨拔插 USB 保留，用于追溯真实崩溃原因）
    crash_monitor_print_history();

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

    //    b) SPI 数据引脚 (SCK/MOSI/MISO) 与触摸屏中断引脚 (Touch IRQ)：
    //       配置为输入并启用内部上拉，防止悬空导致误触发。
    //       注意：触摸屏片选 (Touch CS) 不在此配置，由 touch_driver_init()
    //       配置为输出并手动控制，以避免干扰共享 SPI 总线。
    gpio_config_t lcd_spi_conf = {
        .pin_bit_mask = (1ULL << GPIO_LCD_SCK) |
                        (1ULL << GPIO_LCD_MOSI) |
                        (1ULL << GPIO_LCD_MISO) |
                        (1ULL << GPIO_TOUCH_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   // 内部上拉，防止引脚悬空
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&lcd_spi_conf));
    ESP_LOGI(TAG, "LCD SPI & touch IRQ pins configured with internal pull-up");

    // 3. 初始化核心逻辑与加热器 GPIO
    ESP_ERROR_CHECK(thermostat_init(&s_thermostat, GPIO_HEATER_RELAY));

    // 3.1 从 NVS 读取记忆的 Sleep Timer 设定值
    //     需求：Sleep Timer 时长设置改动后记忆，下次上电读取记忆的设置。
    //     首次开机无记录时保持默认值 (30 分钟)。
    thermostat_sleep_timer_load(&s_thermostat);

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

    // 7.1 初始化 XPT2046 触摸屏驱动
    //     注意：触摸屏与 LCD 共用 SPI2_HOST 总线，必须在 lcd_display_init()
    //     之后调用（该函数初始化了 SPI 总线）。
    // 【2026-08-16 重新启用】用户已在硬件上为 GPIO13 (Touch CS) 增加外部上拉
    //     电阻到 3.3V，用于解决此前 GPIO13 初始化电平异常 (level=0) 的问题。
    //     现重新启用触摸驱动初始化与触摸轮询任务，验证触摸功能是否恢复正常。
    //
    // 校准参数：若 NVS 中已保存校准参数（已校准），则加载使用；
    // 否则使用默认参数，并在首次上电时执行交互式校准流程（见下方 9.1 节）。
    touch_config_t touch_cfg = {
        .spi_host = SPI2_HOST,
        .cs_gpio  = GPIO_TOUCH_CS,
        .irq_gpio = GPIO_TOUCH_IRQ,
        .lcd_cs_gpio = GPIO_LCD_CS,   // 触摸 SPI 事务期间强制关闭 LCD 片选，避免共享总线干扰
        .calibration = {
            .x_min = 300,
            .x_max = 3800,
            .y_min = 300,
            .y_max = 3800,
            .swap_xy = false,
            .invert_x = false,
            .invert_y = false,
        },
    };
    ESP_ERROR_CHECK(touch_driver_init(&touch_cfg));

    // 7.2 检查触摸屏校准状态
    //     若 NVS 中已有"已校准"标记，则加载校准参数，无需重新校准；
    //     否则在首次上电时执行交互式校准流程（见 9.1 节）。
    if (touch_driver_is_calibrated()) {
        // 已校准：从 NVS 加载校准参数（内部会更新运行时校准参数）
        touch_calibration_t saved_calib;
        if (touch_driver_load_calibration(&saved_calib)) {
            ESP_LOGI(TAG, "Touch already calibrated, loaded calibration from NVS");
        } else {
            ESP_LOGW(TAG, "Calibrated flag set but load failed, will re-calibrate");
        }
    } else {
        ESP_LOGI(TAG, "Touch NOT calibrated, will run calibration on first power-on");
    }

    // 8. 初始化 Matter 协议栈
    ESP_ERROR_CHECK(app_matter_init(&s_thermostat));

    // 8.1 注册 Wi-Fi 事件处理：当设备获得 IP 地址后触发 SNTP 时间同步。
    //     注意：Matter 配网通过 BLE 进行，Wi-Fi 凭证在配网过程中才被下发，
    //     因此启动时设备尚无网络连接。若在启动时直接调用 time_sync_init()，
    //     SNTP 首次请求会因无网络而失败，且重试间隔长达 1 小时，导致时间长时间
    //     停留在占位符。改为在 IP_EVENT_STA_GOT_IP 事件触发后再同步时间。
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_got_ip_event_handler, NULL));

    // 8.2 预先设置时区并初始化 SNTP 客户端（此时 Wi-Fi 可能尚未连接，
    //     首次同步会失败；待 GOT_IP 事件触发后会自动重新同步）。
    time_sync_init();

    // 默认为开机模式
    thermostat_set_mode(&s_thermostat, THERMOSTAT_MODE_ON);

    // 9. 创建后台并发 Task
    //    注意：lcd_ui_task 会执行 LVGL 控件操作 (lv_label_set_text / lv_obj_set_style_* 等)，
    //    这些操作需要较大的栈空间。若栈过小，长时间运行后可能触发栈溢出，
    //    导致内存损坏与系统重启。故将 lcd_ui_task 栈提升至 8192。
    xTaskCreate(temp_control_task, "temp_control_task", 4096, NULL, 5, NULL);
    xTaskCreate(led_ui_task,       "led_ui_task",       3072, NULL, 4, NULL);
    xTaskCreate(lcd_ui_task,       "lcd_ui_task",       8192, NULL, 4, NULL);
    xTaskCreate(button_poll_task,  "button_poll_task",  3072, NULL, 6, NULL);
    // 触摸轮询任务：检测触摸屏按下，映射到 Timer 按钮并切换 30 分钟倒计时。
    // 触摸屏与 LCD 共用 SPI2_HOST 总线，触摸驱动已改为手动控制片选 (spics_io_num=-1)，
    // 避免 spi_bus_add_device() 重新配置共享总线导致 LCD 显示异常。
    // 【2026-08-16 重新启用】配合 GPIO13 外部上拉修复，重新启用触摸轮询任务。
    xTaskCreate(touch_poll_task,   "touch_poll_task",   3072, NULL, 4, NULL);

    // 9.1 首次上电触摸校准
    //     若 NVS 中无"已校准"标记，则启动交互式校准任务。
    //     校准任务会显示校准页面，引导用户依次触摸屏幕 4 个角，
    //     采集原始 ADC 值计算校准参数并保存到 NVS，完成后自动退出。
    //     已校准的设备跳过此流程，直接使用 NVS 中保存的校准参数。
    if (!touch_driver_is_calibrated()) {
        xTaskCreate(touch_calibration_task, "touch_calib_task", 4096, NULL, 5, NULL);
        ESP_LOGI(TAG, "Touch calibration task started (first power-on)");
    }

    ESP_LOGI(TAG, "Initialization complete. Thermostat tasks running.");
}
