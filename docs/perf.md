# 性能基线与测量手册（docs/perf.md）

> 状态：T0.1 基线快照 · 2026-11-06 · `HEAD = 32408ff`
> 配套计划：[`.agent/Plan_v3.md`](../.agent/Plan_v3.md)。口径（D1）由该文件定义；本文件只放**可复现的事实与数字**，以及待填的 A/B / 预算 / 稳定性表。
> 规则：本文件只记录实测；未实测的写「待测」或明确标注「假设」。

## 1. 口径定义（D1）

### 1.1 三个一级指标

| 指标 | 定义 | 测量方式 | 单位 | 采样 |
|---|---|---|---|---|
| **RTT** | 一条 64 B DAP 命令从 USB OUT 到 USB IN 完整返回的墙钟时间 | `mdw <addr> 1` 单次往返，外部计时（openocd 内建 `time` 或 `/usr/bin/time` 单次进程） | ms | n ≥ 30，报 p50/p99/max |
| **批量吞吐** | 链路真实字节搬运速率 | `dump_image <addr> <len>`（读）/ `load_image` 到 RAM（写），len ≥ 32 KiB | KiB/s | n ≥ 5，报 p50/p99 |
| **烧录端到端** | 一次完整烧录的墙钟时间 | `program <elf> verify reset`，外部 `/usr/bin/time` | s | n ≥ 10，报 median/p99 |

### 1.2 固定成本（单列，不进吞吐口径）

- `verify_image` 的时间 = 目标侧 CRC 算法成本（reset halt + 下载算法 + 执行 + 回读 CRC），基本与镜像大小无关（F407 55 KB 也是 0.24–0.26 s）。
- `flash write_image` 的写速率含 sector erase（F407 实写 65,536 B 含 4 个 16 KB sector 擦除）。

### 1.3 不能当吞吐的数字（常见误判）

| 误用 | 实际含义 |
|---|---|
| `verified 55152 bytes in 0.26s (208 KiB/s)` | 目标侧 CRC 固定成本，不是链路带宽 |
| `wrote 65536 bytes in 4.1s (15.6 KiB/s)` | 含擦除；不等于纯搬运速率 |
| 历史「20.6 / 31.6 / 40.8 / 42.4 KiB/s」阶梯 | 全部是 §1.2 的 verify 口径，**只可作 A/B 对比** |

### 1.4 统一实验条件（否则数据不可比）

信道 1；无线速率档 **24 Mbps**（2026-11-07 起固件默认；§2 的旧快照是 54 Mbps 档，跨档比较要注明）；SWD 时钟按用例给定；同一目标板与同一镜像（SHA 固定）；同一 PC 与 USB 口；每次 openocd 独立进程、先 `init; halt`；单变量 A/B、每点 ≥30 次。

## 2. 基线快照（2026-11-06，`HEAD = 32408ff`）

### 2.1 固件基线（五组合，构建于 19:05–19:07）

| 组合 | target | role | Total image size | `bridge.bin` | md5 | 无线速率 |
|---|---|---|---|---|---|---|
| wired | esp32s3 | `BRIDGE_ROLE_WIRED` | 273,603 B | 273,728 B | `41218f6c8128d12627008ee8288129d7` | — |
| host | esp32s3 | `BRIDGE_ROLE_WIRELESS_HOST` | 831,215 B | 831,328 B | `c675b42c295941910f63e61195feb855` | 54 Mbps |
| slave_s3 | esp32s3 | `BRIDGE_ROLE_WIRELESS_SLAVE` | 841,839 B | 841,952 B | `40078e25259e7431d73fd7a6a6e9ed55` | 54 Mbps |
| slave_c6 | esp32c6 | `BRIDGE_ROLE_WIRELESS_SLAVE` | 981,148 B | 981,520 B | `140fc82738855f14c7ef8d47c165e73a` | 54 Mbps |
| slave_c3 | esp32c3 | `BRIDGE_ROLE_WIRELESS_SLAVE` | 855,912 B | 856,272 B | `3de0573daa074cfef85add64cf8a06c4` | 54 Mbps |

证据：`build_r_<v>/size_perf1.log`（`Total image size` 行）、`build_r_<v>/bridge.bin`（md5 见上）。`.bin` 因对齐填充略大于 image size，属正常。

### 2.2 参数基线（五组合一致，除角色/速率）

