---
name: esp-idf-build
description: 在本仓库（ESP-NOW-DAP / END_nightly）用 ESP-IDF v6.0.2 构建验收固件。覆盖五个组合（wired、host、slave_s3、slave_c6、slave_c3）的命令、成功判据、体积采集与失败处理。任何需要编译本仓库的场合都先读本技能。
whenToUse: 需要编译/验证构建 END_nightly 固件，或改动后要确认五个角色组合仍可编译时。
---

# ESP-IDF 构建技能（END_nightly）

本文件是**编译动作的唯一合法流程**。不属于本技能的动作（烧录、上板、改源码、改配置默认值）一律不做。

## 0. 硬性边界

允许：

- `source ~/.espressif/tools/activate_idf_v6.0.2.sh`
- `idf.py ... set-target / build / size`（仅本技能给出的参数形态）
- 读取 `sdkconfig.r_*`、`build_r_*`、源码与日志
- 新建/删除 `build_r_<variant>` 与 `sdkconfig.r_<variant>`（均被 `.gitignore` 覆盖）

禁止：

- 手改 `sdkconfig.defaults*`、`sdkconfig.r_*`、`build_r_*/` 内部文件、`CMakeLists.txt`、`Kconfig.projbuild`
- 任何烧录/串口/monitor 命令（那是 esp-idf-flash 技能）
- `git checkout/reset/clean`、删除工作区源码
- 同时对两个变体跑 `idf.py`（CMake 缓存互踩）
- 失败后自动换参数重试（猜测式重试）

失败即停：按 §7 汇报，交回用户。

## 1. 环境激活（每条命令所在 shell 都要做）

```bash
source ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py --version          # 期望：ESP-IDF v6.0.2
```

- 必须用 `activate_idf_v6.0.2.sh`（EIM 生成，把 `idf.py` 定义成 shell 函数）。**不要**用 `~/.espressif/v6.0.2/esp-idf/export.sh`：本机它指向不存在的 `~/.espressif/python_env/idf6.0_py3.12_env`，会直接报错。
- 每个新 shell 都是干净的：命令写成 `bash -lc 'source ~/.espressif/tools/activate_idf_v6.0.2.sh; idf.py ...'` 这种自带激活的形式，或先 `source` 再跑。
- 首次为某目标配置时会联网拉取组件（S2/S3 的 tinyusb 等）；断网会失败，按 §7 处理。

## 2. 构建矩阵（固定，不得自创变体）

| variant | 目标芯片 | defaults 组合 | 说明 |
|---|---|---|---|
| wired | esp32s3 | `sdkconfig.defaults;sdkconfig.defaults.wired` | USB 有线桥，含 MSC |
| host | esp32s3 | `sdkconfig.defaults;sdkconfig.defaults.wireless_host` | 无线主机（PC 侧） |
| slave_s3 | esp32s3 | `sdkconfig.defaults;sdkconfig.defaults.wireless_slave` | 无线从机（S3） |
| slave_c6 | esp32c6 | `sdkconfig.defaults;sdkconfig.defaults.wireless_slave` | 无线从机（C6） |
| slave_c3 | esp32c3 | `sdkconfig.defaults;sdkconfig.defaults.wireless_slave` | 无线从机（C3） |

命名约定（flash 技能共用，勿改）：

- 构建目录：`build_r_<variant>`
- 生成配置：`sdkconfig.r_<variant>`

不要动仓库里已有的 `build/`（历史构建，C6 slave SWD）。

## 3. 构建命令

### 3.1 首次构建（`build_r_<variant>` 不存在时）

```bash
source ~/.espressif/tools/activate_idf_v6.0.2.sh
V=wired; T=esp32s3; D="sdkconfig.defaults;sdkconfig.defaults.wired"
idf.py -B build_r_$V -D SDKCONFIG=sdkconfig.r_$V -D SDKCONFIG_DEFAULTS="$D" set-target $T
idf.py -B build_r_$V -D SDKCONFIG=sdkconfig.r_$V -D SDKCONFIG_DEFAULTS="$D" build 2>&1 | tee build_r_$V/build_skill.log
```

