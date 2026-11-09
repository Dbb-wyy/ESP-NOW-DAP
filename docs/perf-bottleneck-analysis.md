# 无线 SWD 性能瓶颈分析（宏观推测版）

> 状态：**推断文档，未做任何新实测**（2026-11-07，硬件不在场）。
> 依据：仓库代码静态阅读 + openocd v0.12.0 源码 + `docs/perf.md` 中**既有测量数据**（只借用原始值，不沿用其归因结论）。
> 标注：【推断】= 由代码/数据反推，误差 ±30–50%；【实测】= 引用 `docs/perf.md` 已有测量；【证据】= 可直接引用的代码位置。
> 注：Kconfig/文档里的「24 Mbps」= 802.11g 24 Mbit/s 速率，工作在 2.4 GHz 频段。

---

## 0. 一页结论

| # | 瓶颈 | 位置 | 判定 | 可优化性 |
|---|---|---|---|---|
| ① | **命令粒度**：每包只有 48 B 有效数据 | CMSIS-DAP 一问一答 + openocd 按包尺寸打包 + 固件把 `DAP_PACKET_SIZE` 钉死 64 | 吞吐**主**瓶颈 | 大：128 B 安全档可拿 1.6–2.0× |
| ② | **每命令固定开销 1.17–1.61 ms** | USB-FS 帧量化（≈1 ms/帧）+ 停等往返 + 8–10 次任务/队列跳 | RTT **主**瓶颈 | 中：可压 10–20% |
| ③ | **SWD 有效时钟只有名义 54%** | `PIN_DELAY_SLOW` 迭代周期与 `DELAY_SLOW_CYCLES=3` 不符（S3 从机再差 1.72×） | 次要瓶颈 | 中：RTT −10%、大包场景 −18% |
| ④ | **ESP-NOW @24 Mbps 空口** | 每命令 2 帧、每帧 0.1–0.36 ms 的固定延迟税 | **不是速率瓶颈**；是**固定延迟税 + 可靠性风险** | 小（提速无效；减少帧数有效） |
| ⑤ | **失败路径太长** | 54 Mbps 丢帧 + 250 ms×3 重试 + 2 s IN backstop + 全局 `abort()` | 稳定性**主**瓶颈 | 大：2.9–6.2 s → ≤1 s |

一句话：**24 Mbps 空口已经越过「速率敏感」拐点——24→54 Mbps 只能省 2%，提速无意义；真正的吞吐杠杆是把每命令的数据从 48 B 变多（包大小），真正的 RTT 杠杆是 USB 帧量化与任务链，真正让挂死的是失败路径设计而不是无线速率。**

附：把传输层从 ESP-NOW 换成 Wi-Fi（IP/UDP/TCP）**不改变上述任何一项**——预期吞吐在噪声内、RTT 反而变差；ESP-NOW 自身已能配 HT20/HT40 高速率，真收益（大帧、流水线）也与传输层无关。详见 §9。

---

## 1. 方法与口径

- 不做新测量，只用 `docs/perf.md` 的**原始值**（吞吐 / RTT p50 / 烧录 WALL / 速率 A/B）重新反推，并逐项标注推断误差。
- 既有数据的**同口径相对比较可信**（重复性 ≈1%），**绝对归因不可信**（理由见 §2.4）。
- 本文所有「收益预测」都是 `T = 固定开销 + 每命令数据量 / 单位数据成本` 的模型外推，量级可信、绝对值 ±30–50%。

---

## 2. 事实基线

### 2.1 数据通路（代码事实）

```
openocd ──USB FS bulk(64 B)──> S3 host(TinyUSB task)
  OUT xfer_cb → probe_process_data → 队列(depth 8) → swd_relay task
      → wlink_send(SWD, seq, ≤64 B) ──ESP-NOW──> slave Wi-Fi 回调
          → 队列 → swd_link_rx task → debug_probe_process_data（DAP 引擎）
              → DAP 引擎 → ringbuf(1 KiB) → swd_link_tx task
                  → wlink_send(SWD, seq, ≤64 B) ──ESP-NOW──> host
  host Wi-Fi 回调 → ringbuf(8 KiB) → usb_send_task → IN xfer ──> openocd
```

