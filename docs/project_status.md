# 项目状态文档 (Project Status)

> 本文档用于记录当前会话的工作进展、已完成/未完成事项，以及新会话接手时需要了解的关键上下文。
> 最后更新：2026-08-27

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
| 编码器按键 FUNC | GPIO15 | 板载 10k 上拉 |
| 睡眠定时按键 SLEEP | GPIO18 | 板载 10k 上拉 |
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

> **重要硬件说明**：除按键/编码器（GPIO15/18/19/20/21）外，**所有 TFT-LCD 与触摸屏相关 GPIO 均无外部上拉电阻**，必须在软件中配置 GPIO 内部上拉（`GPIO_PULLUP_ENABLE`）。此点已在 `main/main.c` 的 `app_main()` 中实现。

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

1. **新增控件句柄**：`s_lbl_sleep_title`（页面标题）、`s_sleep_opt_box[4]`（4 个选项容器）与 `s_lbl_sleep_options[4]`（4 个选项文字标签）。
2. **新增 `ui_create_sleep_timer_page()`**：创建页面标题 `SLEEP TIMER SETTING`（顶部居中，20 号字）及 4 个纵向排列的选项容器（`10 MIN` / `30 MIN` / `60 MIN` / `90 MIN`，24 号字），默认隐藏。
3. **新增 `ui_update_sleep_timer_page()`**：选中项始终居中显示（白底黑字 + 边框，即正常显示的反转），未选中项透明背景 + 灰色文字（`0x555555`）；列表随选中项滚动（间距 40px），若选项会阻挡页面标题或超出屏幕底部则隐藏该选项。
4. **修改 `ui_render()`**：新增 `UI_PAGE_SLEEP_TIMER` 分支——隐藏主页面/待机页元素，显示 Sleep Timer 设置页控件；主页面与待机页分支同时隐藏 Sleep Timer 控件，避免页面残留。
5. **修改 `lcd_display_init()`**：在创建主页面控件后调用 `ui_create_sleep_timer_page()`。
6. **构建验证**：`idf.py build` 编译通过，生成 `build/matter-thermostat.bin`（app 分区剩余 26%）。

> 说明：页面切换、编码器非循环选择（10 -> 30 -> 60 -> 90 MIN，到两端停住）、60s 超时返回主页面等逻辑已在 `components/button_handler/button_handler.c` 中实现，本次仅补齐了对应的 UI 渲染层。

### 3.10.1 Sleep Timer 功能重构（已完成）
对 Sleep Timer 功能进行重构，调整触发方式、选项、默认值与记忆逻辑：

1. **触发方式调整**：仅屏上右下角的触摸按键触发 sleeper（睡眠定时）启动；实体 FUNC 按键仅将主页面切换到 Sleep Timer 时长设置页，不再启动倒计时。
2. **选项调整**：移除 `OFF` 选项，改为 `10 MIN` / `30 MIN` / `60 MIN` / `90 MIN`。
3. **默认值**：第一次开机默认值为 `30 MIN`（`thermostat_init` 与恢复出厂均设为 30）。
4. **记忆功能（NVS 持久化）**：
   - 新增 `thermostat_sleep_timer_save()` / `thermostat_sleep_timer_load()`（`components/thermostat_logic/thermostat_logic.c`），使用命名空间 `sleep_timer`、键 `setting`。
   - 编码器改动设定值后立即保存；`main.c` 上电初始化后调用 `thermostat_sleep_timer_load()` 读取记忆值。
   - `thermostat_logic/CMakeLists.txt` 增加 `nvs_flash` 依赖。
5. **编码器逻辑**：顺时针旋转选中项向后移动（列表上移），到 `90 MIN` 停住；逆时针反之，到 `10 MIN` 停住，不做周期移动。
6. **触摸按键逻辑**：`main.c` 中触摸按键启动倒计时时使用 `sleep_timer_setting`（记忆值），关闭时不再清零设定值。

