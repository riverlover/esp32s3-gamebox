#!/usr/bin/env python3
"""iNES 1.0 头校验器（fetch-nes-roms skill 配套）。

用法：
    python3 check_nes_header.py <文件.nes | 目录> [...]

每行输出：大小 / PRG(16K) / CHR(8K) / mapper / 异常标记。
- `map -1`：mapper 字节解出 >255 的垃圾值，头损坏，按 SKILL.md 第 4 节处置，
  不要直接扔文件（payload 可能是真的）。
- `MAP?`：mapper 少见，不一定坏，查 nofrendo 支持列表再定。
- `tail-padded`：尾部一段 0xFF，通常是容量对齐，一般无害。

解析公式是 iNES 1.0（16 字节头），别拿 iNES 2.0 的 offset 来套，
那就是 255/3843 一类假损坏的来源：
    PRG = b[4]      CHR = b[5]
    mapper = (b[6] & 0x0F) | (((b[7] >> 4) & 0x0F) << 8)
"""

import os
import sys

KNOWN = {
    0: "NROM", 1: "MMC1", 2: "UxCommon", 3: "MMC3", 4: "MMC4", 5: "MMC5",
    6: "FFE-C1", 7: "NAMCO106", 8: "Konami VCC", 9: "Konami A5V",
    10: "CKNamco", 11: "Yoko", 14: "Sunsoft", 15: "Yoko2", 16: "Taito TC0190",
    17: "Taito X1", 18: "Yoko3", 19: "YMZ", 21: "MK3", 22: "Ketai",
    23: "100-in-1(noG)", 24: "Sunsoft(#2)", 25: "NESTO", 26: "MagneForce",
    27: "Sachen", 28: "HP2N", 29: "Sunsoft-SPC711", 30: "Sunsoft-SPC700(#1)",
    33: "VS", 34: "Sunsoft-SPC700(#2)", 41: "MtAris", 45: "KOEI", 46: "LTD",
    48: "PCF", 53: "Contra", 64: "TVC", 65: "PANEL-FP", 66: "100-in-1",
    67: "AE-11", 68: "PCS-E1102", 69: "Taito TC0350", 70: "Taito Kiwei",
    71: "Taito X7", 72: "Irem", 73: "Konami SCC", 74: "CAuld", 75: "CAULD-2",
    81: "VRC2", 82: "VRC4", 83: "VRC6", 84: "VRC7", 85: "WH1", 86: "WH5J",
    87: "XE colher", 92: "GCON-7220", 98: "PAL-CBX5", 99: "CAulas",
    100: "#SNVS", 105: "KONAMI-057", 106: "KONAMI-058", 107: "KONAMI-104",
    109: "KONAMI-108", 110: "SAI", 111: "Taito X?, X?", 112: "KONAMI-131",
    116: "Ebara", 117: "PK003", 118: "AWA", 119: "XE комментарии",
    120: "Konami BTC-002", 126: "BL", 130: "LZ", 133: "KONAMI-132",
    135: "Sunsoft-5A59", 140: "Konami-MG-B2", 141: "Tesla",
    142: "Sunsoft-5001(#D)", 145: "Path", 150: "KONAMI-184", 168: "SHA-125",
    188: "NX", 189: "RW", 191: "TB-X", 194: "TC-0150", 200: "SNIC",
    206: "MPS-1004", 207: "VIC24", 208: "Vanguard-C", 209: "PS-W",
    210: "ZN-231", 211: "PS-E1008", 212: "ADAM-1", 213: "LZ-12F-E",
    214: "EPT-H101", 216: "Konami TSCC", 218: "Namco-132", 221: "Fx-1",
    224: "QX", 225: "MG-1", 227: "PAL #421", 228: "Circle",
    231: "E chờ", 232: "SE W19", 235: "EVM-Gen", 236: "Slar-X",
    238: "Wk swap", 240: "EALA512K",
}


def check(path):
    with open(path, "rb") as f:
        b = f.read()
    size = len(b)
    if b[:4] != b"NES\x1a":
        print("%-40s %8d B  NOT Nes magic" % (os.path.basename(path), size))
        return
    prg, chr_ = b[4], b[5]
    mapper = (b[6] & 0x0F) | (((b[7] >> 4) & 0x0F) << 8)

    flags = []
    if mapper > 255:
        mapstr = "map -1 [HEADER CORRUPT]"
    elif mapper in KNOWN:
        mapstr = "map %2d %s" % (mapper, KNOWN[mapper])
    else:
        mapstr = "map %3d MAP?" % mapper
        flags.append("unknown mapper")

    if size > 4096 and b[-4096:] == b"\xff" * 4096:
        flags.append("tail-padded")

    print("%-40s %8d B  PRG %2d x16K  CHR %2d x8K  %s  %s"
          % (os.path.basename(path), size, prg, chr_, mapstr,
             "  ".join(flags) if flags else "ok"))


def main():
    targets = sys.argv[1:] or ["roms/nes"]
    files = []
    for t in targets:
        if os.path.isdir(t):
            files += [os.path.join(t, n) for n in sorted(os.listdir(t))
                      if n.lower().endswith(".nes")]
        else:
            files.append(t)
    for f in files:
        check(f)


if __name__ == "__main__":
    main()
