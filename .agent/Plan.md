# END_nightly 移植计划：从机多目标（C6/C3/S3）+ 保留 wired/host（S3）

> 位置：`.agent/Plan.md`（与 `Agent.md`、`Todo.md` 同在 `.agent/`；文中提及的这三个文件名均指该目录下的文件）。
> 修订 v6 —— 新增第 9 条；按实际代码校正阶段状态、风险表与阶段 2 细则，并清理仓库（用户 m00518）。此前 v5 —— 新增第 8 条与「阶段 1.1」（从机 SWD 首请求丢失修复，用户 m00318/m00346）；v4 —— 依据用户澄清调整：
> 1. 强调词「!从机!」= 强调从机，而非「仅从机」。
> 2. END_nightly **保留全部三个角色**（wired / wireless host / wireless slave）；wired 与 host **仅兼容 S3**，不做 C6/C3；**slave 兼容 C6/C3/S3**。
> 3. 「我」= 用户。**agent 只写代码/文档，禁止编译、烧录、运行任何测试**；每个阶段结束由用户验证，通过后方可进入下一阶段。
> 4. 已复核 R1–R5「原样拷贝」：功能上成立，但有三处需注意（见 §2 检查结果）。
> 5. END_nightly 是已 git init 的 idf 项目；阶段 0 把源仓库文件导入其中，用其自身 git 记录（不保留源仓库 git 历史）。
> 6. tinyusb 条件范围保留 target in [esp32s2, esp32s3]。
> 7. （m00266）新增调试宏 `CONFIG_WLINK_DEBUG_LINK_TEST`：ESP-NOW 配对后，从机每秒向主机发送 `connected N`（N 持续递增），用于定位链路问题；默认关闭，仅调试时启用。
> 8. （m00318）**偏离 §1 R2「bridge_glue 从机侧零改动」**：用户报告「偶见第一次 SWD 烧录失败」，定位到源仓库遗留 bug（从机 `s_swd_pending_seq` 静态初值 0 与主机首个请求 seq 0 相撞，被当作重复请求丢弃，三次重传仍用 seq 0 故首条 DAP 命令必丢）。用户选定方案 B（请求序号 FIFO 关联响应），作为**阶段 1.1 补丁**并入阶段 1 的 gate，改动见下方「阶段 1.1」。
> 9. （m00518）按实现现状校正本计划：阶段 0/1/1.1 已通过用户验证（`d271245` / `05ab033` / `d8d3afc`）；补充「debug_probe 全量进 IRAM」与「S3 从机仍编译 tinyusb」两条风险；细化阶段 2 的 defaults 清单与体积检查；仓库清理——删除 `test/` 与 `CHANGELOG.md`，`docs/` 转为正常跟踪，`.clangd`/`.cz.toml`/`.gitlab-ci.yml` 保留在盘上但不追踪。

---

## 0. 目标与范围

- **目标**：在 END_nightly（= 源仓库 ESP-NOW-DAP 的完整拷贝 + 改造）中，让 **wireless slave 角色** 可用 idf.py set-target esp32c6|c3|s3 构建对应芯片固件；wired 与 wireless host 维持现状，仅 S3。
- **范围**：
  - slave：C6 / C3 / S3（本次新增 C6/C3）。
  - wired、wireless host：仅 S3（不动，保持与源仓库一致）。
- **END_nightly 与源仓库关系**：END_nightly 是已 git init 的 idf 项目（创建时是 create-project 默认脚手架；阶段 0 已用源仓库文件覆盖并提交 `d271245`）。在 END_nightly 自己的 git 上记录、**不保留源仓库 git 历史**；在其上做**手术式改动**（不重写、不删角色）。
- **工作方式**：
  1. 每个阶段一个 commit，便于回滚。
  2. 阶段之间是硬 gate：前一阶段的「用户验证」全部通过才进入下一阶段。
  3. agent 在阶段内只产出代码/文档；用户验证报错时 agent 按日志修复，**同阶段内迭代**，不跨阶段。

---

## 1. 从机职责分析（对照源仓库）