每命令往返：≥8–10 次任务/中断切换、≥4 次 memcpy、2 次 ESP-NOW 帧收发，且**严格停等**（`bridge_host.c` 的 swd_relay 发完等 binary sem，上限 `250 ms × 3`）。

### 2.2 每命令搬多少数据

openocd `cmsis_dap_swd_write_from_queue()`（`cmsis_dap.c:763-825`）把 `pending_queue_len` 个 transfer 打成一条 `DAP_Transfer(0x05)`：
请求 = `1(cmd)+1(index)+1(count)+Σ(1 或 5)` B；应答 = `2 + 5×read 数` B；数据 = `4×transfer 数`。

设设备上报包大小 `P`，`N=(P-4)/5`：

| P | N | 读请求 | 读应答 | 数据/命令 | 写请求 | 写应答 |
|---|---|---|---|---|---|---|
| 64（现状） | 12 | 15 B | 62 B | **48 B** | 63 B | 14 B |
| 128 | 24 | 27 B | 122 B | **96 B** | 123 B | 26 B |
| 246 | 48 | 51 B | 242 B | **192 B** | 243 B | 50 B |
| 512 | 101 | 104 B | 507 B | **404 B** | 508 B | 103 B |

### 2.3 反推：固定开销、SWD 单位成本、RTT 分解

用同条件（S3 host + C6 slave、24 Mbps、ch1、F407）两组既有数据联立：

| 口径 | 500 kHz | 2000 kHz | 反推结果【推断】 |
|---|---|---|---|
| bulk-read p50 | 14.627 KiB/s → 3205 µs/命令 | 27.939 KiB/s → 1678 µs/命令 | 固定 **1169 µs**；SWD **42.4 µs/transfer**（名义 23 µs → **1.84×**） |
| bulk-write p50 | 14.131 KiB/s → 3318 µs/命令 | 26.461 KiB/s → 1772 µs/命令 | 固定 **1257 µs**；SWD **42.9 µs/transfer**（1.87×） |
| RTT p50 | 3515.5 µs | 2084.5 µs | 时钟相关 1908/477 µs；非流水固定 **1607.5 µs** |

自洽性：
- RTT 的时钟相关部分 477/1908 = 0.25，正好等于 2000/500 的倒数 → SWD 延迟模型成立。
- bulk 固定（1169）< RTT 固定（1607.5），差 0.44 ms = 停等/帧对齐成本（bulk 被 openocd 的 3 深流水摊掉一部分）。

### 2.4 既有结论哪里不可信（4 条硬证据）

1. **同表内部矛盾**：`docs/perf.md` §3 说 2000 kHz bulk-read「每包 2237 µs（固定 1728 + SWD 509）」，但同文档记录的吞吐 27.939 KiB/s 反推应为 **1678 µs**——差 559 µs（33%）。其「固定开销 1728 µs 是地板」的说法系统性高估了可压缩余量。
2. **SWD 按名义时钟记账**：`DAP.h:329` 的 `PIN_DELAY_SLOW`（`volatile count; while(--count);`）实际每迭代 4.5–5 周期，而 `DAP.c:46-49` 按 `DELAY_SLOW_CYCLES=3` 折算 → 有效时钟只有名义 54%；S3 从机（Xtensa）与 C6（RISC-V）再差 1.72×。旧分解没有建模这一点。
3. **「64 B 是上限」从未验证**：openocd 明确支持设备上报包大小（见 §3.1），固件侧的 64 B 全是应用层常量（见 §3.1 证据）。
4. **bench.py 会藏失败/错标环境**：`:377-384` 重试成功即丢失败记录；`:92-109` classify 缺 `Pipe error`/`timeout waiting for algorithm` 分支；`:147` 快照硬编码 host/slave_c6（S3 轮被标错）；`:186-187` by-id 解析永不生效；`:242-253` 无 p99；`:528` flash-program 的默认 ELF 是 F103 镜像。

---

## 3. 逐项判定

### 3.1 瓶颈①：命令粒度（吞吐主瓶颈）

【证据】openocd `cmsis_dap.c:1202-1246`：冷启动按 64 B 假设 `pending_queue_len = 12`；随后主动查 `INFO_ID_PKT_SZ`，**设备上报不同包大小就重算 `pending_queue_len = (pkt_sz-4)/5` 并重分配收发缓冲**。其注释原文：

