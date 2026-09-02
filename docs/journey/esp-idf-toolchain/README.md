# ESP-IDF 工具链（本机 macOS）— 复盘索引

> 日期：2026-08-29  
> 目的：新会话 **不用再摸索**，直接按「一键命令」编译、烧录、监视。

本机已验证：**ESP-IDF v5.4 + Python 3.12 venv + DevKitC USB-Serial/JTAG**。
开机界面是 GAME / WORDS / SETTINGS（上游 sync 后；本机 START/SELECT 仍用大键 A/D）。

## 新会话先看这里（复制即用）

```bash
# 0) 代理（外网慢时，本机 FlClash）
export HTTP_PROXY=http://127.0.0.1:7890 HTTPS_PROXY=http://127.0.0.1:7890 ALL_PROXY=http://127.0.0.1:7890

# 1) 强制用 Homebrew Python 3.12，再激活 IDF（勿用系统默认 3.14）
export PATH="$HOME/.espressif/python_env/idf5.4_py3.12_env/bin:/opt/homebrew/opt/python@3.12/libexec/bin:/opt/homebrew/opt/python@3.12/bin:/opt/homebrew/bin:$PATH"
export IDF_PATH="$HOME/esp/esp-idf"
. "$IDF_PATH/export.sh"

# 2) 确认环境（应看到 3.12.x 且能 import click）
python --version          # 期望：Python 3.12.x
python -c "import click; print('click', click.__version__)"
which idf.py

# 3) 查端口（BOOT+RESET 后号码会变）
ls /dev/cu.usbmodem* /dev/cu.usbserial-* 2>/dev/null

# 4) 编译 / 烧录 / 监视
cd /Users/lizhenhe/esp32/esp32s3-gamebox
idf.py build
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)   # 或 usbserial-*
idf.py -p "$PORT" -b 115200 flash
# monitor 需要本机 TTY；在 Cursor 外置终端跑，或让用户自己跑：
# idf.py -p "$PORT" monitor           # 退出: Ctrl+]

# 5) 换游戏：拷到 TF 卡（不要再跑 flash-roms）
#    /roms/nes/  /roms/gb/  /roms/gbc/  /roms/snes/  /roms/md/
# 教材发音包变了才需要：
# idf.py flash-word-audio
```

**烧录前进下载模式**：按住 **BOOT** → 点一下 **RESET** → 松开 BOOT。  
**烧完进应用**：只按 **RESET**，不要按住 BOOT。

**换游戏（2026-09-02 起）**：拷到 TF 卡 `/roms/{nes,gb,…}/`，RESET 即可。  
旧的 `flash-roms` 分区方案已废弃（详见 [upstream-sync](../upstream-sync-2026-09-02/README.md)）。  
教材发音变了才跑 `idf.py flash-word-audio`。细节见 [03-编译烧录](03-build-flash.md)。

## 阅读顺序

1. [01-目标与背景](01-goal-and-background.md)
2. [02-安装 ESP-IDF](02-install.md)
3. [03-编译烧录日常流程](03-build-flash.md)
4. [04-踩坑与思考](04-pitfalls.md)

## 关键路径与版本（本机实测）

| 项 | 值 |
|----|-----|
| ESP-IDF | `~/esp/esp-idf`，tag **v5.4**（commit `67c1de1e` 一带） |
| Python venv | `~/.espressif/python_env/idf5.4_py3.12_env` |
| 解释器 | Homebrew `python@3.12`（**不要用 3.14**） |
| 芯片 / 板 | ESP32-S3-DevKitC-1 N16R8 |
| 烧录口（本次） | `/dev/cu.usbmodem*`（丝印 **USB**，USB-Serial/JTAG） |
| 文档里的 COM 口 | `/dev/cu.usbserial-*`（丝印 **COM**，FT232）；本机未插时没有 |
| 工程 | `/Users/lizhenhe/esp32/esp32s3-gamebox` |
| 游戏 ROM | **TF 卡** `/roms/nes/…`（不必入库） |
| 嵌入回退 ROM | `main/roms/smb.nes` 等（版权 ROM 不入库） |

## 图

- [assets/toolchain-flow.mmd](assets/toolchain-flow.mmd) — 激活 → 编译 → 烧录流程
