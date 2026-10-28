#!/usr/bin/env python3
"""Build the Morse trainer's resource pack, sd/CWPACK.BIN, from tools/cw_pack/.

Every training activity reads its material from the SD card rather than from
flash: the Koch courses and their lesson groups, English words, CW
abbreviations, call sign prefixes and the course START LEARNING runs
(tools/cw_pack/README.md says where each comes from). Core/Src/cw_pack.c reads the pack; Core/Src/cw_pack_id.inc gives the
image the pack it was built with, so a card carrying another build's pack is
recognised (the console's `card` lists it, tools/flash_rev1.py replaces it).

CWPACK.BIN, little-endian:
  0   "CWPK"
  4   u32 FNV-1a of every byte from offset 12 on (the pack's id)
  8   u16 format (1)
  10  u16 sections
  12  the section table, 8 bytes each: u8 kind, u8 record size, u16 records,
      u32 offset of the first record from the start of the file
  ... the sections, in table order

Symbols are bytes as Core/Inc/mod_morse_tables.h has them: upper-case ASCII
for characters, 1..6 for the prosigns AS KA KN SK VE BK. A symbol's index is
its place in the alphabet ALPHABET below (mod_morse_tables.c's table order,
MORSE_Index); a word's mask has bit i set for each symbol i it uses, and its
length in bits 56..63.

Sections:
  1 COURSE  64-byte records: u8 length, the symbols in teaching order
            (M32, LCWO, CW Academy, LICW base order: the radio rotates LICW
            from the carousel entry point), then at byte 52 a u56 of lesson
            stops: bit i set when a lesson ends with i + 1 characters
  2 WMASK   u64 word masks          3 WTEXT  12-byte words, 0-padded
  4 AMASK   u64 abbreviation masks  5 ATEXT  12-byte abbreviations
  6 PREFIX  4-byte call sign prefixes, 0-padded, repeated in proportion to
            how often they are heard: one random record is a weighted pick
  7 STEPS   the course START LEARNING runs (tools/cw_pack/steps.txt), 4-byte
            steps: op, a, b, text. b is source | (group - 1) << 3 | prompt
            << 6 for the steps that play; a is the trials, or the step a
            jump goes to; text is a card text (HELP.BIN), 1 for
            TXT_TR_PADDLE_DIT and on from there, 0 for none

Deterministic, so --check can compare the files on disk with this output
(validate_current.py does). Usage:
  gen_cw_pack.py            write sd/CWPACK.BIN and Core/Src/cw_pack_id.inc
  gen_cw_pack.py --check    fail if either file on disk is not this output
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "tools" / "cw_pack"
BINARY = ROOT / "sd" / "CWPACK.BIN"
TARGET = ROOT / "Core" / "Src" / "cw_pack_id.inc"

FORMAT = 2
PROSIGNS = {"S": 1, "A": 2, "N": 3, "K": 4, "E": 5, "B": 6}  # Morserino's letters: <as> <ka> <kn> <sk> <ve> <bk>
ALPHABET = [ord(c) for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,:-/=?@+"] + list(range(1, 7))
INDEX = {s: i for i, s in enumerate(ALPHABET)}
TEXT_MAX = 12        # a record's characters (the trainer's prompt holds 12)
WORDS_MAX = 2048     # the radio's eligibility bitmap: 256 bytes
ABBR_MAX = 256
EARLY_LESSON = 12    # words usable this early in any course are always kept
PREFIX_SLOTS = 2048
OPS = {"PADDLE": 1, "EXPLORE": 2, "INTRO": 3, "LISTEN": 4, "ECHO": 5, "MASTER": 6, "SPEED": 7, "GOTO": 8}
SOURCES = {"CHARS": 0, "WORDS": 1, "ABBR": 2, "CALLS": 3, "MIXED": 4, "DRILL": 5}  # mtrain_src_t, then DRILL
PROMPTS = {"SOUND": 0, "TEXT": 1, "BOTH": 2}  # mtrain_show_t
STEPS_MAX = 32
CARD_TEXT = ROOT / "Core" / "Inc" / "card_text.def"
TEXT_BASE = "TXT_TR_PADDLE_DIT"
K_COURSE, K_WMASK, K_WTEXT, K_AMASK, K_ATEXT, K_PREFIX, K_STEPS = range(1, 8)


def fnv1a(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def lines(name: str) -> list[str]:
    return [ln.split("#", 1)[0].strip() for ln in (SRC / name).read_text().splitlines()
            if ln.split("#", 1)[0].strip()]


def course_symbols(order: str) -> list[int]:
    out = []
    for c in order:
        if c in PROSIGNS:
            out.append(PROSIGNS[c])
        elif c.isupper():
            raise SystemExit(f"courses.txt: {c!r} is no Morserino prosign letter")
        else:
            out.append(ord(c.upper()))
    if sorted(out) != sorted(ALPHABET):
        raise SystemExit(f"courses.txt: {order} is not the whole alphabet, each symbol once")
    return out


def text_symbols(word: str) -> list[int] | None:
    syms = [ord(c) for c in word.upper()]
    return syms if all(s in INDEX for s in syms) and 1 <= len(syms) <= TEXT_MAX else None


def mask(syms: list[int]) -> int:
    m = 0
    for s in syms:
        m |= 1 << INDEX[s]
    return m | (len(syms) << 56)


def words(courses: list[list[int]]) -> list[list[int]]:
    rows = []
    for ln in lines("words.txt"):
        w, weight = ln.split()
        syms = text_symbols(w) if re.fullmatch(r"[a-z]+", w) and len(w) <= 10 else None
        if syms:
            rows.append((int(weight), w, syms))
    rows.sort(key=lambda r: (-r[0], r[1]))
    ranks = [{s: i + 1 for i, s in enumerate(c)} for c in courses]
    early = [r for r in rows if min(max(rk[s] for s in r[2]) for rk in ranks) <= EARLY_LESSON]
    keep = {r[1] for r in early}
    for r in rows:
        if len(keep) >= WORDS_MAX:
            break
        keep.add(r[1])
    # Most frequent first: the radio favours the front of the list.
    return [r[2] for r in rows if r[1] in keep]


def abbreviations() -> list[list[int]]:
    out = []
    for a in lines("abbrev.txt"):
        syms = text_symbols(a)
        if not syms:
            raise SystemExit(f"abbrev.txt: {a!r} has a character the radio cannot send")
        out.append(syms)
    if len(out) > ABBR_MAX:
        raise SystemExit("abbrev.txt: more abbreviations than the radio's bitmap holds")
    return out


def prefixes() -> list[bytes]:
    rows = []
    for ln in lines("prefixes.txt"):
        p, _cont, _zone, weight = ln.split()
        # 4U_I, FR/E, SV/A: names of DXCC entities rather than prefixes a
        # call sign starts with (all of weight 1 too, so they get no slot).
        if re.fullmatch(r"[0-9A-Z]{1,4}", p):
            rows.append((p, int(weight)))
    total = sum(w for _, w in rows)
    # Largest remainders: each prefix gets its share of the slots.
    shares = [(p, w * PREFIX_SLOTS / total) for p, w in rows]
    slots = {p: int(s) for p, s in shares}
    left = PREFIX_SLOTS - sum(slots.values())
    for p, s in sorted(shares, key=lambda x: (-(x[1] - int(x[1])), x[0]))[:left]:
        slots[p] += 1
    return [p.encode().ljust(4, b"\0") for p, _ in rows for _ in range(slots[p])]


def stops(spec: str, n: int) -> int:
    """'2+1' or '4,9,15+1': lessons end with that many characters, then every
    N more up to the whole course; as a mask, bit i for i + 1 characters."""
    listed, _, every = spec.partition("+")
    sizes = [int(x) for x in listed.split(",")]
    step = int(every) if every else 0
    while step and sizes[-1] + step <= n:
        sizes.append(sizes[-1] + step)
    if sizes != sorted(set(sizes)) or sizes[0] < 2 or sizes[-1] != n:
        raise SystemExit(f"courses.txt: lesson stops {spec!r} must rise from 2 or more to {n}")
    return sum(1 << (k - 1) for k in sizes)


def card_texts() -> list[str]:
    """card_text_t's names in value order (Core/Inc/card_text.def)."""
    text = re.sub(r"/\*.*?\*/", "", CARD_TEXT.read_text(), flags=re.S)
    return re.findall(r'TEXT\(\s*(\w+)\s*,', text)


