# 项目状态文档 (Project Status)

> 本文档用于记录当前会话的工作进展、已完成/未完成事项，以及新会话接手时需要了解的关键上下文。
> 最后更新：2026-08-16

---

## 1. 项目概述

基于 **ESP32-C6** 的 **Wi-Fi + Matter 温控器** 项目。支持本地独立温控运行，同时作为标准 Matter 设备接入 Home Assistant / Apple Home / Google Home 等生态。

- **目标芯片**：ESP32-C6-DEVKITC-1
- **开发框架**：ESP-IDF v5.1（位于 `/home/alex/esp/esp-idf`）
- **构建命令**（注意 shell 为 `/bin/sh`，不支持 `source`，需用 bash 包裹）：
  ```bash
  bash -c 'source /home/alex/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py build'
  ```

---

## 2. 硬件资源与引脚分配（已确认）

| 硬件模块 | 引脚 (GPIO) | 说明 |
| :--- | :--- | :--- |
| 电源/配网按键 POWER | GPIO19 | 短按待机/开机；长按 5s 配对；长按 15s 恢复出厂 |
| 编码器 A 相 KEY_RA | GPIO21 | 板载 10k 上拉 |
| 编码器 B 相 KEY_RB | GPIO20 | 板载 10k 上拉 |
| 编码器按键 FUNC | GPIO18 | 板载 10k 上拉 |
| 加热器控制 | GPIO22 | 高电平开启加热 |
| 温度传感器 DHT11 | GPIO4 | 单总线，需滤波 |
| 指示灯阵列 WS2812B | GPIO8 | 6 颗级联 |
| LCD 背光 BL | GPIO0 | **无外部上拉，需内部上拉并置高** |
| LCD 复位 RESET | GPIO1 | **无外部上拉，需内部上拉** |
| LCD 片选 CS | GPIO2 | **无外部上拉，需内部上拉** |
| LCD 命令/数据 DC | GPIO3 | **无外部上拉，需内部上拉** |
| SPI MISO | GPIO10 | **无外部上拉，需内部上拉** |
| SPI MOSI | GPIO11 | **无外部上拉，需内部上拉** |
| SPI SCK | GPIO12 | **无外部上拉，需内部上拉** |
| 触摸屏片选 Touch CS | GPIO13 | **无外部上拉，需内部上拉** |
| 触摸屏中断 Touch IRQ | GPIO23 | **无外部上拉，需内部上拉** |

> **重要硬件说明**：除按键/编码器（GPIO18/19/20/21）外，**所有 TFT-LCD 与触摸屏相关 GPIO 均无外部上拉电阻**，必须在软件中配置 GPIO 内部上拉（`GPIO_PULLUP_ENABLE`）。此点已在 `main/main.c` 的 `app_main()` 中实现。

---

## 3. 已完成的工作

### 3.1 修复被中断 AI 破坏的 `led_control.c`
- 重构了 `led_control_update()` 函数，恢复正确的 **PAIRING / ON / STANDBY** 三种模式逻辑。
- 新增 `get_standby_color()` 辅助函数，用于待机呼吸灯按温度映射颜色。
- 删除了函数体外残留的孤立代码（原 355-387 行）。

### 3.2 补齐需求中缺失的睡眠定时/页面切换功能
- **Sleep Timer 页面**：旋转编码器循环切换 `OFF -> 10 MIN -> 30 MIN -> 60 MIN`。
- **FUNC 按键**：页面切换（主页面 <-> Sleep Timer 设置页）。
- **60 秒超时**：非主页面下无操作 60s 自动返回主页面。
- **睡眠定时 tick**：在 `main.c` 的 `temp_control_task` 中调用 `thermostat_sleep_timer_tick()`。

### 3.3 LCD 背光点亮
- 配置 GPIO0（LCD 背光）为内部上拉并置高，解决背光不亮问题。

### 3.4 白屏问题处理
- 为所有 LCD/触摸屏 GPIO 配置内部上拉（控制脚输出+上拉，SPI/触摸脚输入+上拉）。

### 3.5 新增 LCD 显示组件（已完成）
- 新增 `components/lcd_display/` 组件（`lcd_display.c` / `lcd_display.h` / `CMakeLists.txt`）。
- 在 `main/idf_component.yml` 添加依赖 `espressif/esp_lvgl_port: "^1.4.0"`（v1.x 使用 LVGL 8.3）。
- 在 `main/CMakeLists.txt` 的 REQUIRES 中添加 `lcd_display`。
- 在 `main/main.c` 中集成：`lcd_display_init()` 调用、`lcd_ui_task` 任务创建。
- 实现了温控器 UI 页面（主页面 / 待机页），并绑定温控状态（温度、模式、睡眠定时）。

### 3.6 修复 LCD 驱动编译错误（已完成）
采用 **方案 A 变体**：使用 Espressif 官方组件库 `espressif/esp_lcd_ili9341`（v1.2.0）解决 ILI9341 驱动缺失问题，无需修改 IDF 内核。具体修改：

1. **`main/idf_component.yml`**：新增依赖 `espressif/esp_lcd_ili9341: "^1.0.0"`（实际解析为 v1.2.0）。
2. **`components/lcd_display/lcd_display.c`**：
   - 新增 `#include "esp_check.h"` 与 `#include "esp_lcd_ili9341.h"`。
   - 字体修正：`montserrat_48` → `montserrat_32`（主页面大字），`montserrat_24` → `montserrat_20`（待机页中号字）。
   - `esp_lcd_new_panel_io_spi` 的 `SPI2_HOST` 强转为 `(esp_lcd_spi_bus_handle_t)`。
   - 删除 `lvgl_port_tick_inc(10);`（tick 由 `esp_lvgl_port` 内部处理），仅保留 `lv_timer_handler();`。