> 4 bytes of command header + 5 bytes per register write. **For bulk read sequences just 4 bytes are needed per transfer, so this is suboptimal.**

固件侧的 64 B 限制全是应用层常量：
- `components/debug_probe/DAP/Config/DAP_config.h`：`DAP_PACKET_SIZE 64U`；
- `components/usb_device/eub_vendord.c`：`TU_VERIFY(xferred_bytes > 0 && xferred_bytes <= 64)`、`eub_vendord.h:16` `EUB_VENDORD_EPSIZE 64`；
- `components/bridge/bridge_priv.h`：`BRIDGE_SWD_PKT_SIZE 64`（无线中继逐帧 64 B）。

底层能力足够：
- `managed_components/espressif__tinyusb/src/device/usbd.c:1328-1356` `usbd_edpt_xfer()` 无 `≤MPS` 检查；
- `.../portable/synopsys/dwc2/dcd_dwc2.c:331,585` 按 `tu_div_ceil(total_bytes, max_size)` 设置 `packet_count` 搬运多包 → **DCD 支持 >64 B 的 xfer**；
- libusb 的 bulk transfer 在 FS 上自然拆成 64 B 包。

收益模型（固定开销不随 P 变，保守）：

| P | 数据/命令 | 每命令耗时 | bulk-read | 相对 | +SWD 标定后 |
|---|---|---|---|---|---|
| 64 | 48 B | 1.68 ms | 27.9 KiB/s | 1.0× | 33.2（1.19×） |
| **128** | 96 B | 2.19 ms | 43.9 | **1.57×** | 55.8（**2.00×**） |
| 246 | 192 B | 3.20 ms | 59.9 | 2.14× | 84.5（3.02×） |
| 512 | 404 B | 5.45 ms | 74.1 | 2.65× | 116（3.7–4.1×，需分片） |

**协议安全上限**：openocd 用同步 `libusb_bulk_transfer`、不设 `URB_ZERO_PACKET`、不补 ZLP；设备端一次 xfer 只靠「短包」完成。**任何长度恰好是 64 整数倍的 USB 传输都会挂死**。P=128 时 `N=24`，所有请求/应答长度都不可能是 64 的倍数（`3+5c=64/128`、`2+5c=64/128` 在 `c≤24` 内无解）→ **128 是无需 ZLP 处理的协议安全上限**；P≥129 时 `c=25`（写请求 128 B）与 `c=38`（读应答 192 B）可达 → 会出现挂死。因此 246/512 必须引入短包/ZLP/分段重组逻辑，风险显著。

### 3.2 瓶颈②：每命令固定开销（RTT 主瓶颈）

- bulk 每命令固定 1.17–1.26 ms；RTT 固定 1.61 ms（差 0.44 ms = 停等/帧对齐）。
- 构成（按代码，量级推断）：USB-FS 帧量化 1 ms 级（`RTT ≈ 2 帧`）+ 任务/队列跳 0.1–0.3 ms + 空口 0.2–0.7 ms（见 §3.3）。
- 可动空间：任务链合并/减少 memcpy/IN 提前 arm → 100–250 µs（RTT −5~15%）；SWD 标定 → −218 µs（−10%）。
- **RTT 的 0.8× 目标（2084.5 → ≤1668 µs）可行性**：标定（−218）+ 固定开销削减（−200）≈ 1667 µs，**刚好压线，且取决于 §3.3 中空口税的真实值**（最大未知项）。

### 3.3 瓶颈④：ESP-NOW @24 Mbps 空口是不是瓶颈？（本次新增重点）

**判定：不是速率瓶颈；是「每命令固定延迟税」+「可靠性风险」。** 分三张表说明。

**(a) 每帧成本拆解**（64 B DAP 载荷 → wlink 帧 68 B → MAC 帧 ≈101 B = 808 bit）：

| 速率档 | PHY 前导+PLCP 头 | 数据段 808 bit | 帧 TX（不含 ACK） | 备注 |
|---|---|---|---|---|
| 1 Mbps（11b long） | 192 µs | 808 µs | **≈1000 µs** | 与代码注释「1 ms/packet」一致【证据】`wireless_link.c:74-79` |
| 11 Mbps（11b short） | 96 µs | 73 µs | ≈170 µs | |
| **24 Mbps（11g）** | 20–24 µs | 34 µs | **≈54–58 µs** | 当前默认【实测配置】 |
| 54 Mbps（11g） | 20–24 µs | 15 µs | ≈35–40 µs | 会丢帧【实测】 |

