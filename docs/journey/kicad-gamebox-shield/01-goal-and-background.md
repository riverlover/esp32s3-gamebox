# 01 — 目标与背景

## 你想解决什么问题？

当前 Gamebox 是 **ESP32-S3-DevKitC-1 + 杜邦线 + 洞洞板/飞线** 拼出来的：

- ST7789 SPI 屏
- JoyStick Shield V1.A（摇杆 + 6 键）
- MAX98357 I2S 功放 + 喇叭

固件已经稳定跑 NES / GB / GBC / Genesis 等，但硬件连接靠手工飞线。仓库 `README.md` 里记过一次**摇杆两轴被洞洞板电阻性短路**的排障——根因是两根信号线之间几百欧耦合，万用表通断档还不响。

**载板（Shield）的目标**：把 DevKitC 和外设插座做到一块 PCB 上，减少飞线、降低接错/短路概率，以后装机更像「插上去就能玩」。

## 为什么选 KiCad + MCP + Cursor？

| 组件 | 作用（小白版） |
|------|----------------|
| **KiCad 10** | 免费开源 EDA：画原理图、布局 PCB、导出 Gerber 给嘉立创/JLC 打板 |
| **uv** | 现代 Python 包管理器，一条命令跑 `mcp-server-kicad`，不用自己配 venv |
| **mcp-server-kicad** | 让 AI（Cursor）通过 MCP 协议**直接改 KiCad 工程文件**，而不是让你手抄 s 表达式 |
| **Cursor MCP** | 在 IDE 里接上 MCP 服务器，对话里可以说「跑 DRC」「把这个电阻挪过去」 |

你当时的原话是：**「全部安装（推荐）→ KiCad 10 + uv + mcp-server-kicad + Cursor MCP」**，并要根据本项目分析生成 PCB。

## 和「只写固件」的关系

载板设计**不能拍脑袋接脚**。本仓库的优势是：

- `docs/hardware.md` 有 DevKitC 双排针顺序、ST7789 接线、MAX98357 接线
- `main/display.h`、`main/input_gamepad.h`、`main/audio_output.c` 里 `#define` 了实际 GPIO

所以流程是：**固件引脚 → 原理图网络 → PCB 封装 →（将来）布线打板**，而不是先画板再改代码。

## 本阶段交付物（v0 定义）

v0 **不是**可直接投产的成品板，而是：

1. 工具链可重复安装
2. KiCad 工程骨架（连接器位号、网络名、封装）
3. 文档让你能复盘、能分享、能接着在 KiCad 里改

「能下单打板」是 v1+ 的事（核对引脚、布线、DRC、打样验证）。

## 思考：为什么不先做集成摇杆的「一体机」？

JoyStick Shield 是 Arduino UNO 尺寸，**物理上插不进 DevKitC**。载板 v0 选择的是：

- 板上留 **1×10 母座**，继续用现有 Shield + 杜邦线，但把 **DevKit 侧** 的 10 根线焊在板上
- 将来 v2 可以把摇杆电位器、按键直接画进 PCB，去掉 Shield

这样改动最小，也和当前固件 `input_gamepad.h` 注释里的接线表一致。
