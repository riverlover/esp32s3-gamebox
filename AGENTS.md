# AGENTS.md

本仓库给编码 agent 的说明。`CLAUDE.md` 只是 `@AGENTS.md` 一行引用，
所以**改这个文件就够了**，别把内容抄成两份。

ESP32-S3-DevKitC-1（N16R8）+ ST7789 SPI 屏上跑多平台模拟器。
ESP-IDF v5.4，纯 C，无测试套件 —— 验证靠烧板子 + 看串口输出。

代码注释和提交信息都用中文，重点写「为什么这么做」而不是「做了什么」。
延续这个风格：这个仓库的注释里存了大量硬件实测数据和踩坑结论，是主要的知识载体。

## 编译 / 烧录 / 验证

本机 macOS 完整步骤（Python 3.12、端口、BOOT/RESET、踩坑）见  
`docs/journey/esp-idf-toolchain/`；新会话优先复制该目录 README 顶部命令块。

```bash
# PATH 先放 IDF 的 3.12 venv（本机默认 python3 可能是 3.14，会坏 click / install）
export PATH="$HOME/.espressif/python_env/idf5.4_py3.12_env/bin:/opt/homebrew/opt/python@3.12/bin:$PATH"
. ~/esp/esp-idf/export.sh                                  # 每个新终端都要跑一次
idf.py build
# 端口每次 ls：丝印 USB → cu.usbmodem*；丝印 COM → cu.usbserial-*（号码会变）
idf.py -p /dev/cu.usbmodemXXXX -b 115200 flash monitor     # 退出 monitor: Ctrl+]
./flash-roms.sh                                            # 只在加/删顶层 roms/ 后跑（封装环境+端口）
# 等价：idf.py -p /dev/cu.usbmodemXXXX flash-roms
```

- DevKitC 两个 Type-C：本机实测用 **USB** 口（`cu.usbmodem*`，USB-Serial/JTAG）可烧；
  **COM** 口是板载 FT232R（`cu.usbserial-*`）。烧前按住 BOOT→点 RESET→松开；烧完只按 RESET。
- 仓库根 `flash-roms.sh`：激活 3.12 venv + `export.sh`、自动选端口、跑 `idf.py flash-roms`。
- `flash-roms` 是顶层 `CMakeLists.txt` 注册的自定义 target，**故意不挂在 `idf.py flash` 上**：
  ROM 分区 13 MB，Deflate 镜像目前几 MiB（随游戏增删浮动），烧一次仍较久，而它几乎从不变。
  烧录时间只跟镜像实际字节数走（`esptool write_flash` 写的是文件），跟分区开多大无关。
- 顶层 `roms/` 为空时 `pack_roms.py` 会让 **整个 build 失败**——至少放一个 `.nes`。
- `sdkconfig` 不入库，由 `sdkconfig.defaults` 生成。要固化配置就改 `.defaults`，
  别改 `sdkconfig`（会被覆盖）。里面每一条都有理由（240 MHz CPU、1000 Hz tick、
  OCT PSRAM、`SPIRAM_MALLOC_RESERVE_INTERNAL=8192`）——改动前先读那些注释。

⚠️ **烧录和串口监视需要接着板子**，agent 通常做不到。改完代码把命令交给用户跑，
让他把串口输出贴回来，别假设烧录成功。

### 新克隆的副本会链接失败

`main/CMakeLists.txt` 的 `EMBED_FILES` 引用了 5 个版权 ROM（`main/roms/{smb,tetris,contra,pacman,drmario}.nes`），
它们在 `.gitignore` 里。自备文件放进去，或者把 `main/nes_emu.c` 顶部的 `ROM_CHOICE`
改成 5/6/7（随仓库分发的公有领域测试 ROM）并从 `EMBED_FILES` 删掉缺失的行。
顶层 `roms/`（给 `flash-roms` 用的本地游戏）同样不入库。

### 板上验证的诊断开关

改完相关代码优先打开对应开关，比盯代码快：

| 开关 | 位置 | 作用 |
|---|---|---|
| `DISP_PROFILE`（默认 1） | `main/display.c` | 每秒打一行核 1 推屏耗时。调 `BAND_LINES` / 画布尺寸时看这个 |
| `DIAG_TIMING` | `main/nes_emu.c` | 开机跑一遍分阶段计时（只 CPU / +PPU / 完整），定位核 0 瓶颈 |
| `SHOW_DISPLAY_SELFTEST` | `main/main.c` | 点屏诊断图，验旋转/颜色顺序/反色 |
| `OVERCLOCK_LEVEL`（默认 0，关闭） | `main/main.c` / `main/overclock.c` | 开机下发 BBPLL 微调寄存器把 CPU 推过 Kconfig 240MHz 上限，档位范围 `[-8, 8]`。没有标定 MHz——效果因片而异，实测主频打在串口 `overclock` tag 下，配合下面的 "CPU 余量" 自报行判断效果、单变量调档。本机实测：4 档能跑，6 档触发看门狗复位（`TG1WDT_SYS_RST`）不稳定，5 档没测过 |