| # | 职责 | 源仓库位置 | 关键函数 | 对 C3/C6 的影响 |
|---|---|---|---|---|
| R1 | ESP-NOW 无线链路 | components/wireless_link/ | wlink_init、wlink_recv_cb、wlink_send、wlink_beacon_cb、wlink_peer_ready | 功能零改动（esp_wifi/esp_now 在 C3/C6 都有）；**新增**调试自检宏 `CONFIG_WLINK_DEBUG_LINK_TEST`（阶段 1，默认关闭） |
| R2 | CMSIS-DAP SWD 调试 | components/debug_probe/ + main/bridge_glue.c 从机侧 | DAP_ProcessCommand、debug_gpio.c/h（dedic_gpio 位打 SWCLK/SWDIO）、bridge_slave_swd_rx(_task)、bridge_slave_swd_tx_task | 零功能改动（C3/C6 有 dedic_gpio，RISC-V 用 CSR，API 一致）；**例外**：从机 SWD 排队/序号关联的遗留 bug 修复（阶段 1.1，见 §3） |
| R3 | 目标 UART 串口 | components/serial_handler/ | serial_handler_init、serial_handler_send_data、uart_event_task | 零改动（C3/C6 有 UART0/UART1） |
| R4 | 串口控制信号 + 复位时序 | main/bridge_glue.c | bridge_on_link_sercmd、bridge_apply_line_state、serial_handler_set_boot_reset_pins（含 esptool 10ms 补丁、TDI strapping） | 零改动（纯 GPIO/UART/esp_timer） |
| R5 | 状态 LED | components/util/include/led_io.h + bridge_glue.c 的 bridge_led_* | GPIO 翻转 | 零改动，仅引脚重配 |
| R6 | 从机不使用 USB | 源仓库 main/main.c、main/CMakeLists.txt、main/idf_component.yml | （USB 描述符、tinyusb、vendor bulk、CDC、MSC） | **按角色条件编译**——从机构建排除 USB；wired/host 保留 USB |

**结论**：从机六项职责中 R1–R5 的组件功能可原样搬来，不需要为 C3/C6 改写。代码改造分三类：①**R6**「USB 栈按角色条件编译」（阶段 1，唯一的移植性改造）；②**R1** 新增的调试自检宏（阶段 1，默认关闭，不改变功能）；③**R2** 的从机 SWD 请求序号 FIFO（阶段 1.1，修源仓库遗留 bug）。

---

## 2. 每项职责的移植方案

### R1 无线链路 —— 原样拷贝 + 新增调试自检宏
- 拷贝整个 components/wireless_link/。
- 新增（阶段 1）：`CONFIG_WLINK_DEBUG_LINK_TEST`（默认 `n`），配对后从机每 1 s 发一条 `connected N\r\n`（N 递增）走 `WLINK_MSG_SERIAL`，用于定位链路问题；host 侧分支只打印一句提示。用独立任务而非 `esp_timer`，因为 `wlink_send` 要拿发送互斥锁，定时器回调不能阻塞。
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
  1. main/CMakeLists.txt：eub_vendord.c / serial_bridge.c / usb_phy.c 及 tinyusb/usb 依赖，仅在「非 slave」时加入；`noflash.lf`（把 debug_probe 放 IRAM 的 linker fragment）也只在非 slave 时加入（用 `set(ldfragments)` 条件化，空值安全）；msc.c 仍仅 wired。
  2. main/main.c：用 #if !CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE 包住全部 USB 代码（tusb.h 等 include、USB 描述符、tud_* 回调、init_serial_no、tusb_device_task、start_usb_device）。
  3. main/idf_component.yml：espressif/tinyusb 改为按 target 条件（if: target in [esp32s2, esp32s3]）。
  4. main/Kconfig.projbuild：wired / wireless host 增加 depends on SOC_USB_OTG_SUPPORTED；角色默认改为「有 OTG 则 wired，否则 slave」。
  5. tinyusb 组件的 `tusb_config.h` include 路径补全改为「组件是否在 BUILD_COMPONENTS」判断，与角色解耦——S3 从机仍会编译 tinyusb，必须能编过（教训见 Agent.md E4）。

---

## 3. 分阶段可执行计划

> 每个阶段的「用户验证」就是 gate；未通过则停在该阶段，由 agent 修复后重验。

### 阶段 0 —— 仓库基线（导入源仓库文件到已 git init 的 END_nightly）

**状态**：已通过（commit `d271245`，2026-10-31，用户验收「完美复刻旧仓库功能」）。

**改动**
1. 把 ESP-NOW-DAP 的全部文件导入已 git init 的 END_nightly：拷贝源仓库的 components/、main/、docs/、sdkconfig.defaults*、根 CMakeLists.txt、README 等，覆盖 idf 默认脚手架（根 CMakeLists 用源仓库的；project 名保持 bridge）。
2. 保留 END_nightly 已有的 .git（不引入源仓库 git 历史）。
3. 不做任何代码改动。
4. 首次 commit（nightly-p0，成为该仓库首个提交）。

