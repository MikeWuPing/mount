@echo -off
map
load fs0:\drivers\iso9660_x64.efi
map -r
map
dir fs1:\
type fs1:\marker.txt
pause
