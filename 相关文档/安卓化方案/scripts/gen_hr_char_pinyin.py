# 从 mozillazg/pinyin-data 生成 hr_char_pinyin.txt（紧凑 汉字=拼音 表）
# 约定对齐现网 HomophoneReplacer：TONE3（音节末尾数字声调），轻声按注释"低声用1声"处理，
# ü 记作 v（pypinyin TONE3 惯例）。仅保留 CJK 区间字符。
import re
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\tang5\AppData\Local\Temp\pinyin.txt"
DST = sys.argv[2] if len(sys.argv) > 2 else "models/sherpa-onnx/hr/hr_char_pinyin.txt"

# 声调符号 -> (基字母, 声调)
TONE_MAP = {
    "ā": ("a", 1), "á": ("a", 2), "ǎ": ("a", 3), "à": ("a", 4),
    "ō": ("o", 1), "ó": ("o", 2), "ǒ": ("o", 3), "ò": ("o", 4),
    "ē": ("e", 1), "é": ("e", 2), "ě": ("e", 3), "è": ("e", 4),
    "ī": ("i", 1), "í": ("i", 2), "ǐ": ("i", 3), "ì": ("i", 4),
    "ū": ("u", 1), "ú": ("u", 2), "ǔ": ("u", 3), "ù": ("u", 4),
    "ǖ": ("v", 1), "ǘ": ("v", 2), "ǚ": ("v", 3), "ǜ": ("v", 4),
    "ń": ("n", 2), "ň": ("n", 3), "ǹ": ("n", 4),
    "ḿ": ("m", 2),
}


def to_tone3(syllable: str) -> str:
    out = []
    tone = 0
    for ch in syllable:
        if ch in TONE_MAP:
            base, t = TONE_MAP[ch]
            out.append(base)
            tone = t
        elif ch == "ü":
            out.append("v")
        elif ch.isalpha() and ch.isascii():
            out.append(ch.lower())
        # 其他符号（如 ' 声调分隔、. 等）丢弃
    if not out:
        return ""
    if tone == 0:
        tone = 1  # 轻声 → 1（对齐现网表约定"低声用1声"）
    return "".join(out) + str(tone)


def is_cjk(cp: int) -> bool:
    return (0x4E00 <= cp <= 0x9FFF) or (0x3400 <= cp <= 0x4DBF) or (0xF900 <= cp <= 0xFAFF)


pat = re.compile(r"^U\+([0-9A-Fa-f]+):\s*([^#]+?)\s*(?:#.*)?$")
count = 0
seen = set()
with open(SRC, encoding="utf-8") as f, open(DST, "w", encoding="utf-8", newline="\n") as out:
    for line in f:
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        m = pat.match(line)
        if not m:
            continue
        cp = int(m.group(1), 16)
        if not is_cjk(cp):
            continue
        readings = m.group(2).split(",")
        if not readings or not readings[0].strip():
            continue
        py = to_tone3(readings[0].strip())
        if not py:
            continue
        ch = chr(cp)
        if ch in seen:
            continue
        seen.add(ch)
        out.write(f"{ch}={py}\n")
        count += 1

print(f"wrote {count} entries -> {DST}")
