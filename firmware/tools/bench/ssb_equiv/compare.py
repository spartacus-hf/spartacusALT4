"""Compare two equivalence dumps block by block; report the first differing
frame and field. Usage: compare.py base.bin new.bin"""
import struct
import sys

HS_FLOATS, HS_WORDS = 26, 12


def blocks(path):
    d = open(path, "rb").read()
    p = 0
    while p < len(d):
        magic, nl, frames = struct.unpack_from("<3I", d, p); p += 12
        assert magic == 0x4B4C4230, hex(magic)
        name = d[p:p + nl].decode(); p += nl
        ts, = struct.unpack_from("<I", d, p); p += 4
        fr = []
        for _ in range(frames):
            n = 48 * ts * 2 + 4 + 12 + 4 * HS_FLOATS + 4 * HS_WORDS
            fr.append(d[p:p + n]); p += n
        yield name, ts, fr


def fields(raw, ts):
    fmt = "<48I" if ts == 4 else "<48H"
    p = 0
    a = struct.unpack_from(fmt, raw, p); p += 48 * ts
    c = struct.unpack_from(fmt, raw, p); p += 48 * ts
    fl = raw[p:p + 4]; p += 4
    cnt = struct.unpack_from("<3I", raw, p); p += 12
    hs = struct.unpack_from(f"<{HS_FLOATS}f", raw, p); p += 4 * HS_FLOATS
    hw = struct.unpack_from(f"<{HS_WORDS}I", raw, p)
    return {"a": a, "c": c, "flags": tuple(fl), "counts": cnt, "state": hs, "words": hw}


def main(pa, pb):
    ba, bb = list(blocks(pa)), list(blocks(pb))
    if len(ba) != len(bb):
        print("block count differs", len(ba), len(bb))
    bad = 0
    frames = 0
    for (na, ts, fa), (nb, _, fb) in zip(ba, bb):
        frames += len(fa)
        for k, (x, y) in enumerate(zip(fa, fb)):
            if x != y:
                bad += 1
                if bad <= 8:
                    fx, fy = fields(x, ts), fields(y, ts)
                    for key in fx:
                        if fx[key] != fy[key]:
                            diffs = [(i, u, v) for i, (u, v) in enumerate(zip(fx[key], fy[key])) if u != v]
                            print(f"{na!r} frame {k}: {key} differs at {diffs[:4]}")
    print(f"blocks {len(ba)} frames {frames} differing frames {bad}")
    return 1 if bad or len(ba) != len(bb) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
