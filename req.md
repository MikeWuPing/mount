项目目的：做一个UEFI Shell下mount工具。要求：
1. 纯命令行工具
2. 类似Linux下的mount命令，但运行在UEFI Shell下。
3. 使用emulator-uefi-shell-app进行app开发。edk2在上级目录已经下载，编译工具和QEMU已经安装，不要重复下载和安装
4. app要建立自己的目录，组织形式可以参考上一级目录下面的gufile目录和guedit。后期我会在github上建个私仓。
5. 支持Windows的NTFS，可以用开源的实现，可以是只读的。可以达成类似 mount -NTFS，就会加入NTFS的支持，然后自动进行类似map -r的动作，挂载出NTFS的 fs。
6. 支持ISO的格式，如在U盘中copy进来一个安装iso，我们用mount -ISO 这个文件，就可以把它也mount上了，也是自动进行类似map -r的动作，挂载出改ISO的 fs。
7. 可以支持更多格式，如Linux下的主流格式（ext4、btrfs、xfs等），命令形态沿用mount -<格式>，格式支持要可扩展。
8. 帮助信息中需要展示：Author: Mike Wu。
9. 帮助、提示和report信息全部使用英文（标准UEFI Shell控制台不支持中文显示）。