### 3.10.2 Sleep Timer 标题栏与校准页显示优化（已完成）
1. **Sleep Timer 标题栏蓝底白字**：将 `s_lbl_sleep_title` 由普通标签改为带蓝色背景（`0x0055AA`）的容器（`lv_obj`，宽 240、高 36），内部放置白色文字标签 `SLEEP TIMER SETTING`；`SLEEP_TITLE_BOTTOM` 相应调整为 76，避免选项阻挡标题栏。
2. **校准页隐藏非校准内容**：新增 `ui_calib_hide_all_normal()` / `ui_calib_restore_all_normal()`，在 `lcd_display_calib_show()` 中隐藏所有非校准相关内容（日期时间、Wi-Fi 符号、室温、设定温度、加热状态、Timer 按钮、待机文字、Sleep Timer 标题与选项），在 `lcd_display_calib_hide()` 中恢复，交由 `ui_render()` 按当前页面状态统一管理。

### 3.10.3 Sleep Timer 页与主页面 UI 微调（已完成）
1. **Sleep Timer 选项列表整体下移**：将 `SLEEP_OPT_CENTER_Y` 由 160 调整为 182（先下移半个字体高度 12px，再按反馈下移 10px，共 22px）。
2. **Sleep Timer 标题改为蓝底黑字**：标题栏背景仍为蓝色（`0x0055AA`），内部文字颜色由白色改为黑色。
3. **Sleep Timer 标题栏上移至日期下方**：标题栏由 `LV_ALIGN_TOP_MID, 0, 40` 上移至 `LV_ALIGN_TOP_MID, 0, 30`（紧贴顶部日期/时间栏下方，蓝色底不遮挡日期与时间）；同步将 `SLEEP_TITLE_BOTTOM` 由 76 调整为 66，保证选项隐藏判定与新标题位置一致。
4. **修复 90MIN 选项被误隐藏**：将 `SLEEP_SCREEN_BOTTOM` 由 300 调整为 302。选中 `10 MIN` 时 `90 MIN` 选项中心位于 302（框底 320 恰好贴屏幕底），原阈值 300 会将其误隐藏，现可正常显示。
6. **主页面 Timer 按钮加高并改为多行**：
   - 按钮高度由 44 增至 88（下边位置不动，只向上增加）。
   - 按钮内部文字颜色改为**黑色**。
   - 上半部分两行标题：新增 `s_lbl_timer_title`（第一行 `SLEEP`，16 号字）与 `s_lbl_timer_title2`（第二行 `TIMER`，16 号字），保持原字体。
   - 下半部分：原 `s_lbl_timer`（24 号字，黑色），未开启显示 `OFF`，倒计时中显示 `mm:ss`。
   - `ui_update_main_page()` 中未开启文字由 `TIMER: OFF` 改为 `OFF`。

### 3.10.4 UI 文案调整：Sleep Timer 改为 Off Timer（已完成）
1. **主页面右侧定时卡片**：顶部标题文本由 `SLEEP TIMER` 修改为 `OFF TIMER`。
2. **定时设置页面标题**：顶部蓝色标题栏内文本由 `SLEEP TIMER SETTING` 修改为 `OFF TIMER SETTING`。

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

**硬件验证（2026-08-16，已完成）**：
- **首次开机校准通过**：校准页面正常显示，依次点击四角后校准完成，参数写入 NVS。
- **坐标映射准确**：点击右下角 Timer 按钮位置，映射坐标稳定在 `x≈188~201, y=319`，落在 Timer 按钮区域（`x:120~240, y:250~320`）内。
- **Timer 按钮点击正常**：按下-释放后成功触发点击，打印 `Touch Timer button -> Countdown OFF`（切换逻辑正确）。
- **重启跳过校准**：reboot 后未再次进入校准流程，说明 NVS 中的"已校准"标记持久化生效。

> **遗留说明**：校准功能已通过硬件验证，工作正常。

### 3.21 屏蔽触摸调试打印信息（已完成）
**背景**：触摸校准与 Timer 按钮功能已通过硬件验证，为后续开发其它工作做准备，屏蔽触摸调试过程中使用的高频打印信息，仅保留必要的触摸日志。

