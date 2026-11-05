---
name: swd-debug
description: 通过 ESP-NOW-DAP 桥（S3 host 的原生 USB CMSIS-DAP）用 OpenOCD + gdb-multiarch 调试外部 SWD 目标（如 STM32）。覆盖 DAP 枚举核对、单次连接测试、常驻 gdb server、gdb 会话、非破坏性校验、故障排查与汇报。任何"用桥做 SWD/JTAG 调试"的场合先读本技能。
whenToUse: 用户要求通过本仓库的无线桥调试外部 SWD 目标、跑 openocd、或用 gdb-multiarch 连接 :3333 时。
---

# swd-debug — 用 ESP-NOW-DAP 桥调试外部 SWD 目标

> 实测环境：2026-11-05，openocd 0.12.0（`/usr/bin/openocd`）、gdb-multiarch 15.1、目标 STM32F103C8T6（p01 固件）。
> 本技能只覆盖「桥当 CMSIS-DAP 用」的调试链路，不覆盖 ESP32 自身的烧录/日志（那是 `esp-idf-flash`）。

## 0. 何时用 / 不用

用：
- 外部目标芯片（STM32 等）的 SWD/JTAG 调试、寄存器/内存查看、断点单步、`verify_image`
- 需要 openocd 或 gdb server 的场合

不用：
- 烧录/抓日志 ESP32 桥自身 → `esp-idf-flash` / `esp-idf-build`
- 需要高速大批量烧写外部目标（无线链路实测约 26 KiB/s @500 kHz，大文件请直接有线 DAP）

## 1. 前置条件（先核对，缺一不可）

1. **S3 host 板**：运行 host 角色固件，**原生 USB 口**（不是 CH340 口）插到 PC。
2. **从机板**（C6/C3）：运行 slave 角色固件，SWD 接口已启用，SWD 引脚（默认 C6 TCK=GPIO4、TMS=GPIO5）接到目标板 SWD 引脚，**GND 共地**。
3. 目标板已上电、SWD 引脚未被禁用（部分芯片需要先解除读保护 / 关闭 WDG）。
4. 两侧桥在同一信道并已配对（桥启动日志里有 `Link up ... channel N` 与 `Peer discovered`）。

核对命令：

```bash
lsusb | grep -i '303a:1002'                     # 期望：Espressif ... CMSIS-DAP
ls -l /dev/serial/by-id/ | grep -i cmsis-dap    # 期望：...CMSIS-DAP_<序列>-if00 -> ttyACMx
```

> 注意：CH340（`1a86:*`）只是控制台/烧录口，**不承载 DAP**；DAP 只走 S3 原生 USB。

## 2. 环境

- 用系统 openocd：`/usr/bin/openocd`（0.12.0），脚本在 `/usr/share/openocd/scripts/`
- 用 ESP-IDF 的 openocd 也可以，但脚本路径不同（`~/.espressif/tools/openocd-esp32/*/openocd-esp32/share/openocd/scripts`）
- **不要**先 `source activate_idf_*.sh` 再跑 openocd（可能把 `openocd` 指向 ESP 版本并污染脚本搜索路径）；本技能与 IDF 环境无关
- gdb 客户端：`/usr/bin/gdb-multiarch`

## 3. 三步流程

### 步骤 1：一次性连接测试（必须通过才继续）

```bash
cd <elf 所在目录>
timeout 30 openocd \
  -f interface/cmsis-dap.cfg \
  -f target/<target>.cfg \
  -c "adapter speed 100" \
  -c "init; halt; exit"
```

- 起始速度用 **100 kHz**；确认稳定后可升到 **500 kHz**（已验证）
- 期望看到：`CMSIS-DAP: SWD supported`、`SWD DPIDR ...`、`Cortex-M3 ... processor detected`、`halted due to debug-request`
- 出现 `Warn : Using CMSIS-DAPv2 interface 2 with wrong class 255 subclass 255 or protocol 1` 是**良性告警**（桥把 DAP 接口声明为 vendor class），不影响调试

### 步骤 2：常驻 gdb server（后台）

```bash
cd <elf 所在目录>
openocd -f interface/cmsis-dap.cfg -f target/<target>.cfg \
  -c "adapter speed 100" > /tmp/openocd_gdb_server.log 2>&1 &
```

- 等日志出现 `Listening on port 3333` 再连
- 一次只开一个 server；用完必须杀掉（否则 3333 被占、下次连的是旧连接）

### 步骤 3：gdb-multiarch 会话

```bash
gdb-multiarch -q build/<firmware>.elf \
  -ex "set pagination off" -ex "set confirm off" \
  -ex "target extended-remote :3333" \
  -ex "monitor reset halt" \
  -ex "info registers pc sp lr" \
  -ex "bt" \
  -ex "break main" \
  -ex "continue" \
  -ex "bt" \
  -ex "info registers pc" \
  -ex "x/5i \$pc" \
  -ex "detach" -ex "quit"
```

