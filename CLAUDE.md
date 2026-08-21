# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# mount 项目指南

## 当前状态

v1 已完成（Phase 0~3）：`mount -NTFS`、`mount -ISO <file>`、无参列表/帮助、自检均通过 QEMU 闭环验证，证据保留在 `run_logs/` 与 `snapshot/`。Phase 4 已点亮 EXT4（Tested=TRUE，测试资产 `test_images/mkext4img.py`）；btrfs/xfs 实测为后续迭代。

## 项目概述

mount 是一个运行在 UEFI Shell 下的纯命令行工具，对标 Linux 的 `mount` 命令。需求以 `req.md` 为准，核心两条：

- `mount -NTFS`：加载 NTFS 文件系统支持（可用开源实现，只读即可），然后自动执行等效 `map -r` 的动作，把机器上的 NTFS 卷挂载出 SimpleFileSystem。
- `mount -ISO <文件>`：把一个 ISO 文件虚拟成块设备（相当于 Linux 的 loop device），让固件文件系统驱动栈绑定上去，自动挂载出该 ISO 的文件系统。
- **格式可扩展**：NTFS/ISO 只是起点，命令框架要支持继续加入 Linux 主流格式（ext4、btrfs、xfs 等），即 `mount -<格式>` 的通用形态。架构上每种格式必须是一个可独立增删的驱动单元，新增格式不允许改动挂载主流程代码（映射表/注册机制除外）。

目录组织参照上一级的 `D:\Work\Code\guedit`（GueditPkg）和 `D:\Work\Code\gufile`（GufilePkg）：应用建立自己的包目录，项目自包含，后期会上 GitHub 私仓——**不要依赖本目录之外的相对路径**（edk2、QEMU 等环境路径除外）。

## 环境与工具链（已装好，禁止重复下载安装）