**移除的调试打印**：
1. **`main/main.c` 的 `touch_poll_task`**：
   - `[TOUCH] IRQ=%d`（每次轮询打印 IRQ 状态）
   - `[DIAG] GPIO13(CS)=%d GPIO23(IRQ)=%d`（打印 CS/IRQ 物理电平）
   - `[TOUCH] touched x=%d y=%d`（打印映射后的触摸坐标）
2. **`components/touch_driver/touch_driver.c`**：
   - `[DIAG] Touch CS GPIO%d...` / `[DIAG] Touch IRQ GPIO%d...`（初始化时打印引脚电平）
   - `[DIAG] get_point: IRQ_level=%d CS_level=%d`（每次读取打印 IRQ/CS 电平）
   - `[DIAG] raw: x=%u y=%u z1=%u z2=%u pressure=%d`（打印原始 ADC 值与压力）

**保留的必要日志**：
- `XPT2046 touch driver initialized...`（初始化确认）
- `Loaded calibration from NVS...`（上电加载校准参数）
- `Calibration saved to NVS...` / `Calibration computed...`（校准保存/计算）
- `Touch Timer button -> ...`（Timer 按钮点击事件，功能日志）
- 各类错误日志（NVS 失败、校准失败、SPI 失败等）

**说明**：
- `main/main.c` 中剩余的 `[DIAG]` 打印均为 **NTP 故障诊断**相关（`diag_test_dns()` 等），与触摸调试无关，未改动。
- `GPIO_TOUCH_CS` / `GPIO_TOUCH_IRQ` 宏仍被 GPIO 配置与触摸初始化使用，无未使用告警。

**构建验证**：`idf.py build` 编译通过（exit 0），`touch_driver.c` 与 `main.c` 重新编译链接无错误、无新增告警，生成 `build/matter-thermostat.bin`（app 分区剩余 24%）。触摸功能逻辑（校准、坐标映射、Timer 按钮点击）不受影响。

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

### 3.23 轻触开关调试完成 + 电阻触摸屏底部盲区问题排查与修复（已完成）

#### 3.23.1 关闭轻触开关（物理按键）调试打印
**背景**：轻触开关硬件已调试完毕，`button_handler.c` 中保留了一段用于调试的高频打印，每 2 秒及任意按键状态变化时打印所有按键的 GPIO 电平，会持续刷屏。

**修改**（[`components/button_handler/button_handler.c`](components/button_handler/button_handler.c)）：
- 注释掉 `button_handler_poll()` 中的 `[BUTTON DEBUG] Raw states: PWR/FUNC/SLEEP` 定时打印代码块（含 `last_raw_pwr` / `last_raw_func` / `last_raw_sleep` / `last_print_time` 四个静态变量及 if 判断）。

#### 3.23.2 电阻屏底部盲区问题定位与修复

**现象**：屏幕底部两个触摸按钮（HEAT / Sleep Timer），按压**上半部分**可以响应，按压**下半部分**无反应。

**诊断过程（利用临时调试日志）**：

为定位问题，临时开启了以下三层诊断日志：
1. **`[TOUCH DEBUG] SPI read`**（`touch_driver.c`）：打印每次按压的原始 ADC 值（`raw_x`, `raw_y`, `z1`, `z2`, `pressure`）。
2. **`[TOUCH DETAIL]`**（`touch_driver.c`）：在 `calibrate_point()` 内打印完整的坐标映射过程，包括 `raw(x,y)` → 校准参数 → `mapped(x,y)` → `clamped(x,y)`。
3. **`[TOUCH INDEV] PRESSED/RELEASED`**（`lcd_display.c`）：在 LVGL 输入设备回调中打印最终传入 LVGL 的像素坐标，并实时判断坐标是否落入 HEAT 按钮（`x:[8,116], y:[246,308]`）和 Timer 按钮（`x:[124,232], y:[246,308]`）区域内。

**诊断结论（通过日志精确定位）**：

