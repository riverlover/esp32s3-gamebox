#!/usr/bin/env bash
# 历史脚本：以前把顶层 roms/ 打成 Deflate 镜像烧进 flash 的 roms 分区。
# 上游已改成运行时只从 TF 卡读游戏，CMake 也不再注册 flash-roms target。
# 保留这个入口是为了旧命令习惯——现在只打印正确做法后退出。

set -euo pipefail

cat >&2 <<'EOF'
游戏已经改走 TF 卡，不必再烧 ROM 分区。

  1. 把卡拔下来插电脑，按平台建目录后拷 ROM：
       /roms/nes/   /roms/gb/   /roms/gbc/   /roms/snes/   /roms/md/
  2. 插回板子，按 RESET；开机选 GAME 即可看到新游戏。

固件本身仍用：
  idf.py -p /dev/cu.usbmodemXXXX -b 115200 flash monitor

单词发音包（教材/发音参数变了才需要）：
  idf.py flash-word-audio

详见 README.md「换 ROM」和 docs/hardware.md §10。
EOF
exit 1