开机画面（`main.c`）现在会停下来问 GAME/TEST：选 TEST 进摇杆 + 按键 +
MAX98357 提示音诊断（`input_gamepad_show()`）——`START(E)` 播 beep，`Y(D)` 调音量，
屏上 `SND OK` / `SND OFF` 可区分接线问题和菜单静音到 0%；同时显示两路原始 ADC。

运行时核 0 每秒自报 `NES 60 fps (模拟+转换 8.1 ms/帧，CPU 余量 52%)`。
两个核并行，帧时间是 `max(核 0, 核 1)`，所以两个数要分开看。

## 架构

启动链：`app_main`（main.c）→ `nes_emu_prealloc` → `display_init` → `rom_menu_pick` → 对应模拟器（不返回）。

### 双核分工与「条带流式推屏」

这是整个项目最要紧的一件事，改显示或性能相关代码前必须先理解：

- **没有常驻帧缓冲**。默认画布 288×224 的 RGB565 是 126 KB，双缓冲 252 KB，内部 DMA 内存装不下。
- `display.c` 只留两块最大 320×32 的条带缓冲（各 20 KB），核 1 上的 `blit_task` 一边把第 N+1 条
  转换进一块、一边 DMA 推第 N 条。整块画布由调用方的 `disp_strip_fn` 回调**按条带现算**，不落地。
- **双缓冲下移到了 8 位的 NES `vidbuf` 那一层**（每块 64 KB，比 RGB565 便宜一半）：
  核 0 渲染一块的同时核 1 读另一块。`blit_frame`（nes_emu.c）提交完立刻 `nes_setvidbuf` 换块。
- 默认接口是 `display_stream(fn, ctx)` / `display_stream_sync(...)`；Genesis 用
  `display_stream_sized(..., 320, 224)` 提交原生宽度画布。
  `display_stream` 只在上一帧未推完时阻塞，天然节流。

由此产生几条容易踩的约束：

- ⚠️ **绘图原语（`display_clear` / `display_text` / `display_fill_rect` …）只能在
  `disp_strip_fn` 回调内部调用** —— 它们画的是「当前条带」，坐标仍相对整块画布，
  条带外的部分自动丢弃。同一份默认画布绘制列表每帧被调用 7 次。
  所以「清屏」这种一次性操作也得包成回调（见 `nes_emu.c` 的 `black_strip`）。
- ⚠️ **条带缓冲不能放 PSRAM**：`spi_master` 用 `esp_ptr_dma_capable()` 判断，PSRAM 指针
  一律 false，驱动会每条带临时 malloc + memcpy，白绕一圈。同理 `vidbuf` 必须在内部 SRAM
  （放 PSRAM 时 PPU 渲染从 2 ms 涨到 8.5 ms）。
- ⚠️ **`nes_emu_prealloc()` 必须在 `display_init()` 之前调用**，否则凑不出两块 64 KB 连续内部内存。
- ⚠️ **帧缓冲里存的是字节交换后（大端）的 RGB565**。用 `display.h` 的 `RGB565()` 宏，
  直接往缓冲写像素的代码（调色板表）同样要存大端值。
- 条带越少越快：每条带约 122 µs 固定开销，串行叠加在总线时间上（实测数据见
  `display.c` 文件头和 `docs/hardware.md` §7）。

### 菜单画布 288×224，四款模拟器铺满 320×240 面板

`DISP_FB_W/H` 是画布，`DISP_W/H` 是面板，画布居中，四周黑边由 `display_init()`
开机清一次、之后再不碰。288 = 256×9/8，是为了修 NES 的 8:7 像素宽高比——这段历史
推导仍然成立，但现在只有 `rom_menu.c` 和诊断画面在用这块 288×224 画布。NES、SNES、
GB/GBC（共用 `gbc_emu.c`）、Genesis 都已经改走 `display_stream_sized()` 铺满整块
320×240 面板：早先「铺满 320 宽会把 SPI 带宽拖到 30 fps、且 NES 跳帧会让精灵永久
消失」的测算结论，上板实测已经证伪——铺满后都能稳定 60 fps，没有精灵丢失。
完整推导见 `display.h`。

### ROM 来源：分区 mmap，编译期嵌入做回退

