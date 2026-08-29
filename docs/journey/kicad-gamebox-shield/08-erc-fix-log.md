# 08 — ERC 修正实录（bootstrap v1）

**日期**：2026-08-22  
**触发**：v0 自动连线后 ERC 60 条违规，含 GPIO11 与 GND 同网等致命错连。

## 问题根因（两条）

### 1. J1/J2 竖直叠放 → Y 坐标重叠

v0 把两个 `Conn_01x22` 上下叠放（中心距仅 40 mm），而单排 22 pin 跨度约 54 mm。  
结果 J1 底脚与 J2 顶脚落在同一水平线上，KiCad 把不同网络的导线并成同一网。

| 重叠 Y | v0 错连后果 |
|--------|-------------|
| 85.09 mm | J1 pin17（GPIO11）↔ J2 pin1（GND） |
| 87.63 mm | J1 pin18（GPIO12）↔ J2 pin2（TX） |

### 2. 脚本假设「符号 pin 号 = DevKit 丝印顺序」但未固化映射表

pin 编号本身没错（`Conn_01x22` pin1 在顶端、与 hardware.md 左→右一致），  
问题是布局导致导线图合并，而非映射表写反。

## 修正内容（`bootstrap_gamebox_shield.py`）

1. **J1/J2 改水平并排**（中心 x=48 / x=98，同 y=95），彻底消除 Y 重叠
2. **`DEVKIT_J1_NETS` / `DEVKIT_J2_NETS` 映射表**，与 `docs/hardware.md` §2 一一对应
3. **未用 DevKit 脚加 `no_connect_pin`**（RST、GPIO3/46、5Vin、TX/RX、其余空闲 GPIO）
4. **J6 接 SPK+/SPK-**；J5 pin6（SD）标 NC
5. **删掉右侧装饰性全局标签**（v0 悬空导致 `label_dangling`）

## ERC 结果对比

| 版本 | violation_count | 致命问题 |
|------|-----------------|----------|
| v0 | 60 | GPIO11↔GND、GPIO14↔GPIO1、大量悬空标签 |
| v1（重生后） | **2（均为 warning）** | 无 error |

剩余 2 条 warning：`SPK+` / `SPK-` 各只连 J6 一脚——符合设计（另一端焊在 MAX98357 模块 SPK 焊盘，不在 6pin 座里）。

## 抽样验证（MCP `get_net_connections`）

| 网络 | 应连 | 实测 |
|------|------|------|
| GPIO11_MOSI | J1 pin17 + J3 pin4 | ✓ 仅这两处 |
| GPIO12_SCK | J1 pin18 + J3 pin3 | ✓ |
| GPIO1_JOY_X | J2 pin4 + J4 pin3 | ✓ |
| GND | J1:22, J2:1/21/22, J3/4/5 GND | ✓ 无信号脚混入 |

## 重生命令

```bash
export PATH="$HOME/.local/bin:$HOME/Applications/KiCad.app/Contents/MacOS:$PATH"
export KICAD_SYMBOL_DIR="$HOME/Applications/KiCad.app/Contents/SharedSupport/symbols"
cd /Users/lizhenhe/esp32/esp32s3-gamebox
uv run --with mcp-server-kicad python hardware/kicad/bootstrap_gamebox_shield.py
```

## 下一步

- [ ] KiCad 打开原理图肉眼核对 J1/J2 与 DevKit 丝印（USB 朝右）
- [ ] PCB 板框 + 布线 + DRC → 见 [07-下一步](07-next-steps.md) 阶段 B–D

## PCB 布局修正（同次会话追加）

v1 原理图已水平并排 J1/J2，但 `build_pcb()` 仍把两排 **沿 Y 叠放**（中心距 28 mm），
而 `PinSocket_1x22` 沿 Y 长约 54 mm → PCB 上看起来像一根 44 pin 假座子。

**修正**：J1/J2 **沿 X 相距 28 mm**（与 DevKit 实物两排平行一致），外设座在右侧，并画 **86×58 mm** 板框。

```text
  J3/J4/J5/J6（横排，右侧）
       │
  J1 ║  J2   ← 两列母座，中心距 28 mm
```

重生后 J1@(12,29)、J2@(40,29)，courtyard 在 X 方向不再重叠。

---

回到系列索引：[README.md](README.md)