每跳还要加：ACK（14 B，速率档不随数据速率变，估 30–300 µs）+ SIFS 10–16 µs + DIFS/退避 0–100 µs。
→ **24 Mbps 每帧总成本 ≈ 0.10–0.36 ms；每命令 2 帧 ≈ 0.2–0.7 ms（占 1.68 ms 的 12–42%）**。ACK 速率是最大不确定项，需实测（§6-Q1）。

**(b) 速率档的价值：24→54 只值 2%**

数据段从 33.7 µs 降到 15 µs，每帧省 18.7 µs → 每命令 2 帧省 **37 µs = 2.2%**；前导/ACK/退避不随速率变。
【实测一致性检验】既有两轮数据：bulk-read 24 Mbps 27.939 vs 54 Mbps 28.090 KiB/s（54M 快 **0.54%**）；RTT 2084.5 vs 2064.5 µs（54M 快 **1.0%**）——与 2.2% 的预测同量级，**证明速率敏感部分只有 ~2%**。
仓库自身也已在 Kconfig help 记录同一结论：「The wireless round trip is dominated by the per packet SWD and USB work, so 54 Mbps was not faster」。
⚠️ 但 `wireless_link.c:74-79` 的代码注释仍写「relay 受空口 airtime 限制」——那句话**只对 1 Mbps 默认档成立**；24 Mbps 已越过拐点。两处说法并列未调和，容易误导（本次分析予以澄清）。

**(c) 降速的代价（反证：空口只有在低速档才是瓶颈）**

| 速率档 | 每帧 ≈ | 每命令 2 帧 ≈ | bulk-read 预测 | 相对 24M |
|---|---|---|---|---|
| 1 Mbps | 1.0 ms+ACK | ≥2.1 ms | ~13–14 KiB/s | **−45~55%** |
| 11 Mbps | 0.17 ms+ACK | ≈0.4 ms | ~25 KiB/s | −10% |
| 24 Mbps | 0.05–0.06 ms+ACK | 0.2–0.7 ms | 27.9（实测） | 基准 |
| 54 Mbps | 0.035–0.04 ms+ACK | 0.2–0.65 ms | 28.1（实测） | +0.5%（但丢帧） |

⇒ **1 Mbps 才是「空口瓶颈」的形态**（每包 1 ms，Kconfig 默认仅用于远距兜底）；24 Mbps 已经是拐点之后，所以 24 Mbps 不是瓶颈、54 Mbps 也不值得（收益 2%、丢帧 10%）。

**(d) 空口利用率（容量维度）**

- 当前 bulk-read：596 命令/s × 2 帧 = **1,192 帧/s**；链路帧能力约 2,800–10,000 帧/s → 利用率 **12–42%**。
- 载荷速率需求：≈0.46 Mbps（双向含帧头 ≈0.7 Mbps），24 Mbps 链路的数据能力（64 B 帧下 ≈3–8 Mbps）远未饱和。
- ⇒ **容量不是瓶颈**；瓶颈是「延迟税」和「丢帧」。

**(e) 何时空口会真的变成瓶颈**

1. 降速：1 Mbps 档（−45~55%），仅供远距。
2. 大包分片：无线单帧上限 `WLINK_MAX_PAYLOAD=246 B`。**P=246 时读应答 242 B 恰好单帧**，空口税不变；**P=512 时读应答 507 B 要 3 帧**（246+246+15），每命令 4 帧 → 空口 0.5–0.9 ms，且单命令失败概率随帧数放大。
3. 大流量/流水深度提高时（帧率需求上去后），退避/ACK 开始抖动。
4. 若未来启用 ESP-NOW v2 长帧（1470 B），512 B 可回到单帧——需两端支持，列为可选调研，不作为主线。

**结论**：24 Mbps 无线链路**不是吞吐/RTT 的主瓶颈**（占 12–42%，且其中 98% 与速率无关）；它是**稳定性瓶颈**（丢帧）和**大包方案的约束项**（单帧 246 B 上限）。优化方向应是「减少每命令帧数/提高每帧有效载荷」，而不是提高速率。

