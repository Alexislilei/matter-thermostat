---

# ESP32-C6 (Matter / BLE) SDK 编译配置注意事项

## 1. 目标芯片与基础算法

```ini
CONFIG_IDF_TARGET="esp32c6"
CONFIG_MBEDTLS_HKDF_C=y

```

* **配置说明**：
* 指定目标芯片为 `ESP32-C6`。
* 启用 `MbedTLS HKDF` 密钥导出函数（Matter 协议握手/安全认证必需依赖）。



---

## 2. Flash 容量与自定义分区表 (Partition Table)

```ini
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y

```

* **配置说明**：
* 启用自定义分区表，指定分区表配置文件为 `partitions.csv`。
* 设置 Flash 容量为 **8MB**（支持 factory 分区分配至 3MB，以容纳 Matter 协议栈及 BLE 组件）。



---

## 3. BLE & NimBLE 配网支持 (Matter Commissioning)

```ini
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=5120
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3
CONFIG_BT_CTRL_MODE_EFF=1

```

* **配置说明**：
* 开启蓝牙功能并采用 **NimBLE** 蓝牙协议栈（轻量化，适宜 Matter BLE 配网）。
* 设置 NimBLE Host 任务栈大小为 **5120** 字节，防止栈溢出。
* 最大蓝牙连接数设置为 **3**。
* 启用高效控制器模式（`CONFIG_BT_CTRL_MODE_EFF=1`）。



---

## 4. 关键踩坑点：避免 IRAM 内存溢出 (`iram0_0_seg overflow`)

```ini
# 禁用 Wi-Fi 与 NimBLE 的 IRAM 优化，将部分代码移入 Flash (Flash Read-Only / XIP)
CONFIG_ESP_WIFI_IRAM_OPT=n
CONFIG_ESP_WIFI_RX_IRAM_OPT=n
CONFIG_BT_NIMBLE_HOST_IRAM_OPT=n

```

* **踩坑记录与解决方案**：
* **问题现象**：在同时启用了 Matter、Wi-Fi 与 BLE (NimBLE) 后，系统在编译链接阶段会触发 `iram0_0_seg overflow`（IRAM 静态内存溢出）。
* **原因分析**：默认情况下 ESP-IDF 会将 Wi-Fi 接收/发送逻辑及 NimBLE 处理函数装载进高速 IRAM 中，导致 512KB SRAM 中的 IRAM 区域空间耗尽。
* **解决办法**：关闭 Wi-Fi 及 NimBLE 的 IRAM 优化选项（设置为 `=n`），强制将其代码映射至 External Flash 执行，释放关键的 IRAM 空间供系统中断与核心调度使用。



---

---

## 5. 崩溃诊断配置 (Crash Diagnostic)

```ini
# 启用 Core Dump 到 Flash：崩溃时保存精确调用栈，重启后用 idf.py coredump-info 解码
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y
# 崩溃后延迟 5 秒再重启，确保串口日志完整输出
CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=5
```

* **配置说明**：
* `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`：崩溃时将调用栈（core dump）写入 Flash 的 `coredump` 分区（见 `partitions.csv`），重启后可用 `idf.py coredump-info` 解码定位崩溃位置。
* `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y`：使用 ELF 格式（含符号信息，便于解码）。
* `CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=5`：崩溃后延迟 5 秒再重启，避免串口日志因立即重启而丢失。
* **配套组件**：`components/crash_monitor/` 在启动时读取 `esp_reset_reason()` 并打印崩溃横幅（复位原因 + 崩溃时运行时长），配合 Core Dump 使用。
* **NVS 历史记录**：`crash_monitor` 将每次启动的复位原因持久化到 NVS（命名空间 `crash_mon`，保留最近 8 次），即使拔插 USB 重连串口也能追溯真实崩溃原因。启动日志会打印 `Boot History`。

> **注意**：修改 `sdkconfig.defaults` 后，若 `sdkconfig` 中已存在同名配置项，需同步修改 `sdkconfig`（或删除 `sdkconfig` 重新生成），否则新默认值不会生效。

---

### 💡 提示（给 AI assistant / 开发者）：

> 当在新对话中生成或修改 `sdkconfig` / `sdkconfig.defaults` 时，**请务必保留并包含上述所有配置项目**，特别是禁用 Wi-Fi / NimBLE 的 IRAM 优化部分，否则会导致项目编译失败并提示 IRAM 溢出。