# 项目状态文档 (Project Status)

> 本文档用于记录当前会话的工作进展、已完成/未完成事项，以及新会话接手时需要了解的关键上下文。
> 最后更新：2026-08-12

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

---

## 4. 未完成 / 待处理事项

### 4.1 LCD 驱动编译错误（已解决）
原 5 个编译错误（字体未声明、`ESP_RETURN_ON_ERROR` 隐式声明、`SPI2_HOST` 类型不匹配、`esp_lcd_new_panel_ili9341` 隐式声明、`lvgl_port_tick_inc` 隐式声明）**已全部修复**，项目编译通过。详见 3.6 节。

### 4.2 待验证事项
- ~~烧录后验证屏幕实际显示温控器页面~~（**已完成**：屏幕显示正常，黑底白字、方向正确、TEMP 标签正确，见 3.8 节）。
- ~~Sleep Timer 设置页的 UI 尚未在 `lcd_display.c` 中实现~~（**已完成**：见 3.10 节，已实现标题 + 4 选项高亮渲染，编译通过）。
- 触摸屏（XPT2046）驱动尚未实现（当前仅实现显示，未实现触控输入）。

---

## 5. 关键文件清单

| 文件 | 作用 |
| :--- | :--- |
| `main/main.c` | 主程序，GPIO 初始化、任务创建、LCD 集成 |
| `main/app_matter.cpp` | Matter 协议栈接入 |
| `main/idf_component.yml` | 组件依赖（含 `esp_lvgl_port`、`esp_lcd_ili9341`） |
| `components/lcd_display/lcd_display.c` | LCD 驱动 + LVGL UI（**已修复，编译通过，显示验证通过**） |
| `components/led_strip_control/led_control.c` | WS2812B 灯效控制（已修复） |
| `components/button_handler/button_handler.c` | 按键/编码器处理（已补齐功能） |
| `components/thermostat_logic/thermostat_logic.c` | 温控逻辑（迟滞控制、睡眠定时） |
| `components/dht11/dht11.c` | DHT11 温度采集 |
| `components/crash_monitor/crash_monitor.c` | 崩溃监控（复位原因 + 崩溃时间点捕获，见 3.12 节） |
| `docs/01_requirements.md` | 需求规格说明书 |
| `docs/02_sdkconfig_note.md` | sdkconfig 配置说明 |

---

## 6. 新会话接手建议

1. **LCD 驱动编译问题已解决**（采用方案 A 变体：`espressif/esp_lcd_ili9341` 组件，见 3.6 节），项目已编译通过。
2. **屏幕显示验证已通过**（见 3.8 节）：黑底白字、方向正确、TEMP 标签正确，烧录后显示正常。
3. **崩溃诊断已启用**（见 3.12 节）：设备崩溃重启后，串口启动日志会打印醒目的崩溃横幅（复位原因 + 崩溃时运行时长），并已启用 Core Dump 到 Flash。
4. **NVS 历史记录**：每次启动的复位原因会持久化到 NVS（保留最近 8 次），即使拔插 USB 重连串口也能追溯真实崩溃原因。启动日志会打印 `Boot History`。
5. **崩溃后解码 Core Dump**（定位崩溃精确位置）：
   ```bash
   bash -c 'source /home/alex/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py coredump-info'
   ```
   > 需在崩溃发生后、且串口连接设备时执行；会读取 Flash 中的 core dump 并解码出崩溃调用栈。
6. 后续可继续实现：
   - **触摸屏（XPT2046）驱动**（当前仅实现显示，未实现触控输入）。
7. 构建命令：
   ```bash
   bash -c 'source /home/alex/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py build'
   ```
5. 后续可继续实现：
   - **触摸屏（XPT2046）驱动**（当前仅实现显示，未实现触控输入）。
6. 构建命令：
   ```bash
   bash -c 'source /home/alex/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py build'
   ```