- `roms/**/*.{nes,gb,gbc,sfc,smc,md,bin,zip}` → `tools/pack_roms.py` 打成自定义镜像
  （`.zip` 里恰好有一个可识别 ROM 时自动取出；0 个或多个都只打一行提示跳过。
  不认识的扩展名和 `.7z`/`.rar` 同样会打提示——以前是静默消失，最难查。
  GB/GBC 还会查 mapper：gnuboy 没实现 MBC6/MBC7/MMM01，那类卡带烧进去是黑屏，
  打包时就警告，见 `components/gnuboy/README.gamebox.md`）
  （magic + 定长目录表 + 每个 ROM 独立 raw Deflate）
  → 烧进 `partitions.csv` 里 offset `0x210000` 的 13 MB `roms` 分区（子类型 0x40）。
  分区容量由 `pack_roms.py` 的 `roms_partition_size()` 现读 `partitions.csv`，
  改分区大小不用再同步脚本（以前写死 8 MB，扩容时忘了改会在打包这步炸）。
- `roms/` 按平台分子目录（`nes/` `gb/` `gbc/` `snes/` `md/`），**纯粹是给人看的**：
  平台由 ROM 头判定，不看目录名，放错目录也不影响结果。菜单顺序也和目录无关
  （排序键取文件名，不含路径）。想临时下架一个游戏就挪进 `removed-YYYYMMDD/`，
  `pack_roms.py` 会整棵跳过前缀为 `removed` / `_` / `.` 的目录。
- `rom_store.c` 一次 `esp_partition_mmap` 整个分区，菜单只读目录；确认后只把选中的
  Deflate ROM 解到 PSRAM。SNES 必须直接解到最终可写缓冲，不能先解一份再复制 4 MiB。
  **故意不用 SPIFFS/ZIP 文件系统**：自定义目录已经提供随机访问，套第二层目录只会重复。
- `rom_store_init()` 的校验写得很啰嗦是因为 offset/size 直接当指针用，而数据来自 flash
  （没烧过时全是 0xFF）。所有失败都只返回 0、不 abort，调用方回退到 `ROM_CHOICE` 选的嵌入 ROM。

### 输入

`input_serial_poll() | input_gamepad_poll()`，两路按位或并存，位掩码是 nofrendo 的
`NES_PAD_*`（`components/nofrendo/nes/input.h`）。串口键盘是手柄不灵时的调试后路。

- 摇杆报的是**状态**不是事件，所以菜单必须做边沿检测（`rom_menu.c`）。
- 串口那路只有「按下」没有「松开」，靠 `HOLD_MS=250` 超时松开 + 终端按键重复维持长按。
- `input_gamepad_init()` 开机采 16 次静止读数做中位校准 —— **上电时手别碰摇杆**，
  碰着会被判定「没接好」而整路禁用。

### nofrendo