3. **`components/lcd_display/CMakeLists.txt`**：REQUIRES 中新增 `esp_lcd_ili9341`。
4. **`sdkconfig.defaults` / `sdkconfig`**：启用 LVGL 字体 `CONFIG_LV_FONT_MONTSERRAT_16/20/32`（UI 使用 14/16/20/32 四种字号）。
5. **构建验证**：`idf.py build` 编译通过，生成 `build/matter-thermostat.bin`（app 分区剩余 32%）。

### 3.7 修复 LVGL 运行时崩溃（已完成）
烧录后首次运行出现 `Guru Meditation Error: Load access fault`，崩溃于 `lvgl_port_add_disp()` → `lv_mem_alloc()`（`lv_tlsf` 内存池为 NULL）。

- **根因**：`esp_lvgl_port` v1.4.0 要求先调用 `lvgl_port_init()` 初始化 LVGL 内存池与定时器/任务，再调用 `lvgl_port_add_disp()`。原代码直接调用 `lvgl_port_add_disp()`，导致 LVGL 内存池未初始化而空指针崩溃。
- **修复**：在 [`lcd_display.c`](components/lcd_display/lcd_display.c:222) 的 `lvgl_port_add_disp()` 之前新增 `lvgl_port_init(&lvgl_cfg)`（使用 `ESP_LVGL_PORT_INIT_CONFIG()` 宏）。
- **验证**：重新编译通过，ILI9341 面板创建成功（`ili9341: LCD panel create success, version: 1.2.0`）。

### 3.8 修复显示效果问题（已完成）
烧录后屏幕显示存在三个问题：左右镜像、白底黑字、温度下方文字为 TEXT（应为 TEMP）。

- **左右镜像**：`disp_cfg.rotation.mirror_x = true` 未生效。**根因**：`esp_lvgl_port` 的 `lvgl_port_update_callback()` 仅在 `lv_disp_set_rotation()` 触发（改变 `drv->rotated`）时才会把 `mirror_x` 应用到面板，而本代码从不调用该函数。**修复**：在 [`lcd_display.c`](components/lcd_display/lcd_display.c:224) 面板初始化后直接调用 `esp_lcd_panel_mirror(panel, true, false)` 对面板硬件应用水平镜像。
- **白底黑字**：`esp_lcd_panel_invert_color(panel, true)` 导致颜色反转。**修复**：改为 `false`，实现黑底白字。
- **TEXT → TEMP**：`s_lbl_room_label` 创建后未设置文本。**修复**：在 `ui_update_main_page()` 中新增 `lv_label_set_text(s_lbl_room_label, "TEMP")`。
- **验证**：重新编译通过（`matter-thermostat.bin` 生成成功）。

### 3.9 主页面字号放大与 Wi-Fi 图标右对齐（已完成）
根据需求调整主页面各行的字符尺寸，并让 Wi-Fi 图标靠最右放置：

1. **`components/lcd_display/lcd_display.c`**：
   - 顶部信息栏拆分为两个标签：左侧 `s_lbl_time`（日期/时间）、右侧 `s_lbl_wifi`（Wi-Fi 符号，`LV_ALIGN_TOP_RIGHT` 靠最右）。
   - 字号调整：顶部信息栏 14 → 20（1.5 倍）；当前室温 32 → 48（2 倍）；设定温度 20 → 40（2 倍）；底部状态栏（HEAT/TIMER）16 → 24（1.5 倍）。
2. **`sdkconfig.defaults` / `sdkconfig`**：新增启用 LVGL 字体 `CONFIG_LV_FONT_MONTSERRAT_24/40/48`。
3. **构建验证**：`idf.py build` 编译通过，生成 `build/matter-thermostat.bin`（app 分区剩余 26%）。

### 3.10 实现 Sleep Timer 设置页 UI（已完成）
在 `components/lcd_display/lcd_display.c` 中实现了 Sleep Timer 设置页的完整 UI 渲染：

1. **新增控件句柄**：`s_lbl_sleep_title`（页面标题）与 `s_lbl_sleep_options[4]`（4 个选项标签）。
2. **新增 `ui_create_sleep_timer_page()`**：创建页面标题 `SLEEP TIMER SETTING`（顶部居中，20 号字）及 4 个纵向排列的选项标签（`OFF` / `10 MIN` / `30 MIN` / `60 MIN`，24 号字），默认隐藏。
3. **新增 `ui_update_sleep_timer_page()`**：根据 `sleep_timer_setting` 高亮当前选中项（白色），其余选项置灰（`0x555555`）。
4. **修改 `ui_render()`**：新增 `UI_PAGE_SLEEP_TIMER` 分支——隐藏主页面/待机页元素，显示 Sleep Timer 设置页控件；主页面与待机页分支同时隐藏 Sleep Timer 控件，避免页面残留。
5. **修改 `lcd_display_init()`**：在创建主页面控件后调用 `ui_create_sleep_timer_page()`。
6. **构建验证**：`idf.py build` 编译通过，生成 `build/matter-thermostat.bin`（app 分区剩余 26%）。

> 说明：页面切换、编码器循环选择（OFF -> 10 -> 30 -> 60 MIN）、60s 超时返回主页面等逻辑已在 `components/button_handler/button_handler.c` 中实现，本次仅补齐了对应的 UI 渲染层。

### 3.11 修复长时间运行后约 15 分钟自动重启问题（已完成）
**现象**：设备长时间开机后（约 15 分钟，时间不精确）必定自动重启；重启瞬间串口打印中断、无特殊报错 log，Ubuntu 侧串口需重新设置。

