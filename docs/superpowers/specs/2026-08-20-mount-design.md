# mount —— UEFI Shell 挂载工具 设计文档

日期：2026-08-20
需求来源：`req.md`（7 条，以该文件为准）
开发流程：`.claude/skills/emulator-uefi-shell-app/SKILL.md`

## 1. 背景与需求

mount 是运行在 UEFI Shell 下的纯命令行工具，对标 Linux `mount`：

- `mount -NTFS`：加载 NTFS 支持（开源只读实现），自动执行等效 `map -r` 的动作，把机器上所有 NTFS 卷挂载出 SimpleFileSystem（fsN:）。
- `mount -ISO <文件>`：把 ISO 文件虚拟成块设备（loop device），由固件驱动栈识别并挂载出其文件系统。
- 格式可扩展：`mount -<格式>` 通用形态，Linux 主流格式（ext4/btrfs/xfs 等）可继续加入；新增格式不允许改动挂载主流程代码。

## 2. 已确认的设计决策

| 决策点 | 结论 | 理由 |
|---|---|---|
| 总体架构 | **方案 A：单 mount.efi**（UEFI_APPLICATION，内建 loop 块设备 + 运行时驱动加载） | 方案 B（MountDxe 驻留驱动 + 前端拆分）的收益依赖卸载功能，v1 不含卸载，属 YAGNI |
| efifs 驱动集成 | **先预编译、后源码** | v1 从 efi.akeo.ie 取作者签名的预编译 .efi 部署到 `qemu_disk/drivers/`；VS2022→VS2019 工具链适配验证过后再考虑 vendor 源码进 `MountPkg/Drivers/` |
| 卸载 | **v1 不含** | req.md 未要求；UEFI 应用 Exit 后安装的协议与打开的文件句柄留存，"挂完退出、挂载存活"天然成立 |
| 挂载语义 | **自动挂全部** | `mount -NTFS` 全量重扫，挂载所有发现的 NTFS 卷并报告新增 fsN: |
| ISO 路径参数 | 必须是当前已可访问卷上的路径（如 `fs0:\images\a.iso`） | 卷还没挂出来就读不到其上的 ISO，属合理限制，帮助中写明 |

## 3. 关键技术事实（已核实）

- **edk2 主线无 NTFS 驱动**（全树搜索无结果），必须引入开源实现。
- **efifs（pbatard/efifs）**：GRUB2 只读文件系统驱动的 UEFI 移植，GPLv3+，一个包 38 个独立 `.inf` 驱动：`Ntfs.inf`、`Iso9660.inf`、`Ext2.inf`（通吃 ext2/3/4）、`Btrfs.inf`、`Xfs.inf`、`F2fs.inf`、`ExFat.inf`、`Udf.inf` 等。官方 EDK2 构建示例用 VS2022（另有 gnu-efi/make 路线），本机 VS2019 适配需实测。
- **edk2 自带 UdfDxe 只支持 UDF/ECMA-167，不支持纯 ISO9660**（源码无 CD001/PVD 处理）。OVMF（OvmfPkgX64.fdf）已编入 UdfDxe；EmulatorPkg.fdf 未编入。
- **Windows 10/11 安装 ISO 是 UDF 格式**（install.wim > 4GB 超出 ISO9660 限制），UdfDxe 可识别；Linux 发行版常用纯 ISO9660/Joliet，需 efifs `Iso9660.inf` 补位。efifs 自带 `Udf.inf`，与 UdfDxe 二选一，避免重复绑定。
- **Win11 安装 ISO 约 5.5~7GB，超 FAT32 单文件 4GB 上限** → 端到端场景必须是 `mount -NTFS` 先挂出 NTFS 数据盘，再 `mount -ISO fs3:\win11.iso`。
- 环境：edk2 `D:\Work\Code\edk2`（VS2019/X64/DEBUG，ACTIVE_PLATFORM=EmulatorPkg）；QEMU `C:\Program Files\qemu\qemu-system-x86_64.exe`（10.2.50-dev）；已验证 OVMF `D:\Work\Code\ContraQwen\OVMF_CODE.fd`。QEMU 实证坑：本 OVMF 不用 `-machine q35`；只读盘走 virtio-blk（ide-hd 拒绝 readonly）；FAT 镜像用无分区 superfloppy。
- **efifs 驱动是 GRUB2"救急级"质量，"有驱动"≠"好用"**：btrfs 的 RAID/zstd、xfs 的 v5/CRC、ext4 的新 feature flag 都可能拒读；全部只读。每个格式逐个实测，格式表维护 Tested 标志。

## 4. 总体架构