`components/nofrendo/` 取自 [retro-go](https://github.com/ducalex/retro-go)，**源码未做修改**，
上游更新可以直接覆盖。**不要改这个目录下的文件** —— 有需求就加进 `rg_system.h` 那层垫片，
或者在 `main/` 里适配。唯一的粘合层是 `rg_system.h`（只提供日志 / CRC32 / `IRAM_ATTR` 三样）。
`RETRO_GO` 宏必须是 `PUBLIC`，否则 main 和组件对 `IRAM_ATTR` 的取值会打架。
`-O3` 是性能关键。

⚠️ **本仓库只分发源码，不分发构建产物（`.bin`），这是有意的许可决定。**
`main/` 是 GPL v2（根 `LICENSE`），四个核心各自保持上游许可。链接出来的固件
二进制有两处不可回避的不兼容：Snes9x 的「仅限非商业」附加限制与 GPL 冲突
（注意方向 —— Snes9x 本身允许非商业分发，是 GPL 不许附加限制）；nofrendo 是
GPLv2-**only**（源文件无 or later）而 gwenesis 是 GPL v3+/AGPL v3，两者不能合并。
所以**不要往仓库里提交 `.bin`/`.elf`，也不要加发布二进制的 CI**。
完整推导和三处上游元数据矛盾见 `README.md` 的「授权」一节。

## 改动时要同步的跨文件不变量

| 改这个 | 必须同步 |
|---|---|
| `DISP_FB_W` / `DISP_FB_H`（display.h） | `nes_emu.c` 的三个 `_Static_assert`；`display.c` 的 `BAND_LINES`（最好整除 `DISP_FB_H`） |
| `DISP_H` | `DISP_GAP_Y` 一起改。只改一个的症状是画面偏移 + 有一条边永远刷不到 |
| `partitions.csv` 的 roms offset | 顶层 `CMakeLists.txt` 的 `ROMS_OFFSET` |
| `pack_roms.py` 的 `NAME_LEN` | `rom_store.h` 的 `ROM_STORE_NAME_LEN` |
| 加 `main/roms/*.nes` | `main/CMakeLists.txt` 的 `EMBED_FILES` + `nes_emu.c` 的 `ROM_CHOICE` 分支和 `_binary_<名字>_nes_start` 符号名（非字母数字→下划线） |
| 菜单要显示新汉字 | 不用管了：`main/menu_font.c` 收了 GB2312 全部 6763 字。真撞上 GB2312 之外的字（繁体、假名）才需要往 `tools/gen_menu_font.py` 的 `EXTRA_TEXT` 加一个字并重跑（要 unifont 源文件，不入库） |
| `main/` 新增 .c 或用新驱动 | `main/CMakeLists.txt` 的 `SRCS` 和 `REQUIRES`（显式写了 REQUIRES 就不再自动依赖全部组件） |

## 硬件排障的教训

`README.md` 里记着一次摇杆故障的完整排查（洞洞板上两轴之间几百欧的电阻性短路，
万用表通断档不响）。结论值得每次遇到硬件疑点时重读一遍：

**一次只改一个变量的对照实验（拔掉一根线看行为变不变），比任何隔着几层的推理都可靠。
硬件问题先做实验，再动代码。** 那次走的三条弯路全是同一类错误 —— 用间接推理代替直接实验，
其中两次还得出了和事实相反的结论。

选脚时永久避开：GPIO33~37（Octal PSRAM）、19/20（原生 USB）、0/3/45/46（strapping）。
屏的 SCK/MOSI/CS 必须是 SPI2 的 IOMUX 原生脚（12/11/10），换脚会降到 40 MHz。
摇杆两轴必须在 ADC1 范围（GPIO1~10），当前用 GPIO1/2；Shield E/F 小键
用 GPIO21/47 做 START/SELECT（7/8 本机实测异常后改脚）。

## 学习复盘文档（用户要求，长期有效）

用户是硬件/EDA 新手，**后续所有话题的步骤、踩坑、思考过程都要落成 Markdown**，
供复盘、学习和社媒分享。agent 在本仓库干活时默认遵守：

### 写什么

- **分目录、分文件**：一个话题一个子目录，见 `docs/journey/`。单文件别过长，按阶段拆
  （目标 → 安装 → 配置 → 实操 → 踩坑 → 下一步）。
- **写给小白能看懂**：术语第一次出现要解释；命令可复制粘贴；写清「为什么」而不只列步骤。
- **思考过程也要记**：agent 当时怎么判断的、试了哪条路、为什么放弃，写在对应章节的
  「思考过程」或 `*-thinking.md` 里——这和 `docs/hardware.md` 里记实测踩坑是同一传统。
- **图尽量有**：原理/流程用 Mermaid（放 `assets/*.mmd` 或内嵌代码块）；适合分享的
  总览图可生成 PNG 放进同目录 `assets/` 并在 md 里引用。
- **和代码/工程对齐**：路径、版本号、引脚表要和仓库里真实文件一致；有脚本就写怎么重跑。

### 什么时候写

- **话题告一段落**（安装完成、工程生成、某步验证通过/失败）就更新或新建 md，别等用户催。
- **AGENTS.md / 固件引脚 / 硬件结论变了**，复盘文档里相关章节同步改，或加「勘误」小节。
- 新话题从 `docs/journey/_template/` 复制结构再填内容。

### 索引

- `docs/journey/README.md` —— 全部复盘系列的目录与写作规范
- 当前已有：`esp-idf-toolchain/`（本机编译烧录）、`kicad-gamebox-shield/`、`joystick-shield/`

## 更多文档

- `README.md` —— 路线图、接线表、操作说明、调色板选择、排障记录
- `docs/hardware.md` —— 板卡细节、引脚、ST7789 三个坑、逐次优化的实测数据（§7）
- `docs/memory.md` —— Flash、内部 SRAM、PSRAM 的统一定义和各模拟器分配账
- `docs/journey/` —— 分话题学习复盘（安装、EDA、PCB 等），见上一节
- 已知问题：屏没引出 TE 信号，推屏和面板扫描无法同步，画面剧变时有轻微撕裂
- 已知问题：只跑 SNES 时背光有轻微闪烁，且和画面忙不忙无关（一直都有）。换了
  供电来源后消失，判断是 SNES 单核跑 CPU 模拟+PPU+音频、持续功耗明显高于
  NES/GBC/Genesis，供电余量不够时电压轨被拉低连带背光一起暗。不是代码 bug，
  用电流更足的供电（真的 5V/1A+ 墙充或独立供电口，别用电脑弱口或劣质线）