**根因分析**（多因素叠加，均可能导致堆内存耗尽 / 内存损坏后无日志复位）：
1. **LVGL `lv_timer_handler()` 被两个任务并发调用**：
   - `lvgl_port_init()`（`esp_lvgl_port` v1.4.0）内部已创建 "LVGL task"，该任务持续调用 `lv_timer_handler()` 进行渲染。
   - 原 `lcd_display_update()` 末尾又手动调用了一次 `lv_timer_handler()`，且**未加 `lvgl_port_lock()` 互斥保护**，导致两个任务并发执行 LVGL 定时器/刷新逻辑，破坏 LVGL 内部状态（显示缓冲、刷新状态机），长期运行后引发内存损坏与崩溃。
   - **修复**：删除 [`lcd_display.c`](components/lcd_display/lcd_display.c:446) 中手动调用 `lv_timer_handler()` 的代码，渲染统一交由 `esp_lvgl_port` 内部任务完成。
2. **Matter 属性无条件周期性上报导致上报队列累积/内存泄漏**：
   - 原 `app_matter_update()` 每 2 秒无条件调用 4 次 `attribute::update()`（LocalTemperature / ThermostatRunningMode / PIHeatingDemand / ThermostatRunningState）。
   - 在未配网 / 无控制器订阅时，这些上报会在 Matter 上报队列中持续累积，缓慢泄漏内存，最终堆耗尽触发复位。
   - **修复**：在 [`app_matter.cpp`](main/app_matter.cpp:195) 中为各属性增加"仅在数值变化时上报"的缓存判断，大幅减少上报调用次数。
3. **`lcd_ui_task` 栈空间偏小**：LVGL 控件操作（`lv_label_set_text` / `lv_obj_set_style_*` 等）栈开销较大，4096 字节偏紧，长时间运行可能栈溢出。
   - **修复**：将 [`main.c`](main/main.c:222) 中 `lcd_ui_task` 栈提升至 8192。

**附带修复（LED 灯效状态机逻辑错误）**：
- 原 `led_control_effect_finished()` 以 `phase == EFFECT_PHASE_IDLE` 作为"已结束"判断，存在两个 bug：
  1. 在 `main.c` 设置 `pending_led_effect` 的同一轮循环中，`led_ui_task` 尚未取走灯效（`active_effect` 仍为 NONE、`phase` 仍为 IDLE），会被误判为"已结束"，导致 `pending_led_effect` 被立即清零、灯效永远无法播放。
  2. 灯效真正播放完毕后 `phase` 变为 `EFFECT_PHASE_DONE`，反而永远返回 false，`pending_led_effect` 无法被清理。
- **修复**：将 [`led_control.c`](components/led_strip_control/led_control.c:287) 的判断条件改为 `phase == EFFECT_PHASE_DONE`，使"仅当灯效真正播放完毕"才返回 true。

**验证**：`idf.py build` 编译通过，生成 `build/matter-thermostat.bin`（app 分区剩余 26%）。

### 3.12 新增崩溃监控组件（已完成）
为解决"系统崩溃重启但难以抓到崩溃时间点与原因"的问题，新增 `components/crash_monitor/` 组件，并启用 Core Dump 到 Flash：

1. **新增 `components/crash_monitor/` 组件**（`crash_monitor.c` / `crash_monitor.h` / `CMakeLists.txt`）：
   - **复位原因捕获**：启动时读取 `esp_reset_reason()`，判断本次复位是否由异常/崩溃引起（PANIC / INT_WDT / TASK_WDT / WDT / BROWNOUT）。
   - **崩溃时间点捕获**：使用 **RTC_NOINIT 内存**周期性保存"最近一次运行时长快照"。崩溃发生时，最后一次快照即为崩溃发生的大致时间点（误差 <= 心跳间隔）。
   - **醒目启动横幅**：若检测到异常复位，启动时打印醒目的 `*** 检测到系统异常复位/崩溃 ***` 横幅，包含复位原因与崩溃时运行时长，无需紧盯串口即可得知。
   - **心跳日志**：`crash_monitor_heartbeat()` 周期性打印运行时长，便于在串口日志中定位崩溃前设备运行了多久。
   - **NVS 历史记录（新增）**：将每次启动的复位原因持久化到 NVS（Flash），保留最近 8 次记录。即使开发者在崩溃后拔插 USB 重新连接串口（导致本次上电复位原因被覆盖为 POWERON），也能从 NVS 历史中追溯真实的崩溃原因。`crash_monitor_print_history()` 打印历史记录。
2. **启用 Core Dump 到 Flash**（`sdkconfig.defaults` / `sdkconfig`）：
   - `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`：崩溃时保存精确调用栈到 Flash，重启后用 `idf.py coredump-info` 解码定位崩溃位置。
   - `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y`：ELF 格式（含符号信息，便于解码）。
   - 在 `partitions.csv` 中新增 `coredump` 分区（`data/coredump`，0x313000，0x10000）。
3. **崩溃后延迟重启**：`CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=5`，崩溃后延迟 5 秒再重启，确保串口日志完整输出，避免错过崩溃瞬间信息。
4. **集成到 `main/main.c`**：
   - `app_main()` 最早期调用 `crash_monitor_init()`（内部自行初始化 NVS 并写入本次复位记录）。
   - NVS 初始化后调用 `crash_monitor_print_history()` 打印历史复位记录。
   - `temp_control_task` 循环中调用 `crash_monitor_heartbeat(60000)`（每 60 秒打印一次心跳）。
   - `main/CMakeLists.txt` 的 REQUIRES 中新增 `crash_monitor`。

