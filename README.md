# ESP-NOW-DAP

**中文** | **[English](#english)**

---

# 中文

> 高价入手的无线调试器有严重缺陷，于是有了这个项目。

ESP-NOW-DAP 是一个基于 [ESP-IDF](https://github.com/espressif/esp-idf) 的项目，把电脑（PC）与目标单片机（MCU）之间的串口和 SWD 调试线「延长」到无线：桥接板用 USB 连 PC，目标侧板子通过 ESP-NOW 接收**同样的**串口与 SWD 两路数据，PC 不再需要与目标有线相连。两路数据可以**同时**使用。

- **有线模式（wired）**：一块 ESP32-S3 直接插在 PC 上，呈现 USB 复合设备——虚拟串口 + CMSIS-DAP 调试器，可替代 CP210x 之类的 USB 转串口芯片。
- **无线主机（host）**：插在 PC 上的 ESP32-S3，把 USB CDC 串口与 CMSIS-DAP 数据通过 ESP-NOW 转发给从机。
- **无线从机（slave）**：放在目标旁边，只接目标板的串口、SWD（SWCLK/SWDIO）与 BOOT/RST，**完全不使用自己的 USB 口**，因此 ESP32-C6 / ESP32-C3 这类无 USB-OTG 的芯片也能当从机。

无线模式下不需要按键、绑定或配对流程，两块板上电后自动发现；`esptool`、`idf.py monitor`、OpenOCD 的用法与有线模式完全一样——CDC 的 line coding 与 DTR/RTS 复位时序都会隧道到从机，目标可以在 PC 侧不做任何改动的情况下被烧录和调试。

本项目派生自 Espressif 的 [ESP USB Bridge](https://github.com/espressif/esp-usb-bridge)（EUB），详见[来源与致谢](#来源与致谢)。上游自带的「大容量存储 / UF2 拖拽烧录」链路已在本仓库的重构中删除，现在只保留串口桥、SWD 桥与无线桥三件事。

## 编译

编译需要 [ESP-IDF](https://github.com/espressif/esp-idf) v5.0 或更新版本（当前实测的是 v6.0.2），请先阅读 ESP-IDF 文档完成环境搭建。

- `idf.py set-target` 用于选择目标芯片，当前支持三种：**ESP32-S3**（有线 / 无线主机 / 无线从机）、**ESP32-C6**（无线从机）、**ESP32-C3**（无线从机）。
- `idf.py menuconfig` 用于修改配置，本项目相关的设置在 **END-config** 子菜单下。
- `idf.py build` 编译固件。
- `idf.py -p PORT flash monitor` 烧录并打开串口监视器。注意这里的 PORT 是连接**桥接芯片串口**（不是桥接芯片原生 USB 口）的 USB 转串口芯片所创建的串口，仅在烧录桥接板自身时需要；烧录完成后，桥接芯片通过 USB 口工作。

初次烧录也可以用其他方式完成，参考[这份文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/usb-otg-console.html#uploading-the-application)。

### 角色与目标芯片

`END-config -> Role configuration -> Firmware role` 决定这份固件实现桥的哪一端。ESP32-S3 上默认是**无线主机**；ESP32-C6 / ESP32-C3 上默认是**无线从机**。

一次只编一个角色，在 menuconfig 里选好角色后正常编译即可：

```bash
idf.py set-target esp32s3
idf.py menuconfig      # END-config -> Role configuration -> Firmware role
idf.py build
```

需要同时保留多个角色时，给每个角色独立的 sdkconfig 与构建目录：

```bash
# 无线主机：放在 PC 旁边（ESP32-S3）
idf.py -B build_host -D SDKCONFIG=sdkconfig.host \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_host" set-target esp32s3
idf.py -B build_host -D SDKCONFIG=sdkconfig.host \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_host" build

# 无线从机：放在目标旁边（以 ESP32-C6 为例，C3 把 esp32c6 依次换成 esp32c3 即可）
idf.py -B build_slave_c6 -D SDKCONFIG=sdkconfig.slave_c6 \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_slave" set-target esp32c6
idf.py -B build_slave_c6 -D SDKCONFIG=sdkconfig.slave_c6 \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_slave" build

# 单板有线模式（仅 ESP32-S3，需要显式指定）
idf.py -B build_wired -D SDKCONFIG=sdkconfig.wired \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wired" set-target esp32s3
idf.py -B build_wired -D SDKCONFIG=sdkconfig.wired \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wired" build
```

芯片相关的默认值（引脚、Flash 容量、USB 标识、状态灯）在 `sdkconfig.defaults.esp32s3` / `sdkconfig.defaults.esp32c6` / `sdkconfig.defaults.esp32c3` 里，IDF 会按 `set-target` 自动追加，不需要写进 `SDKCONFIG_DEFAULTS`。

所有组合共用根目录的 `partitions.csv`：`nvs` 24K、`phy_init` 4K，其余全部给 factory app（4032K，`0x10000`–`0x400000`），即 app 分区用满 4MB Flash。

两个无线角色都要求使用 CMSIS-DAP SWD 调试接口；角色默认配置文件已经选好，若缺少该配置，编译会以明确的错误停止。

### 五种构建组合与验证状态

| 组合 | 芯片 | 角色 | 默认值 | 说明 |
| --- | --- | --- | --- | --- |
| `wired` | ESP32-S3 | 有线 | `sdkconfig.defaults;sdkconfig.defaults.wired` | 单板有线：USB CDC + CMSIS-DAP |
| `host` | ESP32-S3 | 无线主机 | `sdkconfig.defaults;sdkconfig.defaults.wireless_host` | 插 PC，转发串口与 SWD |
| `slave_s3` | ESP32-S3 | 无线从机 | `sdkconfig.defaults;sdkconfig.defaults.wireless_slave` | 目标侧从机（S3 冗余方案） |
| `slave_c6` | ESP32-C6 | 无线从机 | `sdkconfig.defaults;sdkconfig.defaults.wireless_slave` | 目标侧从机，体积更小 |
| `slave_c3` | ESP32-C3 | 无线从机 | `sdkconfig.defaults;sdkconfig.defaults.wireless_slave` | 目标侧从机，体积更小 |

截至 2026-11，以上五种组合均已在 ESP-IDF v6.0.2 下从零构建通过；ESP32-S3 主机与 ESP32-C3 / ESP32-C6 从机已上板验证串口日志、ESP-NOW 配对与启动流程。SWD 烧录吞吐已实测（见[无线性能](#无线性能)）；距离、穿墙与串口吞吐仍待测。

## 来源与致谢

本仓库是 Espressif **[ESP USB Bridge](https://github.com/espressif/esp-usb-bridge)** 的派生作品。

所有让桥接在 USB 上工作的部分都来自该项目：USB 复合设备、CDC 串口桥、CMSIS-DAP SWD 调试传输、DTR/RTS 到 BOOT/RST 的复位逻辑，以及 magic baud 下载模式。上游还包含大容量存储 / UF2 拖拽烧录器，这部分已在本仓库中删除（见下文「与上游的差异」）。

本仓库新增的是无线层，以及让角色切换成为可能的中间层：

- `components/wireless_link/` —— 带对端发现与按流分发的 ESP-NOW 链路
- `components/bridge/` —— 在 USB、目标串口/SWD 引脚与链路之间做角色分发的中间层（`bridge.c` 共享链路 + `bridge_wired/host/slave.c` 每角色一个文件）
- `docs/wireless.md` —— 无线部分的设计说明

所有源自 EUB 的文件中，上游版权声明均原样保留；本仓库不携带上游仓库的提交历史。

与上游的差异：

- 删除了大容量存储（假 U 盘）与 UF2 拖拽烧录链路，以及它带来的 `esp-serial-flasher` 依赖；替换为一条极简的目标串口初始化实现，行为不变。
- 新增无线主机 / 无线从机两种角色，以及 ESP32-C6 / ESP32-C3 从机构建支持。
- 新增可选的 WS2812 状态灯，显示链路与流量状态。

另外，感谢 viric 在 Espressif 的 ESP USB Bridge 项目中提交的 [PR #40](https://github.com/espressif/esp-usb-bridge/pull/40)。其中关于 `dedic_gpio` 下 SWD 输出无法正确关闭的问题及其修复思路，帮助解决了本项目开发过程中的关键问题。

## 开发板

桥接端使用一块 **ESP32-S3** 板子（有线或无线主机角色），与目标单片机的串口、SWD 与复位引脚相连；从机端可以用 ESP32-S3、ESP32-C6 或 ESP32-C3。引脚号可以在 `idf.py menuconfig` 中修改，本仓库的默认值：

| 信号 | ESP32-S3 | ESP32-C6 | ESP32-C3 |
| --- | --- | --- | --- |
| UART TXD | GPIO5 | GPIO7 | GPIO1 |
| UART RXD | GPIO6 | GPIO6 | GPIO2 |
| BOOT | GPIO4 | GPIO10 | GPIO6 |
| RST | GPIO7 | GPIO9 | GPIO7 |
| SWD TCK | GPIO17 | GPIO4 | GPIO4 |
| SWD TMS / SWDIO | GPIO18 | GPIO5 | GPIO3 |
| SWD TDI（仅 JTAG） | GPIO3 | — | — |
| SWD TDO（仅 JTAG） | GPIO9 | — | — |
| 状态灯 | GPIO48 | GPIO8 | 不启用（-1） |

## 串口桥

USB 协议栈会创建一个虚拟串口，通过它访问目标单片机的串口。该串口在操作系统下可能是 `/dev/ttyACMx` 或 `COMx`，与烧录桥接芯片所用的 PORT 不是同一个。

例如，可以用下面的命令烧录并监视一个 ESP32 目标：

```bash
cd AN_ESP32_PROJECT
idf.py build
idf.py -p /dev/ttyACMx flash monitor
```

[esptool](https://github.com/espressif/esptool) 或任何终端程序、串口助手也都可以连接这个虚拟串口。目标板支持 ROM 下载模式时，向该串口发送 1200 波特率再关闭，桥会据此把目标的 BOOT/RST 拉进下载模式。

## SWD 调试桥

ESP-NOW-DAP 使用 CMSIS-DAP 协议提供 ARM Serial Wire Debug（SWD）设备。

在 OpenOCD 一侧，CMSIS-DAP 相关的更新会持续从主线同步到 Espressif 的分支，因此始终建议使用最新的 release 或 master。

下面的命令可以访问目标单片机的 SWD 口：

```bash
openocd -s tcl -f interface/cmsis-dap.cfg -f target/<target.cfg>.cfg -c 'adapter speed 5000'
```

请把 `target.cfg` 换成实际的目标配置文件。

目前只支持 USB bulk 传输作为后端，不支持 HID 后端。

## 无线桥

两块运行本固件的板子可以通过 ESP-NOW 桥接**同样的**串口与 SWD 数据，PC 不再需要与目标有线相连。串口与 SWD 可以同时使用。

```
 PC ──USB── ESP32-S3「无线主机」 ══ESP-NOW══ 「无线从机」(S3 / C6 / C3) ──UART+SWD── 目标 MCU
```

- **无线主机**插在 PC 上。它的 USB CDC 口与 CMSIS-DAP bulk 接口会被转发到链路上。
- **无线从机**与目标的串口、SWD（SWCLK/SWDIO）以及 BOOT/RST 引脚相连。它完全不使用自己的 USB 口，因此 ESP32-C6 / ESP32-C3 这类没有 USB-OTG 的芯片也能胜任。
- 两块板子会自动配对，并且必须设置成相同的 `END-config -> Wireless link -> Wi-Fi channel`（默认 1）。不需要按键或绑定流程。

`esptool`、`idf.py monitor` 与 OpenOCD 的用法和 wired 模式完全一样：CDC 的 line coding 与 DTR/RTS 复位时序都会被隧道到从机，因此目标可以在 PC 侧不做任何改动的情况下被烧录和调试。

```bash
# 通过主机板做 SWD 调试
openocd -f interface/cmsis-dap.cfg -f target/esp32s3.cfg -c 'adapter speed 1000'

# 同时使用串口
idf.py -p /dev/ttyACMx monitor
```

编译方法见[角色与目标芯片](#角色与目标芯片)；协议、SWD 重传模型、引脚配置与已知限制记录在 [docs/wireless.md](docs/wireless.md) 中。

### 无线性能

目前只试过主机放「电梯口」、从机放「实验室」，能正常烧录。（乐）距离上限、穿墙能力和串口吞吐仍待测。

**SWD 烧录吞吐（实测，2026-11-05）**：S3 主机（CMSIS-DAP）经 ESP-NOW 无线连接到 C6 从机，从机 SWD 接 STM32F103C8（目标 flash 64 KiB）。

| 指标 | 实测值 |
|---|---|
| 最大可用 SWD 时钟 | ≈5.95 MHz（6 MHz 起目标 flash 写入算法中止） |
| 写入吞吐（64 KiB 镜像） | ≈12.2 KiB/s |
| 校验吞吐（64 KiB 镜像） | ≈61 KiB/s |
| 整片 64 KiB 烧录 + 校验 + 复位 | 6.8–7.2 s |
| 8.7 KiB 小镜像烧录 + 校验 + 复位 | ≈2.0 s |

参考：STM32F103 用有线 ST-Link 通常可达 20–50 KiB/s，无线链路约为其 1/2–1/4，适合调试与小固件迭代，大固件仍建议有线 DAP。

## 状态灯

可选的一颗 WS2812（灯带上的第一颗也可以）用来显示桥在做什么，主机、从机与有线（wired）角色的活动判定标准完全一致：

| 状态 | 颜色 | 效果 |
| --- | --- | --- |
| 无线角色：ESP-NOW 未连接 | 黄 255,180,0 | 闪 |
| 有线角色：上电空闲 | 白 255,255,255 | 呼吸 |
| SWD 与串口同时在用 | 紫 140,0,255 | 闪 |
| SWD 调试会话中 | 蓝 0,80,255 | 闪 |
| 串口有流量 | 绿 0,255,0 | 闪 |
| 无线角色：已连接、空闲 | 青 0,180,180 | 呼吸 |

优先级从上往下：断开的黄色压过一切，「两路都在用」压过单路。无线角色上电后还没配对时看到的是黄闪，配对成功、两路都安静下来之后转成青呼吸；有线角色没有链路概念，上电即为白呼吸，有 SWD/串口活动时按上表切换。

蓝色看的是**调试会话**而不是某一条命令：CMSIS-DAP 的 Connect 命令打开它，Disconnect 命令关闭它。这样 target 停住时调试器可以几分钟不发一个包（你正在看代码），蓝灯照样亮着；退出调试器则立刻回落。串口没有会话概念，仍然是「最近有字节流过」。默认不设超时，只有 Disconnect 能结束蓝色会话；若你的调试器被强杀（来不及发 Disconnect），可以在同一菜单里给 SWD session timeout 填一个毫秒数做兜底，代价是真正空闲的会话也会在那么久之后结束。

板级默认值里已经配好：ESP32-S3 用 GPIO48、ESP32-C6 用 GPIO8（都是 devkit 板载的那颗 RGB 灯），亮度 16，三个角色通用；ESP32-C3 的默认配置不启用（引脚保持 -1），其他板子同理。要改就在：

```
idf.py menuconfig
END-config -> Pin configuration -> Status LED
  [*] Enable the WS2812 status LED
  (48) GPIO connected to the WS2812 data input (-1 = none)
```

引脚填 `-1` 等于关灯：LED 引脚因板而异，填错的数字会去驱动别的功能脚。
填成已经分配给 UART、BOOT/RST 或 SWD 的引脚会被 `status_led_init()` 拒绝并打日志，桥本身照常工作。
其余观感参数（整体亮度、活动保持时间、闪烁周期与亮占比、呼吸周期与最暗百分比、呼吸 gamma）都在同一个菜单里，可按实际板子调；链路判定超时在 `END-config -> Wireless link -> Peer timeout`。
呼吸曲线默认 gamma 2.2（`Breathing gamma (x100)` = 220），作用是把线性三角波换成视觉上等速的亮度变化；不想要就填 100 回到线性，觉得低端太暗就调到 150 左右。
亮度决定可用级数：青在 brightness 64 时整个呼吸只有 0..45 共 46 个码，直接四舍五入会出现肉眼可见的台阶，所以渲染器做逐帧抖动，把台阶摊成占空比。但抖动本身在级数太少时会变成几赫兹的闪烁（brightness 8 时一个码就是峰值的 17%），所以峰值不足 16 个码时会自动退回四舍五入（默认的 brightness 16 属于这一档，呼吸是 11 级微台阶、不闪烁）。也就是说极低亮度下「台阶」和「闪烁」只能二选一——想两样都没有，请把 `Overall brightness` 提到 32 以上（1 个码 ≤ 4%）。
驱动使用托管组件 `espressif/led_strip`（RMT 后端），由组件管理器自动拉取。

wired 角色同样支持状态灯（默认随 `sdkconfig.defaults.esp32s3` 用 GPIO48）：它不采样 ESP-NOW 链路，空闲时呼吸白色，SWD/串口活动与无线角色一致；源码里由 `status_led_set_local(true)` 声明。

## 许可证

本项目基于 Apache License 2.0 授权，见 [LICENSE](LICENSE)。

项目中的代码来自多个来源，每一处上游声明都原样保留在对应文件的 SPDX 头中：

- 源自 **ESP USB Bridge** 的部分：Copyright 2020-2026 Espressif Systems (Shanghai) Co Ltd.
- `components/debug_probe/DAP/` 下的 CMSIS-DAP 源码：Copyright (c) 2013-2022 ARM Limited.
- 无线层（`components/wireless_link/`、`components/bridge/`、`docs/wireless.md`）以及本仓库中的其他修改：Copyright 2026 Dbb.

## 贡献

欢迎以 bug 报告、功能请求与 pull request 的形式参与贡献，请使用本仓库的 issue tracker。

提交 pull request 时请遵循 ESP-IDF 项目的[贡献指南](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/contribute/index.html)。

本仓库没有 CI / pre-commit 门禁；提交前请至少跑通 `esp-idf-build` 技能里的五个构建组合（wired、host、slave_s3、slave_c6、slave_c3），涉及行为变化的改动再按 `esp-idf-flash` 技能上板冒烟。

---

<a name="english"></a>

# English

> The expensive wireless debugger I bought had serious flaws — so this project was born.

ESP-NOW-DAP is an [ESP-IDF](https://github.com/espressif/esp-idf) project that "extends" the serial and SWD debug lines between a computer (PC) and a target microcontroller (MCU) over the air: a bridge board is plugged into the PC over USB, while a board next to the target receives the **same** serial and SWD streams through ESP-NOW. The PC no longer needs a wired connection to the target, and the two streams can be used **at the same time**.

- **Wired mode**: a single ESP32-S3 plugged into the PC presents a USB composite device — a virtual serial port plus a CMSIS-DAP debugger — and can replace a USB-to-serial chip such as the CP210x.
- **Wireless host**: an ESP32-S3 next to the PC forwards its USB CDC serial port and CMSIS-DAP traffic to the slave over ESP-NOW.
- **Wireless slave**: a board next to the target, wired only to the target's UART, SWD (SWCLK/SWDIO) and BOOT/RST pins. It does not use its own USB port at all, so chips without USB-OTG such as the ESP32-C6 / ESP32-C3 can serve as slaves.

There is no button, binding or pairing procedure in the wireless roles: the two boards discover each other automatically. `esptool`, `idf.py monitor` and OpenOCD are used exactly as in wired mode — the CDC line coding and the DTR/RTS reset sequence are tunneled to the slave, so the target can be flashed and debugged without changing anything on the PC side.

This project is derived from Espressif's [ESP USB Bridge](https://github.com/espressif/esp-usb-bridge) (EUB); see [Origin and credits](#origin-and-credits). The upstream "mass storage / UF2 drag-and-drop flashing" chain has been removed in this repository's refactor; only the serial bridge, the SWD bridge and the wireless bridge remain.

## Compilation

Building requires [ESP-IDF](https://github.com/espressif/esp-idf) v5.0 or newer (v6.0.2 is what has been tested here). Please read the ESP-IDF documentation to set up the environment first.

- `idf.py set-target` selects the target chip. Three are supported: **ESP32-S3** (wired / wireless host / wireless slave), **ESP32-C6** (wireless slave) and **ESP32-C3** (wireless slave).
- `idf.py menuconfig` changes the configuration; the settings related to this project are under the **END-config** sub-menu.
- `idf.py build` builds the firmware.
- `idf.py -p PORT flash monitor` flashes the firmware and opens the serial monitor. Note that PORT is the serial port created by the USB-to-serial chip connected to the bridge chip's serial port (not the chip's native USB port), and is needed only for flashing the bridge board itself. After flashing, the bridge chip works through the USB port.

The initial flashing can also be done by other means; see [this guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/usb-otg-console.html#uploading-the-application).

### Roles and target chips

`END-config -> Role configuration -> Firmware role` selects which end of the bridge the firmware implements. On the ESP32-S3 the default is the **wireless host**; on the ESP32-C6 / ESP32-C3 the default is the **wireless slave**.

Build one role at a time by picking the role in menuconfig and building normally:

```bash
idf.py set-target esp32s3
idf.py menuconfig      # END-config -> Role configuration -> Firmware role
idf.py build
```

To keep several roles side by side, give each role its own sdkconfig and build directory:

```bash
# wireless host: sits next to the PC (ESP32-S3)
idf.py -B build_host -D SDKCONFIG=sdkconfig.host \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_host" set-target esp32s3
idf.py -B build_host -D SDKCONFIG=sdkconfig.host \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_host" build

# wireless slave: sits next to the target (ESP32-C6 shown; substitute esp32c3 for the C3)
idf.py -B build_slave_c6 -D SDKCONFIG=sdkconfig.slave_c6 \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_slave" set-target esp32c6
idf.py -B build_slave_c6 -D SDKCONFIG=sdkconfig.slave_c6 \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_slave" build

# single-board wired mode (ESP32-S3 only, selected explicitly)
idf.py -B build_wired -D SDKCONFIG=sdkconfig.wired \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wired" set-target esp32s3
idf.py -B build_wired -D SDKCONFIG=sdkconfig.wired \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wired" build
```

Chip-specific defaults (pins, flash size, USB identifiers, status LED) live in `sdkconfig.defaults.esp32s3` / `sdkconfig.defaults.esp32c6` / `sdkconfig.defaults.esp32c3`; IDF appends the matching file automatically after `set-target`, so it does not need to be listed in `SDKCONFIG_DEFAULTS`.

All combinations share the project-root `partitions.csv`: `nvs` 24K, `phy_init` 4K and the rest goes to the factory app (4032K, `0x10000`–`0x400000`), i.e. the app partition uses the whole 4 MB flash.

Both wireless roles require the CMSIS-DAP SWD debug interface; the role default files select it, and the build stops with an explicit error if it is missing.

### The five build combinations and what has been verified

| Combination | Chip | Role | Defaults | Purpose |
| --- | --- | --- | --- | --- |
| `wired` | ESP32-S3 | wired | `sdkconfig.defaults;sdkconfig.defaults.wired` | Single board: USB CDC + CMSIS-DAP |
| `host` | ESP32-S3 | wireless host | `sdkconfig.defaults;sdkconfig.defaults.wireless_host` | Plugged into the PC, forwards serial and SWD |
| `slave_s3` | ESP32-S3 | wireless slave | `sdkconfig.defaults;sdkconfig.defaults.wireless_slave` | Slave next to the target (S3 option) |
| `slave_c6` | ESP32-C6 | wireless slave | `sdkconfig.defaults;sdkconfig.defaults.wireless_slave` | Slave next to the target, smaller footprint |
| `slave_c3` | ESP32-C3 | wireless slave | `sdkconfig.defaults;sdkconfig.defaults.wireless_slave` | Slave next to the target, smaller footprint |

As of 2026-11 all five combinations build from scratch on ESP-IDF v6.0.2, and an ESP32-S3 host paired with an ESP32-C3 / ESP32-C6 slave has been verified on hardware (boot log, ESP-NOW pairing, startup sequence). SWD flashing throughput has been measured — see [Wireless performance](#wireless-performance); range, wall penetration and serial throughput are still unmeasured.

## Origin and credits

This repository is a derivative work of **[ESP USB Bridge](https://github.com/espressif/esp-usb-bridge)** by Espressif Systems.

Everything that makes the bridge work over USB comes from that project: the composite USB device, the CDC serial bridge, the CMSIS-DAP SWD transport, the DTR/RTS to BOOT/RST reset logic and the magic baud download mode. The upstream project also ships a mass storage / UF2 drag-and-drop flasher; it has been removed from this repository (see "Differences from upstream" below).

What this repository adds is the wireless layer and the plumbing that makes a role switch possible:

- `components/wireless_link/` — an ESP-NOW link with peer discovery and per-stream dispatch
- `components/bridge/` — the role dispatching layer between USB, the target UART/SWD pins and the link (`bridge.c` shared, `bridge_wired/host/slave.c` one file per role)
- `docs/wireless.md` — the wireless design notes

Upstream copyright notices are retained unchanged in every file that came from EUB; this repository does not carry the upstream commit history.

Differences from upstream:

- The mass storage (fake USB drive) and UF2 drag-and-drop flashing chain, together with its `esp-serial-flasher` dependency, has been removed; it is replaced by a minimal target-UART initialisation with identical behaviour.
- Two new roles, wireless host and wireless slave, plus ESP32-C6 / ESP32-C3 slave build support.
- An optional WS2812 status LED showing link and traffic state.

Additionally, thanks to viric for [PR #40](https://github.com/espressif/esp-usb-bridge/pull/40) in Espressif's ESP USB Bridge project. The issue with the SWD output under `dedic_gpio` not being disabled correctly, along with the fix approach described there, helped resolve a key problem during the development of this project.

## Development Board

A **ESP32-S3** board is used on the bridge side (wired or wireless host), connected to the target MCU's serial port, SWD and reset pins; the slave side can be an ESP32-S3, ESP32-C6 or ESP32-C3. The pin numbers can be changed in `idf.py menuconfig`; these are the defaults shipped by this repository:

| Signal | ESP32-S3 | ESP32-C6 | ESP32-C3 |
| --- | --- | --- | --- |
| UART TXD | GPIO5 | GPIO7 | GPIO1 |
| UART RXD | GPIO6 | GPIO6 | GPIO2 |
| BOOT | GPIO4 | GPIO10 | GPIO6 |
| RST | GPIO7 | GPIO9 | GPIO7 |
| SWD TCK | GPIO17 | GPIO4 | GPIO4 |
| SWD TMS / SWDIO | GPIO18 | GPIO5 | GPIO3 |
| SWD TDI (JTAG only) | GPIO3 | — | — |
| SWD TDO (JTAG only) | GPIO9 | — | — |
| Status LED | GPIO48 | GPIO8 | disabled (-1) |

## Serial Bridge

The USB stack creates a virtual serial port through which the target MCU's serial port is accessed. Under the operating system this port may be `/dev/ttyACMx` or `COMx`, and it is not the same PORT used for flashing the bridge chip.

For example, an ESP32 target can be flashed and monitored with the following commands:

```bash
cd AN_ESP32_PROJECT
idf.py build
idf.py -p /dev/ttyACMx flash monitor
```

[esptool](https://github.com/espressif/esptool), any terminal program or serial assistant can also connect to this virtual serial port. If the target supports the ROM download mode, opening this port at 1200 baud and closing it makes the bridge put the target's BOOT/RST pins into download mode.

## SWD Debug Bridge

ESP-NOW-DAP provides an ARM Serial Wire Debug (SWD) device using the CMSIS-DAP protocol.

On the OpenOCD side, CMSIS-DAP-related updates are continuously synced from the mainline to the Espressif fork. Therefore, it is always recommended to use the latest release or the latest master branch.

The following command can be used to access the SWD port on a target MCU

```bash
openocd -s tcl -f interface/cmsis-dap.cfg -f target/<target.cfg>.cfg -c 'adapter speed 5000'
```

Make sure to replace `target.cfg` with the actual target config file.

Currently, only USB-bulk transfers are supported as a backend. The HID backend is not supported.

## Wireless Bridge

Two boards running this firmware can bridge the **same** serial and SWD traffic over ESP-NOW, so the PC no longer has to be wired to the target. Serial and SWD work at the same time.

```
 PC ──USB── ESP32-S3 "wireless host" ══ESP-NOW══ "wireless slave" (S3 / C6 / C3) ──UART+SWD── target MCU
```

- The **wireless host** plugs into the PC. Its USB CDC port and its CMSIS-DAP bulk interface are forwarded over the link.
- The **wireless slave** is wired to the target's UART, SWD (SWCLK/SWDIO) and BOOT/RST pins. It does not use its USB port at all, which is why chips without USB-OTG such as the ESP32-C6 / ESP32-C3 can serve as slaves.
- The two boards pair automatically and must be set to the same `END-config -> Wireless link -> Wi-Fi channel` (default 1). No button or binding procedure is involved.

`esptool`, `idf.py monitor` and OpenOCD are used exactly as in wired mode: the CDC line coding and the DTR/RTS reset sequence are tunneled to the slave, so the target can be flashed and debugged without changing anything on the PC side.

```bash
# SWD debugging through the host board
openocd -f interface/cmsis-dap.cfg -f target/esp32s3.cfg -c 'adapter speed 1000'

# serial, in parallel
idf.py -p /dev/ttyACMx monitor
```

Build instructions are in [Roles and target chips](#roles-and-target-chips); the protocol, the SWD retransmission model, the pin configuration and the known limitations are documented in [docs/wireless.md](docs/wireless.md).

### Wireless performance

So far we've only tried the host at the "elevator entrance" and the slave in the "lab", and flashing works. (heh) Range, wall penetration and serial throughput are still unmeasured.

**SWD flashing throughput (measured 2026-11-05)**: an S3 host (CMSIS-DAP) paired with a C6 slave over ESP-NOW, the slave driving an STM32F103C8 (64 KiB flash) through SWD.

| Metric | Measured |
|---|---|
| Max usable SWD clock | ≈5.95 MHz (the target flash-write algorithm aborts at 6 MHz) |
| Write throughput (64 KiB image) | ≈12.2 KiB/s |
| Verify throughput (64 KiB image) | ≈61 KiB/s |
| Full 64 KiB program + verify + reset | 6.8–7.2 s |
| 8.7 KiB image program + verify + reset | ≈2.0 s |

For reference, a wired ST-Link on the same STM32F103 typically reaches 20–50 KiB/s; the wireless link is about 1/2–1/4 of that — fine for debugging and small-firmware iteration, while large images are better flashed with a wired DAP.

## Status LED

An optional WS2812 (the first LED of a strip is fine) reports what the bridge is doing. The host and the slave use exactly the same rules:

| State | Colour | Effect |
| --- | --- | --- |
| ESP-NOW link down | yellow 255,180,0 | flashing |
| SWD and serial both in use | violet 140,0,255 | flashing |
| SWD debug session open | blue 0,80,255 | flashing |
| serial traffic | green 0,255,60 | flashing |
| link up, idle | cyan 0,180,180 | breathing |

The table is also the priority order: "down" outranks everything and "both busy" outranks a single stream. So right after power-up, before the pair exists, the LED flashes yellow; once the peer is found and the link falls quiet it settles into the cyan breathing.

Blue is a **debug session**, not one command: the CMSIS-DAP Connect command opens it and Disconnect closes it, so a debugger sitting on a halted target can send nothing for minutes while the LED stays blue, and quitting the debugger drops the colour immediately. Serial has no session concept and stays "bytes seen recently". No timeout is set by default: only Disconnect ends a blue session. If your debugger gets killed before it can send Disconnect, set a millisecond value in `SWD session timeout` in the same menu as a safety net — at the cost of ending a genuinely idle session after that much silence.

The board defaults already configure it: GPIO48 on the ESP32-S3 and GPIO8 on the ESP32-C6 (the RGB LED each devkit carries) at brightness 16; the ESP32-C3 defaults leave it off with the pin at -1, as does every other board. To change it:

```
idf.py menuconfig
END-config -> Pin configuration -> Status LED
  [*] Enable the WS2812 status LED
  (48) GPIO connected to the WS2812 data input (-1 = none)
```

Leaving the pin at `-1` drives nothing: the LED pin is a per-board choice and a wrong number would drive a pin that carries something else. A pin that
is already used by the UART, BOOT/RST or SWD is rejected by `status_led_init()` with a log line,
and the bridge keeps running. Brightness, the activity window, the blink period and duty, the
breathing period and floor, and the breathing gamma are all in the same menu; the link timeout
lives in `END-config -> Wireless link -> Peer timeout`. The breathing curve defaults to gamma 2.2
(`Breathing gamma (x100)` = 220), which turns the linear triangle into an even-looking brightness
ramp; 100 gives the old linear one back, and 150 or so is a gentler compromise. Brightness sets
how many codes the breath can use: cyan spans 0..45 at brightness 64, so plain rounding would show
visible steps, and the renderer dithers them into a duty cycle frame by frame. Dithering has its
own artefact when the codes run out - at brightness 8 one code is 17% of the peak and the pulses
become a few-hertz flicker - so it falls back to rounding below a peak of 16 codes (the shipped
brightness of 16 lands in that band: 11 levels, small steps, no flicker). At such a low brightness
"steps" and "flicker" are the only two choices; for neither, set `Overall brightness` to 32 or
more (one code is then at most 4%). The driver is the managed component
`espressif/led_strip` (RMT backend), fetched by the component manager.

The wired role has no status LED: the option is behind `depends on !BRIDGE_ROLE_WIRED`, so it
is only available to the two wireless roles.

## License

Licensed under the Apache License, Version 2.0 — see [LICENSE](LICENSE).

This project contains code from several origins, and every upstream notice is retained
as-is in the SPDX header of the file it belongs to:

- Portions originating from **ESP USB Bridge**: Copyright 2020-2026 Espressif Systems (Shanghai) Co Ltd.
- CMSIS-DAP sources under `components/debug_probe/DAP/`: Copyright (c) 2013-2022 ARM Limited.
- The wireless layer (`components/wireless_link/`, `components/bridge/`, `docs/wireless.md`)
  and other modifications made in this repository: Copyright 2026 Dbb.

## Contributing

Bug reports, feature requests and pull requests are welcome — please use this repository's
issue tracker.

Contributions in the form of pull requests should follow ESP-IDF project's [contribution guidelines](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/contribute/index.html).

This repository has no CI / pre-commit gate. Before committing, at least build all five combinations covered by the `esp-idf-build` skill (wired, host, slave_s3, slave_c6, slave_c3); for behaviour changes, also smoke-test on hardware via the `esp-idf-flash` skill.
