# END_nightly 移植计划：从机多目标（C6/C3/S3）+ 保留 wired/host（S3）

> 修订 v3 —— 依据用户澄清调整：
> 1. 强调词「!从机!」= 强调从机，而非「仅从机」。
> 2. END_nightly **保留全部三个角色**（wired / wireless host / wireless slave）；wired 与 host **仅兼容 S3**，不做 C6/C3；**slave 兼容 C6/C3/S3**。
> 3. 「我」= 用户。**agent 只写代码/文档，禁止编译、烧录、运行任何测试**；每个阶段结束由用户验证，通过后方可进入下一阶段。
> 4. 已复核 R1–R5「原样拷贝」：功能上成立，但有三处需注意（见 §2 检查结果）。
> 5. END_nightly 是已 git init 的 idf 项目；阶段 0 把源仓库文件导入其中，用其自身 git 记录（不保留源仓库 git 历史）。
> 6. tinyusb 条件范围保留 target in [esp32s2, esp32s3]。

---

## 0. 目标与范围

- **目标**：在 END_nightly（= 源仓库 ESP-NOW-DAP 的完整拷贝 + 改造）中，让 **wireless slave 角色** 可用 idf.py set-target esp32c6|c3|s3 构建对应芯片固件；wired 与 wireless host 维持现状，仅 S3。
- **范围**：
  - slave：C6 / C3 / S3（本次新增 C6/C3）。
  - wired、wireless host：仅 S3（不动，保持与源仓库一致）。
- **END_nightly 与源仓库关系**：END_nightly 是已 git init 的 idf 项目（当前为 create-project 默认脚手架、无提交）。阶段 0 把源仓库 ESP-NOW-DAP 的全部文件**导入**其中（源文件覆盖脚手架），用 END_nightly 自己的 git 记录、**不保留源仓库 git 历史**；在其上做**手术式改动**（不重写、不删角色）。
- **工作方式**：
  1. 每个阶段一个 commit，便于回滚。
  2. 阶段之间是硬 gate：前一阶段的「用户验证」全部通过才进入下一阶段。
  3. agent 在阶段内只产出代码/文档；用户验证报错时 agent 按日志修复，**同阶段内迭代**，不跨阶段。

---

## 1. 从机职责分析（对照源仓库）

| # | 职责 | 源仓库位置 | 关键函数 | 对 C3/C6 的影响 |
|---|---|---|---|---|
| R1 | ESP-NOW 无线链路 | components/wireless_link/ | wlink_init、wlink_recv_cb、wlink_send、wlink_beacon_cb、wlink_peer_ready | 零改动（esp_wifi/esp_now 在 C3/C6 都有） |
| R2 | CMSIS-DAP SWD 调试 | components/debug_probe/ + main/bridge_glue.c 从机侧 | DAP_ProcessCommand、debug_gpio.c/h（dedic_gpio 位打 SWCLK/SWDIO）、bridge_slave_swd_rx(_task)、bridge_slave_swd_tx_task | 零功能改动（C3/C6 有 dedic_gpio，RISC-V 用 CSR，API 一致） |
| R3 | 目标 UART 串口 | components/serial_handler/ | serial_handler_init、serial_handler_send_data、uart_event_task | 零改动（C3/C6 有 UART0/UART1） |
| R4 | 串口控制信号 + 复位时序 | main/bridge_glue.c | bridge_on_link_sercmd、bridge_apply_line_state、serial_handler_set_boot_reset_pins（含 esptool 10ms 补丁、TDI strapping） | 零改动（纯 GPIO/UART/esp_timer） |
| R5 | 状态 LED | components/util/include/led_io.h + bridge_glue.c 的 bridge_led_* | GPIO 翻转 | 零改动，仅引脚重配 |
| R6 | 从机不使用 USB | 源仓库 main/main.c、main/CMakeLists.txt、main/idf_component.yml | （USB 描述符、tinyusb、vendor bulk、CDC、MSC） | **按角色条件编译**——从机构建排除 USB；wired/host 保留 USB |

**结论**：从机六项职责中 R1–R5 组件功能上可原样搬来，唯一需要代码改造的是 R6——把 USB 栈从「无条件编译」改为「仅在 wired/host 角色编译」，使从机构建在 C3/C6 上不再依赖 tinyusb/usb。

