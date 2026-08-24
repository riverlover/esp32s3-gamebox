#!/usr/bin/env python3
"""用 macOS Daniel 语音生成 WORDS 的离线英式 IMA-ADPCM 发音包。

产物只放 build/，不进入源码仓库。第一次生成会逐词调用 say 和 ffmpeg，已生成
的 24 kHz PCM 会缓存；教材只改几个词时无需重做其余 400 多条。
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

MAGIC = b"GBWUK01\0"
VERSION = 1
SAMPLE_RATE = 24_000
HEADER = struct.Struct("<8s8I")
ENTRY = struct.Struct("<4I")

IMA_INDEX = (-1, -1, -1, -1, 2, 4, 6, 8,
             -1, -1, -1, -1, 2, 4, 6, 8)
IMA_STEP = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
    13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)

# 缩写用空格明确要求逐字母读；Daniel 对 Mrs 默认会按字母拼读，不符合教材语境。
SPEAK_AS = {
    # Daniel 把孤立的大写 I 当成字母名称之外的特殊文本处理，上板听感不对；
    # 用同音词锁定小学教材要求的 /aɪ/，不依赖系统 TTS 的大小写启发式。
    "I": "eye",
    "PE": "P E",
    "UK": "U K",
    "USA": "U S A",
    "TV": "T V",
    "Mrs": "missus",
    "Mr": "mister",
    "jiaozi": "jiao zi",
    "tangyuan": "tang yuan",
}


def fnv1a(data: bytes) -> int:
    value = 2_166_136_261
    for byte in data:
        value ^= byte
        value = (value * 16_777_619) & 0xFFFF_FFFF
    return value


def load_words(source: Path) -> list[str]:
    text = source.read_text(encoding="utf-8")
    raw_words = re.findall(r'\bU\(\s*\d+\s*,\s*"((?:\\.|[^"\\])*)"', text)
    words = [ast.literal_eval(f'"{raw}"') for raw in raw_words]
    if len(words) != 512:
        raise RuntimeError(f"应从 8 册教材读到 512 条，实际 {len(words)} 条")
    return list(dict.fromkeys(words))


def make_pcm(word: str, cache: Path, voice: str, rate: int,
             say: str, ffmpeg: str) -> bytes:
    spoken = SPEAK_AS.get(word, word)
    key = hashlib.sha256(f"v3\0{voice}\0{rate}\0{spoken}".encode()).hexdigest()[:20]
    pcm_path = cache / f"{key}.pcm"
    if pcm_path.exists():
        cached = pcm_path.read_bytes()
        if cached and len(cached) % 2 == 0:
            return cached
        # macOS 语音服务在受限进程里可能只生成空文件。不能把这种失败当缓存，
        # 否则解除限制重跑后仍会永久卡在同一个词。
        pcm_path.unlink()
    if not pcm_path.exists():
        aiff_path = cache / f"{key}.aiff"
        subprocess.run([say, "-v", voice, "-r", str(rate), "-o", str(aiff_path), spoken],
                       check=True)
        # 去掉系统 TTS 两端较长空白，再保留 40/100 ms 呼吸边界。轻度响度统一让
        # 单词和短句在小尺寸 MAX98357 喇叭上的主观音量不要忽大忽小。
        # stop_periods=1 会把短句第一个自然停顿误认成结尾，例如 Here I am 只
        # 剩下 Here。结尾先反转成开头再裁，内部停顿就完全保留。
        audio_filter = (
            "silenceremove=start_periods=1:start_silence=0.04:start_threshold=-45dB,"
            "areverse,"
            "silenceremove=start_periods=1:start_silence=0.10:start_threshold=-45dB,"
            "areverse,loudnorm=I=-18:TP=-2:LRA=7"
        )
        subprocess.run([
            ffmpeg, "-nostdin", "-loglevel", "error", "-y", "-i", str(aiff_path),
            "-af", audio_filter, "-ar", str(SAMPLE_RATE), "-ac", "1",
            "-f", "s16le", str(pcm_path),
        ], check=True)
        aiff_path.unlink(missing_ok=True)
    pcm = pcm_path.read_bytes()
    if len(pcm) < 2 or len(pcm) % 2:
        raise RuntimeError(f"{word!r} 的 PCM 长度异常：{len(pcm)}")
    return pcm


def encode_ima(pcm: bytes) -> tuple[bytes, int]:
    samples = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    predictor = samples[0]
    step_index = 0
    nibbles: list[int] = []
    for sample in samples[1:]:
        step = IMA_STEP[step_index]
        delta = sample - predictor
        code = 8 if delta < 0 else 0
        if delta < 0:
            delta = -delta
        diff = step >> 3
        if delta >= step:
            code |= 4
            delta -= step
            diff += step
        if delta >= step >> 1:
            code |= 2
            delta -= step >> 1
            diff += step >> 1
        if delta >= step >> 2:
            code |= 1
            diff += step >> 2
        predictor = predictor - diff if code & 8 else predictor + diff
        predictor = min(32767, max(-32768, predictor))
        step_index = min(88, max(0, step_index + IMA_INDEX[code]))
        nibbles.append(code)

    packed = bytearray(struct.pack("<hBB", samples[0], 0, 0))
    for pos in range(0, len(nibbles), 2):
        value = nibbles[pos]
        if pos + 1 < len(nibbles):
            value |= nibbles[pos + 1] << 4
        packed.append(value)
    return bytes(packed), len(samples)


def verify_pack(image: bytes, words: list[str]) -> None:
    if len(image) < HEADER.size:
        raise RuntimeError("发音包短于文件头")
    (magic, version, sample_rate, count, index_offset, data_offset,
     image_size, index_hash, _reserved) = HEADER.unpack_from(image)
    if (magic, version, sample_rate, count, index_offset, image_size) != (
            MAGIC, VERSION, SAMPLE_RATE, len(words), HEADER.size, len(image)):
        raise RuntimeError("发音包文件头自检失败")
    index_bytes = image[index_offset:data_offset]
    if len(index_bytes) != count * ENTRY.size or fnv1a(index_bytes) != index_hash:
        raise RuntimeError("发音包索引长度或校验值错误")

    expected = {fnv1a(word.encode("utf-8")) for word in words}
    actual: set[int] = set()
    previous_hash = -1
    cursor = data_offset
    for pos in range(count):
        hashed, offset, byte_count, sample_count = ENTRY.unpack_from(
            image, index_offset + pos * ENTRY.size)
        if hashed <= previous_hash or offset != cursor or sample_count == 0:
            raise RuntimeError(f"发音包第 {pos} 条的排序、偏移或采样数错误")
        if byte_count != 4 + sample_count // 2 or offset + byte_count > len(image):
            raise RuntimeError(f"发音包第 {pos} 条的 ADPCM 长度错误")
        previous_hash = hashed
        actual.add(hashed)
        cursor += byte_count
    if cursor != len(image) or actual != expected:
        raise RuntimeError("发音包末尾边界或教材词条集合错误")
    if len(image) > 13 * 1024 * 1024:
        raise RuntimeError("发音包超过 13 MiB 分区")


def build(source: Path, output: Path, cache: Path, voice: str, rate: int) -> None:
    say = shutil.which("say")
    ffmpeg = shutil.which("ffmpeg")
    if not say or not ffmpeg:
        raise RuntimeError("生成语音包需要 macOS say 和 ffmpeg")

    words = load_words(source)
    cache.mkdir(parents=True, exist_ok=True)
    records: list[tuple[int, str, bytes, int]] = []
    hashes: dict[int, str] = {}
    for number, word in enumerate(words, 1):
        hashed = fnv1a(word.encode("utf-8"))
        if hashed >= 0xFFFF_FFFC or hashed in hashes:
            raise RuntimeError(f"FNV-1a 哈希冲突：{word!r} / {hashes.get(hashed)!r}")
        hashes[hashed] = word
        pcm = make_pcm(word, cache, voice, rate, say, ffmpeg)
        adpcm, samples = encode_ima(pcm)
        records.append((hashed, word, adpcm, samples))
        print(f"[{number:3d}/{len(words)}] {word:<28} {samples / SAMPLE_RATE:4.2f}s",
              flush=True)

    records.sort(key=lambda item: item[0])
    data_offset = HEADER.size + len(records) * ENTRY.size
    cursor = data_offset
    index = bytearray()
    payload = bytearray()
    for hashed, _word, adpcm, samples in records:
        index += ENTRY.pack(hashed, cursor, len(adpcm), samples)
        payload += adpcm
        cursor += len(adpcm)

    header = HEADER.pack(
        MAGIC, VERSION, SAMPLE_RATE, len(records), HEADER.size, data_offset,
        cursor, fnv1a(index), 0,
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    image = header + index + payload
    verify_pack(image, words)
    output.write_bytes(image)
    print(f"完成：{len(records)} 个不重复词条，{cursor / 1024 / 1024:.2f} MiB -> {output}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--voice", default="Daniel")
    parser.add_argument("--rate", type=int, default=155,
                        help="macOS say 每分钟词数；155 适合小学阶段跟读")
    args = parser.parse_args()
    cache = args.cache or args.output.parent / "word_audio_cache_uk"
    try:
        build(args.source, args.output, cache, args.voice, args.rate)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
