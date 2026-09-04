# 同步上游 fork（kiooss/esp32s3-gamebox）

> 日期：2026-09-02  
> 目的：本机 fork 落后上游 28 个提交（自己多 2 个：E/F 改脚、fetch-nes skill），在已有 TF 卡的前提下把上游合进来。

## 背景

| 项 | 值 |
|---|---|
| 本仓库 | `riverlover/esp32s3-gamebox`（GitHub fork） |
| 上游 | `kiooss/esp32s3-gamebox` |
| 本地 remote | `origin` + 新增 `upstream` |
| 分叉状态 | diverged：上游 +28 / 本机 +2 |

上游大改：TF 卡读 ROM、PC Engine、单词学习、SETTINGS、gruvbox UI、Genesis 内存修复等。  
本机独有：Shield E/F 故障后大键 A/D 做 START/SELECT + Controller beep 自检；`fetch-nes-roms` skill。

## 操作步骤

```bash
git remote add upstream https://github.com/kiooss/esp32s3-gamebox.git   # 若尚未添加
git fetch upstream
git merge upstream/main
# 解冲突 → git add → git commit（完成 merge）
idf.py build   # 本机已通过
```

## 冲突怎么判（思考过程）

一次只定一条原则，避免「看起来两边都对」时乱留：

1. **ROM 加载 / 菜单架构 / WORDS / PCE** → 收上游（用户已有 TF 卡，目标就是跟齐）。
2. **Shield START/SELECT 脚位** → 保留本机实测：E/F 停用，大键 A=START、D=SELECT。
3. **音频 API** → 两边都留：上游的 `shutdown` / `submit_stereo_wait` / `flush`（WORDS 需要）+ 本机的 `ready` / `beep`（诊断需要）。
4. **Controller Test UI** → 上游 gruvbox 配色 + TF 占用行；按键标签改成本机 A/D 映射，并保留 SND 行。
5. **`flash-roms` CMake target** → 上游已删；本机脚本改成「提示改走 TF 后 exit 1」，避免旧习惯踩坑。

冲突文件（7）：`AGENTS.md`、`README.md`、`docs/hardware.md`、`main/audio_output.{c,h}`、`main/input_gamepad.{c,h}`。

## 本机验证

- [x] `idf.py build` 通过（仅上游既有 unused 警告）
- [ ] 插 TF 卡烧录后：GAME 能列出卡上 ROM；SETTINGS → Controller Test 大键 A/D 与 beep
- [ ] （可选）`idf.py flash-word-audio` 后再进 WORDS

若选 GAME 后**直接进超级玛丽**、看不到平台菜单：那是 TF 目录为空时的回退，
不是「只扫到一部游戏」。见 [tf-card-empty-menu](../tf-card-empty-menu/README.md)。

## 下一步

1. 把现有游戏按 `/roms/{nes,gb,gbc,snes,md}/` 拷进 TF。
2. 烧 app：`idf.py -p PORT -b 115200 flash monitor`。
3. 若要用单词发音，再跑一次 `idf.py flash-word-audio`。
