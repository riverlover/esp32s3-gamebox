# 03 — 编译 / 烧录日常流程

## 激活环境（每个新终端）

完整块见 [README.md](README.md)。最短自检：

```bash
export PATH="$HOME/.espressif/python_env/idf5.4_py3.12_env/bin:/opt/homebrew/opt/python@3.12/libexec/bin:/opt/homebrew/opt/python@3.12/bin:/opt/homebrew/bin:$PATH"
. ~/esp/esp-idf/export.sh
python --version    # 3.12.x
python -c "import click"
```

若 `idf.py` 报 `No module named 'click'`：当前 `python` 不是 IDF venv，把  
`$HOME/.espressif/python_env/idf5.4_py3.12_env/bin` **放到 PATH 最前**再 `export.sh` 一次。

## ROM 准备（否则 build 会挂）

顶层 `roms/` **不入库**。`idf.py build` 会跑 `tools/pack_roms.py`，目录空或不存在会直接：

```text
在 .../roms 里没找到 .nes/.gb/... 文件
ninja: build stopped
```

本机测试用（已从 `~/Downloads` 拷过）：

```text
roms/nes/Super Mario Bros. (World).nes
main/roms/smb.nes          # 嵌入回退；ROM_CHOICE=0
```

版权 ROM 勿提交 git（`.gitignore` 已挡）。公有领域测试 ROM 在 `main/roms/{SimpleParallaxDemo,full_palette,flowing_palette}.nes`。

没有版权游戏时：把 `main/nes_emu.c` 的 `ROM_CHOICE` 改成 `5`/`6`/`7`，`main/CMakeLists.txt` 的 `EMBED_FILES` 只留那三个测试 ROM，并至少往 `roms/nes/` 放一份能打包的 `.nes`（可复制测试 ROM）。

## 编译

```bash
cd /Users/lizhenhe/esp32/esp32s3-gamebox
idf.py set-target esp32s3   # 仅首次或清过 build 后
idf.py build
```

成功约有：

```text
Project build complete.
build/esp32s3-gamebox.bin   # ~1.6 MB 量级
build/roms.bin              # 随游戏多少变；仅 SMB 时约 31 KB
```

## 找串口

```bash
ls /dev/cu.usbmodem* /dev/cu.usbserial-* 2>/dev/null
```

| 口 | 板子丝印 | 说明 |
|----|----------|------|
| `cu.usbmodem*` | **USB** | USB-Serial/JTAG，本次烧录用的 |
| `cu.usbserial-*` | **COM** | 板载 FT232；`AGENTS.md` 里旧例子是这个 |

**BOOT+RESET 或拔插后，modem 后面的数字会变**（如 `1234561` → `1101`）。每次烧录前重新 `ls`。

## 进下载模式再烧

1. 按住 **BOOT**
2. 点一下 **RESET**
3. 松开 **BOOT**
4. 立刻烧：

```bash
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
idf.py -p "$PORT" -b 115200 flash
idf.py -p "$PORT" flash-roms    # 改过 roms/ 或首次才需要；与 flash 分开是故意的
```

`flash` 只烧 bootloader + 分区表 + app。  
`flash-roms` 烧 `0x210000` 的 ROM 分区（见 `partitions.csv` / 顶层 `CMakeLists.txt` 的 `ROMS_OFFSET`）。

## 烧完进应用

- **只按 RESET**，不要按住 BOOT  
- 若串口一直刷 `waiting for download` / `boot:0x0 (DOWNLOAD…)`，说明仍在下载模式：松开 BOOT 再 RESET

屏上应出现 **GAME / TEST**。选 GAME 进菜单（应有 Super Mario Bros.）。

## 串口监视

Cursor agent 环境里 `idf.py monitor` 常报 **需要 TTY**，请在本机终端跑：

```bash
idf.py -p "$PORT" monitor    # 退出: Ctrl+]
```

无 TTY 时可用短时抓 log（不推荐日常用）：

```bash
python - <<'PY'
import serial, time, sys, glob
port = sorted(glob.glob('/dev/cu.usbmodem*'))[0]
ser = serial.Serial(port, 115200, timeout=0.2)
# 不要乱拨 RTS，否则可能又进下载模式
t0 = time.time()
while time.time() - t0 < 15:
    d = ser.read(4096)
    if d: sys.stdout.write(d.decode('utf-8', 'replace')); sys.stdout.flush()
ser.close()
PY
```

## Agent 注意

- 烧录/监视要板子在线；失败时把串口原文贴回，别假设成功  
- 改完代码把命令交给用户，或确认端口后再 `flash`
