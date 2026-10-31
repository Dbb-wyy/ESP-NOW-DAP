# ESP-NOW-DAP

**中文** | **[English](#english)**

---

# 中文

ESP-NOW-DAP 是一个基于 [ESP-IDF](https://github.com/espressif/esp-idf) 的项目，使用 ESP32-S2 或 ESP32-S3，在电脑（PC）与目标单片机（MCU）之间搭建桥接。通过 USB 使用时，它可以替代 USB 转串口芯片（例如 CP210x），也可以作为调试器；在无线角色下，**同样的**串口与 SWD 两路数据会被**通过 ESP-NOW 隧道**转发到另一块板子，PC 不再需要与目标有线相连。两路数据可以**同时**使用。

本项目派生自 Espressif 的 [ESP USB Bridge](https://github.com/espressif/esp-usb-bridge)（EUB），详见[来源与致谢](#来源与致谢)。

ESP-NOW-DAP 通过 USB 线连接时，会向 PC 呈现一个 USB 复合设备，主要功能如下。

- **串口桥**：可以运行 [esptool](https://github.com/espressif/esptool)，或用终端程序连接 USB CDC 提供的串口，数据在 PC 与目标单片机之间双向传输。
- **SWD 调试桥**：可以在 PC 上运行 [openocd-esp32](https://github.com/espressif/openocd-esp32)，它会连接到 ESP-NOW-DAP；桥接芯片在 PC 与目标之间双向转发 SWD 通信。
- **无线桥**：两块板子可以通过 ESP-NOW 桥接同样的串口与 SWD 数据，PC 无需与目标有线连接。见[无线桥](#无线桥)。

## 编译

编译需要 [ESP-IDF](https://github.com/espressif/esp-idf) v5.0 或更新版本，请先阅读 ESP-IDF 文档完成环境搭建。

- `idf.py set-target` 用于选择目标芯片，目前支持 ESP32-S2 与 ESP32-S3。
- `idf.py menuconfig` 用于修改配置，本项目相关的设置在 **END-config** 子菜单下。
- `idf.py build` 编译固件。
- `idf.py -p PORT flash monitor` 烧录并打开串口监视器。注意这里的 PORT 是连接桥接芯片串口（不是芯片原生 USB 口）的 USB 转串口芯片所创建的串口，仅在烧录时需要。烧录完成后，桥接芯片可以通过 USB 口工作。

初次烧录也可以用其他方式完成，参考[这份文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/api-guides/usb-otg-console.html#uploading-the-application)。

板子的引脚、Flash 容量与 USB 标识都放在 `sdkconfig.defaults.esp32s3` 里，因此直接 `idf.py build` 和各个无线角色的构建都从同一套硬件配置出发。

### 编译无线角色

`END-config -> Role configuration -> Firmware role` 决定这份固件实现桥的哪一端。默认是 wired 角色，所以直接 `idf.py build` 得到的仍是原来的单板固件。

**一次只编一个角色** —— 在 menuconfig 里选好角色后正常编译：

```bash
idf.py set-target esp32s3
idf.py menuconfig      # END-config -> Role configuration -> Firmware role
idf.py build
```

**同时保留多个角色** —— 给每个角色独立的 sdkconfig 与构建目录，切换角色不会互相影响：

```bash
# wired
idf.py -B build_wired -D SDKCONFIG=sdkconfig.wired set-target esp32s3
idf.py -B build_wired -D SDKCONFIG=sdkconfig.wired build

# 无线主机：放在 PC 旁边
idf.py -B build_host -D SDKCONFIG=sdkconfig.host \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_host" set-target esp32s3
idf.py -B build_host -D SDKCONFIG=sdkconfig.host \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_host" build

# 无线从机：放在目标旁边
idf.py -B build_slave -D SDKCONFIG=sdkconfig.slave \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_slave" set-target esp32s3
idf.py -B build_slave -D SDKCONFIG=sdkconfig.slave \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_slave" build
```

两个无线角色都要求使用 CMSIS-DAP SWD 调试接口；角色默认配置文件已经选好，若缺少该配置，编译会以明确的错误停止。

## 来源与致谢

本仓库是 Espressif **[ESP USB Bridge](https://github.com/espressif/esp-usb-bridge)** 的派生作品。

所有让桥接在 USB 上工作的部分都来自该项目：USB 复合设备、CDC 串口桥、CMSIS-DAP SWD 调试传输、大容量存储 / UF2 烧录器、DTR/RTS 到 BOOT/RST 的复位逻辑，以及 magic baud 下载模式。

本仓库新增的是无线层，以及让角色切换成为可能的中间层：

- `components/wireless_link/` —— 带对端发现与按流分发的 ESP-NOW 链路
- `main/bridge_glue.c` —— 在 USB、目标串口/SWD 引脚与链路之间做角色分发的中间层
- `docs/wireless.md` —— 无线部分的设计说明

所有源自 EUB 的文件中，上游版权声明均原样保留；本仓库不携带上游仓库的提交历史。

另外，感谢 viric 在 Espressif 的 ESP USB Bridge 项目中提交的 [PR #40](https://github.com/espressif/esp-usb-bridge/pull/40)。其中关于 `dedic_gpio` 下 SWD 输出无法正确关闭的问题及其修复思路，帮助解决了本项目开发过程中的关键问题。

## 开发板

桥接端使用一块 ESP32-S2 或 ESP32-S3 板子，与目标单片机的串口、SWD 与复位引脚相连。引脚号可以在 `idf.py menuconfig` 中修改，本仓库使用的默认值收集在 `sdkconfig.defaults.esp32s3` 中。

## 串口桥

USB 协议栈会创建一个虚拟串口，通过它访问目标单片机的串口。该串口在操作系统下可能是 `/dev/ttyACMx` 或 `COMx`，与烧录桥接芯片所用的 PORT 不是同一个。

例如，可以用下面的命令烧录并监视一个 ESP32 目标：

```bash
cd AN_ESP32_PROJECT
idf.py build
idf.py -p /dev/ttyACMx flash monitor
```

[esptool](https://github.com/espressif/esptool) 或任何终端程序、串口助手也都可以连接这个虚拟串口。

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
 PC ──USB── ESP32-S3「无线主机」 ══ESP-NOW══ ESP32-S3「无线从机」 ──UART+SWD── 目标 MCU
```

- **无线主机**插在 PC 上。它的 USB CDC 口与 CMSIS-DAP bulk 接口会被转发到链路上。
- **无线从机**与目标的串口、SWD（SWCLK/SWDIO）以及 BOOT/RST 引脚相连。它完全不使用自己的 USB 口。
- 两块板子会自动配对，并且必须设置成相同的 `END-config -> Wireless link -> Wi-Fi channel`（默认 1）。不需要按键或绑定流程。

`esptool`、`idf.py monitor` 与 OpenOCD 的用法和 wired 模式完全一样：CDC 的 line coding 与 DTR/RTS 复位时序都会被隧道到从机，因此目标可以在 PC 侧不做任何改动的情况下被烧录和调试。

```bash
# 通过主机板做 SWD 调试
openocd -f interface/cmsis-dap.cfg -f target/esp32s3.cfg -c 'adapter speed 1000'

# 同时使用串口
idf.py -p /dev/ttyACMx monitor
```

编译方法见[编译无线角色](#编译无线角色)；协议、SWD 重传模型、引脚配置与已知限制记录在 [docs/wireless.md](docs/wireless.md) 中。

### 无线性能

还没做过严谨的无线测试。目前只试过主机放“电梯口”、从机放“实验室”，能正常烧录。（乐）
具体通信速度、距离上限和穿墙能力待测。

## 许可证

本项目基于 Apache License 2.0 授权，见 [LICENSE](LICENSE)。

项目中的代码来自多个来源，每一处上游声明都原样保留在对应文件的 SPDX 头中：

- 源自 **ESP USB Bridge** 的部分：Copyright 2020-2026 Espressif Systems (Shanghai) Co Ltd.
- `components/debug_probe/DAP/` 下的 CMSIS-DAP 源码：Copyright (c) 2013-2022 ARM Limited.
- 无线层（`components/wireless_link/`、`main/bridge_glue.*`、`docs/wireless.md`）以及本仓库中的其他修改：Copyright 2026 Dbb.

## 贡献

欢迎以 bug 报告、功能请求与 pull request 的形式参与贡献，请使用本仓库的 issue tracker。

提交 pull request 时请遵循 ESP-IDF 项目的[贡献指南](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/contribute/index.html)。

另外请在提交代码前安装 [pre-commit](https://pre-commit.com/#install) 钩子：

```bash
pip install pre-commit
pre-commit install
```

---

# English

ESP-NOW-DAP is an [ESP-IDF](https://github.com/espressif/esp-idf) project that uses an
ESP32-S2 or ESP32-S3 to build a bridge between a computer (PC) and a target
microcontroller (MCU). When used over USB, it can replace a USB-to-serial chip (for
example, CP210x) and can also serve as a debugger; in the wireless roles, the **same**
serial and SWD streams are forwarded **through an ESP-NOW tunnel** to another board, so
the PC no longer needs a wired connection to the target. The two streams can be used
**at the same time**.

This project is derived from Espressif's [ESP USB Bridge](https://github.com/espressif/esp-usb-bridge)
(EUB); see [Origin and credits](#origin-and-credits).

When connected through a USB cable, ESP-NOW-DAP presents a USB composite device to the
PC, with the following main features.

- **Serial bridge**: [esptool](https://github.com/espressif/esptool) can be run, or a terminal program can connect to the serial port provided by USB CDC; data is transferred in both directions between the PC and the target MCU.
- **SWD debug bridge**: [openocd-esp32](https://github.com/espressif/openocd-esp32) can be run on the PC and connects to ESP-NOW-DAP; the bridge chip forwards SWD communication in both directions between the PC and the target.
- **Wireless bridge**: two boards can bridge the same serial and SWD data over ESP-NOW, so the PC does not need a wired connection to the target. See [Wireless bridge](#wireless-bridge).

## Compilation

Building requires [ESP-IDF](https://github.com/espressif/esp-idf) v5.0 or newer. Please
read the ESP-IDF documentation to set up the environment first.

- `idf.py set-target` selects the target chip; ESP32-S2 and ESP32-S3 are supported at the moment.
- `idf.py menuconfig` changes the configuration; the settings related to this project are under the **END-config** sub-menu.
- `idf.py build` builds the firmware.
- `idf.py -p PORT flash monitor` flashes the firmware and opens the serial monitor. Note that PORT is the serial port created by the USB-to-serial chip connected to the bridge chip's serial port (not the chip's native USB port), and is needed only for flashing. After flashing, the bridge chip can work through the USB port.

The initial flashing can also be done by other means; see [this guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/api-guides/usb-otg-console.html#uploading-the-application).

The board pins, flash size and USB identifiers live in `sdkconfig.defaults.esp32s3`, so
a plain `idf.py build` and every wireless role build start from the same hardware setup.

### Building the wireless roles

`END-config -> Role configuration -> Firmware role` selects which end of the bridge the firmware
implements. The wired role is the default, so a plain `idf.py build` still produces
the original single-board firmware.

**One role at a time** — pick the role in menuconfig and build normally:

```bash
idf.py set-target esp32s3
idf.py menuconfig      # END-config -> Role configuration -> Firmware role
idf.py build
```

**Several roles side by side** — give each role its own sdkconfig and build
directory, so switching roles never touches the other builds:

```bash
# wired
idf.py -B build_wired -D SDKCONFIG=sdkconfig.wired set-target esp32s3
idf.py -B build_wired -D SDKCONFIG=sdkconfig.wired build

# wireless host: sits next to the PC
idf.py -B build_host -D SDKCONFIG=sdkconfig.host \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_host" set-target esp32s3
idf.py -B build_host -D SDKCONFIG=sdkconfig.host \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_host" build

# wireless slave: sits next to the target
idf.py -B build_slave -D SDKCONFIG=sdkconfig.slave \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_slave" set-target esp32s3
idf.py -B build_slave -D SDKCONFIG=sdkconfig.slave \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wireless_slave" build
```

Both wireless roles require the CMSIS-DAP SWD debug interface; the role default
files select it, and the build stops with an explicit error if it is missing.

## Origin and credits

This repository is a derivative work of **[ESP USB Bridge](https://github.com/espressif/esp-usb-bridge)**
by Espressif Systems.

Everything that makes the bridge work over USB comes from that project: the composite USB
device, the CDC serial bridge, the CMSIS-DAP SWD transport, the mass storage / UF2 flasher,
the DTR/RTS to BOOT/RST reset logic and the magic baud download mode.

What this repository adds is the wireless layer and the plumbing that makes a role switch
possible:

- `components/wireless_link/` — an ESP-NOW link with peer discovery and per-stream dispatch
- `main/bridge_glue.c` — the role dispatching glue between USB, the target UART/SWD pins and the link
- `docs/wireless.md` — the wireless design notes

Upstream copyright notices are retained unchanged in every file that came from EUB; this
repository does not carry the upstream commit history.

Additionally, thanks to viric for [PR #40](https://github.com/espressif/esp-usb-bridge/pull/40)
in Espressif's ESP USB Bridge project. The issue with the SWD output under `dedic_gpio` not
being disabled correctly, along with the fix approach described there, helped resolve a key
problem during the development of this project.

## Development Board

An ESP32-S2 or ESP32-S3 board is used on the bridge side, connected to the target MCU's
serial port, SWD and reset pins. The pin numbers can be changed in `idf.py menuconfig`;
the defaults used by this repository are collected in `sdkconfig.defaults.esp32s3`.

## Serial Bridge

The USB stack creates a virtual serial port through which the target MCU's serial port is
accessed. Under the operating system this port may be `/dev/ttyACMx` or `COMx`, and it is
not the same PORT used for flashing the bridge chip.

For example, an ESP32 target can be flashed and monitored with the following commands:

```bash
cd AN_ESP32_PROJECT
idf.py build
idf.py -p /dev/ttyACMx flash monitor
```

[esptool](https://github.com/espressif/esptool), any terminal program or serial assistant
can also connect to this virtual serial port.

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

Two boards running this firmware can bridge the **same** serial and SWD traffic over
ESP-NOW, so the PC no longer has to be wired to the target. Serial and SWD work at
the same time.

```
 PC ──USB── ESP32-S3 "wireless host" ══ESP-NOW══ ESP32-S3 "wireless slave" ──UART+SWD── target MCU
```

- The **wireless host** plugs into the PC. Its USB CDC port and its CMSIS-DAP bulk
  interface are forwarded over the link.
- The **wireless slave** is wired to the target's UART, SWD (SWCLK/SWDIO) and
  BOOT/RST pins. It does not use its USB port at all.
- The two boards pair automatically and must be set to the same
  `END-config -> Wireless link -> Wi-Fi channel` (default 1). No button or binding
  procedure is involved.

`esptool`, `idf.py monitor` and OpenOCD are used exactly as in wired mode: the
CDC line coding and the DTR/RTS reset sequence are tunneled to the slave, so the
target can be flashed and debugged without changing anything on the PC side.

```bash
# SWD debugging through the host board
openocd -f interface/cmsis-dap.cfg -f target/esp32s3.cfg -c 'adapter speed 1000'

# serial, in parallel
idf.py -p /dev/ttyACMx monitor
```

Build instructions are in [Building the wireless roles](#building-the-wireless-roles);
the protocol, the SWD retransmission model, the pin configuration and the known
limitations are documented in [docs/wireless.md](docs/wireless.md).

### Wireless performance

Not rigorously tested yet. So far we've only tried the host at the “elevator entrance” and the slave in the “lab”, and flashing works. (heh)
Actual throughput, maximum range, and wall penetration remain to be measured.

## License

Licensed under the Apache License, Version 2.0 — see [LICENSE](LICENSE).

This project contains code from several origins, and every upstream notice is retained
as-is in the SPDX header of the file it belongs to:

- Portions originating from **ESP USB Bridge**: Copyright 2020-2026 Espressif Systems (Shanghai) Co Ltd.
- CMSIS-DAP sources under `components/debug_probe/DAP/`: Copyright (c) 2013-2022 ARM Limited.
- The wireless layer (`components/wireless_link/`, `main/bridge_glue.*`, `docs/wireless.md`)
  and other modifications made in this repository: Copyright 2026 Dbb.

## Contributing

Bug reports, feature requests and pull requests are welcome — please use this repository's
issue tracker.

Contributions in the form of pull requests should follow ESP-IDF project's [contribution guidelines](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/contribute/index.html).

Additionally please install [pre-commit](https://pre-commit.com/#install) hooks before committing code:

```bash
pip install pre-commit
pre-commit install
```