### 3.4 瓶颈③：SWD 延迟标定误差

【证据】`DAP.h:329` + `DAP.c:46-49`：`DELAY_SLOW_CYCLES=3` 按常量折算，实际 `while(--count)` 在 Xtensa -Og 下 ≈4.5–5 周期、RISC-V 不同 → 有效时钟只有名义 **54%**（1.84×），S3 从机再慢 1.72×（1642 vs 954 SWCLK/包）。
影响：SWD 段 509 µs（@2000 kHz，12 transfer）→ 修正后 276 µs；每命令 −233 µs（bulk −14%）；RTT −218 µs（−10.5%）；大包场景放大（128 B 档 SWD 段 −46%）。
要求：**必须运行时标定**（用 `esp_cpu_get_cycle_count()` 实测延迟循环），因为 `-Og → -O2` 会改变迭代周期。

### 3.5 瓶颈⑤：稳定性（失败路径）

| 现象【实测】 | 机制【推断】 |
|---|---|
| 54 Mbps：F407 烧录 0/5、16 KiB dump 9/10 丢 1 帧；24 Mbps 10/10 零丢帧 | `wlink_send` 只返回 `esp_now_send` 的首个错误，**无 send callback、无帧级 ACK**；54 Mbps 偶发整帧丢失（从机 seq 缺口与主机放弃 1:1） |
| 丢 1 帧后 2.9–6.2 s 才报错 | `250 ms×3` 重试（750 ms）→ 放弃 → USB IN stall；`eub_vendord.c` 还有 2000 ms IN backstop；openocd 自身的重试/复位再叠加 |
| 偶发整桥重启 | 任一 ops 失败即 `eub_abort()` → `abort()`（`eub_abort.c:15`），无端点级恢复 |

修法：超时 100–120 ms、重试 1 次、放弃即 abort、IN backstop 2 s→0.5 s、ringbuf 满背压而非静默丢、去掉全局 `abort()`；验收「注入单帧丢失 → ≤1 s 报错、重试一次成功 + 100 次 0 挂死」。

---

## 4. 提升点清单（按收益/风险排序）

| # | 提升点 | 机制 | 预估收益【推断】 | 风险 | 工作量 |
|---|---|---|---|---|---|
| A | **DAP 包 64→128 B** | openocd 自动按上报值打包；每命令 48→96 B | bulk 读/写 **1.57–1.59×**；叠加标定 **2.0×** | 中（跨 DAP/USB/bridge 三层，但协议安全，无线仍单帧） | 中 |
| B | SWD 延迟运行时标定 | 实测延迟循环周期 → 修正 `clock_delay` | RTT **−10%**；128 B 档 SWD 段 −46%；消除 S3/C6 差异 | 低-中（必须运行时标定 + 硬件验证信号质量） | 中 |
| C | 失败快速化 | 超时/重试/backstop/abort 路径改造 | 失败可见 **2.9–6.2 s → ≤1 s**；挂死率 → 0 | 低 | 小 |
| D | 固定开销削减 | host/slave relay 合并、减 memcpy、IN 提前 arm | bulk −8~15%；RTT **0.82×** | 低-中 | 中 |
| E | 包 246/512（需 ZLP/分片） | 每命令 192/404 B；需解 §3.1 协议边界 + 无线分片 | bulk **2.1–2.7×**（含标定 3–4×） | 高（短包边界、分片重组、失败代价↑） | 大 |
| F | 测量基座修复 | bench.py 6 缺陷 + p99 + 分类 + provenance + `--gate` | 让 A/B 可信 | 无 | 小-中 |
| G | 空口时间戳计数器 | `esp_now_send_cb` + slave 收发时间戳 | 把 §3.3 的 0.2–0.7 ms 不确定项变成实测值 | 低 | 小 |

**不做**：提高速率档（24→54 只值 2% 且丢帧）、只卷 SWD 时钟、改 openocd、重写 DAP 引擎。

---

## 5. 推荐路线（阶段化，每步单变量）

