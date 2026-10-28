#!/usr/bin/env python3
"""Pack the settings help (Core/Inc/ui_help_text.def) for UI_SettingHelp
(Core/Src/ui_help.c).

The help is ~13 KB of plain ASCII, read one text at a time into the menu's
help panel or a console line. It is stored byte-pair coded: every byte value
the texts do not use (1..31 and 127..255) stands for a pair of bytes, each a
character or another pair, chosen greedily by frequency, and the stored help
comes to about 60 % of the text.

Since v8.09 the same file carries the card texts too (Core/Inc/card_text.def:
console help and usage lines, the menu's category descriptions, export
headers), packed with the help's pair table; text_at[] gives where each
starts.

Since v8.08 the packed help lives on the SD card, which the radio requires:
sd/HELP.BIN holds the pair table and the packed texts (~7.9 KB that no longer
take the MCU's flash), and Core/Src/ui_help_packed.inc keeps only where each
setting's text starts and the file's hash, so a card carrying another build's
help is recognised rather than misread.

HELP.BIN: "RHLP", u32 FNV-1a of everything after the header, u16 pair count,
u16 blob length, then where each text starts (u16, its blob offset + 1: one
a setting, by setting_id_t, then one a card text, by card_text_t, then the
USB CLEAN bypass line), the pairs (2 bytes each) and the blob; little-endian.
Since v8.10 that index is on the card too: flash keeps only its length.

The setting names (Core/Inc/ui_setting_names.def) are packed the same way
but stay in flash, pairs and all, in Core/Src/ui_names_packed.inc: the menu
and the console need them with or without a card.

Deterministic, so --check can compare the files on disk with this output
(validate_current.py does). Usage:
  gen_help_text.py            write Core/Src/ui_help_packed.inc, sd/HELP.BIN
                              and Core/Src/ui_names_packed.inc
  gen_help_text.py --check    fail if any file on disk is not this output
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "Core" / "Inc" / "ui_help_text.def"
CARD_TEXT = ROOT / "Core" / "Inc" / "card_text.def"
TARGET = ROOT / "Core" / "Src" / "ui_help_packed.inc"
BINARY = ROOT / "sd" / "HELP.BIN"
SPECIAL = ("HELP_NONE", "HELP_BYPASSED")
SETTINGS_ENUM = ROOT / "Core" / "Inc" / "ui_priv.h"
NAMES = ROOT / "Core" / "Inc" / "ui_setting_names.def"
NAMES_TARGET = ROOT / "Core" / "Src" / "ui_names_packed.inc"


def setting_ids() -> list[str]:
    """setting_id_t's names in value order (it has no conditionals)."""
    text = SETTINGS_ENUM.read_text()
    body = text[text.index("SET_PROFILE = 0"):text.index("} setting_id_t;")]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    if "#" in body:
        raise SystemExit("setting_id_t gained a conditional: HELP.BIN's index needs fixed ids")
    names = [n.split("=")[0].strip() for n in body.split(",")]
    names = [n for n in names if n]
    if names[-1] != "SET_COUNT":
        raise SystemExit("setting_id_t must end with SET_COUNT")
    return names[:-1]


def parse(text: str) -> list[tuple[str, str]]:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    entries = re.findall(r'HELP\(\s*(\w+)\s*,\s*"((?:[^"\\]|\\.)*)"\s*\)', text)
    for key, body in entries:
        if "\\" in body or any(not (0x20 <= ord(ch) < 0x7F) for ch in body):
            raise SystemExit(f"{key}: help text must be plain printable ASCII without escapes")
    keys = [k for k, _ in entries]
    dup = {k for k in keys if keys.count(k) > 1}
    if dup:
        raise SystemExit(f"help given twice: {', '.join(sorted(dup))}")
    for s in SPECIAL:
        if s not in keys:
            raise SystemExit(f"{s} missing from {SOURCE.name}")
    return entries


def parse_card_text(text: str) -> list[tuple[str, str]]:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    entries = re.findall(r'TEXT\(\s*(\w+)\s*,\s*"((?:[^"\\]|\\.)*)"\s*\)', text)
    for key, body in entries:
        if "\\" in body or any(not (0x20 <= ord(ch) < 0x7F) for ch in body) or not body:
            raise SystemExit(f"{key}: card text must be non-empty plain printable ASCII without escapes")
    keys = [k for k, _ in entries]
    dup = {k for k in keys if keys.count(k) > 1}
    if dup:
        raise SystemExit(f"card text given twice: {', '.join(sorted(dup))}")
    return entries


