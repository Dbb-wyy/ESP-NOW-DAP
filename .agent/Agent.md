# Agent.md — END_nightly 开发规则与失败经验

> 版本：v1.3（2026-10-31）
> 位置：`.agent/Agent.md`（与 `Plan.md`、`Todo.md` 同在 `.agent/`）。
> 与 Plan.md 并列，是开发过程的**强制性约束**。
> 优先级：用户当次指令 > Plan.md / 本文件的明确规则 > 一般工程习惯。
> 任何规则冲突或需要偏离，先停下、报告、由用户裁定，并同步更新本文件。

---

## 0. 角色与边界

- Agent 只写代码/文档；**禁止编译、烧录、运行测试**。所有构建、烧录、功能验证由用户（「我」）完成。
- Agent 只在 Plan.md 定义的「阶段」内产出改动；每个阶段以用户验证为 gate，未通过不得进入下一阶段。
- 改动限定在会话工作区 /home/dbb/Workspace/DSH_Workspace 内；工作区外只读查阅（如 IDF、源仓库），不写。

---

## 1. 规则

### R1 意外即停（用户规则 1）

遇到任何**预料外的问题**，立即停止当前执行，不自行猜测、不绕路重试。向用户报告：

1. 现象（具体报错/日志/可观察现象）；
2. 触发条件与上下文（阶段/文件/命令）；
3. 已排除的可能（可选）；
4. 建议方案（至少 2 个，并给出默认推荐）。
   等候用户处理或裁定后再继续；同时把该问题写入第 2 节「失败经验」。

### R2 阶段门禁

- 严格按 Plan.md 的阶段推进；每阶段结束只做「移交验证」，**不自行判定通过**。
- 用户在阶段验证报错时，Agent 只在该阶段内修复并说明改动，不跨阶段做「顺手」修改。

### R3 计划即契约

- Plan.md 是唯一执行依据。若必须偏离（例如某项「零改动」的组件实际要改），先停下（触发 R1），说明偏离原因，由用户决定是否修订 Plan.md；修订同步后再继续。
- Agent.md 记录的规则/经验变化，同样先征得用户同意再写入（或按用户指示直接写）。

### R4 不绕过沙箱/权限

- 沙箱或权限拒绝时，**不换路径、不换方式重试**；按工具指引以最小权限（workspace-write）+ 中文理由申请一次。
- 被拒绝则停止并说明原因，不做 workaround。

### R5 改动最小、可回滚

- 每个阶段一个 commit，提交信息用 Plan.md 约定的 nightly-pN。
- 优先 surgical edit（定向修改），避免大范围重写；确需重写，先在聊天中说明范围。
- 多处编辑后必须回读/校验关键字符串，确认无「半成品」状态。

### R6 文档同步

- Plan.md / Agent.md 与代码同步；规则或经验变化立即追加到 Agent.md。
- Plan.md 变更用修订号（vN）标注并在头部记录。

### R7 禁止破坏性操作

- 不执行删除/移动（rm/mv），除非用户明确要求且已核对绝对路径。
- 不使用 git reset --hard / git clean 等不可逆命令，除非用户明确授权。
- 不覆盖用户手写的非本项目文件。

---

## 2. 失败经验（持续追加，按时间倒序）

格式：日期/阶段 · 现象 · 根因 · 教训 · 修正。

### E7（2026-10-31 · 阶段2 · 凭「源码大小直觉」写体积风险，被实测证伪）

- **现象**：阶段 2 首次构建成功（C3 854,920 B / C6 954,226 B）。核对后发现计划风险表里「`debug_probe` 全量进 IRAM 会吃掉 C3 余量、约 98 KB」与实测差一个数量级：`libdebug_probe.a` 的 `text` 合计只有 8,381 B，C3 上真正落进 `iram0_text` 的是 7,598 B，整个 `iram0_text` 也只有 83,968 B。
- **根因**：写风险条目时用的是「组件源码目录大小」的直觉，没有按链接产物核算；同时又低估了 `-ffunction-sections` + `--gc-sections` 与 `#if` 剪枝的效果（`JTAG_DP.c`/`esp_usb_jtag.c` 编出来是空对象）。
- **教训**：① 任何写进文档的「体积/占用」数字都必须来自产物（`idf.py size`、`riscv32-esp-elf-size libX.a`、链接 `.map`），不能来自源码行数或目录大小。② 读 ELF/map 时先分清**占位段**：C3 的 `.dram0.dummy` 有 83,968 B、`.flash_rodata_dummy` 有 655,360 B，它们是对齐填充而不是真实占用，直接用 `readelf -S` 求和会把 DRAM 高估 3 倍。③ 判断「分区放不放得下」要拿 `Total image size` 对比**实际分区表**（本仓库是默认 `partitions_singleapp`，`factory` 1 MB），而不是对比 flash 容量。
- **修正**：Plan.md 升 v8，把风险条目改为「已实测证伪」并补上真实数字，新增「C6 已占 app 分区 91 %」的风险与缓解（改 `partitions.csv`）；阶段 2 增加「体积数据」表作为后续阶段基线。