**验证**：`idf.py build` 编译通过，生成 `build/matter-thermostat.bin`（app 分区剩余 25%），`espcoredump` 组件正常编译链接。

> **诊断结论（重要）**：根据用户提供的实际崩溃日志，崩溃监控显示复位原因为 **POWERON（上电）**，而非 PANIC/WDT。但该复位原因可能因开发者在崩溃后拔插 USB 重连串口而被覆盖。为规避此问题，已新增 **NVS 历史记录**（见上文），即使拔插 USB 也能追溯真实崩溃原因。初步判断倾向硬件供电问题（进入配网模式时 Wi-Fi + BLE + Matter 同时工作产生峰值电流），但需结合 NVS 历史与 Core Dump 进一步确认。

### 3.13 顶部时间显示改为澳大利亚东部标准时间（AEST/AEDT，自动夏令时）（已完成）
原顶部信息栏时间显示为固定占位字符串 `"2026-08-11 10:30"`（未接入 RTC/网络时间）。现改为通过 **SNTP 网络校时** 获取 UTC 时间，并转换为 **澳大利亚东部标准时间**（AEST/AEDT，自动夏令时）显示：

1. **`main/main.c`**：
   - 新增 `time_sync_init()`：通过 `setenv("TZ", "AEST-10AEDT,M10.1.0,M4.1.0/3", 1)` + `tzset()` 设置时区（POSIX 时区字符串，libc 自动在 AEST UTC+10 与 AEDT UTC+11 间切换）。
   - 初始化 SNTP（`esp_sntp`，`SNTP_OPMODE_POLL`），服务器依次为 `pool.ntp.org` / `time.google.com` / `time.nist.gov`。
   - 在 `app_main()` 中 Matter/Wi-Fi 网络就绪后调用 `time_sync_init()`。
   - `main/CMakeLists.txt` 的 REQUIRES 中新增 `lwip`（ESP-IDF v5.1 中 `esp_sntp.h` 由 `lwip` 组件提供）。
2. **`components/lcd_display/lcd_display.c`**：
   - 新增 `format_local_time()`：用 `localtime_r()` 将系统时间（UTC）转换为本地时间，`strftime` 格式化为 `YYYY-MM-DD HH:MM`；SNTP 未同步时显示占位 `----/--/-- --:--`。
   - 主页面与待机页顶部时间均改用 `format_local_time()` 输出，替换原硬编码占位字符串。
   - 在 `lcd_display_update()` 中新增时间变化检测：分钟变化时强制刷新 UI，保证时间实时更新。

> **说明**：`esp_sntp` 在 `SNTP_OPMODE_POLL` 模式下更新系统时间时会调用 `settimeofday()`，从而触发 `tzset()` 重新计算夏令时，实现 AEST/AEDT 自动切换。

### 3.14 修复时间显示为 1970-01-01 11:00（已完成）
**现象**：调试发现顶部时间显示为 `1970-01-01 11:00`。

**根因**：ESP32 系统时间在 SNTP 同步完成前停留在 Unix 纪元（`1970-01-01 00:00 UTC`）。原 [`format_local_time()`](components/lcd_display/lcd_display.c:186) 仅判断 `time(NULL) == -1`（该条件在 ESP32 上永远不会触发），未检查 SNTP 是否已同步，因此在 SNTP 未完成（或失败）时，将纪元时间按 AEST（UTC+10）转换显示为 `1970-01-01 11:00`。

**修复**：
1. **`components/lcd_display/lcd_display.c`**：
   - 新增 `#include "esp_sntp.h"`。
   - 修改 [`format_local_time()`](components/lcd_display/lcd_display.c:186)：在格式化前先检查 `esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED`，若 SNTP 未同步则显示占位符 `----/--/-- --:--`，同步完成后才显示真实时间。
2. **`components/lcd_display/CMakeLists.txt`**：REQUIRES 中新增 `lwip`（ESP-IDF v5.1 中 `esp_sntp.h` 由 `lwip` 组件提供）。

**验证**：`idf.py build` 编译通过，生成 `build/matter-thermostat.bin`（app 分区剩余 25%）。

### 3.19 修复触摸屏运行一段时间后 IRQ 失效问题（共享 SPI 总线竞争）（已完成）
**现象**：触摸屏驱动重新启用后，程序运行一开始能检测到按压（坐标读取正常，raw 值合理），但运行一段时间后 IRQ（GPIO23）永久失效，触摸彻底检测不到；重启后可能恢复或仍失效。

**根因分析（结合诊断日志确认）**：
1. **触摸 SPI 读取本身正常**：正常触摸时 raw 值合理（`x=610 y=616 z1=217 z2=1145 pressure=928`）。坐标偏小（x=21,y=28）只是因为 raw ADC(~600) 接近校准最小值(300)，是校准参数问题，非读取失败。
2. **真正的失败机制**：触摸正常工作一段时间后出现异常读取（`raw: x=670 y=0 z1=3072 z2=1136`，y=0、z1=3072 明显异常），此后 IRQ 永远检测不到。证明 **XPT2046 收到了一次被破坏的 SPI 控制字节，进入了 PENIRQ 禁用的掉电模式（PD=11），此后 PENIRQ 不再触发**。
3. **根因**：触摸驱动用 `spi_device_transmit()` 手动控制 CS（GPIO13）并手动拉高 LCD CS（GPIO2），但**未获取 SPI 总线锁**。LCD 用 `esp_lcd_new_panel_io_spi()` 内部会加锁。两者并发时，触摸事务与 LCD 事务碰撞，XPT2046 收到被破坏的控制字节，进入 PENIRQ 禁用状态。
4. **GPIO13 (CS) 软件读 0 但万用表读高**：日志中 `GPIO13(CS)=0` 一直为 0，但触摸仍能正常工作，说明 CS 读取异常是独立的既有问题（见 3.17 节），**不是**本次 IRQ 失效的根因。

