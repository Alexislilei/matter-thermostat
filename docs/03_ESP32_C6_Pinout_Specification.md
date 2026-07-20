# ESP32-C6 主控板硬件 GPIO 管脚分配说明书 (v1.1)

## 1. 文档概述
本文档为 ESP32-C6 主控板的 GPIO 管脚功能定义及硬件接口说明文件。作为系统的基础硬件抽象规范，用于指导硬件底层驱动开发及 AI 编程助手（如 Cursor、GitHub Copilot 等）的代码生成与协议实现。

---

## 2. 硬件引脚映射表 (GPIO Mapping Table)

| GPIO 引脚 | 信号名称 | 信号方向 (Pin Dir) | 对应外设 / 功能模块 | 说明与逻辑动作 |
| :--- | :--- | :--- | :--- | :--- |
| **GPIO4** | `DHT11_DATA` | 双向 (In/Out) | DHT11 温湿度传感器 | 单总线 (One-Wire) 数据通信接口 |
| **GPIO5** | `HS0038` | 输入 (Input) | HS0038 红外接收头 | 载波接收/红外遥控信号输入 (NEC格式) |
| **GPIO6** | `MRX_STX` | 输入 (Input) | 显示控制板 UART1 | ESP32-C6 侧的 **RX** 引脚，连接显示板的 TX |
| **GPIO7** | `MTX_SRX` | 输出 (Output) | 显示控制板 UART1 | ESP32-C6 侧的 **TX** 引脚，连接显示板的 RX |
| **GPIO15** | `DISP_RST` | 输出 (Output) | 显示控制板复位控制 | C6 作为 Master，输出低/高电平控制显示板 Reset |
| **GPIO21** | `KEY_TEMP_PLUS` | 输入 (Input) | 按键 - 加值 (TEMP+) | **Temp页**：设定温度 +1℃；**设置页**：参数增加 |
| **GPIO20** | `KEY_TEMP_MINUS` | 输入 (Input) | 按键 - 减值 (TEMP-) | **Temp页**：设定温度 -1℃；**设置页**：参数减少 |
| **GPIO19** | `KEY_FUNC` | 输入 (Input) | 按键 - 功能 (FUNC) | 切换功能页面（如：设定温度 → 设定定时关机） |
| **GPIO18** | `KEY_BACK` | 输入 (Input) | 按键 - 返回 (BACK) | 返回上级页面 / 取消当前操作 |
| **GPIO22** | `HEATER` | 输出 (Output) | 加热器 Relay / MOS 控制 | 继电器或 MOS 管控制开关（高电平开启/低电平关闭） |

---

## 3. 模块逻辑与系统行为说明

### 3.1 显示板通讯与复位 (UART1 & DISP_RST)
* **串口配对**：ESP32-C6 使用硬件 UART1 接口（GPIO6 为 RX，GPIO7 为 TX）与外部显示控制板进行异步串行通信。
* **复位逻辑**：`DISP_RST` (GPIO15) 专用于对显示控制板实施硬件复位控制。

### 3.2 交互按键矩阵 (Keypad Logic)
* 所有按键在系统软件逻辑中需具备防抖（Debounce）处理。
* **复用逻辑定义**：
  * **主界面（温度设定）**：`KEY_TEMP_PLUS`（增温）、`KEY_TEMP_MINUS`（降温）。
  * **配置界面（如时间设置等）**：`KEY_TEMP_PLUS`（数值+1）、`KEY_TEMP_MINUS`（数值-1）。
  * **系统导航**：`KEY_FUNC` 控制菜单循环切页，`KEY_BACK` 用于退出或返回。

### 3.3 执行器控制 (HEATER)
* `HEATER` 修改为使用 **GPIO22** 进行开关量控制（避免使用带有 Boot Strapping 功能的 GPIO9）。
* 外部电路建议增加 10kΩ 下拉电阻，确保上电高阻态时不发生误触发。

---

## 4. 管脚电气特征 (Electrical Characteristics)
*(待补充：请在此处填入各个 Pin 的上拉/下拉配置、驱动电流、高低电平有效性等参数)*