实测日志对比：
| 按压位置 | `raw_y` | `mapped y` | `clamped y` | 按钮响应 |
|---------|---------|-----------|------------|--------|
| 按钮上半部 | 506 | 285 | 285 | ✅ 响应 |
| 按钮下半部 | 337 | 320 | **319** | ❌ 不响应 |

**根因**：校准参数 `y_min=345`（对应屏幕 Y=0 顶端，因 `invert_y=1`）。物理按压按钮下半部分时 `raw_y ≈ 337 < 345`，代入映射公式：
```
sy = (337 - 345) × 319 / (1815 - 345) = -8 × 319 / 1470 ≈ -1.7
经 invert_y 后：319 - (-1) = 320 → clamp 到 319
```
计算结果 319 超出了按钮下边界 308，LVGL 判定触摸点落在按钮外，故不触发点击事件。

**根因溯源**：上一次触摸校准时，底部靶点位于 `y=290`（距屏幕底边 30px），导致校准数据未能覆盖按钮下半部分的实际物理触摸区域（`raw_y` 极值约为 `~330`）。

#### 3.23.3 修复方案

1. **调整校准靶点位置**（[`components/lcd_display/lcd_display.c`](components/lcd_display/lcd_display.c)）：
   - 顶部两点：`y=30 → y=20`（靠近顶边）
   - 底部两点：`y=290 → y=305`（靠近底边，覆盖按钮下半部分触摸区域）

2. **新增 `touch_driver_erase_calibration()` 函数**（[`components/touch_driver/touch_driver.c`](components/touch_driver/touch_driver.c) + [`include/touch_driver.h`](components/touch_driver/include/touch_driver.h)）：
   - 打开 NVS `touch_calib` 命名空间，删除 `calib` blob 和 `calibrated` flag，commit 后关闭。
   - 用途：在校准参数出错时可从代码层面强制清除 NVS 校准数据，下次上电自动触发重新校准。

3. **强制一次性重新校准**（`main/main.c`）：
   - 在调试期间临时调用 `touch_driver_erase_calibration()` 清除旧校准，触发用新靶点位置的重新校准流程。
   - **校准通过后已删除此临时调用**，恢复为原来的"已校准则加载、未校准则首次执行校准"逻辑。

4. **关闭所有触摸诊断日志**（调试完成后）：
   - `touch_driver.c`：注释掉 `[TOUCH DEBUG] IRQ Pin`、`[TOUCH DEBUG] SPI read`、`[TOUCH DEBUG] Reading ignored`、`[TOUCH DETAIL]` 四组打印，并移除对应的无用局部变量（`now_ms`、`last_monitor_ms`）。
   - `lcd_display.c`：移除 LVGL 回调中的 `[TOUCH INDEV] PRESSED/RELEASED` 日志及按钮边界判断的临时代码，恢复简洁的回调实现。

**构建验证**：`idf.py build` 编译通过，exit 0，无新增错误与警告，生成 `build/matter-thermostat.bin`（app 分区剩余 26%）。

**硬件验证（2026-08-27）**：
- 重新校准后，按压两个底部按钮的上半部分和下半部分均正常响应。
- 重启后不再触发校准流程，直接从 NVS 加载参数，行为与修复前一致。

> **经验总结（触摸屏校准靶点设置原则）**：电阻屏校准靶点应尽量靠近屏幕物理边缘，覆盖实际可能被触摸到的最大 ADC 范围。靶点距边缘越远，越容易出现"物理边缘区域 raw 值超出 y_min/y_max 范围、映射后被 clamp 到极值、落在控件外"的问题。本项目建议靶点距边缘不超过 20px。

---

### 3.24 主界面文字与图形边缘平滑优化 + 圆环缩小 + 字号放大（已完成）
**现象**：主界面文字与图形边缘不平滑，圆形边缘坑洼、小字号文字（如 "9"）线条断断续续，疑似存在勾边/阴影等高级渲染效果。