### E6（2026-10-31 · 阶段2 · 组件 REQUIRES 用了角色判断，C6/C3 上 tinyusb 解析失败）

- **现象**：`idf.py -B build_c6 set-target esp32c6` 直接停在配置阶段：
  `CMake Error at .../tools/cmake/build.cmake:380 (message): Failed to resolve component 'tinyusb' required by component 'main': unknown name.`
  S3 同样代码却正常。
- **根因**：`main/CMakeLists.txt` 把 `tinyusb`/`usb` 追加进 `REQUIRES` 的条件写成了角色判断 `if(NOT CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE)`。但组件的 `REQUIRES` 是由 **早展开**（`tools/cmake/scripts/component_get_requirements.cmake`，由 `build.cmake` 的 `__component_get_requirements` 在 `__kconfig_generate_config` / `__build_import_configs` **之前**调用）读取的；该脚本只 include 构建属性文件与各组件 CMakeLists.txt，**从不 include sdkconfig.cmake**，所以那一趟 `CONFIG_*` 全是空字符串 → `NOT ""` 为真 → 从机也被要求 tinyusb。而 `main/idf_component.yml` 的 rules 只在 S2/S3 提供 tinyusb，C3/C6 上没有这个组件，"unknown name" 即在解析 REQUIRES 时抛出（`__build_resolve_and_add_req`）。
- **教训**：① CMakeLists 里的内容分两类——早展开可见且被采纳的（`REQUIRES`、`PRIV_REQUIRES`、`INCLUDE_DIRS`、`KCONFIG`/`KCONFIG_PROJBUILD`）与只有真实 pass 才生效的（`SRCS`、`LDFRAGMENTS`、任何 `target_*`/`idf_build_*` 调用）。**前者只能用构建属性（`IDF_TARGET` 等）判断，后者才可以用 `CONFIG_*`**。② 「在 S3 上一直能构建」不代表判断是对的：S3 恰好有 tinyusb，把角色判断写错这件事完全掩盖了；跨芯片 target 才把它暴露成硬错误。③ 见到 "Failed to resolve component 'X' required by component 'Y'"，先查两件事：X 的 manifest/kconfig target 规则是否排除当前 target，以及 Y 把 X 写进 REQUIRES 的条件是不是用了早展开看不到的变量。
- **修正**：把两类判断拆开——源码仍按角色（`if(NOT CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE)` 追加 `eub_vendord.c`/`serial_bridge.c`/`usb_phy.c`），依赖改为按 target：

  ```cmake
  set(bridge_usb_stack FALSE)
  if(IDF_TARGET STREQUAL "esp32s2" OR IDF_TARGET STREQUAL "esp32s3")
      set(bridge_usb_stack TRUE)
  endif()
  if(bridge_usb_stack)
      list(APPEND dependencies "tinyusb" "usb")
  endif()
  ```

  S3 三个角色语义不变（wired/host 有源码+依赖；slave 无 USB 源码但仍 REQUIRES tinyusb/usb，与阶段 1 gate 通过时一致），C3/C6 两者皆无。

### E5（2026-10-31 · 阶段1.1 · 从机 SWD 首请求丢失，源仓库遗留）

- **现象**：ESP-NOW 已配对时，偶见「第一次 SWD 烧录失败、重跑就好」。开 `CONFIG_WLINK_SWD_VERBOSE_LOG=y` 后首条命令的日志是 `already in flight, ignored`，且前面没有一条 `queued`。
- **根因**：主机请求序号从 0 起（`static uint8_t s_seq`、`return s_seq++`），从机用 `hdr->seq == s_swd_pending_seq` 判断「重复请求」，而 `s_swd_pending_seq` 的静态初值也是 0 → 新鲜上电会话的首条请求与初值碰撞被丢弃；主机三次重传都用同一个 seq，因此首条命令必丢。连带三处缺陷：pending 从不复位（语义退化为「等于最后入队值」）、响应序号取「出队那一刻的 pending」导致主机跳号后串号、队列在回调注册之后才创建。
- **教训**：① 用数值（序号/索引/计数）表示「有无在途」时，0 或 1 这类合法值会与静态初值撞车——要么加显式 valid 标志，要么用先进先出列表表达「在途集合」。② 异步应答（DAP 引擎在自己的任务里回包）**不能靠读「当前请求」关联响应**，必须按接受顺序排队，应答时弹出队头。③ 初始化顺序即协议的一部分：先建设施（队列）再注册回调，否则内存里的「极小窗口」在真实时序上稳定命中。④ 复现条件要写进报告：「偶见」的真实含义是「仅两端同时上电的会话首条」，不解释这点会被误判成硬件抖动。
- **修正**：`main/bridge_glue.c` 从机侧改为请求序号 FIFO（push 于接受时、pop 于应答/拒绝时），并新增宏 `BRIDGE_SWD_REQ_QUEUE_DEPTH`/`BRIDGE_SWD_SEQ_FIFO_DEPTH`；队列创建移至 `bridge_uplink_init` 注册回调之前。详见 Plan.md 阶段 1.1。

