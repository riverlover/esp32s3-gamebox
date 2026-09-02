#!/usr/bin/env python3
"""从 GNU Unifont .hex(.gz) 生成菜单用的 8x16 ASCII + 16x16 中文字形表。

字形来自 GNU Unifont 17.0.04；Unifont 采用 SIL OFL 1.1，或 GPL v2+
（带字体嵌入例外）。

用法：
    python3 tools/gen_menu_font.py unifont_all-17.0.04.hex.gz main/menu_font.c

源文件不入库（解开 8 MB），需要时从
https://ftp.gnu.org/gnu/unifont/unifont-17.0.04/unifont_all-17.0.04.hex.gz 取。
只在换字符集时才需要重跑本脚本——生成的 menu_font.c 是入库的。
"""

import gzip
from pathlib import Path
import sys


# 以前这里是一张手写的「菜单实际出现的汉字」表，只固化用到的那 106 个。
# 问题是游戏名由 roms/ 里的文件名决定：每加一个中文名游戏就要回来补字，
# 忘了也不报错，只是菜单上显示成 `?`——隐性负担，迟早踩。
#
# 改成一次收全 GB2312 的 6763 个汉字（一级 3755 + 二级 3008）。代价是固件
# 多约 225 KB（每字形 34 字节：uint16 码点 + 32 字节点阵），app 分区当时
# 还剩 548 KiB，装得下。这个项目里 flash 是最不紧张的资源，拿它换掉一类
# 「会忘、忘了还不报错」的维护负担很划算。
#
# 2026-08-28 又把符号区（1~9 区）补上，理由和上面完全一样。原来只收汉字，
# 结果游戏名里的 ★☆ 全被 display_text_16() 当成未知码点，回落到 ASCII 分支
# 后按 `?` 画出来 —— 而且不止这两个：○ ※ 、 「」 和全角字母、假名，凡是
# GB2312 里的非汉字符号一个都没有。补全只多 682 个字形（约 23 KB）。
def gb2312_codepoints():
    """GB2312 全部 7445 个字符：符号区 1~9 区 + 汉字 16~87 区。

    按区位 0xA1A1~0xF7FE 遍历解码得到，比维护一张字表可靠——字表抄错了
    不会有人发现。10~15 区在 GB2312 里是空的，解码失败自然跳过。"""
    out = set()
    for high in range(0xA1, 0xF8):
        for low in range(0xA1, 0xFF):
            try:
                out.add(ord(bytes([high, low]).decode("gb2312")))
            except UnicodeDecodeError:
                pass                    # 区位表里的空洞
    return out


# 游戏名偶尔会撞上 GB2312 之外的字（繁体、日文假名）。往这里加单个字即可，
# 比整段换成 GBK（21886 字、约 740 KB）划算得多。
EXTRA_TEXT = ""


def widen(narrow):
    """把 8x16 字形居中填进 16x16 的格子，返回 32 字节。

    符号区里有 295 个字符（○ ● ■ ※ ← → “ ” … 罗马数字、希腊、西里尔、
    制表符）在 Unifont 里画的是 8x16 半角字形，但它们在 GB2312 里本来就占
    一个双字节码位、是**全角**字符 —— Unifont 只是把字形画窄了。所以居中
    补进全角格子才是对的，渲染出来的间距符合中文排版预期。

    唯一的代价是制表符（─ │ ┌ ┐ …）左右会各留 4px，拼不成连续的线。这个
    项目不画表格，游戏名里也基本不会出现，不值得为它把字库拆成宽窄两套、
    再给 display_text_16() 加一条 9px 步进的分支。"""
    out = bytearray()
    for byte in narrow:
        row = byte << 4                 # 左右各留 4px
        out.append(row >> 8)
        out.append(row & 0xFF)
    return bytes(out)