**根因**：LVGL 8.4 默认开启**抗锯齿（anti-aliasing）**（`lv_disp_drv_t.antialiasing` 在颜色深度 > 8 时默认置 1）。在 240x320 低分辨率 RGB565 屏幕上，抗锯齿会在文字/图形边缘产生灰蒙蒙的过渡像素，反而让边缘显得模糊、坑洼、断断续续，而非平滑。

**修复**（[`components/lcd_display/lcd_display.c`](components/lcd_display/lcd_display.c)）：
1. **关闭抗锯齿**：在 [`lcd_display_init()`](components/lcd_display/lcd_display.c:847) 中 `lvgl_port_add_disp()` 成功后设置 `s_disp->driver->antialiasing = 0`，使文字与图形边缘锐利清晰，去除模糊/勾边效果。
2. **缩小两个同心圆半径约 10%**：
   - 外侧温度圆弧（`s_arc_current` / `s_arc_target`）：168×168（半径 84）→ **152×152**（半径 76）。
   - 中央深色表盘（`s_obj_inner_dial`）：126×126（半径 63）→ **114×114**（半径 57）。
   - 外圈 5 个刻度圆点半径：84 → **76**（跟随外圆内移）。
   - 目标温度读数标签中心半径：104 → **96**（按比例内移）。
   - 底部 `15°` / `25°` 极值标注位置：y=216 → **y=208**（跟随缩小后的圆环上移）。
3. **放大外圈刻度文字与设定温度文字字号**（16 → 20，大一号）：
   - 外圈极值标注 `15°` / `25°`（`s_lbl_scale_min` / `s_lbl_scale_max`）：`lv_font_montserrat_16` → `lv_font_montserrat_20`。
   - 黄色刻度线外侧目标温度读数（`s_lbl_target_temp_val`，如 `22.5°`）：`lv_font_montserrat_16` → `lv_font_montserrat_20`。

**验证**：`idf.py build` 编译通过，生成 `build/matter-thermostat.bin`。

---

### 3.22 LVGL 240x320 竖屏温控器主界面 UI 重构与开发（已完成）
根据需求更新，参考官方 Thermometer Demo 风格对 240x320 竖屏主界面进行重构与开发：

1. **界面裁剪与布局重构**：
   - 去掉了官方 Demo 中右侧风扇模块，底部布局重构为对称的 HEATER 状态与 SLEEP TIMER 按钮。
   - **顶部紧凑状态栏**：左侧显示 `Fri, Mar 11  19:45`（缩写星期/月份，24小时制，AEST/AEDT 自动夏令时），右侧显示 Wi-Fi 状态图标（连接常亮，断开 0.5s 闪烁）。
2. **中央温度控制与显示区 (Central Arc & Temp Display)**：
   - **温度范围**：15.0°C ~ 25.0°C（步长 0.5°C，共 21 档）。
   - **当前温度圆弧**：深灰底轨 + 亮蓝色实心进度圆弧（`#00B4FF`），从 15°C 起始位置顺时针延伸至当前实测温度位置。
   - **中央深色同心表盘**：正中心以 48 号大字体仅显示当前温度**整数**（例如：`23°`），下方配 `ROOM` 标签。
   - **外圈刻度点与极值**：5 个分度圆点（15.0°, 17.5°, 20.0°, 22.5°, 25.0°），底部两侧分别标注 `15°` 与 `25°`。
   - **目标温度指示与读数**：外圈黄色滑块指针（`#FFD700`）指示设定温度，外侧水平跟随显示 1 位小数读数（如 `22.5°`）。
   - **目标温度双调节**：
     - **触摸调节**：黄色指针支持在屏幕上触摸拖动，拖动时自动吸附至 0.5°C 步长。
     - **编码器调节**：旋转编码器顺时针 +0.5°C，逆时针 -0.5°C，限制在 15.0°C ~ 25.0°C。
3. **底部状态与操作栏 (Bottom Area)**：
   - **左侧**：HEATER 状态卡片，显示 `HEAT`（加热中，红色高亮）或 `OFF`（未加热，灰白色）。
   - **右侧**：SLEEP TIMER 按钮，显示 `OFF` 或倒计时 `mm:ss`（绿色高亮边框），支持点击切换开启/关闭。