1. **F**：修 bench.py（p99、失败不丢、分类、provenance、by-id、ELF 默认值）+ 加固件计数器（无需硬件）。
2. **A**：64→128 B（协议安全档），单变量 A/B，目标 bulk ≥1.5×。
3. **B**：SWD 运行时标定，目标 RTT −10%、SWD 段 −40%+。
4. **C**：失败快速化 + 100 次稳定性（0 挂死、≤1 s 可见）。
5. **D**：固定开销削减，目标 RTT 0.82×。
6. **E/G**：视实测结果决定是否挑战 246 B（需先解决短包边界）或 512 B + ESP-NOW v2 长帧。

---

## 6. 落地前必须实测的未知量

| # | 未知量 | 怎么测 | 影响 |
|---|---|---|---|
| Q1 | 空口+ACK 每命令真实耗时（当前估 0.2–0.7 ms，最大不确定项） | host 注册 `esp_now_send_cb` 记录 `esp_now_send`→回调耗时；slave 侧收/发时间戳 | 决定 RTT 0.8× 目标是否可行（§3.2） |
| Q2 | 反推分解（固定 1169 µs / SWD 42.4 µs/transfer / 有效时钟 54%） | 固件内分段计时（T2.2 计数器） | 决定 A/B 优先级 |
| Q3 | 单条 `mdw` 实际触发多少 SWD transfer | DAP 引擎计数 + openocd `-d3` | RTT 模型是否成立（§2.3 疑点：477 µs 时钟相关部分暗示 ~12 transfer） |
| Q4 | P=128 真实收益（预测 1.57×/2.0×） | 单变量 A/B，n≥30，p50/p99 | 决定是否继续往 246/512 走 |
| Q5 | 失败快速化后的恢复时间与挂死率 | 注入丢帧 + 100 次稳定性 | 验收门槛 |

## 7. 风险与不变量

- 全部数字是**推断**（±30–50%），不得写入验收基线；验收以实测为准。
- 128 B 改动面：`DAP_config.h`、`debug_probe.h`（静态断言）、`eub_vendord.{c,h}`、`bridge_priv.h`、`bridge_host.c`/`bridge_slave.c`、`swd.c`；openocd 无需改。
- 不得破坏：DAP 命令恰好执行一次、seq FIFO 语义、无线帧格式/启动锚点、五组合产物除目标改动外不变。
- 大包化后**失败代价上升**（每命令数据变多），必须与 C（快速失败）同批验证。

## 8. 证据索引

- 包大小协商/打包：`/tmp/ocd/src/jtag/drivers/cmsis_dap.c:1202-1246`、`:763-825`、`cmsis_dap_usb_bulk.c`
- 固件包大小常量：`DAP_config.h`、`eub_vendord.c`、`eub_vendord.h:16`、`bridge_priv.h`
- USB 多包能力：`usbd.c:1328-1356`、`dcd_dwc2.c:331,585`
- SWD 延迟标定：`DAP.h:329`、`DAP.c:46-49`、`SW_DP.c`
- 无线链路：`wireless_link.c`（含 `:74-79` 空口注释）、`wireless_link_priv.h`（`WLINK_MAX_PAYLOAD=246`）、`Kconfig.projbuild:295-335`
- 失败路径：`bridge_host.c`、`eub_vendord.c`、`eub_abort.c:15`
- 测量工具缺陷：`tools/perf/bench.py:92-109,147,186-187,242-253,377-384,528`
- 原始数据：`docs/perf.md` §2.4/§3/§3.3/§3.6/§5
- ESP-NOW 速率能力（§9）：`esp_wifi_types_generic.h:1483-1489`（`wifi_tx_rate_config_t` = phymode/rate/ersu/dcm）、`:670-673`（`WIFI_PHY_MODE_11B/11G/HT20/HT40`）、`esp_now.h:86`（`typedef wifi_tx_rate_config_t esp_now_rate_config_t;`）、`esp_now.h:36`（`ESP_NOW_MAX_DATA_LEN_V2 1470`）
- TCP 可靠性参数（§9.7，IDF v6.0.2 内置 lwIP）：`lwip/src/include/lwip/opt.h:1384`（`LWIP_TCP_RTO_TIME 3000`）、`lwip/src/include/lwip/priv/tcp_priv.h:116-126`（`TCP_TMR_INTERVAL 250`、`TCP_SLOW_INTERVAL 500`）、`lwip/src/core/tcp.c:163-165`（backoff `{1,2,3,4,5,6,7,…}`）、`tcp.c:1285-1304`（`rtime >= rto` 才重传 + RTO 指数退避）

