# 07 — 下一步（小白 checklist）

v0 是「能打开的工程和文档」，离「能下单打板」还有几步。按顺序做，别跳。

## 阶段 A：在 KiCad 里把原理图弄对（最重要）

- [ ] 打开 `hardware/kicad/gamebox-shield/gamebox-shield.kicad_pro`
- [ ] 对照 [04-硬件引脚分析](04-hardware-pin-analysis.md) 和 `docs/hardware.md` §2
- [ ] 旋转 J1/J2，使符号 pin 1 对应 DevKit **下排最左边 3V3**
- [ ] 逐网络检查：GPIO1/2、GPIO4–18、屏、音频
- [ ] 运行 **ERC**，清零错误或确认「故意不连」的脚（RST、UART 等）

**验证技巧**：在原理图给要用到的脚加 `testpoint` 或导出网表，和杜邦线接线表逐行勾。

## 阶段 B：PCB 布局

- [ ] 画板框（建议先 90×60 mm 左右，按你手柄和屏的实物尺寸改）
- [ ] J1/J2 间距 = DevKit 两排针间距（约 28 mm 中心距，以实测为准）
- [ ] J3 屏座：线尽量短，SPI 远离喇叭走线
- [ ] J5 靠近 J6 喇叭座；**I2S 线短、GND 完整**

## 阶段 C：布线

- [ ] 先布 **GND**（铺铜或星形）
- [ ] 再布 **+3V3**
- [ ] SPI：SCK、MOSI、CS、DC、RST、BLK
- [ ] I2S：BCLK、LRC、DIN
- [ ] 摇杆 ADC：GPIO1/2 尽量对称、远离开关噪声（经验：短、粗一点无妨）

可用 Cursor MCP：

```text
对 gamebox-shield 跑 autoroute（或逐网络 route），然后 DRC
```

## 阶段 D：检查与导出

- [ ] **DRC** 无错误（或只剩你知道可忽略的）
- [ ] 3D 视图看一眼插座方向
- [ ] 导出 **Gerber + 钻孔文件**
- [ ] 嘉立创/JLC 下单前用其 CAM 工具预览

## 阶段 E：打样后硬件验证

和 `README.md` 摇杆排障一样：**一次只测一项**。

1. 只插 DevKit + 载板，上电看 3V3/GND 不短路
2. 只接屏，跑 `SHOW_DISPLAY_SELFTEST` 或进游戏看画面
3. 只接手柄，开机选 TEST 看摇杆诊断
4. 只接喇叭，听开机菜单音效

## 阶段 F：文档继续更新

每完成一阶段，在本目录追加或新建 md，例如：

- `08-erc-fix-log.md`
- `09-routing-and-drc.md`
- `10-jlc-order.md`

并在 [README.md](README.md) 索引里打勾。

## 社媒分享建议（可选）

若你要发图文/视频，本系列里可直接用的素材：

- 总览图：`assets/gamebox-shield-overview.png`
- 踩坑故事：[06-思考与踩坑](06-thinking-and-pitfalls.md) §6 自动连线
- 前后对比：洞洞板飞线 vs 载板渲染图（PCB 3D 截图待你补）

**标题示例**：

- 「ESP32 掌机从洞洞板到 KiCad 载板：我把 AI 接进了 KiCad」
- 「小白第一次用 Cursor + MCP 画 PCB，踩了 pin 编号这个坑」

---

回到系列索引：[README.md](README.md)