def is_code(b: int) -> bool:
    return b < 0x20 or b >= 0x7F


def pair_index(code: int) -> int:
    """Where code's pair sits in help_pair (the C side does the same)."""
    return code - 0x7F if code >= 0x7F else code + 128


def pack(texts: list[str]) -> tuple[list[tuple[int, int]], list[list[int]]]:
    codes = [c for c in list(range(0x7F, 0x100)) + list(range(0x01, 0x20))]
    seqs = [list(t.encode()) for t in texts]
    pairs: list[tuple[int, int]] = []
    for code in codes:
        count: Counter = Counter()
        for q in seqs:
            for a, b in zip(q, q[1:]):
                count[(a, b)] += 1
        if not count:
            break
        (a, b), n = min(count.items(), key=lambda kv: (-kv[1], kv[0]))
        if n < 2:
            break
        pairs.append((a, b))
        for k, q in enumerate(seqs):
            out, i = [], 0
            while i < len(q):
                if i + 1 < len(q) and q[i] == a and q[i + 1] == b:
                    out.append(code)
                    i += 2
                else:
                    out.append(q[i])
                    i += 1
            seqs[k] = out
    return pairs, seqs


def unpack(pairs: list[tuple[int, int]], seq: list[int]) -> tuple[str, int]:
    """The C loop, in Python: the text and the deepest stack it needed."""
    stack: list[int] = []
    out, deepest, i = [], 0, 0
    while True:
        if stack:
            c = stack.pop()
        else:
            if i == len(seq):
                break
            c = seq[i]
            i += 1
        while is_code(c):
            a, b = pairs[pair_index(c)]
            stack.append(b)
            deepest = max(deepest, len(stack))
            c = a
        out.append(chr(c))
    return "".join(out), deepest


