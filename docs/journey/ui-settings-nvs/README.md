# SETTINGS 音量/背光写入 NVS

> 2026-09-02：SETTINGS 里调完的音量和背光，重启后要能记住。

## 背景

改之前音量、背光只活在 RAM 里。`audio_output_settings_init()` 每次开机把音量
重置成 50%，`display_init()` 把背光拉回 100%。注释里写过原因：怕用户静音或
调很低之后，按 RST 换游戏还停在静音。

实际用起来更希望「调到 20% 就一直是 20%」，和单词学习进度一样跨重启保留。

## 做法

| 项 | 键 | 默认 | 写入时机 | 读回时机 |
|---|---|---|---|---|
| 音量 | `ui_prefs` / `volume` | 50% | `audio_output_set_volume()` | `audio_output_settings_init()` |
| 背光 | `ui_prefs` / `backlight` | 100% | `display_backlight()` | `display_init()`（只 `apply`，不白写 NVS） |

- 用 `nvs_set_u8` / `nvs_get_u8`，值域音量 0~100、背光 5~100（跟 SETTINGS 下限一致）。
- `nvs_flash_init()` 失败时**不**整区 erase：NVS 是公共分区，擦了会误伤单词进度。
- Controller Test 里调音量也会走 `audio_output_set_volume()`，同样会落盘。

## 思考过程

曾考虑「只有 SETTINGS 退出时才保存」，避免诊断页临时把静音抬到 50% 写进 NVS。
后来觉得多余：诊断页本来就会改运行时音量且退出不恢复，和「留下最后一次档位」
一致；再拆一套 ephemeral API 不划算。

## 上板怎么验

1. SETTINGS → Volume 调到 20%，Brightness 调到 70%，B 回首页。
2. 按 RST 或拔电重上。
3. 串口应看到类似 `音量恢复：20%`、`背光恢复：70%`；SETTINGS 里档位也对应。
4. （可选）擦 NVS 后应回退 50% / 100%：`idf.py erase-otadata` 不够，要用
   `idf.py erase-flash` 或专门擦 nvs 分区——日常不必。

## 相关文件

- `main/audio_output.c` / `.h`
- `main/display.c` / `.h`
- `main/main.c`（启动注释）
- `AGENTS.md` / `README.md`（行为说明）