方案 A：单一 `mount.efi`（UEFI_APPLICATION），内部三个功能模块：

```
Mount.c      —— 入口：版本戳、参数解析、格式分发表、分发
FsDriver.c   —— 格式驱动加载：查重、LoadImage/StartImage、ConnectController 全扫
LoopDisk.c   —— ISO 虚拟块设备：伪造 BlockIo，后端转发到 ISO 文件读
MapReport.c  —— map 刷新（ShellExecute "map -r"）+ 新旧 FS 句柄差集报告
```

格式驱动（ntfs.efi、iso9660.efi…）作为独立 `.efi` 文件部署在 mount.efi 旁的 `drivers/` 目录，运行时按需加载；app 退出后挂载留存（UEFI 语义：固件不回收应用安装的协议与打开的文件句柄）。

## 5. 项目结构与构建闭环

```
mount/                              # 项目根（未来 GitHub 私仓根）
├── req.md / CLAUDE.md / VERSION.txt
├── MountPkg/
│   ├── MountPkg.dec
│   ├── MountPkg.dsc                # OUTPUT_DIRECTORY → edk2\Build\MountPkg
│   └── Application/Mount/
│       ├── Mount.inf
│       ├── Mount.c / FsDriver.c,.h / LoopDisk.c,.h / MapReport.c,.h
│       └── Version.h               # 构建时生成
├── qemu_disk/                      # mount.efi + startup.nsh + expected_version.txt
│   └── drivers/                    # efifs 预编译 .efi（ntfs/iso9660/ext2…）
├── test_images/                    # 测试资产生成脚本（产物不入库）
│   ├── New-NtfsVhd.ps1             # diskpart 造 64MB NTFS VHD + 中文名标记文件（需管理员）
│   └── New-TestIso.ps1             # IMAPI2FS 造纯 ISO9660+Joliet / 纯 UDF 小 ISO
├── tools/                          # Build-Mount.ps1 / Run-MountQemu.ps1 / Check-Version.ps1
├── snapshot/ / run_logs/           # 证据（不入库）
└── docs/superpowers/specs/         # 设计文档
```

- 构建唯一入口 `tools/Build-Mount.ps1`（沿用 gufile 实证形态）：BUILD+1 → 生成 Version.h → `set PACKAGES_PATH=D:\Work\Code\mount&& edksetup.bat&& build -p MountPkg/MountPkg.dsc -a X64 -t VS2019 -b DEBUG` → 拷贝 mount.efi 进 qemu_disk → 写 expected_version.txt。产物 `D:\Work\Code\edk2\Build\MountPkg\DEBUG_VS2019\X64\mount.efi`。
- 版本戳 CLI 形态：启动首行 `Print` + 串口 `DEBUG()` 双发 `APP_VERSION=<版本+构建号+时间戳>`。禁止复用旧版本产物。
- DSC 沿用 guedit 验证结论：DebugLib→`BaseDebugLibSerialPort`+`SerialIoLib`（COM1 0x3F8，`qemu -serial file:` 捕获）、`FixedDebugPrintErrorLevelLib`、`PcdDebugPropertyMask|0x03` + `PcdFixedDebugPrintErrorLevel|0xFFFFFFFF`；INF 编译标志 `/DEDK2_BUILD /utf-8 /wd4819 /wd4100`；读 shell 参数用 `gEfiShellParametersProtocolGuid`。

## 6. 命令行接口与格式分发表

```
mount                    # 无参：当前 fsN: 映射清单 + 支持的格式表（含已实测/待实测标注）
mount -NTFS              # 加载 ntfs.efi（已加载则跳过）→ 全量重扫 → 报告新增 fsN:
mount -EXT4 / -BTRFS …   # 同构，查表找驱动
mount -ISO <路径>        # 建 loop 块设备 → 重扫 → 报告新增 fsN:
mount selftest           # 隐藏自检（QEMU 回归断言锚点）
mount -?                 # 帮助
```

"选项即格式名"，代码上是分发表查表，无格式专属分支：

```c
typedef struct {
  CHAR16   *Name;          // L"NTFS"
  CHAR16   *DriverFile;    // L"drivers\\ntfs.efi"（相对 mount.efi 所在目录）
  CHAR16   *Notes;         // L"efifs GRUB2 移植，只读"
  BOOLEAN  Tested;         // mount 无参列表里展示
} MOUNT_FORMAT_ENTRY;

STATIC MOUNT_FORMAT_ENTRY mFormats[] = {
  { L"NTFS",  L"drivers\\ntfs.efi",  L"efifs, 只读", FALSE },
  { L"EXT4",  L"drivers\\ext2.efi",  L"efifs Ext2 模块通吃 ext2/3/4, 只读", FALSE },
  { L"BTRFS", L"drivers\\btrfs.efi", L"efifs, 只读, RAID/zstd 卷可能拒读", FALSE },
  { L"XFS",   L"drivers\\xfs.efi",   L"efifs, 只读, v5/CRC 支持待实测", FALSE },
  // 新增格式 = 这里加一行 + drivers/ 放一个文件
};
```