**阶段 0 后的仓库清理**（用户 m00518）：删除 `test/` 与 `CHANGELOG.md`（确认无用，README 中的引用同步去掉）；`docs/` 不再被 `.gitignore` 忽略、转为正常跟踪；`.clangd`、`.cz.toml`、`.gitlab-ci.yml` 与项目功能无关，保留在盘上但**不追踪**。

**交付物**：END_nightly 与源仓库内容一致。

**用户验证**
- S3 三角色照旧构建：wired（默认）、wireless host、wireless slave（sdkconfig.defaults.wireless_host/_slave）各 build 通过。
- 现有硬件行为照旧（至少：S3 host + S3 slave 配对正常）。

**通过判据**：三角色构建通过 + 无线配对照旧（证明拷贝忠实）。

### 阶段 1 —— USB 栈按角色条件编译（核心重构）

**状态**：已通过（commit `05ab033`；阶段 1.1 的修复随同一 gate 一并验证）。

**改动**
1. main/CMakeLists.txt：srcs 与 deps 按 §2 R6 条件化；`noflash.lf` 用 `set(ldfragments)` 条件化（仅非 slave 加）；tinyusb 的 `public_include`（`tusb_config.h`）补全按 `BUILD_COMPONENTS` 判断，与角色解耦（教训见 Agent.md E4）。
2. main/main.c：#if !CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE 包 USB 代码。
3. main/idf_component.yml：tinyusb 加 rules（target in [esp32s2, esp32s3]）。
4. main/Kconfig.projbuild：wired/host 加 depends on SOC_USB_OTG_SUPPORTED；角色默认改「有 OTG 则 wired，否则 slave」。
5. components/wireless_link：新增调试宏 `CONFIG_WLINK_DEBUG_LINK_TEST` + 从机每秒发送 `connected N` 的自检任务（走 WLINK_MSG_SERIAL，见 §阶段1 说明）。
6. commit（nightly-p1）。

**交付物**：slave 构建不再**链接** USB 栈；wired/host 不变。注意：S3 从机仍会编译 tinyusb 组件（依赖按 target 解析，与角色无关），C3/C6 上 tinyusb 完全不进构建图。

**调试宏启用方式**（默认关闭）：`idf.py -B build_slave ... menuconfig` → `Wireless Link Configuration` → *Debug: send a link test line from the slave every second*；或在自己的 defaults 文件里加一行 `CONFIG_WLINK_DEBUG_LINK_TEST=y`。

**用户验证**
1. S3 wired 构建通过，USB 设备功能照旧（CDC 串口 + SWD/CMSIS-DAP + MSC）。
2. S3 wireless host 构建通过，与从机配对照旧。
3. S3 wireless slave 构建通过，且固件体积明显小于 wired/host（无 USB 栈）。

**通过判据**：S3 三角色都构建通过 + 行为照旧 + slave 体积变小。

### 阶段 1.1 —— 修复从机 SWD 首请求被误判为重复而丢弃（并入阶段 1 gate）

**状态**：已通过（commit `d8d3afc`，2026-10-31，用户回「阶段1和1.1已通过验证」）。

**背景（源仓库遗留 bug，用户 m00318 报告）**：主机 `wlink_next_seq()` 从 0 开始（components/wireless_link/wireless_link.c:36 的 `static uint8_t s_seq`，:333-335 `return s_seq++`），从机 `s_swd_pending_seq` 静态初值也是 0，接收函数用 `hdr->seq == s_swd_pending_seq` 判断「重复请求」，于是每个新鲜上电会话的首条 DAP 命令被丢弃；主机三次重传用同一个 seq，全部被丢，约 3×250ms 后放弃，USB 侧超时。表现为「偶见第一次烧录失败、重跑就好」（只有 adapter 初始化阶段的 DAP_Info/DAP_Connect 失败才会终止整次烧录）。

**方案 B（用户 m00346 选定）：请求序号 FIFO 关联响应**