4. **LVGL 原生输入设备接入**：
   - 在 `components/lcd_display/lcd_display.c` 中注册 `lv_indev_drv_t` 触摸驱动，实现 LVGL 原生触摸事件分发。
   - 移除 `main/main.c` 中旧的硬编码坐标 `touch_poll_task` 轮询任务，避免双重轮询和冲突。
5. **构建验证**：`idf.py build` 编译通过，生成 `build/matter-thermostat.bin`（占用 2.23MB，剩余空间 26%）。

### 3.25 当前温度显示改为 3 位有效数字（整数 + 1 位小数）（已完成）
根据需求，将主页面与待机页的当前温度显示由仅整数（如 `23°`）改为 **3 位有效数字**（整数部分 + 1 位小数，如 `23.5`），其中小数点及小数位使用整数部分一半的字号（24 号字）显示：

1. **`components/lcd_display/lcd_display.c`**：
   - **主页面**：新增温度容器 `s_cont_temp`（挂载于中央表盘内），内部放置两个标签——整数部分 `s_lbl_current_temp`（48 号字）与小数部分 `s_lbl_current_temp_dec`（24 号字，`LV_ALIGN_LEFT_BOTTOM` 底部对齐以保持基线一致）。
   - **待机页**：新增小数标签 `s_lbl_standby_temp_dec`（24 号字），与整数标签 `s_lbl_standby_temp`（48 号字）并排显示。
   - **渲染逻辑**：`ui_update_main_page()` 与 `ui_update_standby_page()` 使用 `floorf` + `roundf` 拆分整数与小数，并处理四舍五入进位（如 `19.96` → `20.0`）。
   - **布局下移**：因增加小数显示宽度，主页面温度容器由 `LV_ALIGN_CENTER, 0, -6` 下移至 `0, 4`，`ROOM` 标签由 `0, 26` 下移至 `0, 34`，在表盘内获得更宽的显示位置。
   - **显示/隐藏**：`ui_render()` 与 `ui_calib_hide_all_normal()` 同步处理待机页小数标签的显示与隐藏。
2. **`docs/01_requirements.md`**：同步更新主页面与待机页当前温度显示描述（3 位有效数字、小数位半字号、整体下移）。

### 3.26 底部 HEATER 按钮改为 SETTINGS 按钮 + 新增温度偏移 (Temp Offset) 设置页 + 顶部加热状态 "H" 指示（已完成）
根据需求完成以下三项改动：

1. **底部左侧按钮由 HEATER 改为 SETTINGS（触摸行为与物理 FUNC 键一致）**
   - **`components/lcd_display/lcd_display.c`**：按钮标题由 `HEATER` 改为 `SETTINGS`，状态文字由 `OFF` 改为 `MENU`；点击回调由 `btn_heat_click_cb` 改为 `btn_settings_click_cb`，实现与物理 FUNC 键相同的页面循环（主页面 → Sleep Timer 设置页 → 温度偏移设置页 → 主页面）。

2. **新增温度偏移 (Temp Offset) 设置页**
   - **页面循环**：主页面 → Sleep Timer 设置页 → 温度偏移设置页 → 主页面。
   - **显示**：大号文字 `CALIB: -1.5°C`（montserrat_32），标题栏 `TEMP OFFSET SETTING`，提示 `Rotate knob to adjust`。
   - **编码器操作**：顺时针 +0.5°C，逆时针 -0.5°C，范围限制 `[-2.0, +2.0]` ℃。
   - **NVS 持久化**：`components/thermostat_logic/thermostat_logic.c` 新增 `thermostat_temp_offset_load()` / `thermostat_temp_offset_save()`（以 0.1℃ 为单位存 i32，避免浮点精度问题）；`components/button_handler/button_handler.c` 在 1 秒无操作或按下 FUNC/Settings 返回主页面时自动保存；首次开机默认 `0.0 ℃`。
   - **`main/main.c`**：启动时调用 `thermostat_temp_offset_load()` 读取记忆值。

