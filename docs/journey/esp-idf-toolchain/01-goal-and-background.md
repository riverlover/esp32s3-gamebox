# 01 — 目标与背景

## 要解决什么

2026-08-29 在本机（macOS）第一次把 gamebox 固件跑通：

- 屏已接线，需要 **编译 → 烧录 → 看到 GAME/TEST**
- 仓库 `AGENTS.md` 写的是 `. ~/esp/esp-idf/export.sh`，但当时本机 **根本没有** `~/esp/esp-idf`，也没有 `~/.espressif`

所以这一整套是「从零装工具链 + 记下本机特有坑」，不是改模拟器逻辑。

## 和别的文档怎么分工

| 文档 | 管什么 |
|------|--------|
| `AGENTS.md` / `README.md` | 项目约定、引脚、架构 |
| **本系列** | **这台 Mac 上怎么把 IDF 装对、命令怎么敲** |
| `handoff.md` | 新会话第一眼：摘要 + 复制命令 |

## 成功判据（已达成）

1. `idf.py build` 退出码 0，生成 `build/esp32s3-gamebox.bin` + `build/roms.bin`
2. `idf.py -p /dev/cu.usbmodem… flash` + `flash-roms` 成功
3. 屏上出现 **GAME / TEST** 开机选择