def read_hex(path):
    wanted_wide = gb2312_codepoints() | {
        ord(ch) for ch in EXTRA_TEXT if ord(ch) > 0x7F
    }
    wanted_ascii = set(range(0x20, 0x7F))
    found_wide = {}
    found_ascii = {}
    opener = gzip.open if str(path).endswith(".gz") else open
    with opener(path, "rt", encoding="ascii") as fh:
        for line in fh:
            code_hex, sep, bitmap_hex = line.strip().partition(":")
            if not sep:
                continue
            codepoint = int(code_hex, 16)
            if codepoint not in wanted_wide and codepoint not in wanted_ascii:
                continue
            bitmap = bytes.fromhex(bitmap_hex)
            if codepoint in wanted_ascii:
                if len(bitmap) != 16:
                    raise SystemExit(
                        "U+%04X ASCII 字形应为 16 字节，实际 %d 字节" %
                        (codepoint, len(bitmap)))
                found_ascii[codepoint] = bitmap
            else:
                if len(bitmap) == 16:
                    bitmap = widen(bitmap)
                elif len(bitmap) != 32:
                    raise SystemExit(
                        "U+%04X 全角字形应为 32 或 16 字节，实际 %d 字节" %
                        (codepoint, len(bitmap)))
                found_wide[codepoint] = bitmap

    missing = (wanted_wide - found_wide.keys()) | (wanted_ascii - found_ascii.keys())
    if missing:
        raise SystemExit("缺少字形: " + " ".join("U+%04X" % cp
                                                for cp in sorted(missing)))
    return found_ascii, found_wide


def render(ascii_glyphs, wide_glyphs):
    lines = [
        "/* 此文件由 tools/gen_menu_font.py 生成，不要手改。",
        " * 字形来自 GNU Unifont 17.0.04：SIL OFL 1.1，或 GPL v2+ 字体嵌入例外。 */",
        "#include <stddef.h>",
        '#include "menu_font.h"',
        "",
        "typedef struct {",
        "    uint16_t codepoint;",
        "    uint8_t bitmap[32];     /* 16 行，每行高位在左 */",
        "} menu_glyph_t;",
        "",
        "static const uint8_t ASCII_GLYPHS[95][16] = {",
    ]
    for codepoint, bitmap in sorted(ascii_glyphs.items()):
        values = ", ".join("0x%02X" % b for b in bitmap)
        lines.append("    { %s }, /* %s */" % (values, chr(codepoint)))
    lines += [
        "};",
        "",
        "static const menu_glyph_t GLYPHS[] = {",
    ]
    for codepoint, bitmap in sorted(wide_glyphs.items()):
        values = ", ".join("0x%02X" % b for b in bitmap)
        lines.append("    { 0x%04X, { %s } }, /* %s */" %
                     (codepoint, values, chr(codepoint)))
    lines += [
        "};",
        "",
        "const uint8_t *menu_font_ascii_glyph(uint32_t codepoint)",
        "{",
        "    if (codepoint < 0x20 || codepoint > 0x7E) codepoint = '?';",
        "    return ASCII_GLYPHS[codepoint - 0x20];",
        "}",
        "",
        "const uint8_t *menu_font_glyph(uint32_t codepoint)",
        "{",
        "    int lo = 0;",
        "    int hi = (int)(sizeof(GLYPHS) / sizeof(GLYPHS[0])) - 1;",
        "    while (lo <= hi) {",
        "        int mid = lo + (hi - lo) / 2;",
        "        uint32_t value = GLYPHS[mid].codepoint;",
        "        if (value == codepoint) return GLYPHS[mid].bitmap;",
        "        if (value < codepoint) lo = mid + 1;",
        "        else hi = mid - 1;",
        "    }",
        "    return NULL;",
        "}",
        "",
    ]
    return "\n".join(lines)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    ascii_glyphs, wide_glyphs = read_hex(Path(sys.argv[1]))
    output = Path(sys.argv[2])
    output.write_text(render(ascii_glyphs, wide_glyphs), encoding="utf-8")
    print("生成 %d 个 ASCII + %d 个中文点阵字形 -> %s "
          "(%.0f KB 源码，固件里约 %.0f KB)"
          % (len(ascii_glyphs), len(wide_glyphs), output,
             output.stat().st_size / 1024,
             (len(ascii_glyphs) * 16 + len(wide_glyphs) * 34) / 1024))


if __name__ == "__main__":
    main()