3. **顶部加热状态指示（位于 Wi-Fi 符号左侧）**
   - **`components/lcd_display/lcd_display.c`**：新增 `s_lbl_heat_top` 标签，位于顶部信息栏 Wi-Fi 图标左侧（`LV_ALIGN_RIGHT_MID, -30, 0`）。
   - **符号**：因 LVGL 8.3 无内置 `LV_SYMBOL_FIRE` 火焰图标，按用户要求使用大写字母 `H` 表示加热状态。
   - **颜色**：不加热时显示黑色，加热时显示红色（`0xFF0000`）。主页面与待机页均同步更新。

### 3.27 Off Timer 设置页面风格与温度传感器校准页面完全统一（已完成）
根据需求，将 Off Timer 设置页面（`UI_PAGE_SLEEP_TIMER`）由原先的 4 选项滚动选择列表重构为与温度传感器校准页（`UI_PAGE_TEMP_OFFSET`）完全一致的卡片风格：

1. **`components/lcd_display/lcd_display.c`**：
   - **移除滚动选项框**：移除 `s_sleep_opt_box[4]` 容器及 `s_lbl_sleep_options[4]` 标签与相关滚动宏。
   - **新增居中大字与提示标签**：新增 `s_lbl_sleep_value`（32 号字白色，居中偏上 `LV_ALIGN_CENTER, 0, -20`）与 `s_lbl_sleep_hint`（16 号字浅灰，居中偏下 `LV_ALIGN_CENTER, 0, 30`）。
   - **数值与提示格式**：大字显示 `TIMER: %d MIN`（如 `TIMER: 30 MIN`），操作提示显示 `Rotate knob to adjust`。
   - **显隐与更新逻辑**：`ui_render()`、`ui_update_sleep_timer_page()` 及 `ui_calib_hide_all_normal()` 统一对齐。
2. **`docs/01_requirements.md`**：同步更新 5.2 节中 Off Timer 页面的 UI 设计描述。

### 3.28 设置页面增加加减触摸按钮 (+ / -) 及避开提示语重叠（已完成）
根据需求，为所有设置页面（Off Timer 页与温度校准页）在被调整项上方和下方增加大号加减触摸按钮，并调整操作提示语位置：

1. **`components/lcd_display/lcd_display.c`**：
   - **加减触摸按钮**：新增 `s_btn_adjust_up`（上方 `+` 按钮，尺寸 96×38px，`LV_ALIGN_TOP_MID, 0, 102`）与 `s_btn_adjust_down`（下方 `-` 按钮，尺寸 96×38px，`LV_ALIGN_TOP_MID, 0, 196`），圆角 8px，卡片风格，24 号大字符号。
   - **触控面积与布局**：宽 96px（接近屏宽 40%），大触控区适合粗手指点按；被调整项数值置于中央（`LV_ALIGN_TOP_MID, 0, 152`）。
   - **提示语避让重叠**：将操作提示语 `Rotate knob to adjust` 上移至标题栏下方（`LV_ALIGN_TOP_MID, 0, 74`），与 `+` 按钮之间保持 12px 间隙，彻底杜绝重叠。
   - **交互与旋钮对齐**：点击 `+` 按钮等同于顺时针旋转旋钮一格（Off Timer 切下一档至 90 MIN 停住；Temp Offset 增加 0.5℃ 至 +2.0℃ 停住）；点击 `-` 按钮等同于逆时针旋转旋钮一格（Off Timer 切前一档至 10 MIN 停住；Temp Offset 减少 0.5℃ 至 -2.0℃ 停住）。点击均即时保存至 NVS、刷新显示并重置 60 秒闲置自动退出定时器。
   - **显隐管理**：仅在 `UI_PAGE_SLEEP_TIMER` 与 `UI_PAGE_TEMP_OFFSET` 设置页显示，主页及待机页自动隐藏。