驱动路径解析：取 mount.efi 自身镜像所在目录（`LoadedImage->DeviceHandle` + 设备路径反推），mount.efi 与 drivers/ 整体拷到任何盘都能用。`-ISO` 是分发表之外的保留字，走 loop 设备逻辑。

## 7. 驱动加载与 map 刷新流程（mount -NTFS 执行序列）

```
1. 快照 LocateHandleBuffer(ByProtocol, gEfiSimpleFileSystemProtocolGuid) → 旧 FS 句柄集
2. 查重：LocateHandleBuffer(ByProtocol, gEfiLoadedImageProtocolGuid)，
   比对镜像文件路径后缀 == L"drivers\\ntfs.efi" → 已加载则跳到 4
3. LoadImage(FALSE, gImageHandle, 驱动设备路径) → StartImage；失败即报错返回
4. 全量重连：LocateHandleBuffer(AllHandles)，逐句柄 ConnectController(H, NULL, NULL, TRUE)
   （等价 Shell 的 connect -r；新 NTFS 卷在此刻被绑定出 SimpleFileSystem）
5. map 刷新：ShellExecute(gImageHandle, L"map -r", FALSE, NULL, NULL)
6. 再快照 SimpleFileSystem 句柄集，与 1 求差；每个新句柄：
   DevicePathToText 出设备路径 + GetMapFromDevicePath 拿 fsN: 名
   + OpenVolume→GetInfo(FileSystemVolumeLabelInfo) 拿卷标
7. 打印报告，退出码 0
```

报告样例：

```
MOUNT: 驱动 drivers\ntfs.efi 加载成功
MOUNT: 新增卷 fs3:  卷标="数据盘"  PciRoot(0x0)/Pci(0x1,0x0)/HD(1,GPT,...)
MOUNT: map 已刷新，共新增 1 个卷
```

## 8. ISO loop 虚拟块设备（技术核心）

```c
typedef struct {
  EFI_BLOCK_IO_PROTOCOL    BlockIo;
  EFI_BLOCK_IO_MEDIA       Media;        // BlockSize=2048, ReadOnly=TRUE,
                                         // RemovableMedia=TRUE, LogicalPartition=FALSE
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;  // ISO 文件路径节点链 + 自增 VendorMedia 节点（保证唯一）
  EFI_FILE_PROTOCOL        *BackingFile; // ISO 文件句柄，挂载期间保持打开
  EFI_HANDLE               Handle;       // 新建的虚拟盘句柄
  UINT64                   FileSize;
} LOOP_DISK;
```

关键决策：

- **块大小 2048**：光盘 LBA 语义（与 QEMU `-cdrom` 对固件暴露的一致），UdfDxe/iso9660 驱动按此探测。`LastBlock = FileSize/2048 - 1`，非 2048 整数倍截断并警告。
- **ReadBlocks = SetPosition(LBA×2048) + Read**，无缓存（v1 不优化；浏览目录/读小文件够用，整卷拷 5GB install.wim 慢但正确）。WriteBlocks 恒返回 `EFI_WRITE_PROTECTED`。同步实现、无事件回调，天然避开 TPL 问题。
- **设备路径**：`FileDevicePath()` 给出"物理盘→FAT 分区→\path\file.iso"节点链，末尾追加带自增 GUID 的 VendorMedia 节点——多 ISO 并存不冲突，`map` 输出可读。
- **注册与触发**：`InstallMultipleProtocolInterfaces(&Handle, DevicePath + BlockIo)` → `ConnectController(Handle, NULL, NULL, TRUE)` 只连新句柄。PartitionDxe 探测（ISO 无分区表则跳过）→ 文件系统驱动 Supported() 认领裸设备（与真实光驱行为一致）。
- **格式嗅探只提示不拦截**：读 0x8001 查 `"CD001"`（ISO9660 PVD）、sector 256 附近查 UDF anchor（`BEA01`/`NSR0x`）。纯 ISO9660 且 `drivers\iso9660.efi` 缺失时提前提示，仍继续挂载尝试，让驱动栈自己裁决。
- **多 ISO 并存**：每挂一个建一个 LOOP_DISK 实例，v1 不设人工上限。