---

## 2. 每项职责的移植方案

### R1 无线链路 —— 原样拷贝，零改动
- 拷贝整个 components/wireless_link/。
- 验证点（非改动）：C3/C6 单核上 esp_now 接收回调（Wi-Fi 任务）、beacon 定时器与 SWD 位打共用一核，有抖动——留给阶段 3/4 用「调低 adapter speed」验证。

### R2 SWD 调试 —— 原样拷贝，零功能改动（1 处可选清理）
- 拷贝整个 components/debug_probe/（含 DAP/、noflash.lf、Kconfig、CMakeLists）。
- 已核实：C3/C6 的 dedic_gpio_cpu_ll 用 RISC-V CSR（GPIO_OUT_USER / GPIO_IN_USER）实现，read_in / write_mask 签名与 S3 一致，位打可用。
- **检查结果（需注意但非必须改）**：compiler.h 硬编码 include "xtensa_gcc.h"，其内容仅为 GCC 宏 + nop（nop 在 RISC-V 通用），能编译通过，但命名误导；建议收尾阶段加 riscv 分支或改名（阶段 5 可选）。
- esp_usb_jtag.c 由 CONFIG_DEBUG_PROBE_IFACE_JTAG 保护，SWD 下编译为空；保留它，因为 wired 角色的 JTAG 接口仍要用。
- DAP_config.h 的 CPU_CLOCK 随 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 自动适配（C3/C6 = 160MHz），无需手改。

### R3 UART —— 原样拷贝，零改动（1 处构建验证）
- 拷贝整个 components/serial_handler/（含 idf_component.yml 拉 esp-serial-flasher）。
- **检查结果（构建期验证，阶段 2 内迭代）**：esp-serial-flasher 无 target 限制、esp32_port.c 通用；但它在 IDF6 依赖 esp_driver_uart。serial_handler/CMakeLists.txt 已按 >=5.3 分支加入 esp_driver_uart，若报缺组件则在组件里补依赖。

### R4 控制信号/复位 —— 逻辑原样搬入 bridge_glue，零改动
- bridge_apply_line_state、bridge_on_link_sercmd、bridge_state_change_timer_cb、serial_handler_set_boot_reset_pins 逐字保留。

### R5 LED —— 原样拷贝，仅引脚重配
- 拷贝 components/util/（含 led_io.h），led_io.h 只用 CONFIG_BRIDGE_GPIO_LED*，纯 GPIO。
- END_nightly 保留完整 main/Kconfig.projbuild（含 LED 及 USB 相关项），LED 配置项照旧。

### R6 从机无 USB —— 把 USB 栈改为「按角色条件编译」（唯一代码改造）
- 目标：slave 构建不链接 tinyusb / espressif-usb / vendor / CDC / MSC；wired 与 host 构建保持不变（仍 S3）。
- 改动点（详见阶段 1）：
  1. main/CMakeLists.txt：eub_vendord.c / serial_bridge.c / usb_phy.c 及 tinyusb/usb 依赖，仅在「非 slave」时加入；msc.c 仍仅 wired。
  2. main/main.c：用 #if !CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE 包住全部 USB 代码（tusb.h 等 include、USB 描述符、tud_* 回调、init_serial_no、tusb_device_task、start_usb_device）。
  3. main/idf_component.yml：espressif/tinyusb 改为按 target 条件（if: target in [esp32s2, esp32s3]）。
  4. main/Kconfig.projbuild：wired / wireless host 增加 depends on SOC_USB_OTG_SUPPORTED；角色默认改为「有 OTG 则 wired，否则 slave」。

---

## 3. 分阶段可执行计划

> 每个阶段的「用户验证」就是 gate；未通过则停在该阶段，由 agent 修复后重验。

### 阶段 0 —— 仓库基线（导入源仓库文件到已 git init 的 END_nightly）

**改动**
1. 把 ESP-NOW-DAP 的全部文件导入已 git init 的 END_nightly：拷贝源仓库的 components/、main/、docs/、test/、sdkconfig.defaults*、根 CMakeLists.txt、README 等，覆盖 idf 默认脚手架（根 CMakeLists 用源仓库的；project 名可保持 bridge 或改 END_nightly，纯命名）。
2. 保留 END_nightly 已有的 .git（不引入源仓库 git 历史）。
3. 不做任何代码改动。
4. 首次 commit（nightly-p0，成为该仓库首个提交）。