### E4（2026-10-31 · 阶段1 · USB 按角色条件编译）

- **现象**：S3 wireless slave 构建失败于 managed_components/espressif__tinyusb/src/tusb_option.h:245:12: fatal error: tusb_config.h: No such file or directory。
- **根因**：idf_component.yml 的 rules 只按 target / idf_version 求值，不认 role。S3 从机构建的 target 仍是 esp32s3，tinyusb 照样进构建图并被编译；而它的配置头 include 路径原先写在 if(NOT CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE) 里，从机时没被加到 tinyusb 目标上。tusb_config.h 实际在 main/public_include/，不在 managed_components 里。
- **教训**：① 「从机不需要 USB」只在**链接期**成立，构建图上 managed 依赖仍按 target 存在；给第三方组件补 include/宏定义时，一律用「该组件是否在 BUILD_COMPONENTS 中」判断，不要用 role 判断。② 反向验证法：若 role 不是 slave，main 会 REQUIRES tinyusb 并自行加 include 路径 — 报错本身即证明 role=slave 且组件由 manifest 拉入。
- **修正**：main/CMakeLists.txt 改用 idf_build_get_property(build_components BUILD_COMPONENTS) + if("espressif__tinyusb" IN_LIST build_components) 后再 target_include_directories(${tusb_lib} PRIVATE public_include)，与角色解耦。

### E1（2026-10-31 · 文档编写）

- **现象**：用 run_code 生成 Plan.md 时抛 `Identifier cannot follow number` 解析错误。
- **根因**：在 JS 模板字符串内放了未转义的反引号（markdown 行内代码），字符串提前终止。
- **教训**：写含反引号的 markdown 时，用「字符串数组 join(换行)」或 quoted heredoc 传递内容，不要用原生模板字符串直接包。
- **修正**：改用数组 join 生成内容。

### E2（2026-10-31 · Plan 修订 v3）

- **现象**：一次 run_code 内连续 6 个 edit，前 3 个成功、第 4 个授权被拒 → 整批中止；Plan.md 处于半更新（§0 已是 v3，阶段 0/附录仍是 v2），前后矛盾。
- **根因**：多步写操作共享一次程序执行，任一步的授权拒绝会抛 ToolCallError 中断后续；写文件没有事务性回滚。
- **教训**：① 授权是逐步的，批量编辑要预期中途可能被拒；② 每次编辑后应校验，不要假设整批原子成功；③ 文档类修改优先「一次全量 write」或「少量 edit + 立即 grep 断言」。
- **修正**：补齐剩余 3 处 edit，并用 grep 断言「旧串 0 处、新串在位」。

### E3（2026-10-31 · 会话上下文）

- **现象**：连续三次「申请修改 Plan.md」，用户怀疑是崩溃循环。
- **根因**：read-only 文件策略下，每次 write/edit 都需单独 one-shot 提升到 workspace-write，故每轮弹一次授权。
- **教训**：开发期应把会话文件策略一次性切到 workspace-write；并在回复中明确「这是授权申请，不是崩溃/重试」。
- **修正**：后续写文件合并操作、先确认策略。

---

## 3. 每次开工前的检查清单

- [ ] 当前处于 Plan.md 的哪个阶段？该阶段 gate 是否已由用户确认通过？
- [ ] 本次改动是否只落在该阶段范围内？
- [ ] 是否有编译/测试动作？（必须无）
- [ ] 是否预见到授权/沙箱问题？文件策略是否已具备写权限？
- [ ] 改完是否回读/校验？
- [ ] 是否新增/改动了「带初值的静态状态变量」？其初值是否可能与首个真实值碰撞（E5）？
- [ ] 是否改动了初始化顺序（队列/回调/任务）？有无「回调先于设施」的窗口（E5）？
- [ ] 若在 CMakeLists 里用 `CONFIG_*` 决定 `REQUIRES`/`INCLUDE_DIRS`/`KCONFIG`，是否确认早展开阶段能看见它？（多半看不见，改用 `IDF_TARGET` 等构建属性，E6）
- [ ] 文档里要写的体积/占用数字，是否来自链接产物而非源码直觉？求和时是否排除了 `*_dummy` 占位段（E7）？
- [ ] 是否需要更新 Agent.md 失败经验？

---

## 4. 与 Plan.md 的关系

- Plan.md：**做什么、按什么阶段、每阶段的验证判据**。
- Agent.md：**怎么做、边界与红线、踩过的坑**。
- Todo.md：**尚未做、且不属于当前阶段 gate 的事**（回流上游、清理、可选加固）；做之前先看这里，完成后回填 commit 与日期，不要悄悄做掉。
- 二者冲突时：以用户当次裁定为准；裁定后同步修正两份文档，并在 Plan.md 头部/Agent.md 记录。
