# Handoff — 新会话接续说明

> 更新：2026-08-29  
> 用途：开新 Cursor 会话时，把本文件 @ 给 agent，或第一句话写「读 handoff.md 继续」。

---

## 项目是什么

**esp32s3-gamebox**：ESP32-S3-DevKitC-1（N16R8）+ ST7789 SPI 屏 + JoyStick Shield + MAX98357，跑 NES/GB/GBC/Genesis 等模拟器。固件 ESP-IDF v5.4，纯 C。

- 权威说明：`AGENTS.md`（agent 必读）、`README.md`、`docs/hardware.md`
- **编译烧录（本机已跑通）**：`docs/journey/esp-idf-toolchain/README.md` ← **新会话烧板子先读这个**
- 验证方式：烧板子 + 看屏 / 串口

---

## 用户背景与长期要求

- **硬件/EDA 小白**，需要步骤清晰、术语有解释
- **所有话题都要落成 Markdown 复盘**，供学习、回忆、可能发社媒
- 规则已写入 `AGENTS.md` §**学习复盘文档**
- 复盘根目录：`docs/journey/`

---

## 固件工具链（2026-08-29 已验证）

| 项 | 值 |
|----|-----|
| IDF | `~/esp/esp-idf`（v5.4） |
| Python | **必须 3.12** venv：`~/.espressif/python_env/idf5.4_py3.12_env`（不要用 3.14） |
| 烧录口 | `/dev/cu.usbmodem*`（丝印 USB）；号码每次 `ls` |
| 状态 | 已 `flash` + `flash-roms`，屏上出现 **GAME/TEST** |
| 测试 ROM | `roms/nes/Super Mario Bros. (World).nes`（自 `~/Downloads`，不入库） |
| 手柄 | E/F 小键本机故障 → **A=START、D=SELECT**；见 `docs/journey/joystick-shield/02-ef-fault-and-face-remap.md` |
| 屏颜色 | `DISP_INVERT_COLOR false`（本机 IPS 开 true 会底片） |

### 每个新终端（复制）

```bash
export HTTP_PROXY=http://127.0.0.1:7890 HTTPS_PROXY=http://127.0.0.1:7890 ALL_PROXY=http://127.0.0.1:7890
export PATH="$HOME/.espressif/python_env/idf5.4_py3.12_env/bin:/opt/homebrew/opt/python@3.12/libexec/bin:/opt/homebrew/opt/python@3.12/bin:/opt/homebrew/bin:$PATH"
. "$HOME/esp/esp-idf/export.sh"
cd /Users/lizhenhe/esp32/esp32s3-gamebox
python -c "import click"   # 自检
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
idf.py build
# 烧录前：按住 BOOT → 点 RESET → 松开 BOOT
idf.py -p "$PORT" -b 115200 flash
# 改过 roms/ 再：
# idf.py -p "$PORT" flash-roms
# 烧完只按 RESET（不要按住 BOOT）
```

详解与踩坑：`docs/journey/esp-idf-toolchain/`（01–04 + README）。

---

## KiCad 载板（并行话题）

### 已完成

| 项 | 状态 | 位置/版本 |
|----|------|-----------|
| uv | 已装 | `brew install uv` → 0.12.5 |
| KiCad | 已装 | `~/Applications/KiCad.app` → **10.0.5** |
| mcp-server-kicad | 已装 | `uv tool install` → **0.20.1** |
| Cursor MCP | 已配置 | `.cursor/mcp.json` |
| KiCad 工程 | 有 | `hardware/kicad/gamebox-shield/` |
| J1/J2 映射 + ERC | v1 | 见 `docs/journey/kicad-gamebox-shield/08-erc-fix-log.md` |
| 复盘 | 有 | `docs/journey/kicad-gamebox-shield/`、`joystick-shield/` |

### 未完成（PCB）

1. **PCB 板框、布线、铺铜**
2. **DRC + Gerber 导出**
3. 打样前肉眼核对 J1/J2 旋转方向 + 对照 `docs/hardware.md`

checklist：`docs/journey/kicad-gamebox-shield/07-next-steps.md`

### KiCad 环境变量

```bash
export PATH="$HOME/.local/bin:$HOME/Applications/KiCad.app/Contents/MacOS:/opt/homebrew/bin:$PATH"
export KICAD_SYMBOL_DIR="$HOME/Applications/KiCad.app/Contents/SharedSupport/symbols"
```

---

## 关键路径

```text
固件工程:       /Users/lizhenhe/esp32/esp32s3-gamebox
IDF:            ~/esp/esp-idf
烧录复盘:       docs/journey/esp-idf-toolchain/README.md
手柄/E-F复盘:   docs/journey/joystick-shield/02-ef-fault-and-face-remap.md
KiCad 工程:     hardware/kicad/gamebox-shield/gamebox-shield.kicad_pro
Bootstrap:      hardware/kicad/bootstrap_gamebox_shield.py
硬件文档:       docs/hardware.md
```

---

## 建议下一会话的第一句话

烧固件 / 改显示：

```text
读 handoff.md 和 docs/journey/esp-idf-toolchain/README.md，按复制块激活环境后编译烧录。
```

继续载板 PCB：

```text
读 handoff.md 和 docs/journey/kicad-gamebox-shield/07-next-steps.md，继续板框/布线/DRC。
```

---

## 文档维护义务（agent）

1. 工具链或烧录方式变了 → 更新 `docs/journey/esp-idf-toolchain/` + 本文件顶部复制块  
2. 手柄接线/映射结论变了 → 更新 `docs/journey/joystick-shield/` + `main/input_gamepad.h`  
3. KiCad 阶段完成 → 更新 `docs/journey/kicad-gamebox-shield/` 勾选  
4. 重大结论变更时改本 `handoff.md` 日期与「未完成」