def fnv1a(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def render() -> tuple[str, bytes, int, int]:
    entries = parse(SOURCE.read_text())
    cards = parse_card_text(CARD_TEXT.read_text())
    texts = list(dict.fromkeys([t for _, t in entries] + [t for _, t in cards]))
    pairs, seqs = pack(texts)
    blob, at, deepest = [], {}, 0
    for t, seq in zip(texts, seqs):
        back, depth = unpack(pairs, seq)
        assert back == t, f"round trip failed for {t!r}"
        deepest = max(deepest, depth)
        at[t] = len(blob) + 1  # 0 is "none"
        blob.extend(seq + [0])
    assert len(blob) < 0xFFFF
    raw = sum(len(t) + 1 for t in texts)
    ids, helps = setting_ids(), dict(entries)
    unknown = sorted(set(helps) - set(ids) - set(SPECIAL))
    if unknown:
        raise SystemExit(f"help for unknown settings: {', '.join(unknown)}")
    index = [at[helps[k]] if k in helps else at[helps["HELP_NONE"]] for k in ids]
    # TXT_NONE's place holds the general line, for a setting id out of range.
    index += [at[helps["HELP_NONE"]]] + [at[t] for _, t in cards] + [at[helps["HELP_BYPASSED"]]]
    body = b"".join(v.to_bytes(2, "little") for v in index) + bytes(v for ab in pairs for v in ab) + bytes(blob)
    digest = fnv1a(body)
    binary = b"RHLP" + digest.to_bytes(4, "little") + len(pairs).to_bytes(2, "little") + \
        len(blob).to_bytes(2, "little") + body
    size = len(binary)
    lines = [
        "/* Generated by tools/gen_help_text.py from Core/Inc/ui_help_text.def",
        f" * and Core/Inc/card_text.def -- do not edit. {len(texts)} texts, {raw} bytes as",
        f" * text, {size} packed ({100 * size // raw} %) in sd/HELP.BIN on the card: a byte",
        " * below 0x20 or from 0x7F up is pair[] of it,",
        f" * two bytes that are each a character or another pair; 0 ends a text. */",
        f"#define HELP_TEXT_MAX {max(len(t) for t in texts)}U",
        f"#define HELP_STACK    {deepest}U",
        f"#define HELP_PAIRS    {len(pairs)}U",
        f"#define HELP_BLOB_LEN {len(blob)}U",
        f"#define HELP_HASH     0x{digest:08X}UL",
        "/* The index on the card: a setting's text at its id, a card text's at",
        " * HELP_KEY_CARD + its id (TXT_NONE's: the general line), the USB CLEAN",
        " * bypass at HELP_KEY_BYPASSED. */",
        f"#define HELP_KEY_CARD     {len(ids)}U",
        f"#define HELP_KEY_BYPASSED {len(ids) + 1 + len(cards)}U",
        f"#define HELP_KEYS         {len(index)}U",
        f"_Static_assert(SET_COUNT == {len(ids)}, \"HELP.BIN indexes this many settings\");",
        f"_Static_assert(TXT_COUNT == {1 + len(cards)}, \"HELP.BIN indexes this many card texts\");",
    ]
    return "\n".join(lines) + "\n", binary, raw, size


def render_names() -> str:
    """The resident setting names: pairs, the coded names, where each starts."""
    text = re.sub(r"/\*.*?\*/", "", NAMES.read_text(), flags=re.S)
    entries = re.findall(r'NAME\(\s*(\w+)\s*,\s*"([^"\\]*)"\s*\)', text)
    ids = setting_ids()
    if [k for k, _ in entries] != ids:
        raise SystemExit(f"{NAMES.name}: one NAME per setting, in setting_id_t's order")
    for key, name in entries:
        if not name or len(name) > 23 or any(not (0x20 <= ord(ch) < 0x7F) for ch in name):
            raise SystemExit(f"{key}: a name is 1 to 23 printable ASCII characters")
    names = [n for _, n in entries]
    if len(set(names)) != len(names):
        raise SystemExit(f"{NAMES.name}: every name is different (the blob is in setting order)")
    pairs, seqs = pack(names)
    blob, at, deepest = [], {}, 0
    for n, seq in zip(names, seqs):
        back, depth = unpack(pairs, seq)
        assert back == n, f"round trip failed for {n!r}"
        deepest = max(deepest, depth)
        at[n] = len(blob)
        blob.extend(seq + [0])
    assert len(blob) < 0xFFFF
    lines = [
        "/* Generated by tools/gen_help_text.py from Core/Inc/ui_setting_names.def",
        f" * -- do not edit. {len(entries)} names, {sum(len(n) + 1 for n in names)} bytes as text, "
        f"{len(blob) + 2 * len(pairs)} coded",
        " * (pairs as in ui_help_packed.inc). */",
        f"#define NAMES_STACK {max(deepest, 1)}U",
        f"_Static_assert(SET_COUNT == {len(ids)}, \"a name for every setting\");",
        f"static const uint8_t name_pair[{max(len(pairs), 1)}][2] = {{",
    ]
    for a, b in pairs:
        lines.append(f"    {{{a}U, {b}U}},")
    lines.append("};")
    lines.append(f"static const uint8_t name_blob[{len(blob)}] = {{")
    for i in range(0, len(blob), 16):
        lines.append("    " + " ".join(f"{v}U," for v in blob[i:i + 16]))
    lines.append("};")
    # Where every 16th name starts; the names between follow, each ended by 0.
    lines.append("static const uint16_t name_at16[(SET_COUNT + 15) / 16] = {")
    marks = [at[n] for _, n in entries[::16]]
    for i in range(0, len(marks), 12):
        lines.append("    " + " ".join(f"{v}U," for v in marks[i:i + 12]))
    lines.append("};")
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args(argv)
    text, binary, raw, size = render()
    names = render_names()
    if args.check:
        on_disk = TARGET.read_text() if TARGET.exists() else ""
        blob_on_disk = BINARY.read_bytes() if BINARY.exists() else b""
        names_on_disk = NAMES_TARGET.read_text() if NAMES_TARGET.exists() else ""
        if on_disk != text or blob_on_disk != binary or names_on_disk != names:
            print(f"{TARGET}, {BINARY} or {NAMES_TARGET}: DIFFERS from the generator; run tools/gen_help_text.py",
                  file=sys.stderr)
            return 1
        print(f"{TARGET} and {BINARY}: match the generator ({raw} bytes of help in {size} on the card)")
        return 0
    TARGET.write_text(text)
    NAMES_TARGET.write_text(names)
    BINARY.parent.mkdir(parents=True, exist_ok=True)
    BINARY.write_bytes(binary)
    print(f"wrote {TARGET} and {BINARY} ({raw} bytes of help in {size})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