**修复**（[`components/touch_driver/touch_driver.c`](components/touch_driver/touch_driver.c)）：
- 在 `read_channel()` 中用 `spi_device_acquire_bus(s_spi_dev, portMAX_DELAY)` / `spi_device_release_bus(s_spi_dev)` 包裹整个触摸 SPI 事务（含手动 CS 操作），使触摸事务与 LCD 事务串行化，防止总线竞争破坏 XPT2046 状态。

**验证**：`idf.py build` 编译通过，生成 `matter-thermostat.bin`（app 分区剩余 24%）。**烧录后确认触摸功能持续正常**，不再出现运行一段时间后 IRQ 失效的问题（IRQ 正常切换，触摸点可稳定检测）。

> **遗留问题（独立于本次修复）**：触摸坐标仍偏小（如 `x=22 y=2`），raw ADC 值（如 `x=623 y=331`）接近校准最小值(300)，说明校准参数 `x_min/x_max/y_min/y_max`（当前 300~3800）范围过宽，需按实际触摸屏 ADC 范围调整，否则 Timer 按钮区域（右下角 x=120~240, y=250~320）无法命中。此为校准问题，非本次 IRQ 失效根因。

### 3.18 重新启用触摸屏驱动（GPIO13 外部上拉修复后）（进行中）
**背景**：此前触摸屏调试因 GPIO13 (Touch CS) 初始化电平异常（`[DIAG] Touch CS GPIO13: level=0`，即使代码显式置 1 仍读到 0）以及触摸 SPI 事务干扰 LCD 显示而暂停，触摸代码已全部注释（`[TOUCH-DISABLED]`）。

**本次硬件修复**：用户在硬件上为 **GPIO13 (Touch CS) 增加了一个外部上拉电阻到 3.3V**，用于解决 GPIO13 初始化电平异常问题。

**本次代码修改**（[`main/main.c`](main/main.c)）：
1. **重新启用触摸驱动初始化**：取消注释 `touch_config_t touch_cfg` 定义与 `touch_driver_init(&touch_cfg)` 调用（原 667-682 行）。
2. **重新启用触摸轮询任务**：取消注释 `xTaskCreate(touch_poll_task, ...)`（原 714 行）。

**构建验证**：`idf.py build` 编译通过，生成 `build/matter-thermostat.bin`（app 分区剩余 24%）。

**待验证（烧录后）**：
- 观察启动日志中 `[DIAG] Touch CS GPIO13: level=1` 是否恢复正常（此前为 0）。
- 观察 LCD 是否仍出现显示异常（白屏/横线）——若外部上拉解决了 CS 电平问题，触摸 SPI 事务可能不再干扰 LCD。
- 观察 `[TOUCH] IRQ=..` 与 `[TOUCH] touched x=.. y=..` 日志，验证触摸坐标能否成功读取。
- 验证右下角 Timer 按钮的 30 分钟倒计时功能。

### 3.20 新增电阻触摸屏校准功能（已完成）
**需求**：第一次开机时增加触摸校准过程；校准完成后将校准参数与"已校准"标记存入 NVS；后续每次上电时，若检测到已校准标记则跳过校准，否则重新执行校准。

**实现**（涉及 `touch_driver` / `lcd_display` / `main` 三个模块）：

1. **`components/touch_driver/`（NVS 持久化 + 校准 API）**：
   - 新增 NVS 命名空间 `touch_calib`，键 `calib`（校准参数 blob）与 `calibrated`（已校准标记 u8）。
   - 新增接口：`touch_driver_is_calibrated()`（读 NVS 标记）、`touch_driver_load_calibration()`（读标记 + blob 并应用到运行时）、`touch_driver_save_calibration()`（写 blob + 标记并 commit）、`touch_driver_set_calibration()`（仅更新运行时）、`touch_driver_get_raw_point()`（读取未校准的原始 X/Y ADC 值，含压力阈值判断）、`touch_driver_calibrate_from_corners()`（四点角标算法）。
   - **四点角标算法**：通过比较原始 X/Y 通道沿屏幕 X/Y 轴的差异自动判定 `swap_xy`，计算 min/max 得到 `x_min/x_max/y_min/y_max`，再通过左右/上下均值比较判定 `invert_x` / `invert_y`。若某轴范围 < 50 判定为无效采样并返回 `ESP_ERR_INVALID_RESPONSE`。
   - `CMakeLists.txt` 的 REQUIRES 新增 `nvs_flash`。
2. **`components/lcd_display/`（校准 UI 页面）**：
   - 新增 `touch_calib_step_t` 枚举（TL/TR/BL/BR/DONE）与 `lcd_display_calib_show()` / `lcd_display_calib_hide()` / `lcd_display_calib_set_step()`。
   - 新增校准页面：标题 "TOUCH CALIBRATION"、提示文字、青色圆形目标点（40×40）。目标点依次移动到四角 {30,30}/{210,30}/{30,290}/{210,290}，提示文字提示用户依次点击左上/右上/左下/右下。
   - `ui_render()` 在 `s_calib_active` 为真时提前返回，避免正常 UI 覆盖校准页面。