**交付物**：END_nightly 与源仓库内容一致。

**用户验证**
- S3 三角色照旧构建：wired（默认）、wireless host、wireless slave（sdkconfig.defaults.wireless_host/_slave）各 build 通过。
- 现有硬件行为照旧（至少：S3 host + S3 slave 配对正常）。

**通过判据**：三角色构建通过 + 无线配对照旧（证明拷贝忠实）。

### 阶段 1 —— USB 栈按角色条件编译（核心重构）

**改动**
1. main/CMakeLists.txt：srcs 与 deps 按 §2 R6 条件化；idf_component_get_property(tusb_lib ...) 包进非 slave 分支。
2. main/main.c：#if !CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE 包 USB 代码。
3. main/idf_component.yml：tinyusb 加 rules（target in [esp32s2, esp32s3]）。
4. main/Kconfig.projbuild：wired/host 加 depends on SOC_USB_OTG_SUPPORTED；角色默认改「有 OTG 则 wired，否则 slave」。
5. commit（nightly-p1）。

**交付物**：slave 构建不再依赖 USB；wired/host 不变。

**用户验证**
1. S3 wired 构建通过，USB 设备功能照旧（CDC 串口 + SWD/CMSIS-DAP + MSC）。
2. S3 wireless host 构建通过，与从机配对照旧。
3. S3 wireless slave 构建通过，且固件体积明显小于 wired/host（无 USB 栈）。

**通过判据**：S3 三角色都构建通过 + 行为照旧 + slave 体积变小。

### 阶段 2 —— C3/C6 目标接入（首次跨芯片构建）

**改动**
1. 新增 sdkconfig.defaults.esp32c6、sdkconfig.defaults.esp32c3（见 §4 引脚表；单核不写 CPU1 WDT 符号）。
2. 确认 C3/C6 从机走 Kconfig 自动角色（slave），不套用 sdkconfig.defaults.wireless_slave（该文件含单核不存在的 CPU1 符号）。
3. 若用户构建报 target 相关错误，agent 按日志修复（本阶段内迭代）。**预期坑**：esp-serial-flasher 的 esp_driver_uart 依赖、compiler.h 命名、单核 WDT 符号。
4. commit（nightly-p2）。

**交付物**：C6、C3 从机固件各可构建。

**用户验证**
- idf.py -B build_c6 set-target esp32c6 && idf.py -B build_c6 build 通过（role 自动 = slave）。
- idf.py -B build_c3 set-target esp32c3 && idf.py -B build_c3 build 通过。

**通过判据**：两个 target 都构建通过。

### 阶段 3 —— C6 从机硬件冒烟

**改动**：预期无代码改动；根据现象调 CONFIG_WLINK_SWD_TIMEOUT_MS / CONFIG_WLINK_SWD_RETRIES / 建议 adapter speed。commit 留 nightly-p3。

**用户验证清单（全过才通过）**
1. 配对：S3 host + C6 从机，日志 Peer discovered。
2. 串口：PC→host→C6→目标 UART 透传（回环 / 目标回显 / esptool chip-id）。
3. SWD：openocd -f interface/cmsis-dap.cfg -f target/<目标>.cfg -c 'adapter speed 500'（先 500，再试 1000）连上目标。
4. 复位/烧录：esptool 经隧道对目标 chip-id + write-flash（验证 DTR/RTS 复位时序在 C6 上正常）。
5. 串口 + SWD 并行不互相阻塞。

**通过判据**：1–5 全过。

### 阶段 4 —— C3 从机硬件冒烟

**改动**：同阶段 3；额外关注 RAM 余量（C3 400KB，打印 esp_get_free_heap_size()）。commit 留 nightly-p4。

**用户验证清单**：与阶段 3 的 5 项一致（芯片换 C3）。

**通过判据**：5 项全过，且 free heap 无明显告急。

### 阶段 5 —— S3 三角色回归 + 文档收尾

