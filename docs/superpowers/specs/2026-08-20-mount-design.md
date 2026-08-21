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
| 输出语言 | **全部用户可见输出（帮助/提示/报告/错误）一律英文** | 标准 UEFI Shell 控制台字体只含拉丁字形，中文 Print 输出为乱码（gufile 能显示中文靠的是 LVGL 自带字库渲染，CLI 无此条件）；串口 DEBUG 日志同用英文，便于 grep |

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

帮助输出固定包含作者署名（req.md 第 8 条），全部英文（req.md 第 9 条），样例：

```
MOUNT - UEFI Shell mount tool  v1.0.0.12
Author: Mike Wu

Usage:
  mount              List current fsN: mappings and supported formats
  mount -NTFS        Load NTFS support and mount all NTFS volumes
  mount -ISO <file>  Mount an ISO image file as a new volume
  mount -<FORMAT>    Load support for FORMAT (EXT4/BTRFS/XFS...)
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
  { L"NTFS",  L"drivers\\ntfs.efi",  L"efifs, read-only", FALSE },
  { L"EXT4",  L"drivers\\ext2.efi",  L"efifs Ext2 covers ext2/3/4, read-only", FALSE },
  { L"BTRFS", L"drivers\\btrfs.efi", L"efifs, read-only; RAID/zstd volumes may fail", FALSE },
  { L"XFS",   L"drivers\\xfs.efi",   L"efifs, read-only; v5/CRC support TBD", FALSE },
  // 新增格式 = 这里加一行 + drivers/ 放一个文件（Notes 会被打印，必须英文）
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

报告样例（英文输出，串口 DEBUG 同步）：

```
MOUNT: driver drivers\ntfs.efi loaded
MOUNT: new volume fs3:  label="DataDisk"  PciRoot(0x0)/Pci(0x1,0x0)/HD(1,GPT,...)
MOUNT: map refreshed, 1 new volume(s)
```

## 8. ISO loop 虚拟块设备（技术核心）

```c
typedef struct {
  EFI_BLOCK_IO_PROTOCOL    BlockIo;
  EFI_BLOCK_IO_MEDIA       Media;        // BlockSize=2048, ReadOnly=TRUE,
                                         // RemovableMedia=TRUE, LogicalPartition=FALSE
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;  // ISO 文件路径节点链 + VendorMedia 节点
                                         // （节点 GUID 由运行计数器生成，每实例唯一）
  EFI_FILE_PROTOCOL        *BackingFile; // ISO 文件句柄，挂载期间保持打开
  EFI_HANDLE               Handle;       // 新建的虚拟盘句柄
  UINT64                   FileSize;
} LOOP_DISK;
```

关键决策：

- **块大小 2048**：光盘 LBA 语义（与 QEMU `-cdrom` 对固件暴露的一致），UdfDxe/iso9660 驱动按此探测。`LastBlock = FileSize/2048 - 1`，非 2048 整数倍截断并警告。
- **ReadBlocks = SetPosition(LBA×2048) + Read**，无缓存（v1 不优化；浏览目录/读小文件够用，整卷拷 5GB install.wim 慢但正确）。WriteBlocks 恒返回 `EFI_WRITE_PROTECTED`。同步实现、无事件回调，天然避开 TPL 问题。
- **设备路径**：`FileDevicePath()` 给出"物理盘→FAT 分区→\path\file.iso"节点链，末尾追加 VendorMedia 节点（GUID 由运行计数器生成，每实例唯一）——多 ISO 并存不冲突，`map` 输出可读。
- **注册与触发**：`InstallMultipleProtocolInterfaces(&Handle, DevicePath + BlockIo)` → `ConnectController(Handle, NULL, NULL, TRUE)` 只连新句柄。PartitionDxe 探测（ISO 无分区表则跳过）→ 文件系统驱动 Supported() 认领裸设备（与真实光驱行为一致）。
- **格式嗅探只提示不拦截**：读 0x8001 查 `"CD001"`（ISO9660 PVD）、sector 256 附近查 UDF anchor（`BEA01`/`NSR0x`）。纯 ISO9660 且 `drivers\iso9660.efi` 缺失时提前提示，仍继续挂载尝试，让驱动栈自己裁决。
- **多 ISO 并存**：每挂一个建一个 LOOP_DISK 实例，v1 不设人工上限。

数据流：`dir fsN:` → Shell → SimpleFileSystem（UdfDxe/iso9660.efi 提供）→ BlockIo->ReadBlocks → LOOP_DISK ReadBlocks → EFI_FILE Read → 物理盘 FAT 驱动 → 磁盘。

## 9. 错误处理与自检

统一约定：失败打印 `MOUNT: error - <reason> (%r)`，返回非零退出码；串口 DEBUG 同步带模块前缀的详细日志。所有消息字符串为英文（见 §2 输出语言约束）。

| 阶段 | 失败 | 行为 |
|---|---|---|
| 参数解析 | 无此格式名 / -ISO 缺路径 | `MOUNT: error - unknown option/format 'XXX'` + 打印用法，退出码 1 |
| 驱动加载 | drivers\xxx.efi 缺失 | `MOUNT: error - drivers\xxx.efi not found (get it from efi.akeo.ie)`，退出码 2 |
| 驱动加载 | LoadImage 返回 SECURITY_VIOLATION | `MOUNT: error - driver blocked by Secure Boot`，退出码 3 |
| 重扫 | 零新增卷 | `MOUNT: driver loaded, but no NTFS volume found`，退出码 **0**（合法结果） |
| ISO | 文件不存在/太小（<32KB，放不下 PVD） | `MOUNT: error - cannot open <path>` / `file too small, not a valid ISO`，退出码 4 |
| ISO | 纯 ISO9660 且 iso9660.efi 缺失 | `MOUNT: warn - ISO9660 detected but drivers\iso9660.efi missing`，仍继续挂载尝试，退出码看结果 |

`mount selftest`：串口输出逐项自检（格式表完整性、驱动文件存在性、BlockIo 安装/卸载往返），`SELFTEST: ALL PASS` 收尾，作 QEMU 无人值守回归断言锚点。

## 10. 验证方案与分期

全部走 emulator-uefi-shell-app 技能的 QEMU 闭环（串口版本断言 + sendkey 脚本 + screendump 截图）。**v1 范围 = Phase 1~3**（骨架 + NTFS + ISO，即 req.md 第 5/6 条）；Phase 0 是开工前置验证，Phase 4/5 为后续迭代。

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

### Phase 0 实测结果（2026-08-20，Task 4 冒烟）

环境：ContraQwen OVMF + QEMU 10.2.50-dev；证据链见 `.superpowers/sdd/2026-08-20-mount-v1/task-4-report.md` 及对应 run_logs/snapshot。三个场景全部按 `test_images/phase0_*.nsh` 跑通，结论如下。

- **条目 5（efifs 兼容性）→ 确认可用，但有介质呈现前提**。`iso9660_x64.efi` 挂 IDE CD-ROM（2048B ATAPI）成功绑定纯 ISO9660 卷：`dir fs1:\` 列出 `marker.txt`/`sub`，`type` 读出 `ISO9660-MOUNT-OK`；`ntfs_x64.efi` 挂 IDE 硬盘（PartitionDxe 正常生成 `HD(1,MBR,0xEA31AC2F)` 子节点）成功：`type fs1:\ntfs_marker.txt` 得 `NTFS-MOUNT-OK`。**但本 OVMF 的 VirtioBlkDxe 是老式协议集**（BlockIo only，无 BlockIo2/DiskInfo，盘符直接落在 PCI 设备句柄上）：efifs 驱动在该类句柄上绑定失败，PartitionDxe 也不为其建分区子节点——virtio-blk 呈现下 ISO 与 NTFS 双双失败。推论：测试与验收一律走 IDE 呈现；**Task 8 的 LoopDisk 必须在新句柄上安装完整协议集（BlockIo + BlockIo2，独立设备路径节点），这是 efifs 能否绑定的前提，Task 8 开工即验**。
- **§3 UdfDxe 疑问 → 确认**：纯 ISO9660 CD 不加载 efifs 驱动时，固件栈（含 UdfDxe）不产生任何 SimpleFileSystem，`map -r` 后仍无 fs1:，`dir fs1:\` 报 `File Not Found`。纯 ISO9660 必须由 efifs `Iso9660.inf` 补位，UdfDxe 只认 UDF/ECMA-167。
- **条目 1（map -r 语义）→ 部分澄清**：EDK2 Shell 源码实证 `map -r` 只做 `ShellCommandCreateInitialMappingsAndPaths()` + `ProbeForMediaChange()`，**重建 Shell 映射表但不触发任何驱动绑定**；绑定由 `load` 命令自带的 ConnectAllEfi（不带 `-nc` 时 StartImage 后对全部句柄 `ConnectController`）或显式 `connect -r` 完成。mount.efi 的正确序列是：LoadImage/StartImage → 自己调 `ConnectController()` → 再借 `map -r` 刷新 Shell 映射。ShellExecute("map -r") 能否刷新宿主表仍待 Phase 2 验证，但"先连接后刷新"的职责划分已明确。
- **QEMU 呈现层新坑（随测随记）**：vpc 挂 ide0-hd1 时本 OVMF AtaBusDxe 静默不枚举（devtree 中整块盘消失）；vpc 挂 virtio-blk 可枚举但落入上述老式句柄问题。因此 Run-MountQemu 规则固化为：`.vhd` 一律先 `qemu-img convert` 成 raw 再挂 IDE；ISO 一律 `-drive media=cdrom` 挂 ATAPI；只读裸盘保留 virtio 路径（仅适用于不需要分区/绑定的场景）。
- **条目 6 进展**：NTFS（Windows diskpart 格式化）Tested=TRUE；ISO9660（IMAPI2FS 纯 ISO）Tested=TRUE。ext4/btrfs/xfs 留待 Phase 4。

### Task 9 实测结果（2026-08-20，mount -ISO 端到端）

- **条目 4（BackingFile 跨退出存活）→ 结论修正后确认**。文件句柄本身确实跨 mount.efi 退出存活（FatPkg 文件实例在固件侧），但**应用程序安装的协议回调函数随应用镜像一起卸载**——Task 8 的"挂落后退出"设计在首次退出后读取时崩溃：`dir fs1:\` 触发 #UD，RIP 落在 iso9660 镜像头区（偏移 0x1AF < .text RVA 0x240），寄存器呈 ReadBlocks 调用帧（RDX=MediaId "MOUN"、R8=LBA、R9=长度）。Task 8 的"persistence smoke"只证明了句柄存活，从未做退出后的真实读取；同镜像页帧复用掩盖了问题。**修复：LoopDxe 常驻驱动**（`MountPkg/Drivers/LoopDxe/`，DXE_DRIVER，入口安装 `MOUNT_LOOP_FACTORY_PROTOCOL`；boot-service driver 入口返回后镜像保持常驻），mount.efi 只做校验/嗅探/编排。修复后 `mount.efi` 退出 → `dir fsN:\` → `type fsN:\marker.txt` 全链绿（ISO9660/UDF 双场景），条目 4 正式关闭。
- **新坑：本 OVMF（EfiFs CI DEBUG 构建）固件死锁**——`ConnectController` 一个**没有任何文件系统驱动能认领**的 BlockIo 句柄时，某 FV 驱动递归 `EfiAcquireLock` 触发 `ASSERT (UefiLib.c: Lock->Lock == EfiLockReleased)` 并 `CpuDeadLoop`（QMP 采样 vCPU 单点自旋 0x0e6a4219，栈上留有 ASSERT 文本与 0x0fe77112 重复帧；串口无输出）。纯数据盘、无驱动的纯 ISO9660 盘均可复现；UDF 盘（UdfDxe 认领）与已加载 efifs 驱动的盘不复现。**规避**：嗅探为纯 ISO9660 且未加载 iso9660 驱动时（Task 4 反证固件栈必不认领），mount 跳过 ConnectController，保留 loop 设备并提示 `load drivers\iso9660_x64.efi` + `map -r` 恢复（已实证：load 的 ConnectAllEfi 完成绑定，`type fs1:\marker.txt` 得 ISO9660-MOUNT-OK）。已知残余：存在未认领 loop 设备时再跑 `mount -<FORMAT>` 的全局重扫仍会踩同一固件缺陷——先加载对应 FS 驱动即可，记入待办。
- **UDF 路径一次通过**：UdfDxe 直接绑定 loop 设备（vendor-media 节点 DP、LogicalPartition=FALSE、2048B 块全绿），§3 的 UdfDxe 疑问在 loop 形态下同步关闭。`map -r` 输出中 loop 卷 DP 形如 `.../Ata(0x0)/\iso9660_test.iso/VenMedia(5C6D7E8F-...)`，可读性符合 §8 预期。

### Task 10 实测结果（2026-08-20，最终打磨与全量回归）

最终版本 `0.1.0+59` 完成全量回归，所有场景串口版本断言一致、功能断言通过：

- **无参模式**：`mount` 输出版本行、`map` 当前映射表（FS0:）、`Author: Mike Wu` 帮助、格式表（NTFS `[tested]`，EXT4/BTRFS/XFS 未实测）。
- **帮助模式**：`mount -h` 输出完整用法与作者署名（UEFI Shell 把 `-?` 解释为命令自身帮助，故回归改用 `-h`）。
- **`-NTFS` 挂载**：NTFS 测试 VHD 挂载为 `fs1:`，卷标 `MOUNTTEST`，`type fs1:\ntfs_marker.txt` 读出 `NTFS-MOUNT-OK`。
- **`-ISO iso9660_test.iso`**：先 `load fs0:\drivers\iso9660_x64.efi`，再挂载纯 ISO9660 镜像，新卷 `fs1:` 卷标 `ISO9660TEST`，`type fs1:\marker.txt` 读出 `ISO9660-MOUNT-OK`；mount.efi 退出后卷仍可读。
- **`-ISO udf_test.iso`**：不加载额外驱动，内置 UdfDxe 直接绑定 loop 设备，新卷 `fs1:` 卷标 `UDFTEST`，`type fs1:\marker.txt` 读出 `UDF-MOUNT-OK`。

v1 范围（Phase 0~3）全部关闭；Phase 4（ext4/btrfs/xfs 等格式逐个实测并更新 `Tested` 标志）留待后续迭代。

### 2026-08-21 跟进：真实 ISO 实测 + ISO9660 驱动自动加载（build 0.1.0+61/62）

- **`mount -ISO` 不再要求手工 `load` iso9660 驱动**。此前纯 ISO9660 且驱动未加载时只打印提示并跳过连接（Task 9 死锁规避），违背 req.md"自动挂载"要求。现从 `MountRunFormat` 提取 `MountLoadDriver()`（FsDriver.c，查重+LoadImage+StartImage+全部报错文案），`MountRunIso` 嗅探到纯 ISO9660（`Iso9660 && !Udf`）且驱动未加载时自动调用——与 `-<FORMAT>` 行为同构，且被驱动认领的句柄天然免疫 OVMF 死锁。驱动文件缺失时退化为原 keep-loop + 手工 `load` + `map -r` 恢复路径（exit 0）。
- **工具**：`Run-MountQemu.ps1` 新增 `-Window`（SDL 可见窗口，screendump 证据不受影响）与 `-Manual`（只拉起 QEMU，不做脚本驱动/版本断言/杀进程，供人工交互）。
- **真实 ISO 验证**（均为用户实盘文件，入 `test_images/` 备份、不入库）：
  - `ubuntukylin.iso`（Ubuntu Kylin 16.04.2 LTS，1.6GB，纯 ISO9660）：自动加载 iso9660 驱动路径，`fs1:` 卷标 `Ubuntu-Kylin 16.04.2 LTS amd64`，mount 退出后 `dir fs1:\` 列出 casper/pool/md5sum.txt 等全根目录。
  - `win10_iot.iso`（Windows 10 IoT Core，710MB，UDF bridge：BEA01/NSR02+CD001）：零外部驱动，固件 UdfDxe 直接绑定 loop 设备，`fs1:` 卷标 `AMBM_x86FREO_EN-US_DV5`，含 `Windows_10_IoT_Core_for_Mbm.msi`（743MB）可列出。
  - `Windows_10.iso`（4.98GB，UDF）：超过 FAT32 单文件 4GiB−1 上限，无法放入启动 FAT 卷——Phase 5 的 NTFS 中转路径（`mount -NTFS` 先挂 NTFS 数据盘再 `mount -ISO fs3:\win10.iso`）是唯一可行方案，已记录于 §3。
- **新 QEMU 坑（人工交互场景）**：SDL 窗口键盘输入受宿主输入法影响——中文输入法处于中文模式时吞掉小写字母（进拼音组字缓冲）、数字透传，表现为"数字能打字母不能打"；切英文模式（Shift/中英切换）即恢复。自动化注入走 QMP sendkey（虚拟 PS/2），天然免疫宿主输入法，QEMU 按键自动重复由 guest PS/2 驱动产生（按住键会刷屏）。
- **新坑（宿主侧）**：Windows 双击 ISO 会自动挂载为虚拟光驱（Explorer 弹 I: 盘），挂载句柄锁住 ISO 文件，导致构建删 qemu_disk 失败（Remove-Item IOException）。处理：Shell.Application Eject 弹出虚拟光驱即释放；与 QEMU 无关，但会阻塞 Build-Mount.ps1。

### Phase 4 ext4 实测（2026-08-21，build 0.1.0+65）

- **测试资产**：`test_images/mkext4img.py`——纯 Python 手写的最小 ext4 镜像生成器（本机无 WSL/Docker/mkfs，且禁止安装；沿用 mkfatimg.py 的"确定性手写镜像"路线）。8MiB、1K 块、单块组，feature 保守（compat=FILE_TYPE、incompat=EXTENTS，无 journal/metadata_csum/64bit/flex_bg）。产物 `test_images/ext4_test.img` 不入库。
- **生成器两处 bug 由 QEMU 实测揪出**（libmagic 全认、efifs/GRUB 拒绝——GRUB 校验更严）：superblock 特性字段整体偏移 2 字节（漏写 `s_block_group_nr`），inode extent 条目写在 `0x2C` 而非 `0x34`（覆盖 extent 头 max/generation）。均已修复，inspect 增加"沿 inode 表走读"路径（superblock→GDT→inode→extent→数据块）防回归。
- **结果**：`mount -EXT4` 全绿——驱动自动加载、`fs1:` 卷标 `EXT4TEST`、`type fs1:\marker.txt` 读出 `EXT4-MOUNT-OK`，版本断言一致。**EXT4 Tested=TRUE**（FsDriver.c 格式表）。
- **边界说明**：Tested 标志覆盖本镜像的保守特性集；真实世界 ext4 卷带 metadata_csum/64bit/flex_bg/journal 等特性，efifs（GRUB）对其中一部分支持、一部分拒读——拿到真实卷镜像后仍需逐卷验证（记为 Phase 4 延伸项）。

### 2026-08-21 范围决策

- **efifs 等二进制驱动的源码化整体推迟**至项目收尾、发布之后（用户决策，2026-08-21）：当前继续使用 efi.akeo.ie 预编译件 + README 的 GPLv3 源码指向，不投入 VS2019 工具链适配。`drivers/README.md` 已同步标注。
- `udf_x64.efi` 已删除（无引用、UDF 由固件 UdfDxe 覆盖，提交 0ae7b40）。

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
| 8. 帮助信息展示 Author: Mike Wu | §6 帮助输出 |
| 9. 帮助/提示/报告信息全英文 | §2 输出语言约束、§6/§7/§9 全部输出样例 |
