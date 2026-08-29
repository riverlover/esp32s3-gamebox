# 06 — 思考与踩坑（含 agent 决策过程）

这一章记录**当时怎么想的、哪里错了、为什么改**。适合复盘，也适合社媒写「踩坑实录」。

---

## 1. 为什么用 mcp-server-kicad，而不是手画？

**背景**：KiCad 工程是结构化文本（s 表达式），人手改极易破坏格式。

**决策**：

- 选用 PyPI 包 **`mcp-server-kicad`**（ProductOfAmerica），支持 KiCad 10、109 个 MCP 工具、byte-preserving 写入
- 用 Python API（`create_project`、`place_component`…）生成 v0，和 Cursor MCP 同源

**备选方案（未选）**：

| 方案 | 没选的原因 |
|------|------------|
| `kicad-mcp-pro` / `kicad-mcp-kipy` | 你点名的是 `mcp-server-kicad` |
| 纯手画 KiCad | 不利于 AI 迭代、不利于你复盘自动化 |
| 只写 Markdown 不接工程 | 无法满足「生成 PCB」 |

---

## 2. KiCad 安装：brew 失败 → DMG 成功

**现象**：`brew install --cask kicad` 要 sudo 写系统目录。

**思考**：agent 环境无法输入密码 → 不能假设 brew cask 一定可用。

**修正**：下载官方 `kicad-unified-universal-10.0.5.dmg`，复制到 `~/Applications/`。

**二次踩坑**：第一次 `cp` 路径写错——DMG 里是 `/Volumes/KiCad/KiCad/KiCad.app`，不是 `/Volumes/KiCad/KiCad.app`。

**给你的 takeaway**：macOS 装 KiCad，**拖进应用程序文件夹** 是最省心的；brew 要 sudo 时改用 DMG。

---

## 3. 符号库名字：Conn_01x22_Female 不存在

**现象**：`place_component` 报错 `Conn_01x22_Female not found`。

**原因**：KiCad 10 自带库叫 `Conn_01x22`，没有 `_Female` 后缀；母座/公座靠**封装**（`PinSocket` vs `PinHeader`）区分，不靠符号名。

**修正**：符号用 `Connector_Generic:Conn_01x22`，封装用 `PinSocket_1x22_P2.54mm_Vertical`。

---

## 4. 2×5 手柄座：Conn_02x05 也不存在

**现象**：`Conn_02x05` 找不到。

**原因**：KiCad 10 里是 `Conn_02x05_Odd_Even` 等变体，引脚编号规则不同。

**进一步思考**：Shield 实物是 **6×2 黄色排针**，但固件只用了 **10 根线**。v0 改成 **1×10** 座子，按 `input_gamepad.h` 杜邦线顺序编号，降低原理图复杂度。

**trade-off**：插 Shield 时仍要对着丝印认针位，但 DevKit 侧 10 根线焊死在板上，比 10 根杜邦可靠。

---

## 5. 原理图页边距：A4 高度 210 mm

**现象**：`add_text` 在 y=213 失败，`A4` 只有 210 mm 高。

**修正**：把页脚说明文字上移到 y≤209。

**教训**：自动化放注解时要查页尺寸，或改用 `set_page_size` 换 A3。

---

## 6. 自动连线的核心坑：符号 pin 号 ≠ DevKit 物理顺序

**这是 v0 最重要的问题。**

脚本里假设「J1 的 pin 4 = GPIO4」，但 KiCad 连接器符号的 pin 编号取决于：

- 符号是 `Conn_01x22` 还是带 `Odd_Even` 变体
- 元件在原理图上的 **旋转角度**
- 引脚在符号定义里的编号顺序

**实测结果**（跑完 `update_pcb_from_schematic` 后查焊盘网络）：

- 部分网络正确：`+3V3`、`GND`、`GPIO15`–`GPIO18`、屏的 SCK/RST/DC/CS/BL 等
- 部分错位：例如 J1 pad 17 被标成 `GND`，而设计意图是 GPIO11 MOSI
- J2 上本应接 GPIO1/2 的脚，很多仍是 `unconnected`

**为什么没在第一版就发现？**

- `wire_pins_to_net` 在**符号坐标系**里成功了（原理图上有同名 label）
- 但 **PCB 网表** 绑定的是焊盘物理 pin；符号旋转后「pin 4」不再对应 DevKit 丝印上的「第 4 个脚」

**正确做法（v1 计划）**：

1. 在 KiCad 原理图里把 J1/J2 **旋转到与 DevKit 丝印一致**，用 `get_pin_positions` 核对
2. 或不用「pin 数字」，改用 **每根线的全局标签 + 手工飞线** 在原理图里画清
3. 对照 `docs/hardware.md` 双排针表，**逐脚 ERC + 万用表蜂鸣** 打样前必做

**这和你仓库里摇杆排障的哲学一致**：一次只改一个变量、用测量验证，别信「看起来对」的间接推理。

---

## 7. power 符号 API 参数顺序

**现象**：`add_power_symbol("+3V3", 40, 45)` 报缺参数 `y`。

**原因**：API 是 `add_power_symbol(lib_id, reference, x, y)`，不是 `(net, x, y)`。

**修正**：`add_power_symbol("power:+3V3", "#PWR01", 40, 45)`。

---

## 8. MCP `cwd` 为什么要指到工程目录？

mcp-server-kicad 默认相对路径解析自 `cwd`。指到 `gamebox-shield/` 后，工具里写 `gamebox-shield.kicad_pcb` 不会找错目录。

---

## 9. 记录本文档的决策（meta）

你提出：**后面所有话题都要 md 复盘，可能要发社媒，思考过程也要留，并写入 AGENTS.md**。

**执行**：

- `AGENTS.md` 新增 §学习复盘文档
- `docs/journey/` 目录 + `kicad-gamebox-shield/` 系列
- 本文件专门存「想的过程」

这样 agent 以后换话题（比如「打样嘉立创」「画外壳」）会按同一模板继续写，不会只留在聊天窗口里。

---

## 踩坑速查表

| 症状 | 可能原因 | 处理 |
|------|----------|------|
| brew 装 KiCad 失败 | 要 sudo | 用官方 DMG → ~/Applications |
| 符号找不到 | 库名/符号名与 KiCad 10 不符 | `grep` 符号库或让 MCP `list` |
| 原理图有网、PCB 脚错了 | 连接器旋转/pin 编号 | 人工核对双排针表 |
| MCP 找不到 kicad-cli | PATH | `.cursor/mcp.json` 的 `env.PATH` |
| 文本放不进原理图 | 超出 A4 | 上移或换页 |

下一步：[07-下一步](07-next-steps.md)
