"""从 mozillazg/pinyin-data 的 pinyin.txt 生成 src/pinyin_table.inc

用法：python tools/gen_pinyin.py
（首次运行会下载 pinyin.txt 到 _tmp/，之后复用缓存）

格式：
  static const unsigned short kPyFirst[];   // U+4E00..U+9FA5 每字首选读音（下标，0xFFFF = 无）
  static const unsigned short kPyAltCp[];   // 有第二读音的码位（升序）
  static const unsigned short kPyAltIdx[];  // 对应第二读音下标
  kPySyl / kPySylOff：音节字符串池
"""
import os
import re
import sys
import unicodedata
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "_tmp", "pinyin.txt")
OUT = os.path.join(ROOT, "src", "pinyin_table.inc")

# jsdelivr 比 raw.githubusercontent 稳（后者在国内经常超时）
URL = "https://cdn.jsdelivr.net/gh/mozillazg/pinyin-data@master/pinyin.txt"

if not os.path.exists(SRC):
    os.makedirs(os.path.dirname(SRC), exist_ok=True)
    print("downloading", URL)
    urllib.request.urlretrieve(URL, SRC)

LO, HI = 0x4E00, 0x9FA5
# kPyFirst 的「无读音」哨兵。不能用 0：0 是音节池里的合法下标
# （U+4E00 一 -> yi 恰好排第一），用 0 当哨兵会让所有读 yi 的字变成没读音。
NONE = 0xFFFF


def strip_tone(s):
    """lè -> le, ǚ -> v"""
    s = s.replace("ü", "v").replace("Ü", "v")
    out = []
    for ch in unicodedata.normalize("NFD", s):
        if unicodedata.combining(ch):
            continue
        out.append(ch)
    return "".join(out).lower()


readings = {}
with open(SRC, encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^U\+([0-9A-Fa-f]{4,6}):\s*([^#]+)", line)
        if not m:
            continue
        cp = int(m.group(1), 16)
        if cp < LO or cp > HI:
            continue
        syls = [strip_tone(x.strip()) for x in m.group(2).split(",") if x.strip()]
        syls = [s for s in syls if s and s.isascii() and s.isalpha()]
        if not syls:
            continue
        # 去重但保序
        uniq = []
        for s in syls:
            if s not in uniq:
                uniq.append(s)
        readings[cp] = uniq

print("chars with reading:", len(readings))

# 音节池
pool = []
pool_idx = {}
for cp in sorted(readings):
    for s in readings[cp][:2]:
        if s not in pool_idx:
            pool_idx[s] = len(pool)
            pool.append(s)

print("syllables:", len(pool))

first = [NONE] * (HI - LO + 1)
alt_cp = []
alt_idx = []
for cp, syls in sorted(readings.items()):
    first[cp - LO] = pool_idx[syls[0]]
    if len(syls) > 1:
        alt_cp.append(cp)
        alt_idx.append(pool_idx[syls[1]])

print("alt entries:", len(alt_cp))


def arr(name, typ, vals, per_line=16, fmt="%d"):
    lines = ["static const %s %s[%d] = {" % (typ, name, len(vals))]
    for i in range(0, len(vals), per_line):
        lines.append("    " + ",".join(fmt % v for v in vals[i:i + per_line]) + ",")
    lines.append("};")
    return "\n".join(lines)


blob_parts = []
offsets = []
pos = 0
for s in pool:
    offsets.append(pos)
    blob_parts.append(s)
    pos += len(s) + 1
offsets.append(pos)

blob = "\\0".join(blob_parts)

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("// 自动生成，勿手改。来源：mozillazg/pinyin-data (MIT)\n")
    f.write("// 生成脚本：tools/gen_pinyin.py\n")
    f.write("#pragma once\n\n")
    f.write("#define PY_CP_LO 0x%04X\n" % LO)
    f.write("#define PY_CP_HI 0x%04X\n" % HI)
    f.write("#define PY_SYL_COUNT %d\n" % len(pool))
    f.write("#define PY_SYL_NONE 0xFFFF\n\n")
    f.write(arr("kPyFirst", "unsigned short", first))
    f.write("\n\n")
    f.write(arr("kPyAltCp", "unsigned short", alt_cp))
    f.write("\n\n")
    f.write(arr("kPyAltIdx", "unsigned short", alt_idx))
    f.write("\n\n")
    f.write("static const char kPySyl[] =\n")
    # 按 100 字节分行，避免超长行
    step = 100
    chunks = []
    i = 0
    cur = 0
    start = 0
    while i < len(blob):
        if i - start >= step and blob[i] == "\\":
            chunks.append(blob[start:i])
            start = i
        i += 1
    chunks.append(blob[start:])
    for c in chunks:
        f.write('    "%s"\n' % c)
    f.write("    ;\n\n")
    f.write(arr("kPySylOff", "unsigned short", offsets, per_line=12))

print("written", OUT, os.path.getsize(OUT), "bytes")
