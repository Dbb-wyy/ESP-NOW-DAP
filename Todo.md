# Todo.md — END_nightly 待办事项

> 用途：记录**尚未做**的事。与 Plan.md（阶段计划）、Agent.md（规则与经验）并列。
> 状态取值：`待办` / `进行中` / `阻塞`（等某条件）/ `完成`（完成后保留条目，标日期与 commit）。
> 新增条目按 T1、T2……顺序编号，不复用已完成的编号。

---

## T1 · 把从机 SWD 请求序号 FIFO 修复回流到 ./ESP-NOW-DAP/

- **状态**：阻塞（等 nightly 阶段 1 的 gate 由用户验证通过；用户 m00427：「先把 nightly 做好」）
- **来源**：END_nightly commit `d8d3afc`（nightly-p1.1），详见 Plan.md「阶段 1.1」与 Agent.md E5
- **为什么回流**：这是源仓库的遗留 bug（非 C3/C6 移植引入），不依赖任何 nightly 新增能力，对既有 S3/S2 用户同样有害 → 应回到上游。
- **目标**：在 ./ESP-NOW-DAP/ 里复现同一行为修复，单独一个 commit；不改动源仓库的其他任何逻辑。

### T1.1 具体修改（全部在 ESP-NOW-DAP/main/bridge_glue.c）

1. **新增两个宏**，紧跟 `#define BRIDGE_SWD_PKT_SIZE 64` 之后：
   - `#define BRIDGE_SWD_REQ_QUEUE_DEPTH 8`（原来 `xQueueCreate(8, ...)` 里的字面量 8）
   - `#define BRIDGE_SWD_SEQ_FIFO_DEPTH 32`
2. **删除** `static volatile uint8_t s_swd_pending_seq;`，**新增**在途请求的序号队列：
   - `static uint8_t s_swd_seq_fifo[BRIDGE_SWD_SEQ_FIFO_DEPTH];`
   - `static size_t s_swd_seq_fifo_head;` / `static size_t s_swd_seq_fifo_count;`
   - `static portMUX_TYPE s_swd_seq_mux = portMUX_INITIALIZER_UNLOCKED;`
   - 保留 `s_swd_last_seq` / `s_swd_last_valid` / `s_swd_last_resp[]` / `s_swd_last_resp_len`（缓存重发机制不变）
   - 四个静态辅助（均要求持锁）：`bridge_swd_seq_pending(uint8_t seq)`（线性查在途）、`bridge_swd_seq_push(uint8_t seq)`（满则 false）、`bridge_swd_seq_pop(uint8_t *seq)`（空则 false）、`bridge_swd_seq_drop_last(void)`（撤销末次 push）
3. **重写 `bridge_slave_swd_rx`**：
   - `len == 0 || len > BRIDGE_SWD_PKT_SIZE` → 直接丢弃并告警；
   - 命中 `s_swd_last_valid && hdr->seq == s_swd_last_seq` 时，**在锁内**把缓存拷到栈上 `cached[BRIDGE_SWD_PKT_SIZE]`，**出锁后**再 `wlink_send`（避免缓存撕裂）；
   - 「`bridge_swd_seq_pending` 查重 → `bridge_swd_seq_push` → `xQueueSend(..., 0)`」必须在**同一个临界区**内完成，入队失败则 `bridge_swd_seq_drop_last()` 回滚；
   - 日志一律在锁外按 duplicate / 队列未就绪或满 / 成功 三种情况输出。
4. **`bridge_slave_swd_rx_task`**：`debug_probe_process_data()` 失败时，在锁内 `bridge_swd_seq_pop()` 弹出最老序号（该请求永远不会有应答），否则后续应答会串号。
5. **`bridge_slave_swd_tx_task`**：不再读 `s_swd_pending_seq`，改为 `bridge_swd_seq_pop(&seq)` 取队头作为本响应的 seq，并在**同一临界区**内写 `s_swd_last_seq/resp_len/resp/valid`；`!have_seq` → 报错丢弃，`n == 0` → 记日志不发送，否则 `wlink_send(WLINK_MSG_SWD, 0, seq, ...)`。
6. **队列创建顺序**：把 `s_swd_req_q = xQueueCreate(...)` 从 `bridge_wireless_slave_start()` 移到 `bridge_uplink_init()` 的从机分支，**紧接在 `wlink_register_rx(WLINK_MSG_SWD, bridge_slave_swd_rx)` 之前**，并加 `if (!s_swd_req_q)` 幂等保护（原先回调先注册、队列后建，配对极快时首个请求会撞空队列被丢）。

### T1.2 不要跟着回流的部分

- nightly-p1（commit `05ab033`）的 USB 按角色条件编译：`main/CMakeLists.txt`、`main/main.c` 的 `#if !CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE`、`main/idf_component.yml` 的 tinyusb `rules`、`main/Kconfig.projbuild` 的 `depends on SOC_USB_OTG_SUPPORTED` —— 源仓库只服务 S3（有 USB OTG）从机不存在「无 USB 角色」，不适用。
- `CONFIG_WLINK_DEBUG_LINK_TEST`（connected N 心跳）：纯调试设施，除非用户要求，否则不加。
- 可选（用户决定）：`main/CMakeLists.txt` 里把 tinyusb 的 `public_include` 从 on role 判断改为「组件在 BUILD_COMPONENTS 中」判断（Agent.md E4）—— 对源仓库无功能影响，属顺手加固。

### T1.3 验证（回流后由用户执行，与 nightly 阶段 1.1 清单一致）

1. 两端同时冷启动，第一条命令即 SWD 烧录，连做 20 次首条成功。
2. `CONFIG_WLINK_SWD_VERBOSE_LOG=y` 下：首条日志为 `queued` → `response seq 0 sent`，没有 `already in flight`、没有约 750ms 停顿。
3. 回归：烧录中途从机复位后继续；两命令之间给从机断电再上电；整片烧录成功；S3 wired/host 角色行为不变。

### T1.4 注意事项

- 行号会随源仓库演进而漂移，**按函数名/字符串定位**，不要按行号硬改。
- 源仓库已有 `CONFIG_WLINK_SWD_VERBOSE_LOG` 与 `BRIDGE_SWD_PKT_LOG`，不需新增。
- 建议 commit 信息：`fix(swd): 用请求序号 FIFO 关联应答，修复首条 DAP 命令被误判为重复而丢弃`，并附「复现条件：两端同时上电的会话首条命令」。
- 回流后把本条目状态改为「完成」，记录 ESP-NOW-DAP 的 commit 号与日期。

---

## 条目模板（复制使用）

```
## Tn · <一句话目标>

- **状态**：待办 / 进行中 / 阻塞（等什么）/ 完成（commit + 日期）
- **来源**：哪个 commit / 哪次讨论（引用消息号）
- **为什么做**：动机，或不做会怎样
- **具体修改**：文件 + 函数 + 关键改动点（够另一个人照着做）
- **不要做的部分**：明确划界，防止顺手扩大范围
- **验证**：用户可执行的检查步骤
```

---

## 与其他文档的关系

- Plan.md：阶段化执行计划与 gate 判据。
- Agent.md：开发规则、红线、失败经验。
- Todo.md：本文件，尚未做、且不属于当前阶段 gate 的事（如回流、清理、可选加固）。
- 三者冲突时以用户当次裁定为准。