**改动**
1. S3 wired / host / slave 回归（确认阶段 2–4 的共享代码改动未破坏 S3）。
2. 更新 README / docs：C3/C6 从机构建说明、引脚表、限制（adapter speed 建议、不加密提醒）；说明 wired/host 仅 S3。
3. 清理：删除构建产物/临时文件，确认仓库干净；可选做 compiler.h 的 riscv 命名清理。
4. 整理提交历史，打 nightly 标签。
5. commit（nightly-p5）。

**用户验证**
- S3 三角色按阶段 0/1 流程回归通过。
- 通读文档，确认引脚表/命令/范围无误。
- git status 干净、git log 清晰。

**通过判据**：回归通过 + 文档就绪 → 完成。

---

## 4. 推荐引脚表（从机默认值，可按实际板子调）

| 功能 | Kconfig 符号 | S3（沿用源仓库） | C6 | C3 |
|---|---|---|---|---|
| SWCLK | CONFIG_DEBUG_PROBE_GPIO_TCK | 17 | 10 | 10 |
| SWDIO | CONFIG_DEBUG_PROBE_GPIO_TMS | 18 | 11 | 11 |
| 目标 TXD | CONFIG_SERIAL_HANDLER_GPIO_TXD | 5 | 4 | 4 |
| 目标 RXD | CONFIG_SERIAL_HANDLER_GPIO_RXD | 6 | 5 | 5 |
| BOOT | CONFIG_SERIAL_HANDLER_GPIO_BOOT | 4 | 6 | 6 |
| RST | CONFIG_SERIAL_HANDLER_GPIO_RST | 7 | 7 | 7 |
| LED1 | CONFIG_BRIDGE_GPIO_LED1 | 12 | 18 | 12 |
| LED2 | CONFIG_BRIDGE_GPIO_LED2 | 13 | 19 | 13 |
| LED3 | CONFIG_BRIDGE_GPIO_LED3 | 14 | 20 | 14 |

- S3：完全沿用源仓库引脚，既有接线不用动。
- C6：避开 strapping（8/9/15）与 USB Serial/JTAG（12/13）。
- C3：避开 strapping（2/8/9）。
- TDI/TDO（JTAG 专用）从机不用，留默认 3/9。

---

## 5. 风险与回滚

| 风险 | 影响 | 缓解 |
|---|---|---|
| 阶段 1 条件编译引入回归 | S3 wired/host 行为异常 | 改动只加 #if 分支、不改逻辑；阶段 1 即三角色回归 |
| 单核 SWD 位打抖动 | C3/C6 上 SWD 不稳 | adapter speed 调低（500 起）；必要时位打函数落 IRAM / 提 swd 任务优先级 |
| C3 RAM 偏紧 | 运行期堆告急 | 阶段 4 打印 free heap；若紧则减小 ringbuffer / 栈 |
| esp-serial-flasher IDF6 依赖 | C3/C6 编译报错 | 阶段 2 内补依赖 |
| C6 引脚冲突 | 功能异常 | 用 §4 映射避开 8/9/12/13/15 |
| Kconfig 目标感知默认引入副作用 | S3 上角色默认变化 | 用 SOC_USB_OTG_SUPPORTED 门控：S3 默认仍 wired，行为不变 |

- **回滚策略**：每阶段一个 commit；失败回退到上一阶段 commit。
- **无线上游兼容**：host（S3）与 slave（C6/C3/S3）只靠 role + CONFIG_WLINK_CHANNEL 配对，芯片型号无关，无需同步改 host。

---

## 附：改动清单（END_nightly = 导入源仓库全部文件 + 以下手术式改动）

| 文件 | 改动 |
|---|---|
| main/CMakeLists.txt | USB srcs/deps 按「非 slave」条件化 |
| main/main.c | #if !CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE 包 USB 代码 |
| main/idf_component.yml | tinyusb 加 rules（target in [esp32s2, esp32s3]） |
| main/Kconfig.projbuild | wired/host depends on SOC_USB_OTG_SUPPORTED；角色默认目标感知 |
| sdkconfig.defaults.esp32c3 / esp32c6 | 新增（引脚/flash/CPU/SWD） |
| sdkconfig.defaults.wireless_slave | 仅用于 S3 从机（含 CPU1 WDT 符号，不用于 C3/C6） |
| 其余全部文件（components、DAP、msc、docs、test 等） | 原样保留 |