常用片段：

| 目的 | 命令 |
|---|---|
| 复位并停在复位向量 | `monitor reset halt` |
| 复位后直接运行 | `monitor reset run` |
| 读变量 | `print <var>`（需 ELF 带调试符号） |
| 读外设寄存器 | `print huart1.Instance->SR` 或 `x/1wx 0x40013800` |
| 读内存/向量表 | `x/8wx 0x08000000`、`mdw 0x08000000 8` |
| 源码级单步 | `next` / `step` / `stepi` |
| 断点 | `break <file>:<line>` / `break <func>`；flash 上自动用**硬件断点** |
| 非破坏性固件校验 | `verify_image build/<firmware>.elf` |
| 退出 | `detach`（保留目标运行）→ 再杀 server |

**推荐校验链（不写 flash）**：

```bash
-ex "monitor reset halt" -ex "reg pc" -ex "reg sp" \
-ex "verify_image build/<firmware>.elf" \
-ex "mdw 0x08000000 8"   # 向量表：首字=初始 SP，次字=复位入口|1
-ex "monitor reset run"
```

## 4. 期望输出（STM32F103C8 实测基线）

| 项目 | 值 |
|---|---|
| SWD DPIDR | `0x1ba01477` |
| 内核 | `Cortex-M3 r1p1`，6 断点 / 4 watchpoint |
| device id | `0x00006410`（STM32F103C8，64 KiB flash） |
| 复位后 PC / SP | `0x080020a4` / `0x20005000` |
| 向量表首 8 字 | `20005000 080020a5 08001161 08001163 ...` |
| 校验吞吐 | 8692 B：100 kHz ≈ 11.9 KiB/s；500 kHz ≈ 26.2 KiB/s |
| gdb 版本 | `GNU gdb (Ubuntu 15.1-1ubuntu1~24.04.1)` |

## 5. 故障排查（按顺序）

| 现象 | 检查 |
|---|---|
| openocd 报 `no CMSIS-DAP device found` / 找不到 DAP | `lsusb` 是否有 `303a:1002`；S3 原生 USB 是否插上（不是 CH340）；host 固件是否在跑 |
| 能识别 DAP 但 `SWD DPIDR` 读不到 / `examine failed` | 目标供电与 GND；SWD 引脚接线（TCK/TMS）；从机 SWD 配置（`CONFIG_DEBUG_PROBE_IFACE_SWD`）；两桥是否配对、同信道；把速度降到 100 kHz 再试 |
| 读到 `0xffffffff` / `DAP error` | 芯片读保护（RDP）或 SWD 引脚复用冲突；目标在复位态（BOOT/RST 被拉低）；线太长/接触不良 |
| 偶发失败、重试后成功 | 无线链路抖动：降低 adapter speed、缩短杜邦线、确认信道干扰（可换信道 1–13） |
| `port 3333` 被占 / gdb 连上却异常 | 旧 openocd 未退出：`pgrep -a openocd` → 杀掉；`ss -ltn | grep 3333` 确认端口 |
| gdb `Remote connection closed` | server 崩了，看 `/tmp/openocd_gdb_server.log` 尾部 |

## 6. 边界与红线

- **默认只做读/校验**；`load`、`program`、`erase`、`mass_erase`、改 option bytes 必须**用户明确要求**才做，且执行前说明会写什么、地址范围
- 不修改目标芯片的读保护/选项字节；不擦用户数据除非被要求
- 用完必须 `detach` + 杀掉 openocd；不留后台 server
- 不把本技能用于 ESP32 自身烧录（那边用 `esp-idf-flash`）
- 无线 SWD 速度有限：大批量烧写建议走有线 DAP 或降低期望

## 7. 汇报模板

每次调试后向用户报告：

1. 链路：PC → S3(DAP, 序列) → ESP-NOW chN → 从机(SWD 引脚) → 目标芯片
2. openocd：版本、target cfg、adapter speed、DPIDR/内核/device id
3. gdb：连上的初始状态（运行/停在哪）、断点命中情况、读取的关键变量/寄存器
4. 是否写过 flash（写则列地址与字节数）；目标最终状态（运行/停止）
5. 清理：server 是否已关、端口是否释放
6. 异常与告警原文（如 CMSIS-DAP 类告警）

## 8. 已验证记录

- 2026-11-05：DAP 枚举（303a:1002, serial ECDA3B94B2F0）、SWD attach、`verify_image` 8692 B 通过、reset halt/run、gdb 断点与单步、`print` 变量与外设寄存器——全部成功
- 已知良性告警：`CMSIS-DAPv2 interface 2 with wrong class 255 subclass 255 or protocol 1`