| 项 | 值 |
|---|---|
| 信道 `CONFIG_WLINK_CHANNEL` | 1 |
| 应答超时 / 重传 | `WLINK_SWD_TIMEOUT_MS=250` × `WLINK_SWD_RETRIES=3` |
| peer 等待 / 判定丢失 | `WLINK_PEER_WAIT_MS=2000` / `WLINK_PEER_TIMEOUT_MS=3000`（后者只喂 LED） |
| 编译档 | `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y`（`-Og`） |
| 日志 | `CONFIG_LOG_DEFAULT_LEVEL=3`（INFO；DEBUG 日志编译期被裁掉） |
| SWD 逐包日志 | `CONFIG_WLINK_SWD_VERBOSE_LOG` 未开 |
| DAP 接口 | `CONFIG_DEBUG_PROBE_IFACE_SWD=y` |
| 包大小 | DAP 64 B（`DEBUG_PROBE_PACKET_SIZE`）、USB bulk 端点 64 B（Full-Speed）、链路帧上限 246 B |
| 缓冲/队列 | host 请求队列 8、host 应答 ringbuf 8 KiB、slave 请求队列 8、seq FIFO 32、DAP 收/发各 1 KiB |

### 2.3 板卡与工具链

| 对象 | 标识 | 备注 |
|---|---|---|
| S3 console | CH340 SN `54E2053871` | `/dev/serial/by-id/usb-1a86_USB_Single_Serial_54E2053871-if00` |
| S3 原生 USB（CMSIS-DAP） | `303a:1002` SN `ECDA3B94B2F0` | `/dev/serial/by-id/usb-Espressif_Systems_Co._Ltd._CMSIS-DAP_ECDA3B94B2F0-if00` |
| S3 MAC | `ec:da:3b:94:b2:f0` | 16 MB flash |
| C6 console | CH340 SN `5C84367346` | `/dev/serial/by-id/usb-1a86_USB_Single_Serial_5C84367346-if00` |
| C6 BASE MAC | `ac:eb:e6:da:7a:f4` | 16 MB flash |
| C3（备用） | 原生 USB-JTAG `88:56:a6:39:e3:f4` | 只构建、不上板 |
| 目标 A | STM32F103C8，DPIDR `0x1ba01477`，device id `0x00006410`，64 KiB | 镜像 `test/p01/build/p01.elf`，8,692 B |
| 目标 B | STM32F407，DPIDR `0x2ba01477`，device id `0x10076413`，1024 KiB | 镜像 `test/shaobing_gimbalchangeauthor.elf`（实写 65,536 B / verify 55,152 B） |
| ESP-IDF | v6.0.2（`/home/dbb/.espressif/v6.0.2/esp-idf`），启动脚本 `~/.espressif/tools/activate_idf_v6.0.2.sh` | esptool 5.3.1 |
| openocd / gdb | `/usr/bin/openocd` 0.12.0；`/usr/bin/gdb-multiarch` | — |
| Python | `/usr/bin/python3` 3.12.3（自带 pyserial 3.5）；IDF venv `~/.espressif/tools/python/v6.0.2/venv/bin/python`（3.12.3，pyserial 3.5） | T1.1 工具**不需要额外装包**（见 §2.5 备注） |

**端口映射提醒**：`/dev/ttyACM*` 序号随枚举变化（本轮为 ttyACM0=S3 console、ttyACM1=CMSIS-DAP、ttyACM2=C6 console），脚本必须走 `/dev/serial/by-id`。

### 2.4 已测得基线数值（附证据）

条件：S3 host + C6 slave_c6、ch1、54 Mbps；除注明外目标为 STM32F103 p01（8,692 B）。