def steps() -> list[bytes]:
    """steps.txt's course: one step a line, ':NAME' names the next step."""
    rows, labels = [], {}
    for ln in lines("steps.txt"):
        if ln.startswith(":"):
            labels[ln[1:]] = len(rows)
        else:
            rows.append(ln)
    texts = card_texts()
    base = texts.index(TEXT_BASE)
    out = []
    for at, ln in enumerate(rows):
        def bad(why: str) -> SystemExit:
            return SystemExit(f"steps.txt: {ln!r}: {why}")
        words, text, target = ln.split(), 0, None
        if words[-1].startswith("@"):
            if words[-1][1:] not in texts:
                raise bad("no such card text")
            text = texts.index(words.pop()[1:]) - base + 1
            if not 1 <= text <= 255:
                raise bad("the card text is out of the trainer's reach")
        if len(words) > 2 and words[-2] == "->":
            if words[-1] not in labels:
                raise bad("no such label")
            target = labels[words.pop()]
            words.pop()
        op, args = words[0], words[1:]
        if op not in OPS:
            raise bad("no such step")
        a, b = 0, 0
        if op in ("LISTEN", "ECHO", "MASTER"):
            if op != "MASTER":
                a = int(args.pop(0))
            if not args or args[0] not in SOURCES:
                raise bad("a source: " + " ".join(SOURCES))
            src = SOURCES[args.pop(0)]
            group = int(args.pop(0)) if args and args[0].isdigit() else 1
            show = PROMPTS[args.pop(0)] if args and args[0] in PROMPTS else 0
            if args or not 1 <= group <= 8 or not 0 <= a <= 255 or (op == "LISTEN" and a == 0):
                raise bad("arguments")
            b = src | (group - 1) << 3 | show << 6
            if op == "MASTER":
                a = 255 if target is None else target
        elif op == "SPEED":
            if target is None or len(args) != 1 or not 1 <= int(args[0]) <= 10:
                raise bad("SPEED n -> LABEL")
            a, b = target, int(args[0])
        elif op == "GOTO":
            if target is None or args:
                raise bad("GOTO -> LABEL")
            a = target
        elif args or target is not None:
            raise bad("takes no arguments")
        out.append(bytes([OPS[op], a, b, text]))
    if not 1 <= len(out) <= STEPS_MAX:
        raise SystemExit(f"steps.txt: 1 to {STEPS_MAX} steps")
    last = rows[-1].split()
    if not (last[0] == "GOTO" or (last[0] == "ECHO" and last[1] == "0")):
        raise SystemExit("steps.txt: the course ends in an endless ECHO 0 or a GOTO, never runs out")
    return out