## 9. 附：如果改用 Wi-Fi（IP）链路，能有多大提升？（推测）

**前提定义**：把 ESP-NOW 换成标准 802.11 + lwIP（UDP 或 TCP socket；host softAP/STA、slave STA，或两边接同一 AP）。

**结论：不能获得「较大」性能提升。预期吞吐在噪声内（−5%~+10%），RTT 大概率变差（+10~40%），稳定性风险上升。**
理由五条：

### 9.1 瓶颈不在空口，换传输层不动瓶颈
§3.3 已量化：24 Mbps 下空口每命令 0.2–0.7 ms（12–42%），其中约 98% 与速率无关（前导、ACK、SIFS、退避、任务/协议栈）。而每命令固定开销（USB-FS 帧量化 ≈1 ms、停等、任务链 1.17–1.61 ms）与命令粒度（48 B/命令）都与传输层无关。→ **换 Wi-Fi 只能动那 12–42% 里的一小部分。**

### 9.2 PHY 速率上限也救不了
- 即便数据速率 24 → 72 Mbps（S3 可 11n HT20 MCS7；C6 受 20 MHz 限制，11ax OFDMA/ERSU 因 host 只支持 11n 用不上），每帧数据段 34 µs → 11 µs，**每命令只省 46 µs = 2.7%**——前导+ACK+退避不随速率缩。
- 而且**不换传输层就能试**：`esp_now_rate_config_t` 就是 `wifi_tx_rate_config_t`（`esp_wifi_types_generic.h:1483-1489`），phymode 支持 `WIFI_PHY_MODE_HT20/HT40`、rate 支持 MCS0–7。零架构改动、1 小时可 A/B，预期收益 <3%。

### 9.3 换 IP 协议栈是净损耗
- 每帧多 28 B（IP 20 + UDP 8）+ LLC/SNAP 8 B（与 ESP-NOW 厂商头同量级）→ 24 Mbps 下每帧 airtime +9~13 µs（+1~1.5%/命令）。
- 多一层 lwIP + socket：每包多 2–3 次任务切换与拷贝，估 +100–300 µs/方向 = **+0.2–0.6 ms/命令（+12–36%）**；TCP 更糟：ACK-only 帧翻倍空口帧数，delayed ACK/Nagle/重传定时器是 ms 级——对 1–2 ms 的请求-响应是灾难。
- 连接管理：AP/STA 关联、beacon（默认 100 ms）、DHCP/静态 IP、断线重连；S3/C6/C3 都只有 2.4 GHz，5 GHz 无收益；若中间过家用 AP，还多一层转发排队/重传，**RTT p99 更难控**。

### 9.4 换传输层换不来「大帧/流水线」这两个真收益
- 吞吐由命令粒度锁死（§3.1）：大帧收益来自 CMSIS-DAP 包大小（P=128 → 1.57–2.0×），与传输层无关。
- 需要 >246 B 时，ESP-NOW v2 单帧已达 1470 B（`ESP_NOW_MAX_DATA_LEN_V2`），无需 Wi-Fi。
- 流水线（多命令在途）是 relay 固件设计问题，ESP-NOW 同样可做。

### 9.5 方案对比表

| 方案 | 吞吐（bulk-read，实测基准 27.9 KiB/s） | RTT | 稳定性 | 改动面/风险 |
|---|---|---|---|---|
| ESP-NOW@24M（现状） | 27.9 | 2084.5 µs | 24M 零丢帧 | — |
| ESP-NOW + HT20/72M（零架构实验） | ~28.5–29（+2~3%） | 近乎不变 | 高 MCS 对 SNR 更敏感 | 小（速率档 + 1 处 config） |
| ESP-NOW + P=128（瓶颈①） | **43.9–55.8（1.57–2.0×）** | 不变 | 中立 | 中 |
| Wi-Fi + UDP（softAP/STA） | ~26–29（−5~+10%） | **+0.2–0.6 ms（+10–36%）** | 中（重连/信道） | 大（协议栈、配网、构建矩阵） |
| Wi-Fi + TCP | ~20–27（ACK/重传损耗） | 更差（Nagle/重传定时器） | 差（超时语义变长） | 大 |
| Wi-Fi + 大帧 + 流水线重构 | 1.5–3×（需协议重构） | 中 | 中 | 很大；**ESP-NOW v2 + 同样重构等效且改动更小** |