数据流：`dir fsN:` → Shell → SimpleFileSystem（UdfDxe/iso9660.efi 提供）→ BlockIo->ReadBlocks → LOOP_DISK ReadBlocks → EFI_FILE Read → 物理盘 FAT 驱动 → 磁盘。

## 9. 错误处理与自检

统一约定：失败打印 `MOUNT: 错误 - <原因>（%r）`，返回非零退出码；串口 DEBUG 同步带模块前缀的详细日志。

| 阶段 | 失败 | 行为 |
|---|---|---|
| 参数解析 | 无此格式名 / -ISO 缺路径 | 打印格式表+用法，退出码 1 |
| 驱动加载 | drivers\xxx.efi 缺失 | 提示从 efi.akeo.ie 下载放入，退出码 2 |
| 驱动加载 | LoadImage 返回 SECURITY_VIOLATION | 明确提示 Secure Boot 拦截，退出码 3 |
| 重扫 | 零新增卷 | "驱动已加载但未发现 X 卷"，退出码 **0**（合法结果） |
| ISO | 文件不存在/太小（<32KB，放不下 PVD） | 明确报错，退出码 4 |
| ISO | 纯 ISO9660 且 iso9660.efi 缺失 | 嗅探后提前提示，仍继续挂载尝试，退出码看结果 |

`mount selftest`：串口输出逐项自检（格式表完整性、驱动文件存在性、BlockIo 安装/卸载往返），`SELFTEST: ALL PASS` 收尾，作 QEMU 无人值守回归断言锚点。

## 10. 验证方案与分期

全部走 emulator-uefi-shell-app 技能的 QEMU 闭环（串口版本断言 + sendkey 脚本 + screendump 截图）：

- **Phase 0（零代码冒烟）**：手工 QEMU → `load fs0:\drivers\ntfs.efi` + `map -r` 对 NTFS 测试 VHD 验证 efifs 驱动；`-cdrom` 挂 UDF/ISO9660 测试 ISO 验证 UdfDxe 行为。**最大风险（预编译驱动兼容性）最早暴露。**
- **Phase 1**：MountPkg 骨架 + 构建闭环 + 版本戳 + `mount selftest` 空壳跑通。
- **Phase 2**：`mount -NTFS` 全链路（查重、加载、重扫、map 刷新、新增卷报告）；验收 = Shell 里 `fs3:` 可读中文名标记文件。
- **Phase 3**：`mount -ISO` loop 设备；首条验收 = **mount 退出后** `dir`/`type` 挂载卷仍正常（验证文件句柄存活假设）；UDF 与纯 ISO9660 两种测试 ISO 各跑一遍。
- **Phase 4**：格式扩展包——ext4/btrfs/xfs 映射表 + 测试镜像（WSL mkfs 生成）逐个实测，更新 Tested 标志。
- **Phase 5（端到端场景）**：diskpart 造 ~8GB 动态 VHD 格式化 NTFS，拷入 Win11 安装 ISO，QEMU 双盘启动 → `mount -NTFS` → `mount -ISO fs3:\win11.iso` → `dir fs4:\` 看到 setup.exe、sources\。这是整条需求链的端到端验收。

测试资产由 `test_images/` 脚本确定性生成，产物（含 Win11 VHD）不入库。

## 11. 实测确认点清单（实现前必须探明的假设）

1. `ShellExecute("map -r")` 能否真正刷新宿主 Shell 映射表（备用：`EFI_SHELL_PROTOCOL.SetMap()` 逐句柄手动注册）。
2. `GetMapFromDevicePath` 在 map 刷新后能否立即取到新 fsN: 名。
3. 已加载驱动查重：LoadedImage 文件路径后缀比对是否可靠。
4. **mount.efi Exit 后 BackingFile 文件句柄持续有效**（ISO 路径成立的前提，Phase 3 首条验收）。
5. efifs 预编译驱动与 OVMF/VS2019 环境的兼容性（Phase 0）。
6. 各格式驱动对真实卷特性集的容忍度（btrfs RAID/zstd、xfs v5、ext4 新 feature flag）——逐个标注 Tested。

## 12. 需求追踪

| req.md 条目 | 设计落点 |
|---|---|
| 1. 纯命令行 | 全局 |
| 2. 类 Linux mount | §6 命令接口 |
| 3. emulator-uefi-shell-app 开发、环境不重复安装 | §5 构建闭环 |
| 4. 自己目录、参照 gufile/guedit | §5 项目结构 |
| 5. mount -NTFS + 自动 map -r | §4 架构、§7 流程 |
| 6. mount -ISO + 自动挂载 | §8 loop 设备 |
| 7. 更多格式可扩展 | §6 格式分发表、§10 Phase 4 |