- edk2：`D:\Work\Code\edk2`，VS2019 工具链，X64/DEBUG，`Conf/target.txt` 的 ACTIVE_PLATFORM 指向 EmulatorPkg。
- QEMU：`C:\Program Files\qemu\qemu-system-x86_64.exe`（10.2.50-dev）。
- OVMF：已验证可用的是 `D:\Work\Code\ContraQwen\OVMF_CODE.fd`；edk2 自己构建的在 `D:\Work\Code\edk2\Build\OvmfX64\DEBUG_VS2019\FV\`。
- **edk2 树本身不修改**：应用包 MountPkg 放本目录，构建时 `PACKAGES_PATH=D:\Work\Code\mount`（gufile 实证需要首项指向项目根，必要时追加 `;D:\Work\Code`）。

## 构建与运行

构建、QEMU 运行、版本断言一律按本项目 `.claude/skills/emulator-uefi-shell-app/SKILL.md` 执行（技能副本已随项目入库，源头在 `D:\Work\Code\gufile\.claude\skills\`；模板脚本在其 `templates/` 下）。参照 gufile 的成熟形态，本项目应建立：

```powershell
# 版本化构建（唯一入口：BUILD+1、再生 Version.h、构建、写 qemu_disk/expected_version.txt）
powershell -ExecutionPolicy Bypass -File tools/Build-Mount.ps1
# QEMU 闭环运行（OVMF + FAT 镜像 + startup.nsh；-Script 驱动 monitor sendkey/screendump）
powershell -ExecutionPolicy Bypass -File tools/Run-MountQemu.ps1 -Script "t30 screendump main"
```

- 构建内部命令形态（在 edk2 目录下执行）：`set PACKAGES_PATH=D:\Work\Code\mount&& .\edksetup.bat&& build -p MountPkg/MountPkg.dsc -a X64 -t VS2019 -b DEBUG`
- 产物位置由 MountPkg.dsc 的 `OUTPUT_DIRECTORY` 决定（参照 gufile：`D:\Work\Code\edk2\Build\MountPkg\DEBUG_VS2019\X64\mount.efi`）。
- `VERSION.txt` 两行格式（`VERSION=x.y.z` / `BUILD=n`）维护在工程根；mount 是纯 CLI 应用，版本戳改为：启动时向串口 `DEBUG()` 输出 `APP_VERSION=<...>` 并同时 `Print` 到 stdout 首行（技能中"画在界面角落"的要求仅适用 GUI 应用）。
- **禁止复用旧版本产物**：构建失败、版本头缺失或 `.efi` 缺失时不得运行旧版本；判定运行成功必须串口日志 `APP_VERSION=` 与 `expected_version.txt` 一致。
- QEMU 已验证的坑（gufile 实证）：本 OVMF 不要加 `-machine q35`；只读盘 ide-hd 拒绝 readonly 节点，要走 virtio-blk；FAT 镜像用无分区 superfloppy，OVMF 直接挂载。
- QEMU 人工交互：SDL 窗口键盘输入受宿主输入法影响（中文模式吞小写字母、数字透传，表现为"数字能打字母不能打"），手动操作先切英文模式；脚本化验证一律走 QMP sendkey（虚拟 PS/2，免疫宿主输入法）。

## 架构与技术要点

### 包结构（参照 GueditPkg）

```
MountPkg/
├── MountPkg.dec / MountPkg.dsc   # dsc 含全部 LibraryClasses 映射
├── Application/Mount/            # mount.efi 本体：参数解析、挂载逻辑
├── Drivers/                      # 随包编译的文件系统驱动（NTFS 等，可选）
└── Include/
```

DSC 关键工程决策直接沿用 guedit/gufile 的验证结论：`DebugLib` 映射到 `BaseDebugLibSerialPort` + PcAtChipsetPkg 的 `SerialIoLib`（16550 COM1 0x3F8），让 QEMU `-serial file:` 捕获日志作为版本断言通道；自带 `FixedDebugPrintErrorLevelLib`；`[PcdsFixedAtBuild]` 里 `PcdDebugPropertyMask|0x03` 与 `PcdFixedDebugPrintErrorLevel|0xFFFFFFFF` 缺一不可，否则 DEBUG()/ASSERT() 被编译期门控干掉。INF 编译标志：`MSFT:*_*_*_CC_FLAGS = /DEDK2_BUILD /utf-8 /wd4819 /wd4100`。读 Shell 命令行参数用 `gEfiShellParametersProtocolGuid`。

### 文件系统驱动来源（NTFS 及全部可扩展格式）

**已确认：edk2 主线没有任何 NTFS 驱动**（全树搜索无结果），必须引入开源实现。**首选 pbatard/efifs**：GRUB2 只读文件系统驱动的 UEFI 移植，纯 GPLv3+，一个包提供 38 个独立 `.inf` 驱动，覆盖本项目全部目标格式——`Ntfs.inf`、`Iso9660.inf`、`Ext2.inf`（GRUB 的 ext2 模块覆盖 ext2/3/4 只读）、`Btrfs.inf`、`Xfs.inf`、`F2fs.inf`、`ExFat.inf`、`Udf.inf` 等。备选 pbatard/uefi-ntfs（ntfs-3g 移植的只读 NTFS 驱动，Rufus 同款，GPLv3）。**已核实的构建注意点**：efifs 官方 EDK2 构建示例用 VS2022（也有 gnu-efi/make 路线），本机是 VS2019，工具链适配需实测；每个驱动是独立 `.efi`，按需取用，不必全量纳入。

落地形态建议：把选用的驱动源码纳入 `MountPkg/Drivers/` 编出独立 `.efi`，mount 在运行时 `LoadImage` + `StartImage` 加载（驱动可随 mount.efi 一起部署到 qemu_disk），然后对所有持有 BlockIo 但尚无 SimpleFileSystem 的句柄执行 `gBS->ConnectController()` 触发绑定。

### ISO 路径（mount -ISO <文件>）

**已确认：edk2 自带 `MdeModulePkg/Universal/Disk/UdfDxe`，但它只是 UDF/ECMA-167 驱动，不认识纯 ISO9660**（源码中无 CD001/PVD 处理；OvmfPkgX64.fdf 已将其编入固件，EmulatorPkg.fdf 没有）。两个推论：Windows 安装 ISO 从 Win10 起是 UDF 格式，UdfDxe 可识别；Linux 发行版常用的纯 ISO9660/Joliet 镜像则需要 efifs 的 `Iso9660.inf` 补位（efifs 也自带 `Udf.inf`，可替代 UdfDxe，二选一避免重复绑定），这是立项时就要实测确认的风险点。

虚拟块设备是这条路径的核心：mount 打开 ISO 文件（`EFI_FILE_PROTOCOL`），新建一个句柄并在其上安装 `EFI_BLOCK_IO_PROTOCOL`（+ 设备路径），ReadBlocks/WriteBlocks 后端转发为对文件偏移的读（只读场景 WriteBlocks 直接返回只读错误）；随后 `gBS->ConnectController()` 让 PartitionDxe/UdfDxe 绑定，新的 FS 即出现。卸载路径要在实现时同步设计：记录创建的句柄，`mount -u` 时 DisconnectController + UninstallProtocolInterface 并关闭文件。**loop 设备本体在常驻驱动 LoopDxe 中**（`drivers\loop_x64.efi`，`MOUNT_LOOP_FACTORY_PROTOCOL`，mount 首次用时自动加载），这是挂载在 mount.efi 退出后仍存活的前提；嗅探到纯 ISO9660 时 mount 自动加载 `drivers\iso9660_x64.efi`，无需手工 `load`（2026-08-21 起）。

### 格式扩展框架（mount -<格式>）

格式支持按"驱动单元 + 映射表"组织，而不是 if-else 硬编码：每种格式对应一个独立 `.efi` 驱动文件（部署在 mount.efi 同目录），mount 内部维护"格式名 → 驱动文件名"的映射表；`mount -EXT4` 这类命令的执行路径与 `mount -NTFS` 完全同构——查表定位驱动、LoadImage/StartImage（已加载则跳过）、ConnectController 重扫。新增一种格式只动映射表和驱动文件，主流程零改动。Linux 主流格式的落点对照（efifs 已全覆盖，均只读）：ext4→`Ext2.inf`、btrfs→`Btrfs.inf`、xfs→`Xfs.inf`、f2fs→`F2fs.inf`、exfat→`ExFat.inf`。命令行解析建议据此设计成"选项即格式名"的通用分发，而不是为每种格式写独立分支。

### "map -r" 等效动作

Shell 的 `map -r` 本质是重连驱动并刷新 Shell 内部映射表。app 内的候选做法（按优先级，需实测确认）：用 ShellLib 的 `ShellExecute()` 执行 `map -r`（app 与 Shell 同进程，映射表共享）；或对新出现的 FS 句柄用 `EFI_SHELL_PROTOCOL.SetMap()` 手动注册映射；兜底是挂载成功后 Print 提示用户手动 `map`。注意 mount 作为 Shell 下运行的 app，退出后新映射必须对后续 Shell 命令可见，这是验收标准。

## 需求要点速查

| 需求（req.md） | 落点 |
|---|---|
| 纯命令行、类 Linux mount | `MountPkg/Application/Mount/`，无 LVGL 依赖 |
| 用 emulator-uefi-shell-app 开发 | `.claude/skills/emulator-uefi-shell-app/SKILL.md`，版本化构建 + QEMU 证据闭环 |
| app 建立自己的目录 | 本目录自包含：`MountPkg/` + `tools/` + `qemu_disk/` + `snapshot/` + `run_logs/` |
| mount -NTFS 挂 NTFS 卷 | 开源只读 NTFS 驱动（efifs 首选/uefi-ntfs 备选）+ LoadImage/StartImage + ConnectController |
| mount -ISO 挂 ISO 文件 | 虚拟 BlockIo（loop 设备）+ UdfDxe（UDF）/efifs Iso9660.inf（纯 ISO9660 补位） |
| 支持更多格式（Linux 主流） | efifs 38 个只读驱动全覆盖（ext4/btrfs/xfs/f2fs/exfat…）；格式名→驱动映射表，主流程零改动扩展 |
| 自动 map -r | ShellExecute("map -r") 或 SetMap()，退出后映射对 Shell 可见 |
| 帮助信息展示 Author: Mike Wu | `mount -?` 帮助输出固定含作者署名行 |
| 帮助/提示/报告信息全英文 | 所有用户可见输出一律英文：标准 UEFI Shell 控制台字体只含拉丁字形，中文 Print 是乱码；串口 DEBUG 日志同用英文便于 grep |
