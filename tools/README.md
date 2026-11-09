# tools/

把开发板跑起来用的辅助脚本。它们不参与固件构建；`git` 跟踪它们，是为了让一份全新的检出
也能直接用。

## `flash.py` — 先认板子，再烧它该跑的变体

桥有五个变体，两块无线板还是同一颗芯片，所以 `/dev/ttyACM0` 说明不了任何事：每次重新枚举
端口号都会变。`flash.py` 先从板子自身取身份，取到了才写：

1. **枚举** `/dev/serial/by-id`（稳定名字）——`--list` 到这里就停，不碰板子；
2. **探测**芯片和 MAC，用 `esptool flash-id`（只读查询 ROM loader；板子会短暂复位进 loader）；
3. **解析变体**：芯片 + MAC 经 `tools/boards.json` 映射，再和编译进 `build_r_<variant>`
   的角色、目标芯片交叉校验；
4. **拒绝**：镜像比源码旧、构建报告的角色不符、或同型号两块板都插着且都没映射，都直接拒绝；
5. **打印表格并询问**——不按 `y` 什么都不会烧；
6. 通过 `idf.py -B build_r_<variant> -p <port> flash` 烧录，然后抓启动日志、核对角色锚点
   （以及 panic/brownout 特征）。

```bash
tools/flash.py --list                    # 只看端口，不探测
tools/flash.py --probe                   # 打印已插板子的芯片 + MAC
tools/flash.py --variant host            # 打印计划、询问、烧录
tools/flash.py --variant slave_s3 --yes  # 无人值守（CI、重复测试）
tools/flash.py --variant slave_s3 --learn  # 顺便记录 MAC -> variant
```

## 一条命令烧两块板

两块板都插上时，`--all` 会探测、按映射解析各自的变体，并**并行**烧录（每块板是独立的
USB 设备，两个 `idf.py` 不共享任何东西：`build_r_<variant>` 目录不同、`sdkconfig.r_<variant>`
也不同）：

```bash
tools/flash.py --all --dry-run     # 只看会烧什么，按板逐条打印
tools/flash.py --all               # 两块板，一次确认
tools/flash.py --all --yes         # 无人值守
tools/flash.py --pair              # 同上，但要求恰好一块 host + 一块 slave
tools/flash.py --all --jobs 1      # 串行烧（最慢、在 hub 上最稳）
```

每个 worker 自己烧、自己抓启动日志（`build_r_<variant>/boot_flash.log`）、自己核对变体锚点；
结尾按板汇总，所以一块失败不影响另一块的报告。不在映射里的板会带警告跳过；两块板映射到
同一个变体是硬错误。

注意：探测会把每块板复位进 ROM loader，所以烧完大约一秒后链路会自己重新配对（1 秒一次的
信标负责这件事）。

`tools/boards.json` 是学到的映射表。给板子加映射：用 `--learn` 烧一次，或直接编辑该文件
（MAC 小写，变体取 `wired|host|slave_s3|slave_c6|slave_c3`）。

退出码：`0` 完成，`1` 拒绝/失败，`2` 需要人决策（没板子、端口有歧义、未知板、`--pair`
时缺 host 或 slave）。

## `perf/` — 测量工具链

见 [`perf/README.md`](perf/README.md) 和 `docs/perf.md`（指标口径与基线）。

## `build_flash.sh` — 一步完成两块无线板的构建和烧录

`flash.py` 负责烧；`build_flash.sh` 先把正确的镜像做出来，再把两步绑在一起——因为 24 Mbps
构建依赖 per-role defaults 链，漏了这条链会静默编出 **1 Mbps** 固件。

```bash
tools/build_flash.sh                 # 构建 + 烧 host 和 slave_c6（默认）
tools/build_flash.sh host slave_c6   # 显式指定变体，按此顺序烧
tools/build_flash.sh --no-flash      # 只构建（例如跑 soak 测试前）
tools/build_flash.sh --no-build      # 烧现有的 build_r_<v> 镜像
tools/build_flash.sh --dry-run       # 只打印计划，什么都不碰
tools/build_flash.sh -y              # 不询问（flash.py --yes）
```

每个变体执行 `idf.py -B build_r_<v> -D SDKCONFIG=sdkconfig.r_<v>
-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.<role>" build`，然后断言
exit 0、出现 `Project build complete`、`sdkconfig.r_<v>` 里的 target/role/24M/channel
正确、且 `bridge.bin` 存在；随后调用 `tools/flash.py --variant <v> --yes`：它从
`/dev/serial/by-id` + `esptool flash-id` + `tools/boards.json` 推导板子身份，镜像过期或
角色不符就拒绝，烧完抓启动日志核对锚点（对端在线时包括
`ESP-NOW transmit rate ... 24 Mbps`）。第一个失败即停，探测/连接的瞬时错误重试一次，
最后打印按变体的构建/烧录汇总和 `RESULT: OK|FAILED`。

### 编译后裁剪（默认开启）

构建目录不做增量开发时，绝大部分体积是中间产物（`build_r_host` 约 157–234M），真正需要
留下的只有烧录和调试用的几件套（约 0.9M + 调试符号）：

```bash
tools/build_flash.sh --no-flash            # 构建后自动裁剪
tools/build_flash.sh --keep-build          # 保留完整构建目录（增量编译用）
tools/build_flash.sh --no-prune            # --keep-build 的别名
tools/build_flash.sh --gz-symbols          # 连 bridge.elf/.map 也 gzip 成 symbols/*.gz
```

裁剪保留：`bridge.bin`、`flash_args`/`flasher_args.json`、`config/sdkconfig.json`、
`bootloader/bootloader.bin`、`partition_table/partition-table.bin`、`bridge.elf`、
`bridge.map`、`symbols/`（`--gz-symbols` 时）以及 `*_skill.log`。烧录用 `flash.py`
读 `flash_args`；openocd/gdb 用 `bridge.elf` 查符号、`bridge.map` 查地址。实测：
`build_r_wired` 157M → 8.9M（省 147M），加 `--gz-symbols` → 2.5M（省 154M）；代价是下次
构建变成全量重编（每个组合约 1–2 分钟）。构建或烧录失败的变体**不裁剪**，保留现场。

### 脚本替你处理的两件事

* **IDF 激活脚本**——它只接受从 `$0` 看起来像 `bash`/`sh` 的 shell 里被 source，脚本直接
  source 会中止；`build_flash.sh` 先用 `bash -c` 重执行自己一次，让 `$0` 成为 `bash`。
  另外它的可选尾部步骤 `eim select` 在受限环境下会失败、让整个脚本返回 1，但环境其实已
  就绪，所以脚本以 `idf.py` 是否在 `PATH` 上为准，只把 rc 打成一注释；
* **串口语法**——`idf.py -D SDKCONFIG=...` 必须按变体设置，烧录绝不能写死 `ttyACM<N>`。
