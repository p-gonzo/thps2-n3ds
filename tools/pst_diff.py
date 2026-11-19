#!/usr/bin/env python3
"""Compare two psxrecomp save states section by section.

    python3 tools/pst_diff.py a.pst b.pst

Prints each section tag that differs; for RAM the differing address ranges,
for the CPU section the differing 32-bit words (gpr 0-31 first).
"""
import struct, sys, zlib

NAMES = {0x01: "CPU", 0x02: "RAM", 0x03: "SPAD", 0x04: "IRQ", 0x05: "TIMER", 0x06: "CLOCK",
         0x07: "GPU", 0x08: "VRAM", 0x09: "SPU", 0x0A: "SPURAM", 0x0B: "CDROM", 0x0C: "DMA",
         0x0D: "SIO", 0x0E: "DIRTY", 0x0F: "MDEC", 0x10: "ICACHE", 0x11: "MODMEM",
         0x12: "GAME_START", 0x13: "GCS", 0x14: "CPUTIM", 0x15: "SPUCD", 0x16: "MOD_HOST",
         0x17: "CPU_EXEC", 0x18: "SCHED", 0x1A: "MEMCTRL", 0x1F: "IRQ_TIMING"}
HEADER = 36


def sections(path):
    d = open(path, "rb").read()
    out, pos = {}, HEADER
    while pos + 16 <= len(d):
        tag, flags, ln = struct.unpack_from("<IIQ", d, pos)
        pos += 16
        data = d[pos:pos + ln]
        pos += ln
        if flags & 1:
            data = zlib.decompress(data[4:])
        out[tag] = data
    return out


def ranges(a, b, gran=16):
    rs, start, last = [], None, None
    for i in range(0, min(len(a), len(b)), gran):
        if a[i:i + gran] != b[i:i + gran]:
            if start is None: start = i
            last = i
        elif start is not None and i - last > 256:
            rs.append((start, last + gran)); start = None
    if start is not None: rs.append((start, last + gran))
    return rs


def main():
    A, B = sections(sys.argv[1]), sections(sys.argv[2])
    for tag in sorted(set(A) | set(B)):
        name = NAMES.get(tag, hex(tag))
        a, b = A.get(tag), B.get(tag)
        if a == b: continue
        if a is None or b is None:
            print(f"{name}: only in {'b' if a is None else 'a'}"); continue
        if name == "RAM":
            rs = ranges(a, b)
            print(f"RAM: {len(rs)} differing ranges: " + ", ".join(f"0x{s:06X}-0x{e:06X}" for s, e in rs[:12]))
        elif name == "CPU":
            wa = struct.unpack_from(f"<{len(a)//4}I", a); wb = struct.unpack_from(f"<{len(b)//4}I", b)
            diffs = [(i, x, y) for i, (x, y) in enumerate(zip(wa, wb)) if x != y]
            print(f"CPU: words differing (index: a vs b): " + ", ".join(f"{i}:{x:08X}/{y:08X}" for i, x, y in diffs[:16]))
        else:
            n = sum(1 for x, y in zip(a, b) if x != y) + abs(len(a) - len(b))
            print(f"{name}: {n} bytes differ (len {len(a)} / {len(b)})")


if __name__ == "__main__":
    main()