### 9.6 建议
- **不作为性能杠杆**：优先级远低于 A（包 128 B）、B（SWD 标定）、C（快速失败）、D（固定开销）。
- 唯一值得的「速率实验」：在 ESP-NOW 上直接配 HT20/MCS7 做 1 次 A/B（预期 <3%，用于闭环证据）。
- 若目标是「PC 端免 USB 转接线 / 多主机共享 / 远程网络接入」，那是**架构需求**（可给 ESP-NOW 加网络中继），与性能无关，另行立项。

### 9.7 TCP 的重传/校验/「断点续传」适合这个场景吗？

**结论：可靠性特性听起来对症，但对「低延迟、失败要快、命令级恰好一次」的调试桥来说，TCP 的方向是反的。** 逐项拆：

| 用户关注点 | TCP 提供什么 | 本场景实际需要 | 判定 |
|---|---|---|---|
| 重传 | 字节流重传，但恢复慢：lwIP 初始 RTO **3 s**（`opt.h:1384` `LWIP_TCP_RTO_TIME 3000`），重传只在 **500 ms** 慢定时器里检查（`tcp_priv.h:116-126`：`TCP_TMR_INTERVAL 250`、`TCP_SLOW_INTERVAL 500`），指数退避 `{1,2,3,…7}`（`tcp.c:163-165`）→ 丢 1 段首次恢复约 **0.5–3 s**，后续 3→6→12 s | 丢帧后 **≤1 s 显式报错**（T3.1 验收），并保留快速重试 | ❌ 把失败「藏起来」且拖长 |
| 队头阻塞 | **单连接字节流**，丢 1 段阻塞其后全部数据 | 每命令独立、可快速失败/重试 | ❌ 55 KB dump 中间丢一段 → 整条流停 0.5–3 s，openocd 秒级超时先炸 |
| 校验 | TCP 16-bit checksum | 已有 802.11 FCS（CRC32，每帧硬件校验）+ USB bulk CRC16 + ESP-NOW 载荷 | ❌ 冗余；且实测 P1 缺陷是**丢帧不是位翻转**（全 0/0xAA 55 KB 通过） |
| 断点续传 | **TCP 只续传同一连接的字节流**；连接一断（对端重启/重连）连接作废，**不能续传** | 一次 target flash 操作的状态在目标侧算法里，CMSIS-DAP 无此语义 | ❌ 要「续传」得改 DAP/openocd 协议，与传输层无关 |
| 恰好一次 | TCP 只保证字节不重不漏，**不保证命令只执行一次**（断连重发可重复执行） | DAP 命令**绝不重复执行** | ❌ 反需再叠应用层请求 ID（= 现有 seq FIFO 已做的事） |
| 失败可见性 | keepalive 默认关，无 RST 时靠重传超时发现（可达数十秒） | 2.9–6.2 s → ≤1 s 可见错误 | ❌ 更不可控 |

关键点：**桥现在其实已经实现了 TCP 可靠性里「有用的那一半」**——`bridge_host.c` 用同一 seq 重发（250 ms×3）、`bridge_slave.c` 用 32 深 seq FIFO + 应答缓存做去重与应答重放，等效于「消息级重传 + 恰好一次」，但没有 Nagle/队头阻塞/3 s RTO/握手/重连。换 TCP 是**用更强的传输语义换更差的时延与失控的失败检测**，收益为负。

**若真想要更强的抗丢帧**，应改应用层而不是换 TCP：
1. 用 `esp_now_send_cb` 确认帧是否真的发出（现在是盲等）；
2. 超时 250→100 ms、重试 1–2 次，失败即返回错误（≤1 s）；
3. 连续丢帧时自动降速 24→11 Mbps（11 Mbps 实测 5/5 不丢），恢复后再升；
4. 加帧级计数器（丢/重/超时）暴露可观测性。

> 例外（可以作为架构选项、但不是性能选项）：若未来要做「网络接入的调试探针」（PC 通过网络连）或大流量 SWO/trace 旁路通道，那里允许几百 ms 级延迟，TCP 是自然选择；但 PC↔桥这一段必须仍是 USB CMSIS-DAP（openocd 不支持网络 CMSIS-DAP），TCP 只能替换内部那一跳。