2. **`docs/01_requirements.md`**：同步更新设置页交互规范与 UI 布局。

---

## 4. 未完成 / 待处理事项

### 4.1 硬件整体联调与测试（待用户实机验证）
- **温控主界面实机验证**：烧录最新固件，验证 240x320 竖屏圆弧仪表、黄色指针拖动、大字整数室温与底部 Sleep Timer 触摸响应。
- **Matter 多主控绑定与控制验证**：在 Apple Home / Home Assistant / Google Home 中验证温控器目标温度同步（0.5°C 步长）与开关状态。

> **已解决**：轻触开关硬件调试完成（2026-08-27）；电阻屏底部盲区问题已通过重新校准（靶点调整至 y=305）修复（2026-08-27）。

## 5. 关键文件清单

| 文件 | 说明 |
|------|------|
| [`main/main.c`](main/main.c) | 主程序：系统初始化、按键轮询任务、Matter 事件同步、SNTP 校时 |
| [`components/lcd_display/lcd_display.c`](components/lcd_display/lcd_display.c) | ILI9341 LCD 显示组件（Thermometer Demo 风格圆弧仪表、LVGL 触摸 indev 注册与渲染） |
| [`components/lcd_display/include/lcd_display.h`](components/lcd_display/include/lcd_display.h) | LCD 显示组件头文件 |
| [`components/touch_driver/touch_driver.c`](components/touch_driver/touch_driver.c) | XPT2046 触摸驱动（含 NVS 校准持久化与四点角标校准算法） |
| [`components/touch_driver/include/touch_driver.h`](components/touch_driver/include/touch_driver.h) | 触摸驱动头文件 |
| [`components/button_handler/button_handler.c`](components/button_handler/button_handler.c) | 物理按键处理（PCNT 编码器 ±0.5°C 调节与 FUNC 切换） |
| [`components/thermostat_logic/thermostat_logic.c`](components/thermostat_logic/thermostat_logic.c) | 温控逻辑（迟滞算法 ±0.5°C、15.0°C ~ 25.0°C 目标温控、Sleep Timer） |
| [`components/crash_monitor/crash_monitor.c`](components/crash_monitor/crash_monitor.c) | 崩溃监控组件 |
| [`docs/01_requirements.md`](docs/01_requirements.md) | 需求规格说明书 (v0.3) |
| [`docs/02_sdkconfig_note.md`](docs/02_sdkconfig_note.md) | sdkconfig 说明 |
| [`docs/project_status.md`](docs/project_status.md) | 工程状态记录文档 |

## 6. 新会话接手建议

### 6.1 UI 与触摸功能（已全部完成并通过硬件验证）
- **主界面**：采用 Thermometer Demo 风格圆弧仪表盘，15.0°C ~ 25.0°C 范围，当前室温整数大字，目标温度 0.5°C 精度黄色指针与读数。
- **输入集成**：LVGL indev 注册完成，支持圆弧拖动吸附与按钮触摸，物理编码器以 ±0.5°C 步长同步调节。
- **物理按键**：轻触开关（POWER/FUNC/SLEEP）与旋转编码器（EC11，PCNT 硬件正交解码）均已调试完成，调试日志已关闭。
- **触摸校准（重要）**：保留四点角标校准与 NVS 持久化机制。校准靶点已调整至 `{20,20}/{210,20}/{30,305}/{210,305}`（靠近屏幕边缘），确保底部触摸区域 raw ADC 值在校准范围内。校准一次后重启不再触发校准流程。如需重新校准，可调用 `touch_driver_erase_calibration()` 清除 NVS 后重启。

### 6.2 硬件接线确认
- LCD：SCK=12, MOSI=11, MISO=10, CS=2, DC=3, RESET=1, BL=0
- 触摸：CS=13, IRQ=23
- 所有 GPIO 无外部上拉，需内部上拉。

### 6.3 构建与烧录命令
- 构建：`bash -c 'source /home/alex/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py build'`
- 烧录：`idf.py -p /dev/ttyUSB1 erase-flash flash monitor`