**改动（全部在 main/bridge_glue.c）**
1. 新增宏 `BRIDGE_SWD_REQ_QUEUE_DEPTH`(8)、`BRIDGE_SWD_SEQ_FIFO_DEPTH`(32)，紧邻 `BRIDGE_SWD_PKT_SIZE`。
2. 删除 `s_swd_pending_seq`；新增 `s_swd_seq_fifo[]` + head/count + `portMUX_TYPE s_swd_seq_mux`，以及四个持锁辅助 `bridge_swd_seq_pending/push/pop/drop_last`。
3. `bridge_slave_swd_rx`：缓存命中时先在锁内拷到栈上再出锁重发（避免撕裂）；「接受请求」与「登记序号」在同一临界区内完成（`pending` 查重 → `push` → `xQueueSend`，入队失败回滚）；日志一律在锁外。
4. `bridge_slave_swd_rx_task`：`debug_probe_process_data` 失败时弹出最老序号（DAP 引擎不会再应答它），防止后续响应串号。
5. `bridge_slave_swd_tx_task`：不再读「当前 pending」，改为弹出 FIFO 头作为该响应的 seq，并在同一临界区写缓存 → 修掉「主机放弃 N 后发 N+1、N 的迟到响应被标成 N+1」的静默错值。
6. 队列创建从 `bridge_wireless_slave_start` 移到 `bridge_uplink_init` 的从机分支、**先建队列再注册 `bridge_slave_swd_rx`**（原先回调先注册、队列后建，配对极快时首个请求会撞上空队列被丢）。

**用户验证（在阶段 1 的 gate 里一并做）**
1. 两端同时冷启动，第一条命令就是 SWD 烧录，连续 20 次首条即成功（修复前会稳定复现首条失败）。
2. 打开 `CONFIG_WLINK_SWD_VERBOSE_LOG=y` 观察：修复前首条应为 `already in flight, ignored` 且无 `queued`；修复后应为 `queued` → `response seq 0 sent`，且无约 750ms 停顿。
3. 回归：从机中途复位后继续烧录；两条命令之间给从机断电再上电；正常整片烧录成功。

**通过判据**：1–3 全过。

### 阶段 2 —— C3/C6 目标接入（首次跨芯片构建）

**状态**：待开始（前置：阶段 1 / 1.1 已通过）。

**改动**
1. 新增 `sdkconfig.defaults.esp32c6`、`sdkconfig.defaults.esp32c3`，**只写与 target 相关、与角色无关**的项：
   | 符号 | C6 | C3 | 说明 |
   |---|---|---|---|
   | `CONFIG_DEBUG_PROBE_IFACE_SWD` | `y` | `y` | 明确选 SWD 接口（不是 JTAG / USB-JTAG） |
   | `CONFIG_DEBUG_PROBE_GPIO_TCK` / `_TMS` | 10 / 11 | 10 / 11 | SWCLK / SWDIO，见 §4 |
   | `CONFIG_SERIAL_HANDLER_GPIO_TXD` / `_RXD` | 4 / 5 | 4 / 5 | 目标 UART |
   | `CONFIG_SERIAL_HANDLER_GPIO_BOOT` / `_RST` | 6 / 7 | 6 / 7 | 复位时序 |
   | `CONFIG_BRIDGE_GPIO_LED1..3` | 18 / 19 / 20 | 12 / 13 / 14 | 状态灯 |
   | `CONFIG_ESPTOOLPY_FLASHSIZE_4MB` | `y` | `y` | 与 S3 一致；实际模组更大再改 |

   **不要写**：`CONFIG_BRIDGE_ROLE_*`（C3/C6 无 `SOC_USB_OTG_SUPPORTED`，Kconfig 默认自动落 `WIRELESS_SLAVE`，写死反而失去默认值随 Kconfig 演进的弹性）、`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`（C3/C6 默认即 160 MHz，`DAP_config.h` 的 `CPU_CLOCK` 自动跟随）、任何 `CPU1` 相关符号（单核）。
2. **不套用** `sdkconfig.defaults.wireless_slave`：它含 `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1`（`sdkconfig.defaults.wireless_slave:12`），单核 target 上没有意义。C3/C6 从机靠 Kconfig 默认值即可，不需要额外给出角色。
3. 叠加顺序：IDF 自动按 `sdkconfig.defaults` → `sdkconfig.defaults.<target>` 叠加，无需手工合并。base 文件只有两条（`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n`、`CONFIG_SERIAL_FLASHER_MD5_ENABLED=y`），对单核同样成立。
4. 构建并**记录体积**（这是阶段 2 的额外产出，供阶段 3/4 与 §5 风险表对照）：
   - `idf.py -B build_c6 set-target esp32c6 && idf.py -B build_c6 build`
   - `idf.py -B build_c3 set-target esp32c3 && idf.py -B build_c3 build`
   - 各跑一次 `idf.py -B build_cX size`，记下 `iram0_text`（debug_probe 全量进 IRAM，见 §5）与 DRAM 余量。
