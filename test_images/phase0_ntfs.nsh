@echo -off
map
load fs0:\drivers\ntfs_x64.efi
map -r
map
dir fs1:\
type fs1:\ntfs_marker.txt
pause