def render() -> tuple[bytes, str, dict]:
    courses, records = [], []
    for ln in lines("courses.txt"):
        _name, order, spec = ln.split()
        courses.append(course_symbols(order))
        head = bytes([len(courses[-1])] + courses[-1]).ljust(52, b"\0")
        records.append((head + stops(spec, len(courses[-1])).to_bytes(7, "little")).ljust(64, b"\0"))
    if len(courses) != 4:
        raise SystemExit("courses.txt: M32, LCWO, CWA, LICW")
    wl, al = words(courses), abbreviations()
    sections = [
        (K_COURSE, 64, records),
        (K_WMASK, 8, [struct.pack("<Q", mask(w)) for w in wl]),
        (K_WTEXT, TEXT_MAX, [bytes(w).ljust(TEXT_MAX, b"\0") for w in wl]),
        (K_AMASK, 8, [struct.pack("<Q", mask(a)) for a in al]),
        (K_ATEXT, TEXT_MAX, [bytes(a).ljust(TEXT_MAX, b"\0") for a in al]),
        (K_PREFIX, 4, prefixes()),
        (K_STEPS, 4, steps()),
    ]
    offset = 12 + 8 * len(sections)
    table, body = b"", b""
    for kind, size, recs in sections:
        assert all(len(r) == size for r in recs)
        table += struct.pack("<BBHI", kind, size, len(recs), offset + len(body))
        body += b"".join(recs)
    rest = table + body
    digest = fnv1a(rest)
    binary = b"CWPK" + struct.pack("<IHH", digest, FORMAT, len(sections)) + rest
    inc = "\n".join([
        "/* Generated by tools/gen_cw_pack.py from tools/cw_pack/ -- do not edit.",
        f" * sd/CWPACK.BIN: {len(binary)} bytes, {len(wl)} words, {len(al)} abbreviations,",
        f" * {len(courses)} courses; its id is the FNV-1a of every byte from offset 12. */",
        f"#define CWPACK_ID     0x{digest:08X}UL",
        f"#define CWPACK_FORMAT {FORMAT}U",
        "",
    ])
    return binary, inc, {"words": len(wl), "abbr": len(al), "size": len(binary)}


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args(argv)
    binary, inc, info = render()
    if args.check:
        on_disk = BINARY.read_bytes() if BINARY.exists() else b""
        inc_on_disk = TARGET.read_text() if TARGET.exists() else ""
        if on_disk != binary or inc_on_disk != inc:
            print(f"{BINARY} or {TARGET}: DIFFERS from the generator; run tools/gen_cw_pack.py", file=sys.stderr)
            return 1
        print(f"{BINARY} and {TARGET}: match the generator ({info['size']} bytes)")
        return 0
    BINARY.parent.mkdir(parents=True, exist_ok=True)
    BINARY.write_bytes(binary)
    TARGET.write_text(inc)
    print(f"wrote {BINARY} ({info['size']} bytes: {info['words']} words, {info['abbr']} abbreviations) and {TARGET}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