5. 若用户构建报 target 相关错误，agent 按日志在本阶段内迭代。**预期坑**：`esp-serial-flasher` 依赖 `esp_driver_uart`（`components/serial_handler/CMakeLists.txt` 已按 IDF >= 5.3 分支加入，若仍报缺则在组件里补依赖）、`compiler.h` 硬编码 `xtensa_gcc.h`（RISC-V 下能编过，命名问题留阶段 5）、单核 WDT / IRAM 相关链接错误。
6. commit（nightly-p2）。

**交付物**：C6、C3 从机固件各可构建；两个 target 的体积数据入档（写进本文件或 Todo.md）。

**用户验证**
1. `idf.py -B build_c6 set-target esp32c6 && idf.py -B build_c6 build` 通过；menuconfig 里确认 `Bridge role` = wireless slave。
2. `idf.py -B build_c3 set-target esp32c3 && idf.py -B build_c3 build` 通过；同上确认角色。
3. 两次 `idf.py size` 的 `iram0_text` 未把 IRAM 撑满（C3 尤其注意），DRAM 余量正常。
4. **构建日志里没有 tinyusb 的编译行**（证明 C3/C6 上 USB 栈完全不进图）。

**通过判据**：两个 target 构建通过 + 体积无告急 + 日志中无 tinyusb。

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
3. 清理：删除构建产物/临时文件，确认仓库干净（`git status` 只应出现被忽略的 `.clangd`、`.cz.toml`、`.gitlab-ci.yml`——按用户决定保留在盘上、不追踪）；可选做 compiler.h 的 riscv 命名清理。
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
| `debug_probe` 全量进 IRAM（`components/debug_probe/noflash.lf` 是 `entries: * (noflash)`，ldgen 的 noflash 方案把 `.text` 放 `iram0_text`、`.rodata` 放 `dram0_data`） | C3 单核 IRAM 余量被吃掉，可能链接失败或挤占缓存 | 阶段 2/4 用 `idf.py size` 看 `iram0_text`；必要时把 fragment 收窄到 SWD 位打所需函数 |
| S3 从机仍编译 tinyusb（依赖按 target 解析，与角色无关 → 构建时间与 `managed_components` 体积不下降） | 无功能影响（不链接）；C3/C6 不进图 | 记录在案，阶段 5 文档说明 |

- **回滚策略**：每阶段一个 commit；失败回退到上一阶段 commit。
- **无线上游兼容**：host（S3）与 slave（C6/C3/S3）只靠 role + CONFIG_WLINK_CHANNEL 配对，芯片型号无关，无需同步改 host。

---

## 附：改动清单（END_nightly = 导入源仓库全部文件 + 以下手术式改动）

| 文件 | 改动 |
|---|---|
| main/CMakeLists.txt | USB srcs/deps 按「非 slave」条件化；`noflash.lf` 用 `set(ldfragments)` 条件化；tinyusb 的 `public_include` 补全按 `BUILD_COMPONENTS` 判断 |
| main/main.c | #if !CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE 包 USB 代码 |
| main/idf_component.yml | tinyusb 加 rules（target in [esp32s2, esp32s3]） |
| main/Kconfig.projbuild | wired/host depends on SOC_USB_OTG_SUPPORTED；角色默认目标感知 |
| components/wireless_link/{Kconfig,wireless_link.c} | 阶段 1：调试自检宏 `CONFIG_WLINK_DEBUG_LINK_TEST` + 从机每秒 `connected N` 任务 |
| main/bridge_glue.c | 阶段 1.1：从机 SWD 请求序号 FIFO 关联响应（修首请求被误丢 + 响应串号 + 队列建立顺序） |
| sdkconfig.defaults.esp32c3 / esp32c6 | 新增（引脚 / flash 容量 / SWD 接口；不写角色与 CPU 频率） |
| sdkconfig.defaults.wireless_slave | 仅用于 S3 从机（含 CPU1 WDT 符号，不用于 C3/C6） |
| .gitignore | 不再忽略 `docs/`；`test/` 与 `CHANGELOG.md` 行随文件删除；`.clangd` / `.cz.toml` / `.gitlab-ci.yml` 继续忽略（保留在盘上） |
| 其余文件（components/serial_handler、components/util、components/debug_probe、main/msc.c、main/eub_vendord.c 等、根 CMakeLists、README、sdkconfig.defaults*） | 原样保留（README/docs 的芯片范围留阶段 5 更新） |