`set-target` 会清空并重建该目录，**只在目录不存在时执行**；之后一律只 `build`。

### 3.2 增量构建（目录已存在）

```bash
source ~/.espressif/tools/activate_idf_v6.0.2.sh
V=wired; D="sdkconfig.defaults;sdkconfig.defaults.wired"
idf.py -B build_r_$V -D SDKCONFIG=sdkconfig.r_$V -D SDKCONFIG_DEFAULTS="$D" build 2>&1 | tee build_r_$V/build_skill.log
```

### 3.3 体积采集（构建成功后必须做一次）

```bash
V=wired; D="sdkconfig.defaults;sdkconfig.defaults.wired"
idf.py -B build_r_$V -D SDKCONFIG=sdkconfig.r_$V -D SDKCONFIG_DEFAULTS="$D" size 2>&1 | tee build_r_$V/size_skill.log
```

只引用该命令输出里的数字（IRAM/DRAM/Flash 的 used/total、`Total image size`），不从源码或链接日志猜测（E7）。

### 3.4 执行方式

- 构建通常 1–5 分钟。用后台任务或较长超时执行，结束后读完整日志；不要用极短超时打断（半成品 build 目录会污染下一次构建）。
- 同一时刻只允许一个构建任务。

## 4. 成功判据（四条全部满足）

1. `idf.py ... build` exit code 0，且日志尾部出现 `Project build complete`。
2. 生成的配置与变体一致：

```bash
grep -m1 '^CONFIG_IDF_TARGET=' sdkconfig.r_$V
grep -m1 -E '^CONFIG_BRIDGE_ROLE_(WIRED|WIRELESS_HOST|WIRELESS_SLAVE)=y' sdkconfig.r_$V
```

期望：

- wired → `CONFIG_IDF_TARGET="esp32s3"` / `CONFIG_BRIDGE_ROLE_WIRED=y`
- host → `"esp32s3"` / `CONFIG_BRIDGE_ROLE_WIRELESS_HOST=y`
- slave_s3 → `"esp32s3"` / `CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE=y`
- slave_c6 → `"esp32c6"` / `CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE=y`
- slave_c3 → `"esp32c3"` / `CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE=y`

3. 产物存在：`build_r_<variant>/bridge.bin`、`bootloader/bootloader.bin`、`partition_table/partition-table.bin`、`flash_args`。
4. 体积命令 exit 0，且输出含 `Total image size`。

warning 不阻断成功，但报告里必须写：warning 数量 + 首条 warning 原文。出现新的 `error`/`FAILED` 视为失败。

## 5. 什么时候跑哪些组合

- 日常：只构建本次改动涉及的组合（如只改无线从机代码 → slave_c6 + slave_s3）。
- 阶段收尾 / 用户要求 / Plan_v2 验收：全矩阵五个，**串行**跑，每个都过 §4。
- 报告里必须完整列出组合清单，不许只写「全过」。

## 6. 已知坑（出现时按此解释，不要瞎改）

- `CMake Error: The current CMakeCache.txt directory ... is different`：build 目录与仓库路径不一致（复制/移动过），报告后由用户决定是否重建。
- `Failed to fetch` / `git clone` 失败：组件管理器网络问题，停。
- `set-target` 后角色不对：检查是否漏了 `-D SDKCONFIG_DEFAULTS`（漏了会落成默认角色）。
- 两个变体共用一个 build 目录：禁止；一个变体一个 `build_r_*`。
- C6/C3 目标配置同样需要网络/工具链，失败按 §7 处理。

## 7. 失败处理

立即停止，报告：

- 变体、目标、完整命令（含 `-D` 参数）
- exit code、错误段（`error:` / `ERROR` / `FAILED` 行 + 前 20 行上下文）
- 已排除项（如「已确认 SDKCONFIG_DEFAULTS 未漏」）
- 不做参数猜测式重试；只有可确定的直接原因（如漏了 defaults）才修正一次并注明

## 8. 汇报格式（每次构建结束都发）

| 变体 | 目标 | 结果 | 角色断言 | IRAM | DRAM | Flash | 备注(warning) |
|---|---|---|---|---|---|---|---|

末尾一句话：全矩阵是否五过五、是否有 warning。
