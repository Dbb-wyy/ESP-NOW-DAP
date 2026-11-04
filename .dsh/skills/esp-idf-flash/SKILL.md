---
name: esp-idf-flash
description: 把 END_nightly 已构建的固件烧录到开发板并抓取启动日志。必须先读构建配置确认目标芯片与角色、探测串口并报告，得到用户明确确认后才执行 flash；烧录后按角色核对启动日志。
whenToUse: 用户要求烧录、上板验证或抓启动日志时。
---

# 烧录技能（END_nightly）

烧录有硬件副作用（烧错板/角色需要人工恢复）。本技能强制 **先报告、后确认**：没有用户明确的「确认 / 烧 / OK」回复，禁止执行 flash。

## 0. 硬性边界

允许：

- 探测串口（`/dev/serial/by-id`、`esptool --port ... flash-id`）
- 读取构建产物与配置（`build_r_*/config/sdkconfig.json`、`flash_args`）
- `idf.py ... flash`
- 抓串口日志（pyserial 读 12 秒；或用户在场时 `idf.py monitor`）

禁止：

- `esptool erase-flash`、改 efuse/分区、手拼 `write-flash` 地址（一律走 `idf.py flash`）
- 写死端口（`/dev/ttyUSB0` 之类必须来自真实枚举）
- 在构建产物缺失或过期时烧录
- 烧录失败后自动重试 / 自动换端口 / 自动换板
- 改源码、改配置（那是代码/文档阶段的动作）

## 1. 确定目标：变体 → 构建目录

输入应明确：变体 ∈ `wired` / `host` / `slave_s3` / `slave_c6` / `slave_c3`（定义见 esp-idf-build 技能 §2）。

- 构建目录 `build_r_<variant>`，配置 `sdkconfig.r_<variant>`。
- 用户说「烧 build/」= 历史目录（C6 slave SWD）；只有用户明确说要烧它才用，且先按 §2 读它自己的 `build/config/sdkconfig.json` 确认身份。
- 无法确定变体 → 问用户，不猜。

## 2. 读固件身份（构建目录必须在，且比源码新）

```bash
python3 - <<'EOF'
import json, os, sys
d = sys.argv[1] if len(sys.argv) > 1 else "build_r_wired"
cfg = json.load(open(f"{d}/config/sdkconfig.json"))
roles = [k for k, v in cfg.items() if k.startswith("BRIDGE_ROLE_") and v is True]
print("target:", cfg.get("IDF_TARGET"), "| roles:", roles, "| led:", cfg.get("BRIDGE_LED_ENABLE"))
for f in ("bridge.bin", "flash_args", "bootloader/bootloader.bin", "partition_table/partition-table.bin"):
    print(f, os.path.exists(f"{d}/{f}"))
EOF
```

- 断言 `target` 与变体表一致，`roles` 恰好一个且与变体一致。
- 产物缺任一 → 先按 esp-idf-build 技能构建，不烧。
- 新鲜度：`build_r_<v>/bridge.bin` 的 mtime 应晚于 `main/`、`components/` 最近改动；否则先增量构建并报告。

## 3. 探测串口（真实枚举，不猜）

```bash
ls -l /dev/serial/by-id/ 2>/dev/null; ls -1 /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

- 无设备 → 报告「未检测到串口设备」，停止，等用户插板。
- 有设备 → 逐个探测芯片：

```bash
source ~/.espressif/tools/activate_idf_v6.0.2.sh
esptool --port <PORT> flash-id 2>&1 | head -20
```

  读 `Chip type:`（如 ESP32-S3 / ESP32-C6 / ESP32-C3）与 `MAC:`。

- esptool v5 命令是连字符形式（`flash-id`、`read-mac`、`chip-id`），`esptool.py` 已弃用，只用 `esptool`。
- 多设备：逐个 `flash-id`，结合用户说明选定；仍不确定 → 问用户。
- 特别提示：若端口是**桥自身的 USB（CDC/OTG）**，不能用它烧桥；必须用开发板的 USB-UART 口（README 的 PORT 定义）。

## 4. 报告并等待确认（强制步骤）

输出表格：

| 变体 | 构建目录 | 固件 target | 固件角色 | 固件时间 | 端口 | 探测 chip | 结论 |
|---|---|---|---|---|---|---|---|

结论只允许两种：

- `可以烧录`（固件 target = 探测 chip，角色符合变体）
- `不可烧录：<原因>`（不一致 / 产物缺失 / 端口不明）

然后**停止**，等用户明确回复（「确认」「烧吧」「OK」等）。未得到确认前不得执行 §5。

## 5. 执行烧录

```bash
source ~/.espressif/tools/activate_idf_v6.0.2.sh
V=wired; D="sdkconfig.defaults;sdkconfig.defaults.wired"
idf.py -B build_r_$V -D SDKCONFIG=sdkconfig.r_$V -D SDKCONFIG_DEFAULTS="$D" -p <PORT> flash 2>&1 | tee build_r_$V/flash_skill.log
```

成功判据：exit 0，且日志中每段含 `Hash of data verified.`，结束行 `Leaving...` / `Hard resetting via RTS pin...`。

失败：**不自动重试**，按 §7 汇报。常见原因对照：

- `Failed to connect to ESP32...` → 让用户手动进入下载模式（按住 BOOT，点按 RST，松开 BOOT）后再确认
- `Permission denied` → 不在 dialout 组 / 端口被占用（monitor 未退出）
- 端口在烧录中消失 → 板子复位或换口，需重新探测
- `MD5 of file does not match` → 产物损坏，重新构建

## 6. 抓启动日志（烧录成功后立即做）

优先用 pyserial（非交互、可脚本化）：

```bash
source ~/.espressif/tools/activate_idf_v6.0.2.sh
python3 - <<'EOF'
import serial, time
port = "<PORT>"
s = serial.Serial(port, 115200, timeout=0.5)
t0 = time.time(); out = []
while time.time() - t0 < 12:
    d = s.read(4096)
    if d:
        out.append(d)
s.close()
print(b"".join(out).decode("utf-8", "replace"))
EOF
```

备选（用户在场、需要交互）：`idf.py -B build_r_<v> -p <PORT> monitor`（退出 `Ctrl+]`）。

启动判据（全部满足）：

- 通用：出现 `Free heap after start-up: N bytes`；无 `Guru Meditation`、`abort()`、`Stack canary`、`Brownout`、`Backtrace:`
- 无线从机（`slave_*`）：出现 `ESP USB Bridge running as wireless slave (UART + SWD over ESP-NOW)`
- 无线主机（`host`）：出现 `ESP USB Bridge running as wireless host (USB <-> ESP-NOW)`
- `wired`：无角色行，只看通用判据（`Mounted` 仅在 USB 枚举后出现，不作硬判据）
- 附带：`Status LED configuration rejected` 说明 LED 引脚配置被拒（不正常），必须报告

日志保存到 `build_r_<v>/boot_skill.log`（gitignore 覆盖）。

## 7. 失败/异常处理

报告包含：变体、身份表、端口、完整命令、exit code、错误段、启动日志关键行、已排除项。

硬件恢复动作（BOOT/RST、换线、断电重上）只提示用户手动做，Agent 不代为操作（Agent.md R4/R7）。

## 8. 汇报格式

| 步骤 | 结果 | 证据 |
|---|---|---|
| 固件身份 | ... | target / role 行 |
| 端口探测 | ... | by-id 行 + Chip type |
| 烧录 | ... | verified / Leaving 行 |
| 启动 | ... | 命中的判据 |

末尾一句话：是否上板通过、是否有异常行。
