# 02 — 安装 ESP-IDF（本机已做过，新机器才重跑）

> 工具已经装在 `~/esp/esp-idf` 和 `~/.espressif/`。  
> **日常编译烧录不必重装**，直接看 [03-build-flash.md](03-build-flash.md)。  
> 下面留给：换电脑、venv 坏了、或误删目录时重装。

## 依赖（Homebrew）

```bash
brew install cmake ninja dfu-util ccache python@3.12 git
xcode-select -p   # 应有 Command Line Tools
```

**为什么指定 3.12**：本机默认 `python3` 曾是 **3.14**，`install.sh` 建 venv 直接失败（见 [04-pitfalls.md](04-pitfalls.md)）。ESP-IDF v5.4 用 3.12 验证通过。

## 克隆 IDF v5.4

```bash
export HTTP_PROXY=http://127.0.0.1:7890 HTTPS_PROXY=http://127.0.0.1:7890 ALL_PROXY=http://127.0.0.1:7890
export IDF_GITHUB_ASSETS="dl.espressif.com/github_assets"

mkdir -p ~/esp
cd ~/esp
# 若半成品目录存在先删掉再克隆
git clone -b v5.4 --recursive --depth 1 https://github.com/espressif/esp-idf.git esp-idf
```

`--recursive` 会拉一堆 submodule，可能要几分钟到十几分钟。结束应有 `~/esp/esp-idf/export.sh`。

## 安装工具链（仅 esp32s3）

```bash
export PATH="/opt/homebrew/opt/python@3.12/libexec/bin:/opt/homebrew/opt/python@3.12/bin:$PATH"
unset IDF_PYTHON_ENV_PATH
cd ~/esp/esp-idf
./install.sh esp32s3
```

成功标志：

```text
All done! You can now run:
  . ./export.sh
```

并出现目录：

```text
~/.espressif/python_env/idf5.4_py3.12_env/
~/.espressif/tools/xtensa-esp-elf/   # 等
```

若残留失败的 `idf5.4_py3.14_env`，先删掉再跑 `install.sh`：

```bash
rm -rf ~/.espressif/python_env/idf5.4_py3.14_env
```

## 激活（每个新终端一次）

见本系列 [README.md](README.md) 顶部「新会话先看这里」。核心是：

1. **先**把 3.12 和 IDF venv 放进 `PATH` 前面  
2. **再** `. ~/esp/esp-idf/export.sh`  
3. 用 `python -c "import click"` 自检