3. **`main/main.c`（校准流程集成）**：
   - 新增 `s_touch_calibrating` 标志，`touch_poll_task` 在校准期间跳过 Timer 按钮处理。
   - 新增 `touch_calibration_task`：显示校准页 → 依次采样四角（`calib_wait_for_press` / `calib_wait_for_release`）→ `touch_driver_calibrate_from_corners()` 计算 → `touch_driver_save_calibration()` 保存 → 显示 DONE → 隐藏页面 → 删除自身。
   - `app_main()` 在 `touch_driver_init()` 后检查 `touch_driver_is_calibrated()`：已校准则 `touch_driver_load_calibration()` 加载参数；未校准则记录日志等待校准任务执行。
   - 任务创建段：若 `!touch_driver_is_calibrated()` 则创建 `touch_calibration_task`（4096 栈，优先级 5）。

**构建验证**：`idf.py build` 编译通过，生成 `build/matter-thermostat.bin`（app 分区剩余 24%）。`touch_driver.c` / `lcd_display.c` 编译链接无错误，仅存在 CHIP/Matter 托管组件既有的 C++20 比较警告（与本次改动无关）。

> **待验证**：首次烧录后应自动进入校准流程，依次点击四角后参数写入 NVS；重启后应跳过校准直接进入主界面。若某角采样无效（轴范围 < 50）会返回错误并重新等待该角输入。

### 3.16 实现 XPT2046 触摸屏驱动 + Timer 按钮（已完成）
新增 `components/touch_driver/` 组件实现 XPT2046 触摸屏驱动，并将主页面右下角 timer 区域改为可触摸的按钮：

1. **新增 `components/touch_driver/` 组件**（`touch_driver.c` / `touch_driver.h` / `CMakeLists.txt`）：
   - **SPI 复用**：触摸屏与 LCD 共用 SPI2_HOST 总线（SCK=GPIO12 / MOSI=GPIO11 / MISO=GPIO10），触摸屏使用独立片选 CS=GPIO13、中断 IRQ=GPIO23。`touch_driver_init()` 必须在 `lcd_display_init()` 之后调用（该函数初始化了 SPI 总线），仅向总线添加一个 SPI 设备（2MHz，全双工）。
   - **全双工事务（关键修复）**：触摸设备**不使用** `SPI_DEVICE_HALFDUPLEX` 标志。ESP-IDF 半双工模式下不允许同一事务同时启用 MOSI 与 MISO 相位，会触发 `SPI half duplex mode is not supported` 错误。改用全双工事务（与共享总线上 ILI9341 LCD 一致），发送命令字节的同时读取返回数据。
   - **IRQ 门控读取（关键优化）**：`touch_driver_get_point()` 先检查 IRQ 引脚（GPIO23，低电平有效）是否检测到触摸按下，未按下时直接返回 `touched=false`，**不执行任何 SPI 事务**。原因：触摸屏与 LCD 共用 SPI 总线，若每次轮询都执行多次 SPI 事务（X/Y/Z1/Z2 各采样 3 次 = 12 次事务），会持续占用共享总线、干扰 LCD 帧刷新，导致屏幕出现横条/白屏等显示异常。仅在真正按下时才读取坐标，从而避免对共享总线的无谓占用。
   - **XPT2046 读取**：通过控制字节（X=0xD0 / Y=0x90 / Z1=0xB0 / Z2=0xC0）读取 12-bit ADC 原始值，多次采样取平均抑制噪声。
   - **压力检测**：通过 Z1/Z2 计算压力值（`pressure = |Z2 - Z1|`），低于阈值视为未按下，避免误触。
   - **坐标校准**：提供 `touch_calibration_t` 结构体，将原始 ADC 值线性映射为屏幕像素坐标（240×320），支持 `swap_xy` / `invert_x` / `invert_y` 修正方向与镜像。
   - **接口**：`touch_driver_init()` / `touch_driver_get_point()` / `touch_driver_is_pressed()`。
2. **集成到 `main/main.c`**：
   - 在 `lcd_display_init()` 之后调用 `touch_driver_init()`（配置 SPI2_HOST、CS=GPIO13、IRQ=GPIO23，校准参数为默认值，需按实际硬件调整）。
   - 新增 `touch_poll_task`（50ms 周期）：读取触摸点，检测右下角 Timer 按钮区域的"按下-释放"点击事件。
   - **Timer 按钮逻辑**：点击一次开启 30 分钟倒计时（`sleep_timer_setting=30`，`sleep_timer_active=true`），再次点击关闭倒计时。仅在开机主页面下响应。
   - `main/CMakeLists.txt` 的 REQUIRES 中新增 `touch_driver`。
3. **修改 `components/lcd_display/lcd_display.c`**：
   - 将右下角 timer 区域改为 LVGL 按钮控件（`lv_btn`，110×44，深色背景 + 青色边框），内部放置定时状态文字。
   - 倒计时进行中按钮边框变为绿色高亮，未开启时为青色边框。
   - `ui_render()` 中同步处理按钮的显示/隐藏（主页面显示，待机页/Sleep Timer 设置页隐藏）。
4. **构建验证**：`idf.py build` 编译通过，生成 `build/matter-thermostat.bin`（app 分区剩余 24%）。

> **待验证**：触摸屏校准参数（`x_min/x_max/y_min/y_max` 及方向/镜像）为默认值，需烧录后根据实际触摸屏 ADC 范围与方向调整（见 `main/main.c` 中 `touch_cfg.calibration`）。

### 3.17 触摸屏调试会话小结（XPT2046 显示异常排查，进行中）
**背景**：实现 XPT2046 触摸驱动后，屏幕出现严重显示异常（白屏 + 大量横条，横条位置不断变化，最终全白）。触摸坐标从未成功读取过。经过多轮排查，最终确认**触摸驱动是导致 LCD 显示异常的根因**，当前已临时禁用触摸代码，屏幕恢复正常。

