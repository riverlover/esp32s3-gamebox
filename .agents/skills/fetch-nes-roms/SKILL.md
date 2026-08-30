---
name: fetch-nes-roms
description: 从 https://www.fcnesyouxi.top/ 拉取 NES ROM 装进本仓库 roms/nes/ 的完整流程：全站目录匹配、S3 直下、iNES 头校验、替换/破例判定、打包验证、文档同步、把烧录命令交还给用户。用户提到「拉/下载 NES（FC）游戏」「从 fcnesyouxi.top 取 ROM」「往 roms/nes 加游戏」「某 ROM 黑屏要换来源」「按清单补 ROM」时都应该用这个 skill，即使用户没有说出站点名。
---

# 从 fcnesyouxi.top 拉 NES ROM 进 roms/nes/

输入是一份「目标文件名 + 站内搜索关键词 + 备注」的清单（默认见
`docs/journey/nes-classic-wishlist/README.md`，用户的临时表格也算）。
输出：文件落进 `roms/nes/`、文档同步、最终报告把 `./flash-roms.sh` 交给用户执行。

## 硬边界（先读，违反任何一条都是事故）

- **只下正作**：跳过名字带「无敌 / 变态 / 30命 / 金身 / 改版 / 金手指」的条目。
  汉化版可以（方便玩 RPG），外挂改版不行。例外只允许两种，且必须写进报告：
  清单备注自己给了替代（「或洛克人3」「可换成热血格斗」），或站内确实没有正作。
- **`roms/` 不入库**：商业 ROM 有版权，全程不要用 `git add` 碰它，也不要提交
  任何 `.bin`/`.elf`。本 skill 只改 `roms/nes/` 下的文件和两处文档。
- **烧录是用户的事**：agent 不跑 `flash-roms.sh`、不接串口。最终报告里给命令，
  让用户粘回串口输出，不要假设烧录成功。
- 下载前先 `ls roms/nes/` 看现有文件，**同名内容不同的不许静默覆盖**；
  同名同内容（MD5 一致）可以跳过。

## 站点事实（实测得出，别再踩）

- 所有下载链接指向同一个公开 S3 桶：`https://youxi.s3.bitiful.net/nes_roms_mutantcat/<文件名>.nes`。
  中文文件名必须百分号编码（Python `urllib.parse.quote`）。桶的 **LIST 接口会超时**，
  只能按已知文件名直接 GET 对象——所以必须先拿到准确文件名。
- 准确文件名的唯一可靠来源是站内页：分页 `https://www.fcnesyouxi.top/?page=1..33`
  （每页 50 条，第 34 页为空）。**优先用分页目录，不要用站内搜索**——
  robots.txt `Disallow: /*?q=*`，而且目录页顺带能发现同名重复文件。
- 站内数字前缀**不可信**：很多文件叫 `0000xxx.nes`，有的根本没有前缀
  （如 `赛尔达传说.nes`、`马戏团.nes`）。匹配靠关键词，不靠 ID。
- 建目录：抓全部 33 页，提取全部 `*.nes` 对象名去重，存一份
  （上次存在 `/tmp/all_roms.txt`，1543 个）。目录建一次就够了，
  短时间内的第二次拉取直接复用，除非用户说站内更新了。

## 工作流

### 1. 匹配

把清单每一行的关键词拿去目录里搜候选（grep 中文关键词即可），按硬边界筛掉
改版类。同一作品有多个候选时**全部下载**，进下一步用 MD5 判，不要凭名字猜。

### 2. 下载暂存

全部下到 `/tmp/nesstage/`（保持站内原文件名），**不直接进 `roms/nes/`**。

### 3. 校验 NES 头

对暂存目录跑：

```bash
python3 .agents/skills/fetch-nes-roms/scripts/check_nes_header.py /tmp/nesstage
```

脚本逐行打印 magic / PRG / CHR / mapper。预期 flutter：
- `map -1` = 头损坏（见下节），需要单独处置；
- `MAP?` = mapper 编号少见，nofrendo（fceumm 内核）大概率支持，但先查
  `components/nofrendo/` 的 mapper 支持列表再定；
- `tail-padded`（尾部一大段 0xFF）一般无害，是容量对齐。

iNES 1.0 头解析公式（别抄错 offset，这是上次翻车的点）：

```
PRG 16K 数 = b[4]        CHR 8K 数 = b[5]
mapper = (b[6] & 0x0F) | (((b[7] >> 4) & 0x0F) << 8)
```

已知 mapper 参考：0=NROM、1=MMC1、3=MMC3、5=MMC5（本仓库内核全部支持）。

### 4. 头损坏的处置（罕见，别直接扔文件）

mapper 解出 >255 的垃圾值时，按顺序判：

1. 看 payload：文件里 grep 英文标题串（如 `DRAGON QUEST III`）、PRG 大小是否
   合理（256~512 KB 常见）——payload 真就还有救。
2. 看 0x10 处的 word 是不是有效 CRC32：**不是** → 排除 iNES 2.0（iNES 2.0
   允许省 mapper 字节但 0x10 必须存 CRC32，那份 DQ3 文件就是这种情况）。
3. payload 真 + iNES2 排除 + 你能从游戏本身确定真 mapper（DQ3 = MMC5）时，
   重写入头：`b[6] = (b[6] & 0xF0) | low4bits; b[7] = b[7] & 0x0F`。
   修复过的文件必须在报告里标成**最高风险项**，让用户上板后优先测它。
4. 以上都不满足就弃用该文件，换同名其他候选或如实报告「站内这份坏了」。

### 5. MD5 判重

同名/疑似同作的多份文件算 MD5：一致任取一份；不一致就是不同改版，
挑最接近正作的（标题最短、无作弊字样），并在报告里说明。

### 6. 安装

把选定的暂存文件按清单的目标文件名（`01xx中文名.nes`）复制进
`roms/nes/`。若目标已存在，先 `cmp`/MD5 对比，相同跳过、不同停下来问用户
（或在报告里高亮待定）。

### 7. 本地打包验证（不烧录）

```bash
python3 tools/pack_roms.py roms/ /tmp/roms_test.bin
```

必须 23+ 个条目全部打印、无跳过行、总大小远小于 13 MB 分区（压后 2.5 MiB
量级属正常）。这一步同时验证显示名没超 39 字节（`NAME_LEN=40` 含结尾 NUL）。

### 8. 文档同步（AGENTS.md 要求，漏了等于没干完）

- `roms/nes/README-wishlist.txt`：已落地清单（目标 ← 来源 + 一行备注）。
- `docs/journey/nes-classic-wishlist/README.md`：
  「拉取结果」表（目标/来源/mapper/替换说明）、DQ3 类修复的单独小节、
  「空间粗算」换成实测值、「思考过程」补执行记录
  （爬目录方式、MD5 判重、头部问题等，别只写结论）。

### 9. 最终报告

三段：① 完成表（20+ 款、总大小、分区余量）；② 需要用户上板留意的项
（头修复的高风险项、改版破例项、移植/重打包非原版项）；③ 命令：

```
./flash-roms.sh
```

让用户跑完把串口输出贴回来再下结论。

## 触发后的自检清单

- [ ] 目录用分页建（没去敲 `?q=` 搜索、没去 LIST 桶）
- [ ] 改版类条目都过滤了
- [ ] 多候选的都 MD5 判过
- [ ] 每个落库文件跑过 `check_nes_header.py`
- [ ] 头修复的文件在报告里标了最高风险
- [ ] `pack_roms.py` 试打通过
- [ ] 两个文档都更新
- [ ] 没有 `git add`，没有跑 `flash-roms.sh`