| 编号 | 场景 | 结果 | 证据 |
|---|---|---|---|
| B1 | `verify_image` soak @500 kHz（3 min） | 469/469 通过、0 失败；mean **31.558** / median 31.613 / min 28.402 / max 33.771 KiB/s；185.2 s；首末四分位 −1.5% | `/tmp/perf1_stats.txt`、`/tmp/perf1_raw.txt`、`/tmp/openocd_perf1_soak.log` |
| B2 | 同口径、ESP-NOW 1 Mbps（B4 基线，已过时） | 20.595–22.380 KiB/s | Plan_v2 §10.1 记录 |
| B3 | SWD 时钟扫描（54 Mbps） | 500/1000/2000/4000/5000/6000 kHz = 31.694 / 36.594 / 40.835 / **42.375** / 40.557 / 41.335 KiB/s；拐点 2000 kHz | Plan_v2 §10.2.1 |
| B4 | 链路批量读 `dump_image` 54,908 B @500 kHz（F407） | **11.929 KiB/s**（4.495 s）= 唯一真实字节搬运口径 | `/tmp/f407w_*.log` |
| B5 | F407 `program ... verify reset` @500 kHz | 9/10 成功，WALL 4.79–5.36 s（median **5.01**） | `/tmp/f407w_1..10.log` |
| B6 | F407 `flash write_image erase` + `reset init` | 8/10 成功，写 median **14.481** KiB/s，WALL median **5.21 s** | `/tmp/f407w_*.log` |
| B7 | 无线烧录挂死率 | **15%（3/20）**；形态固定（`Programming Started` 之后永久挂起），两侧证据齐全 | `/tmp/f407w_console.log`、`/tmp/s3_console_during_program.log`、`/tmp/c6_console_during_hang2.log` |
| B8 | 烧录端到端（F407） | 55,152 B / 5.14 s ≈ **10.6 KiB/s**（含擦除+校验+复位） | 由 B6 派生 |
| B9 | 请求往返 RTT（T1.1 `bench.py rtt`，n=30，`mdw` 单字读 @2000 kHz） | min **1934** / p50 **2541.5** / mean 3101.5 / p95 5646 / max 7265 µs | `perf_out/20261106-224619-rtt.{json,md}` |
| B10 | 批量读 `dump_image` 32 KiB @2000 kHz（`bench.py bulk-read`，n=5） | min **8.145** / p50 **13.798** / mean 15.196 / max 21.151 KiB/s | `perf_out/20261106-224635-bulk-read.{json,md}` |
| B11 | 批量写 RAM `load_image` 32 KiB @2000 kHz（`bench.py bulk-write-ram`，n=5） | min **21.725** / p50 **23.385** / mean 23.226 / max 24.304 KiB/s；1 次挂死被重试救回 | `perf_out/20261106-224956-bulk-write-ram.{json,md}` |
| B12 | 烧录端到端（`bench.py flash-program`，T1.1） | **未取得**：本轮 10 次尝试 **0 成功**——@2000 kHz 5 次（4 次 `hang_during_program` wall≈50 s、1 次 `error` 31 s）、@500 kHz 5 次全部 `hang_during_program` wall≈50 s（均由外部 kill 收尾）；单会话手测 100/500/2000 kHz 也全部挂 | `/tmp/bench_flash.log`、`/tmp/bench_flash500.log`；另有 `perf_out/20261106-230410-flash-program.{json,md}` 记录的是更早一轮 2 次 `dap_dead`（残留 openocd 占 USB 节点所致，属环境干扰，非本行口径） |

**B12 的成因（已定位，属产品缺陷而非工具问题）**：`flash erase_sector` 1.5 s 成功、`flash write_image` 1 KB 成功（0.16 s / 6.181 KiB/s），但 55 KB 图像必定挂在 flash 异步算法的 16 KB 工作区阶段；同刻 S3 控制台恰好一条 `W bridge: DAP request seq 89 was not answered after 3 attempts, link is paired`，C6 侧仅 `debug_gpio: SWD GPIO init done`。即链路丢/迟一次 DAP 事务后，host relay 放弃重试只打日志、不向 USB 侧交付失败（`components/bridge/bridge_host.c:137-143`），USB 侧 `usb_send_task` 阻塞 `portMAX_DELAY`（`components/usb_device/eub_vendord.c:210`），openocd 无超时 → 永久挂起（= B7 的同一缺陷，见 `.agent/Plan_v3.md` §1.3 缺陷 1）。

排除项（本轮都试过，无效）：SWD 时钟 100 / 500 / 2000 kHz；复位 C6（RTS）；`FLASH_SR` 清零 + 解锁；`reset halt` 前置；换 `program` / `flash write_image erase` 两种路径。

另记一条环境坑：被 kill 的 openocd **不响应 SIGTERM**，残留进程会占住 CMSIS-DAP 的 USB 节点（`usbfs: process … did not claim interface 2 before use`、`Input/Output Error`、`CMD_INFO failed`），必须 `kill -9` / `pkill -9 -x openocd`；`bench.py` 已按 SIGTERM→5 s→SIGKILL 处理，但外部直接 kill 父进程仍会留孤儿。