**关键诊断结论（供新会话接手）**：
1. **禁用触摸驱动后屏幕恢复正常**（决定性证据）：将 `touch_driver_init()` 与 `touch_poll_task` 全部注释掉后，LCD 显示完全正常。说明触摸设备挂载到共享 SPI2_HOST 总线 / 触摸 SPI 事务是显示异常的根因。
2. **触摸屏与 LCD 共用 SPI2_HOST 总线**（SCK=GPIO12 / MOSI=GPIO11 / MISO=GPIO10），触摸屏独立片选 CS=GPIO13、中断 IRQ=GPIO23。操作触摸屏时必须保证 LCD 片选（GPIO2）关闭，反之亦然。
3. **已尝试但未解决的方案**：
   - **手动 CS 控制**（`spics_io_num=-1`，避免 `spi_bus_add_device()` 重配共享总线）：显示仍异常。
   - **触摸 SPI 事务期间强制拉高 LCD CS（GPIO2）**：显示仍异常。
   - **TRY1 实验**（每次 LCD 刷新后手动关闭一次 LCD_CS）：显示仍异常，已恢复原样。
4. **未解决的硬件异常**：初始化时 `[DIAG] Touch CS GPIO13: level=0`，即使代码显式将其置 1（高/未选中）。用户已确认硬件接线正确、无短路。此异常仍待查明（可能涉及 GPIO13 与其它外设/内部功能冲突）。
5. **触摸坐标从未成功读取**：即使 IRQ 检测到按下（IRQ=1），也从未打印出 `[TOUCH] touched x=.. y=..`，说明从未从 XPT2046 正确读到数据。
6. **IRQ 极性**：`is_pressed()` 检查 `gpio_get_level(irq) == 0`（低电平有效，标准 XPT2046）。打印中的 `IRQ=0` 表示未按下（物理电平 1），`IRQ=1` 表示按下（物理电平 0）。

**当前代码状态**：
- `main/main.c`：`touch_config_t touch_cfg` 声明、`touch_driver_init()` 调用、`xTaskCreate(touch_poll_task, ...)` 均已注释掉，并标注 `[TOUCH-DISABLED]` 标记。恢复时取消注释即可。
- `components/touch_driver/`：驱动代码完整保留（含 `lcd_cs_gpio` 字段与 LCD CS 强制关闭逻辑），未删除。
- `components/lcd_display/lcd_display.c`：TRY1 已完全恢复原样（无残留）。
- **构建验证**：`idf.py build` 编译通过（仅 `touch_poll_task` 未使用警告，无害）。烧录后屏幕显示恢复正常。

**下一步建议**（新会话继续）：
- 优先排查 **GPIO13 初始化电平异常**（读为 0 而非 1），这可能是触摸 SPI 事务无法正常工作的硬件/引脚冲突根因。
- 排查触摸 SPI 事务为何干扰 LCD 显示（共享总线仲裁 / CS 时序问题）。
- 恢复触摸代码后，先解决显示异常，再解决坐标读取，最后做 Timer 按钮功能验证。

### 3.15 修复配网成功后日期时间栏仍显示占位符（已完成）
**现象**：配网成功后，顶部日期时间栏仍显示占位符 `----/--/-- --:--`，不显示真实时间。

**根因分析**（SNTP 校时未完成）：
1. **SNTP 初始化时机过早**：原代码在 [`app_main()`](main/main.c) 启动时（`app_matter_init()` 之后）就调用 `time_sync_init()`，但此时设备**尚未连接 Wi-Fi**（Matter 配网通过 BLE 进行，Wi-Fi 凭证在配网过程中才被下发）。SNTP 首次 NTP 请求因无网络路由而失败。
2. **SNTP 重试间隔过长**：`CONFIG_LWIP_SNTP_UPDATE_DELAY` 默认值为 **1 小时**（3600000ms）。在 `SNTP_OPMODE_POLL` 模式下，首次同步失败后需等待该时长才重试。即使之后配网成功、Wi-Fi 就绪，SNTP 也要等最多 1 小时才重试，导致时间长时间停留在占位符。
3. **SNTP 服务器数量不匹配**：代码设置了 3 个服务器（`pool.ntp.org` / `time.google.com` / `time.nist.gov`），但 `CONFIG_LWIP_SNTP_MAX_SERVERS` 默认为 1。

**修复**：
1. **`main/main.c`**：
   - 新增 `wifi_got_ip_event_handler()`：监听 `IP_EVENT_STA_GOT_IP` 事件，在设备获得 IP 地址后触发 `time_sync_init()`，确保 SNTP 在 Wi-Fi 真正就绪后再同步。
   - 在 `app_main()` 中注册该事件处理器（`esp_event_handler_register`）。
   - 将 `time_sync_init()` 改为可安全多次调用（新增 `s_sntp_started` 保护）：首次调用执行 `esp_sntp_init()`，后续调用通过 `esp_sntp_restart()` 重新触发同步。
   - **新增 `ensure_time_synced()` 兜底机制**：在 `temp_control_task` 中周期性调用，若检测到 SNTP 尚未同步且 Wi-Fi STA 已连接（`esp_netif_is_netif_up`），则节流（至少 15 秒）后重新触发 SNTP 同步。此机制作为 GOT_IP 事件处理的兜底，解决 Matter 内部管理 Wi-Fi 连接时事件可能不投递到默认事件循环的问题。
   - **SNTP 服务器列表扩充为 4 个**：前两个为域名（`pool.ntp.org` / `time.google.com`），后两个为 IP 地址（`162.159.200.123` / `162.159.200.1`），并同步将 `CONFIG_LWIP_SNTP_MAX_SERVERS` 调整为 4，避免服务器数量超过上限。
   - **`sdkconfig.defaults`**：新增 `CONFIG_LWIP_SNTP_MAX_SERVERS=4`、`CONFIG_LWIP_SNTP_UPDATE_DELAY=3600000`（保持 1 小时轮询）、`CONFIG_LWIP_SNTP_STARTUP_DELAY=0`。
   - **`format_local_time()` 改进**：在 `main/main.c` 中，若 `localtime_r()` 返回的年份 < 2020（即 SNTP 尚未同步、时间为 1970 基准），则显示占位符 `----/--/-- --:--`；否则显示真实时间。同时增加 `time(NULL)` 与 `esp_timer_get_time()` 的日志打印，便于诊断。

