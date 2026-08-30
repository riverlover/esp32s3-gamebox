# 学习复盘文档

本目录记录 **esp32s3-gamebox** 项目相关的学习过程、安装步骤、思考与踩坑。
面向「未来的自己」和可能的社媒分享——写法偏教程，不假设读者已经会 KiCad / ESP-IDF。

## 写作规范（agent 与用户共同遵守）

| 原则 | 说明 |
|------|------|
| 分目录 | 一个话题一个子目录，例如 `kicad-gamebox-shield/` |
| 分文件 | 按阶段拆 md，单篇建议 &lt; 300 行 |
| 可复现 | 命令、路径、版本号与仓库一致；能重跑的写脚本路径 |
| 有思考 | 不只写「做了什么」，还要写「为什么这么做 / 试过什么 / 哪里错了」 |
| 有图 | Mermaid 流程图 + 必要时 `assets/*.png` |

新话题请复制 `_template/` 再填内容。

## 系列索引

| 目录 | 主题 | 状态 |
|------|------|------|
| [esp-idf-toolchain/](esp-idf-toolchain/README.md) | 本机 macOS：ESP-IDF v5.4 + Python 3.12、编译烧录一键命令 | **已验证可烧** |
| [kicad-gamebox-shield/](kicad-gamebox-shield/README.md) | KiCad 10 + uv + mcp-server-kicad + Cursor MCP，生成 Gamebox 载板 PCB v0 | 进行中 |
| [joystick-shield/](joystick-shield/README.md) | JoyStick Shield 接线 + **E/F 故障改 A/D 映射**（已验证） | **已归档** |
| [nes-classic-wishlist/](nes-classic-wishlist/README.md) | NES 装机经典优先清单（约 20 款正作）+ 烧录步骤 | 待自备 ROM |

## 和仓库其他文档的关系

- **固件引脚、实测数据** → `docs/hardware.md`（权威来源）
- **内存与性能** → `docs/memory.md`
- **本目录** → 你怎么从零走到某一步的「游记」，可以引用上面两份，但不重复抄大段