固件启动锚点（基线，供回归比对）：host `Free heap after start-up: 216204`、`SWD relay ready`、`usb_send_task is ready!`、`ESP-NOW transmit rate ... 54 Mbps`；slave `Free heap after start-up: 285708`、`Wireless slave ready: UART + CMSIS-DAP SWD over ESP-NOW`。
证据：`build_r_host/boot_perf1.log`（4,814 B）、`build_r_slave_c6/boot_perf1.log`（5,235 B）。

### 2.5 T1.1 的 Python 环境（conda 独立环境）

测量工具跑在专用 conda 环境 `endperf` 里（用户 m05177 要求独立环境），不污染系统 python 与 IDF venv：

| 项 | 值 |
|---|---|
| 环境路径 | `/home/dbb/miniforge3/envs/endperf`（`mamba activate endperf`） |
| 解释器 | Python **3.12.16** |
| 依赖 | `pyserial 3.5`（仅 `--recover` 的 RTS 复位需要；缺失时该功能降级） |
| 重建命令 | `mamba create -y -n endperf python=3.12 pyserial` |

`tools/perf/bench.py` 本身只用标准库（`subprocess`/`statistics`/`json`/`argparse`/`re`/`hashlib`/`shlex`），所以系统 `python3` 也能跑（只是没有 `--recover`）。外部依赖仅 `openocd` 0.12.0，另需能打开 S3 的 CMSIS-DAP USB 节点。

用法与失败分类见 [`tools/perf/README.md`](../tools/perf/README.md)。

## 3. 预算表（T2.1 待填）

> 单位：µs/64 B 往返。分项和与实测差要求 ≤15%。

| 分项 | 500 kHz / 54 Mbps | 2000 kHz / 54 Mbps | 500 kHz / 24 Mbps | 说明与来源 |
|---|---|---|---|---|
| USB-FS 帧地板（PC↔EP） | 待测 | 待测 | 待测 | Full-Speed 1 ms 帧粒度 |
| host 任务跳（USB 回调→队列→relay→link） | 待测 | 待测 | 待测 | T1.2 bench |
| 空口往返（ESP-NOW + ACK） | 待测 | 待测 | 待测 | T1.2 bench |
| slave 任务跳（rx 回调→队列→DAP→tx） | 待测 | 待测 | 待测 | T1.2 bench |
| DAP 引擎执行 | 待测 | 待测 | 待测 | T1.2 bench |
| SWD 线时间（双向 64 B） | 待测 | 待测 | 待测 | 计算 + bench |
| 未归因残差 | 待测 | 待测 | 待测 | 实测 − 分项和 |

## 4. A/B 记录表（Phase 3 待填）

> 纪律见 Plan_v3 D3：单变量、同条件、每点 ≥30 次、p50/p99 + 失败数。

| 任务 | 变量 | 条件 | before | after | Δ | 失败数 | 结论 | commit |
|---|---|---|---|---|---|---|---|---|
| T3.1 失败恢复 | 丢帧后的行为 | 注入单帧丢失 | 无限挂死 | 待测（目标 ≤1 s 报错） | — | — | — | — |
| T3.2 slave 路径合并 | 任务跳数 | 500/2000 kHz | B1/B4 | 待测 | — | — | — | — |
| T3.3 host 路径 | 拷贝/检查 | 500/2000 kHz | 待测 | 待测 | — | — | — | — |

## 5. 稳定性记录（T1.3 / T4.1 待填）

