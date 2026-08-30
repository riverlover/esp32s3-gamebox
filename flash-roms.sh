#!/usr/bin/env bash
# 只烧顶层 roms/ 打包出的 ROM 分区（offset 0x210000）。
# 加 / 删 / 改名游戏后跑这个；改固件代码仍用 idf.py flash。
#
# 用法（在仓库根目录）：
#   ./flash-roms.sh              # 自动选第一个 cu.usbmodem* / cu.usbserial-*
#   ./flash-roms.sh /dev/cu.usbmodem1101
#
# 烧前：按住 BOOT → 点 RESET → 松开 BOOT
# 烧完：只按 RESET 进应用（菜单里应能看到新游戏）

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# 本机默认 python3 可能是 3.14，会坏 click；强制 IDF 的 3.12 venv 在前
export PATH="$HOME/.espressif/python_env/idf5.4_py3.12_env/bin:/opt/homebrew/opt/python@3.12/libexec/bin:/opt/homebrew/opt/python@3.12/bin:/opt/homebrew/bin:$PATH"
export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"

if [[ ! -f "$IDF_PATH/export.sh" ]]; then
  echo "找不到 ESP-IDF：$IDF_PATH/export.sh" >&2
  exit 1
fi

# shellcheck source=/dev/null
. "$IDF_PATH/export.sh"

if [[ $# -ge 1 ]]; then
  PORT="$1"
else
  PORT="$(ls /dev/cu.usbmodem* /dev/cu.usbserial-* 2>/dev/null | head -1 || true)"
fi

if [[ -z "${PORT:-}" ]]; then
  echo "没有找到串口。请插上板子（优先丝印 USB → cu.usbmodem*），或手动传端口：" >&2
  echo "  ./flash-roms.sh /dev/cu.usbmodemXXXX" >&2
  exit 1
fi

if [[ ! -e "$PORT" ]]; then
  echo "端口不存在：$PORT" >&2
  exit 1
fi

echo "==> 工程：$ROOT"
echo "==> 端口：$PORT"
echo "==> 打包并烧录 ROM 分区（flash-roms）…"
echo "    若卡住：按住 BOOT → 点 RESET → 松开 BOOT，再重跑本脚本"
echo

idf.py -p "$PORT" flash-roms

echo
echo "==> 完成。只按 RESET 重启；选 GAME 进菜单看新 ROM。"