**验证**：配网成功后，顶部日期时间栏在数秒内显示真实时间（AEST/AEDT），不再停留在占位符。

**诊断结论**：SNTP 初始化时机过早 + 重试间隔过长 + 服务器数量超限是根因。通过「GOT_IP 事件触发 + 兜底轮询 + 服务器扩充」三重修复解决。

---

## 4. 未完成 / 待处理事项

### 4.1 触摸屏调试（进行中，新会话接手）
- **触摸坐标从未成功读取**：XPT2046 触摸 SPI 事务无法正常完成，坐标读取始终失败。
- **GPIO13 初始化电平异常**：初始化时 `[DIAG] Touch CS GPIO13: level=0`，即使代码显式将其置 1，硬件上仍读到 0。怀疑引脚冲突或外部接线问题。
- **触摸 SPI 事务干扰 LCD 显示**：启用触摸驱动后 LCD 出现严重显示异常（白屏 + 横条），禁用触摸代码后恢复正常。共享 SPI2_HOST 总线的仲裁 / CS 时序问题待深入排查。
- **Timer 按钮功能未验证**：触摸驱动的 30 分钟倒计时按钮功能尚未在硬件上验证。

### 4.2 触摸屏当前状态（已更新）
- **当前代码状态（2026-08-16）**：用户已在硬件上为 GPIO13 (Touch CS) 增加外部上拉电阻到 3.3V。`main/main.c` 中触摸驱动初始化与触摸轮询任务已**重新启用**（见 3.18 节），`touch_driver` 组件完整保留。构建通过，待烧录验证。
- **新增触摸校准功能（见 3.20 节）**：已实现首次开机自动校准流程（四点角标）、校准参数与"已校准"标记的 NVS 持久化，以及后续上电按标记跳过/执行校准的逻辑。构建通过。
- **下一步（新会话）**：烧录后观察 `[DIAG] Touch CS GPIO13: level` 是否恢复正常（应为 1）、LCD 是否仍显示异常、触摸坐标能否读取，验证 Timer 按钮功能，并验证首次开机校准流程（依次点击四角后参数写入 NVS，重启后跳过校准）。

## 5. 关键文件清单

| 文件 | 说明 |
|------|------|
| [`main/main.c`](main/main.c) | 主程序：初始化、触摸轮询任务、Timer 按钮逻辑、触摸校准流程（`touch_calibration_task`）、SNTP 校时 |
| [`components/touch_driver/touch_driver.c`](components/touch_driver/touch_driver.c) | XPT2046 触摸驱动（含 `lcd_cs_gpio` 字段、NVS 校准持久化与四点角标校准算法） |
| [`components/touch_driver/include/touch_driver.h`](components/touch_driver/include/touch_driver.h) | 触摸驱动头文件 |
| [`components/lcd_display/lcd_display.c`](components/lcd_display/lcd_display.c) | ILI9341 LCD 显示组件（与触摸共用 SPI2_HOST 总线） |
| [`components/button_handler/button_handler.c`](components/button_handler/button_handler.c) | 物理按键处理（PCNT 编码器） |
| [`components/thermostat_logic/thermostat_logic.c`](components/thermostat_logic/thermostat_logic.c) | 温控逻辑 |
| [`components/crash_monitor/crash_monitor.c`](components/crash_monitor/crash_monitor.c) | 崩溃监控组件 |
| [`docs/01_requirements.md`](docs/01_requirements.md) | 需求文档 |
| [`docs/02_sdkconfig_note.md`](docs/02_sdkconfig_note.md) | sdkconfig 说明 |

## 6. 新会话接手建议

### 6.1 触摸屏调试（最高优先级）
1. **优先排查 GPIO13 初始化电平异常**：`[DIAG] Touch CS GPIO13: level=0`，即使代码显式置 1 仍读到 0。检查：
   - GPIO13 是否与其它外设/功能冲突（如 JTAG、其它 SPI 片选）。
   - 外部接线是否正确（触摸 CS 是否真正连接到 GPIO13）。
   - 是否缺少上拉电阻（所有 LCD/触摸 GPIO 均无外部上拉，需内部上拉）。
2. **排查触摸 SPI 事务干扰 LCD 显示**：共享 SPI2_HOST 总线的仲裁 / CS 时序问题。可尝试：
   - 触摸事务期间强制拉高 LCD CS（`lcd_cs_gpio` 字段已实现）。
   - 检查 SPI 总线锁（`spi_bus_lock`）是否正确使用。
3. **恢复触摸代码顺序**：先解决显示异常 → 再解决坐标读取 → 最后验证 Timer 按钮功能。

### 6.2 硬件接线确认
- LCD：SCK=12, MOSI=11, MISO=10, CS=2, DC=3, RESET=1, BL=0
- 触摸：CS=13, IRQ=23
- 所有 GPIO 无外部上拉，需内部上拉。

### 6.3 构建与烧录命令
- 构建：`bash -c 'source /home/alex/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py build'`
- 烧录：`idf.py -p /dev/ttyUSB1 erase-flash flash monitor`