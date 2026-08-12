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
| `docs/01_requirements.md` | 需求规格说明书 |
| `docs/02_sdkconfig_note.md` | sdkconfig 配置说明 |

---

## 6. 新会话接手建议

1. **LCD 驱动编译问题已解决**（采用方案 A 变体：`espressif/esp_lcd_ili9341` 组件，见 3.6 节），项目已编译通过。
2. **屏幕显示验证已通过**（见 3.8 节）：黑底白字、方向正确、TEMP 标签正确，烧录后显示正常。
3. 后续可继续实现：
   - **触摸屏（XPT2046）驱动**（当前仅实现显示，未实现触控输入）。
4. 构建命令：
   ```bash
   bash -c 'source /home/alex/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py build'
   ```