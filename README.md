# ESP-NOW-DAP

**中文** | **[English](#english)**

---

# 中文

ESP-NOW-DAP 是一个基于 [ESP-IDF](https://github.com/espressif/esp-idf) 的项目，使用 ESP32-S2 或 ESP32-S3，在电脑（PC）与目标单片机（MCU）之间搭建桥接。通过 USB 使用时，它可以替代 USB 转串口芯片（例如 CP210x），也可以作为调试器；在无线角色下，**同样的**串口与 SWD 两路数据会被**通过 ESP-NOW 隧道**转发到另一块板子，PC 不再需要与目标有线相连。两路数据可以同时使用。

本项目派生自 Espressif 的 [ESP USB Bridge](https://github.com/espressif/esp-usb-bridge)（EUB），详见[来源与致谢](#来源与致谢)。

ESP-NOW-DAP 通过 USB 线连接时，会向 PC 呈现一个 USB 复合设备，主要功能如下。

- **串口桥**：可以运行 [esptool](https://github.com/espressif/esptool)，或用终端程序连接 USB CDC 提供的串口，数据在 PC 与目标单片机之间双向传输。
- **SWD 调试桥**：可以在 PC 上运行 [openocd-esp32](https://github.com/espressif/openocd-esp32)，它会连接到 ESP-NOW-DAP；桥接芯片在 PC 与目标之间双向转发 SWD 通信。
- **大容量存储设备**：会出现一个 U 盘，可以把 [UF2 格式](https://github.com/microsoft/uf2)的固件拷贝进去，桥接芯片会用它烧录目标。**仅在 wired 角色下可用。**
- **无线桥**：两块板子可以通过 ESP-NOW 桥接同样的串口与 SWD 数据，PC 无需与目标有线连接。见[无线桥](#无线桥)。

## 编译

编译需要 [ESP-IDF](https://github.com/espressif/esp-idf) v5.0 或更新版本，请先阅读 ESP-IDF 文档完成环境搭建。

- `idf.py set-target` 用于选择目标芯片，目前支持 ESP32-S2 与 ESP32-S3。
- `idf.py menuconfig` 用于修改配置，本项目相关的设置在 **Bridge Configuration** 子菜单下。
- `idf.py build` 编译固件。
- `idf.py -p PORT flash monitor` 烧录并打开串口监视器。注意这里的 PORT 是连接桥接芯片串口（不是芯片原生 USB 口）的 USB 转串口芯片所创建的串口，仅在烧录时需要。烧录完成后，桥接芯片可以通过 USB 口工作。

初次烧录也可以用其他方式完成，参考[这份文档](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/api-guides/usb-otg-console.html#uploading-the-application)。

板子的引脚、Flash 容量与 USB 标识都放在 `sdkconfig.defaults.esp32s3` 里，因此直接 `idf.py build` 和各个无线角色的构建都从同一套硬件配置出发。

### 编译无线角色

`Bridge Configuration -> Bridge role` 决定这份固件实现桥的哪一端。默认是 wired 角色，所以直接 `idf.py build` 得到的仍是原来的单板固件。

**一次只编一个角色** —— 在 menuconfig 里选好角色后正常编译：

```bash
idf.py set-target esp32s3
idf.py menuconfig      # Bridge Configuration -> Bridge role
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

所有源自 EUB 的文件中，上游版权声明均原样保留；产生原始代码的提交历史保留在 [CHANGELOG.md](CHANGELOG.md) 中。

## 开发板

桥接端使用一块 ESP32-S2 或 ESP32-S3 板子，与目标单片机的串口、SWD 与复位引脚相连。引脚号可以在 `idf.py menuconfig` 中修改，本仓库使用的默认值收集在 `sdkconfig.defaults.esp32s3` 中。

请注意每块板子都应该有自己的 VID/PID。也可以向 [Espressif 的 VID](https://github.com/espressif/usb-pids) 申请一个 PID。

## 串口桥

USB 协议栈会创建一个虚拟串口，通过它访问目标单片机的串口。该串口在操作系统下可能是 `/dev/ttyACMx` 或 `COMx`，与烧录桥接芯片所用的 PORT 不是同一个。

例如，可以用下面的命令烧录并监视一个 ESP32 目标：

```bash
cd AN_ESP32_PROJECT
idf.py build
idf.py -p /dev/ttyACMx flash monitor
```

[esptool](https://github.com/espressif/esptool) 或任何终端程序也都可以连接这个虚拟串口。

### 更新桥固件（Magic Baud 下载模式）

当主机把 USB CDC 口设置到一个配置好的 magic 波特率、然后关闭该端口时，桥接芯片（ESP32-S2 或 ESP32-S3）可以**自己**复位进入 ROM 下载模式。这样就能用同一根 USB 线更新桥的固件，无需接触 BOOT/RESET 按键。

magic 波特率只是**武装**这次复位；关闭端口时**清除 DTR** 才会**触发**它。随后桥会置位 `RTC_CNTL_FORCE_DOWNLOAD_BOOT` 并重启，ROM bootloader 会在同一根线上提供 USB 下载接口。这**只影响桥接芯片**。桥的应用程序运行期间，该端口的 DTR/RTS 仍然驱动目标的 BOOT 与 RST 引脚；除了"清除 DTR 会触发复位"这一点之外，两者没有关系。

**不要**把 magic 波特率用于正常的目标串口通信。以该波特率打开 CDC 之后再清除 DTR（关闭端口，或主机复位序列取消 DTR 置位），会把**桥**送进下载模式，而不是与目标通信。

在 menuconfig 的 **Bridge Configuration** 中配置该波特率（`CONFIG_BRIDGE_DOWNLOAD_MAGIC_BAUD`，默认 `1200`）。设为 `0` 可禁用。请选一个主机工具不会用于目标的波特率。修改后需要重新编译并烧录。主机的 `bit_rate` 必须完全匹配。

主机侧触发下载模式（若配置不同，请相应调整 `PORT` 与波特率）。触发时 DTR 必须处于置位状态，然后在 magic 波特率下清除它。`pyserial` 打开端口时默认置位 DTR，所以直接关闭端口即可：

```bash
python3 -c "import serial; s=serial.Serial('PORT', 1200); s.close()"
```

桥重启后，通过 USB 烧录：

```bash
idf.py -p PORT flash
```

该功能要求 `EFUSE_DIS_FORCE_DOWNLOAD` 与 `EFUSE_DIS_DOWNLOAD_MODE` 未被烧写（开发芯片上默认为未烧写）。

## SWD 调试桥

ESP-NOW-DAP 使用 CMSIS-DAP 协议提供 ARM Serial Wire Debug（SWD）设备。

在 OpenOCD 一侧，CMSIS-DAP 相关的更新会持续从主线同步到 Espressif 的分支，因此始终建议使用最新的 release 或 master。

下面的命令可以访问目标单片机的 SWD 口：

```bash
openocd -s tcl -f interface/cmsis-dap.cfg -f target/<target.cfg>.cfg -c 'adapter speed 5000'
```

请把 `target.cfg` 换成实际的目标配置文件。

目前只支持 USB bulk 传输作为后端，不支持 HID 后端。

## 大容量存储设备

连接后会向 PC 呈现一个 U 盘，可以像普通 USB 存储设备一样访问。把 [UF2 格式](https://github.com/microsoft/uf2)的固件拷贝到这个盘里，桥接芯片会据此烧录目标单片机。

该接口**只在 wired 角色下存在**：无线角色的主机侧没有本地串口可用于烧录，请改用隧道后的串口，通过 `esptool` 烧录目标。

在目标工程中运行下面的命令，会在 `AN_ESP32_PROJECT/build` 目录下生成 `uf2.bin`：

```bash
cd AN_ESP32_PROJECT
idf.py uf2
```

## 无线桥

两块运行本固件的板子可以通过 ESP-NOW 桥接**同样的**串口与 SWD 数据，PC 不再需要与目标有线相连。串口与 SWD 可以同时使用。

```
 PC ──USB── ESP32-S3「无线主机」 ══ESP-NOW══ ESP32-S3「无线从机」 ──UART+SWD── 目标 MCU
```

- **无线主机**插在 PC 上。它的 USB CDC 口与 CMSIS-DAP bulk 接口会被转发到链路上。
- **无线从机**与目标的串口、SWD（SWCLK/SWDIO）以及 BOOT/RST 引脚相连。它完全不使用自己的 USB 口。
- 两块板子会自动配对，并且必须设置成相同的 `Wireless Link Configuration -> Wi-Fi channel`（默认 1）。不需要按键或绑定流程。

`esptool`、`idf.py monitor` 与 OpenOCD 的用法和 wired 模式完全一样：CDC 的 line coding 与 DTR/RTS 复位时序都会被隧道到从机，因此目标可以在 PC 侧不做任何改动的情况下被烧录和调试。

```bash
# 通过主机板做 SWD 调试
openocd -f interface/cmsis-dap.cfg -f target/esp32s3.cfg -c 'adapter speed 1000'

# 同时使用串口
idf.py -p /dev/ttyACMx monitor
```

编译方法见[编译无线角色](#编译无线角色)；协议、SWD 重传模型、引脚配置与已知限制记录在 [docs/wireless.md](docs/wireless.md) 中。

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

ESP-NOW-DAP is an [ESP-IDF](https://github.com/espressif/esp-idf) project for ESP32-S2 and
ESP32-S3 that bridges a computer (PC) to a target microcontroller (MCU). Over USB it works
as a USB-to-UART replacement and a debugger; in the wireless roles the **same** serial and
SWD streams are tunnelled over **ESP-NOW**, so the PC no longer has to be physically
connected to the target. Both streams work at the same time.

This project is derived from Espressif's [ESP USB Bridge](https://github.com/espressif/esp-usb-bridge)
(EUB) — see [Origin and credits](#origin-and-credits).

ESP-NOW-DAP creates a composite USB device accessible from the PC when they are connected
through a USB cable. The main features are the following.

- *Serial bridge*: The developer can run [esptool](https://github.com/espressif/esptool) or connect a terminal program to the serial port provided by the USB CDC. The communication is transferred in both directions between the PC and the target MCU.
- *SWD bridge*: [openocd-esp32](https://github.com/espressif/openocd-esp32) can be run on the PC, which will connect to ESP-NOW-DAP. The bridge MCU acts as a bridge between the PC and the MCU, and transfers SWD communication between them in both directions.
- *Mass storage device*: A USB mass storage device is created which can be accessed by a file explorer on the PC. Binaries in [UF2 format](https://github.com/microsoft/uf2) can be copied to this disk and the bridge MCU will use them to flash the target MCU. **This is available in the wired role only.**
- *Wireless bridge*: two boards can bridge the same serial and SWD traffic over ESP-NOW, so the PC does not have to be wired to the target. See [Wireless Bridge](#wireless-bridge).

## How to Compile the Project

[ESP-IDF](https://github.com/espressif/esp-idf) v5.0 or newer can be used to compile the project. Please read the
documentation of ESP-IDF for setting up the environment.

- `idf.py set-target` is used to select the chip the firmware is going to be built for. ESP32S2 and ESP32S3 are supported at the moment.
- `idf.py menuconfig` can be used to change the default configuration. The project-specific settings are in the "Bridge Configuration" sub-menu.
- `idf.py build` will build the project binaries.
- `idf.py -p PORT flash monitor` will flash the Bridge MCU and open the terminal program for monitoring. Please note that PORT is the serial port created by an USB-to-UART chip connected to the serial interface of the bridge MCU (not the direct USB interface provided by bridge MCU). This serial connection has to be established only for flashing. The bridge can work through the USB interface after that.

The initial flashing can be done by other means as well as it is pointed out in [this guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/api-guides/usb-otg-console.html#uploading-the-application).

Board pins, flash size and USB identifiers live in `sdkconfig.defaults.esp32s3`, so a
plain `idf.py build` and every wireless role build start from the same hardware setup.

### Building the wireless roles

`Bridge Configuration -> Bridge role` selects which end of the bridge the firmware
implements. The wired role is the default, so a plain `idf.py build` still produces
the original single-board firmware.

**One role at a time** — pick the role in menuconfig and build normally:

```bash
idf.py set-target esp32s3
idf.py menuconfig      # Bridge Configuration -> Bridge role
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

Upstream copyright notices are retained unchanged in every file that came from EUB, and the
commit history that produced the original code is preserved in [CHANGELOG.md](CHANGELOG.md).

## Development Board

An ESP32-S2 or ESP32-S3 board is used as the bridge MCU, wired to the target MCU's UART,
SWD and reset pins. The pin numbers can be changed in `idf.py menuconfig`; the defaults
used by this repository are collected in `sdkconfig.defaults.esp32s3`.

Please note that every board should have its own vendor and product identifiers. There is also a possibility to register a product identifier under the [Espressif vendor identifier](https://github.com/espressif/usb-pids).

## Serial Bridge

The USB stack creates a virtual serial port through which the serial port of the target MCU is accessible. For example, this port can be `/dev/ttyACMx` or `COMx` depending on the operating system and is different from the PORT used for flashing the bridge MCU.

For example, an ESP32 target MCU can be flashed and monitored with the following commands.

```bash
cd AN_ESP32_PROJECT
idf.py build
idf.py -p /dev/ttyACMx flash monitor
```

Please note that [esptool](https://github.com/espressif/esptool) or any terminal program can connect to the virtual serial port as well.

### Updating Bridge Firmware (Magic Baud Download Mode)

The bridge chip (ESP32-S2 or ESP32-S3) can reset **itself** into ROM download mode when the host sets the USB CDC port to a configured magic baud rate and then closes the port. This allows updating bridge firmware over the same USB cable without physical BOOT/RESET access.

The magic baud **arms** the reset; clearing DTR on port close **fires** it. The bridge then sets `RTC_CNTL_FORCE_DOWNLOAD_BOOT` and restarts. The ROM bootloader exposes a USB download interface on the same cable. This affects the **bridge MCU only**. While the bridge application is running, DTR/RTS on this port still drive the target's BOOT and RST lines; they are unrelated to this self-reset path except that clearing DTR is what fires it once armed.

Do **not** use the magic baud for normal target serial traffic. Opening the CDC at that rate and then clearing DTR (port close, or a host reset sequence that deasserts DTR) puts the **bridge** into download mode instead of communicating with the target.

Configure the magic baud in menuconfig under **Bridge Configuration** (`CONFIG_BRIDGE_DOWNLOAD_MAGIC_BAUD`, default `1200`). Set to `0` to disable. Prefer a baud that host tools will not use for the target. Rebuild and flash after changing the value. The host `bit_rate` must match exactly.

To trigger download mode from the host, adjust `PORT` and baud if configured differently.
DTR must be asserted while the magic baud is set, then cleared to fire the reset.
`pyserial` asserts DTR on open by default, so closing the port is enough:

```bash
python3 -c "import serial; s=serial.Serial('PORT', 1200); s.close()"
```

After the bridge reboots, flash over USB:

```bash
idf.py -p PORT flash
```

The feature requires `EFUSE_DIS_FORCE_DOWNLOAD` and `EFUSE_DIS_DOWNLOAD_MODE` not to be burned (default on development chips).

## SWD Bridge

ESP-NOW-DAP provides an ARM Serial Wire Debug (SWD) device using the CMSIS-DAP protocol.

On the OpenOCD side, CMSIS-DAP-related updates are continuously synced from the mainline to the Espressif fork. Therefore, it is always recommended to use the latest release or the latest master branch.

The following command can be used to access the SWD port on a target MCU

```bash
openocd -s tcl -f interface/cmsis-dap.cfg -f target/<target.cfg>.cfg -c 'adapter speed 5000'
```

Make sure to replace `target.cfg` with the actual target config file.

Currently, only USB-bulk transfers are supported as a backend. The HID backend is not supported.

## Mass Storage Device

A mass storage device will show up on the PC connected to the bridge. This can be accessed as any other USB storage disk. Binaries built in [the UF2 format](https://github.com/microsoft/uf2) can be copied to this disk and the bridge MCU will flash the target MCU accordingly.

This interface exists in the wired role only: the wireless roles have no local UART on the
host side to flash through, so flash the target with `esptool` over the tunneled serial port
instead.

Binary `uf2.bin` will be generated and placed into the `AN_ESP32_PROJECT/build` directory by running the following commands.

```bash
cd AN_ESP32_PROJECT
idf.py uf2
```

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
  `Wireless Link Configuration -> Wi-Fi channel` (default 1). No button or binding
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

Build instructions are in [How to Compile the Project](#building-the-wireless-roles);
the protocol, the SWD retransmission model, the pin configuration and the known
limitations are documented in [docs/wireless.md](docs/wireless.md).

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