| 日期 | 固件 | 目标 | 次数 | 成功 | 挂死 | 其它错误 | 成功率 | 证据 |
|---|---|---|---|---|---|---|---|---|
| 2026-11-06 | `32408ff` | F407 | 20 | 17 | 3 | 0（另 2 次反例属配置错误，不计） | 85% | `/tmp/f407w_*.log` |
| 2026-11-06 夜 | `32408ff` | F407 | 10 | 0 | 9 | 1（`error`，31 s） | **0%** | `/tmp/bench_flash.log`、`/tmp/bench_flash500.log` |
| 2026-11-07 00:42 | `32408ff` + T3.1 | F407 | 4 | 1 | 0 | 3（快速失败，5.0–5.2 s） | **25%** | `/tmp/d11_*.log`、`/tmp/d12_*.log` |
| 2026-11-07 00:57 | `7e0216c` @54 Mbps | F407 | 10 | 1 | 0 | 9（`Pipe error` 快速失败 2.65–2.79 s） | **10%** | `/tmp/p2_54m_run_{1..10}.log` |
| 2026-11-07 01:02 | `7e0216c` @24 Mbps | F407 | 10 | **10** | 0 | 0 | **100%** | `/tmp/p2_24m_run_{1..10}.log` |
| 2026-11-07 01:05 | `7e0216c` @24 Mbps，55,152 B | F407 | 3 | **3** | 0 | 0 | **100%** | `/tmp/p2_24m_55k_*.log` |
| 2026-11-07 01:09 | `7e0216c` @11 Mbps | F407 | 5 | 3 | 0 | 2（目标状态类：`failed erasing sectors`、`timeout waiting for algorithm`；传输层 0 丢帧） | 60% | `/tmp/p2_11m_run_{1..5}.log` |

### 已知缺陷 P1（2026-11-07，T3.1 期间定位）

无线 DAP 通道会**间歇性丢掉 1 个请求帧**，表现为 openocd 一次 16 KB 工作区写中随机丢 1 条
命令；被放弃的 seq 与从机侧「缺口」1:1 对齐（例如从机 `seq 166 arrived after 164` ↔ 主机放弃
165），且主机对同一 seq 的 3 次重发**从机一帧都没收到**。

- 已排除：从机处理慢（`kept the DAP engine busy` 与 `took … ms` 均为 0 条）、空应答（0 条）、
  迟到应答（0 条）、从机复位（uptime 单调）、CDC 背压（(a)/(b) 对照同结果）、SWD 时钟
  （100–2000 kHz 同失败）、ELF 解析（展平 raw bin 同失败）、超时边界（250 ms 与 1000 ms 同失败）。
- 失败的边界：写入 ≥ 16,384 B 且载荷非恒定（全 0/全 0xAA 55 KB 通过）；4–15 KB 真实数据通过。
- 影响：一次 4 次运行 1 次成功（25%），失败时 openocd 5 s 级快速报错（T3.1 生效），重试可再试。
- **P2 结论（2026-11-07 01:10，已回答）**：丢帧与 ESP-NOW 发送速率强相关。

  | 档位 | 成功 | not-answered | 16 KB 写 WALL |
  |---|---|---|---|
  | 54 Mbps | 1/10 | 9 | 2.67 s（失败 2.65–2.79 s） |
  | **24 Mbps** | **10/10** | **0** | **2.09 s** |
  | 11 Mbps | 3/5（2 次属目标状态错误，非传输） | 0 | 2.21 s |

  24 Mbps 档下 55,152 B 真实 payload 也 3/3 成功（9.55–10.06 KiB/s）。54 Mbps 唯一成功那次
  （2.23 s / 7.18 KiB/s）反而比 24 Mbps（1.95–2.08 s / 7.7–8.2 KiB/s）慢约 5%——每包往返由
  SWD 执行与 USB 批量传输主导，空口线速率的差别（54M ≈ 20 µs vs 24M ≈ 45 µs/帧）可忽略。
  因此固件默认档已从 54 Mbps 改为 **24 Mbps**（`sdkconfig.defaults.wireless_{host,slave}` +
  `main/Kconfig.projbuild` 的 help 说明），54 Mbps 保留为可选项但标注会丢帧。
- 剩余待查（P2 之后）：host `wlink_send` 侧记录 `esp_now_send` 返回值与 seq，确认 54 Mbps 下
  「3 次重发全部消失」是发送口返回错误还是空口丢帧；若确认是空口，可考虑「重发时降速」的兜底。

阈值（T4.1 定稿时填实）：100 次挂死 0；丢帧时 ≤1 s 快速失败；RTT p99 ≤0.8× 基线；批量读 ≥1.3× 基线；F407 `program` WALL median ≤4.5 s。

## 6. 更新规则

1. 只追加实测数据与新的 A/B 行；旧数据不修改，若口径变化则开新表并注明日期。
2. 每行数据必须带「条件 + 证据路径」；没有证据的数字不入表。
3. `HEAD` 变化时在 §2 顶部追加一行快照（新表），不覆盖旧快照。
4. 阈值只在 T4.1 定稿时写实，之前一律标「待测」。
