# 04 — 踩坑与思考

按踩坑时间大致排序，方便对照日志。

## 1. 本机没有 ESP-IDF

`AGENTS.md` 写 `. ~/esp/esp-idf/export.sh`，但目录不存在。  
**结论**：新 Mac / 新克隆只 clone 了 gamebox 仓库 ≠ IDF 已装。先走 [02-install.md](02-install.md)。

## 2. Python 3.14 导致 install 失败

默认 `python3` 指向 Homebrew 3.14 时：

```text
subprocess.CalledProcessError: ... venv ... idf5.4_py3.14_env ... exit status 1
```

日志末尾可能看起来像「跑完了」，但 `exit_code: 1`，且没有可用的 `idf5.4_py3.12_env`。

**修法**：`PATH` 前置 `python@3.12`，删掉坏掉的 `py3.14` venv，重跑 `./install.sh esp32s3`。

## 3. `export.sh` 之后仍 `No module named 'click'`

原因：把 `/opt/homebrew/opt/python@3.12/libexec/bin` 放在 PATH 前面之后，`python` 指向「裸 3.12」，**不是** IDF venv；`idf.py` 用错解释器。

**修法**：让  
`$HOME/.espressif/python_env/idf5.4_py3.12_env/bin`  
排在 PATH **最前**，再 `. export.sh`，并用 `which python` / `import click` 确认。

## 4. 顶层没有 `roms/` → build 失败

```text
FAILED: roms.bin
在 .../roms 里没找到 .nes/.gb/... 文件
```

和「嵌入 ROM / ROM_CHOICE」是两条线：

- `main/roms` + `EMBED_FILES`：链接进 app，分区坏了才回退  
- 顶层 `roms/` → `pack_roms.py` → `roms.bin`：菜单用的分区镜像，**build 强制依赖**

空目录也会挂。至少放一个 `.nes`。

## 5. 串口路径会变；旧路径直接失败

烧录前是 `/dev/cu.usbmodem1234561`，BOOT+RESET 后变成 `…1101`，旧路径报：

```text
Could not open ... No such file or directory
```

每次先 `ls /dev/cu.usbmodem*`。

## 6. 连上但 `Invalid head of packet (0x61)`

常见：没进下载模式、口不对、或总线上有噪声。  
本机用 **按住 BOOT → RESET → 松开** + `-b 115200` 后成功。

## 7. 烧完一直 `waiting for download`

`boot:0x0 (DOWNLOAD…)` = GPIO0 采样为下载。  
烧录成功后若仍停在下载：多半 BOOT 还按着，或脚本拨了 RTS 又弄进去了。  
**只按 RESET**，不要按住 BOOT。

## 8. Agent 里跑 `idf.py monitor` 失败

```text
Monitor requires standard input to be attached to TTY
```

正常。让用户在系统终端开 monitor，或用 pyserial 短时抓 log。

## 思考摘要

| 判断 | 做法 |
|------|------|
| 工具链问题优先查「哪个 python」 | `which python` + `import click` |
| 硬件连接问题优先实验 | 换口 / BOOT+RESET / 只 RESET，一次改一个变量 |
| 文档里的 `usbserial-A5069RR4` 会随板子变 | 以 `ls /dev/cu.usb*` 为准 |
| ROM 分区与 app 分开烧 | 改代码只 `flash`；改游戏再 `flash-roms` |
