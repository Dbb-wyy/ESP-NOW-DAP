# tools/perf — 性能测量工具（Plan_v3 T1.1）

`bench.py` 用一条命令跑出 `docs/perf.md` §1 定义的三个口径之一，输出 JSON + Markdown
（含参数快照、原始样本、n/min/p50/mean/p95/max、每次失败的分类与重试记录）。
**不属于固件构建**，不改 `main/` 与 `components/`。

## 环境

已建好的 conda 环境（Python 3.12 + pyserial，用于 `--recover` 的 RTS 复位）：

```bash
mamba activate endperf
# 或直接用解释器绝对路径
/home/dbb/miniforge3/envs/endperf/bin/python tools/perf/bench.py --check
```

重建环境：

```bash
mamba create -y -n endperf python=3.12 pyserial
```

`bench.py` 只用标准库（`subprocess`/`statistics`/`json`/`argparse`/`re`/`hashlib` 等）；
pyserial 仅在 `--recover` 时用于给 S3 控制台发 RTS 脉冲，缺失时该功能降级为「不恢复」。

外部依赖：`openocd`（0.12.0）、可选 `gdb-multiarch`；USB 权限需能打开 S3 的 CMSIS-DAP 节点。

## 用例

```bash
# 环境自检（不碰硬件）
tools/perf/bench.py --check

# 打印将执行的 openocd 命令，不运行
tools/perf/bench.py flash-program --elf test/shaobing_gimbalchangeauthor.elf --dry-run

# 1) 单次 64 B DAP 往返 —— 微秒
tools/perf/bench.py rtt --target f4x --swd-khz 2000 --reps 30

# 2) 链路批量读（dump_image，按给定字节数，唯一真实字节搬运口径）—— KiB/s
tools/perf/bench.py bulk-read --target f4x --swd-khz 2000 --len 32768 --reps 5

# 3) 链路批量写 RAM（load_image）—— KiB/s
tools/perf/bench.py bulk-write-ram --target f4x --swd-khz 2000 --len 32768 --reps 5

# 4) 烧录端到端（program ... verify reset）—— 秒
tools/perf/bench.py flash-program --target f4x \
    --elf test/shaobing_gimbalchangeauthor.elf --reps 10 --timeout 45 --recover
```

## 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--swd-khz` | 2000 | SWD 时钟（基准 2000，见 Plan_v2 §10.2.1） |
| `--target` | `f4x` | `f1x` / `f4x`，决定 cfg 与 RAM 默认地址/长度 |
| `--reps` | rtt 30 / bulk 5 / flash 10 | 有效样本数（flash 为成功次数） |
| `--timeout` | 120 s | 单个 openocd 进程超时；超时即判挂死并 kill |
| `--retries` | 1 | 挂死/报错后的额外重试次数（计入 attempts） |
| `--recover` | 关 | 挂死后对 S3 控制台发 RTS 脉冲复位（会触发重新配对） |
| `--host-console` | 自动 | 主机控制台 tty；默认按 `/dev/serial/by-id` 里 S3 的 CH340 序列号取 |
| `--vid-pid` | `0x303a 0x1002` | 显式指定 CMSIS-DAP，避免多探针误选 |
| `--out-dir` | `perf_out` | JSON/Markdown 输出目录（已 gitignore） |

## 失败分类

| 分类 | 含义 | 处置 |
|---|---|---|
| `ok` | 正常完成 | — |
| `hang_during_program` | 停在 `Programming Started` 之后、永不返回 | **P1 缺陷**（relay 丢事务后两端不重同步）；`--recover` 或手动复位 S3；外部必须用 `timeout -k` |
| `hang_before_target` | 未到目标就挂住 | 同上 |
| `dap_dead` | `CMSIS-DAP command CMD_INFO failed`（~6 s 返回） | host vendor 端点 wedge，需复位 S3 |
| `target_error_erase` / `target_error_write` | `failed erasing sectors` / `flash write failed = 0x40` | 缺 `reset init`（F4 必须）；是目标侧配置问题，不是链路 |
| `swd_margin_error` | `Error setting register xPSR` | SWD 时钟过高，降频重测 |

## 与计划的关系

- 口径定义：`docs/perf.md` §1（D1）；基线数值：`docs/perf.md` §2.4。
- 每个 A/B 实验：改一处（commit）→ 用同一用例、同一参数跑 before/after → 把两行填进 `docs/perf.md` §4。
- 脚本改动本身不是性能证据；只有数据行才算。
