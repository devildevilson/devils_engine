"""Name what differs between two frontier_online state dumps (`--dump=FILE`).

usage: world_diff.py LINUX.bin WINDOWS.bin
Decodes the aesthetics world section (aesthetics::serial::dump_world): per component hash, the
entities whose bytes differ and at which offsets, with the raw bytes and the bytes read as
little-endian f32/u32. Prints one ::error:: annotation. Component hashes are
component_registry hashes; struct layout is the serialize() order of the component's fields.
"""
import struct
import sys

MAGIC = struct.pack("<I", 0xDE5A0001)


def parse(path):
    data = open(path, "rb").read()
    o = data.find(MAGIC)
    if o < 0:
        raise SystemExit(f"{path}: no world section")
    o += 8
    _, removed = struct.unpack_from("<QQ", data, o)
    o += 16 + 4 * removed
    (types,) = struct.unpack_from("<I", data, o)
    o += 4
    comps = {}
    for _ in range(types):
        h, length = struct.unpack_from("<II", data, o)
        o += 8
        body = data[o:o + length]
        o += length
        (count,) = struct.unpack_from("<I", body, 0)
        ents = {}
        if count:
            size = (length - 4) // count
            for i in range(count):
                rec = body[4 + i * size:4 + (i + 1) * size]
                ents[struct.unpack_from("<I", rec, 0)[0]] = rec[4:]
        comps[h] = ents
    return comps


a, b = parse(sys.argv[1]), parse(sys.argv[2])
lines = []
for h in sorted(set(a) | set(b)):
    ea, eb = a.get(h, {}), b.get(h, {})
    if ea == eb:
        continue
    size = len(next(iter(ea.values()))) if ea else 0
    lines.append(f"component {h:#010x} ({size} bytes per entity): {len(ea)} vs {len(eb)} entities")
    shown = 0
    for e in sorted(set(ea) | set(eb)):
        x, y = ea.get(e), eb.get(e)
        if x == y:
            continue
        shown += 1
        if shown > 8:
            lines.append("  ...")
            break
        if x is None or y is None:
            lines.append(f"  entity {e:#x}: only on {'windows' if x is None else 'linux'}")
            continue
        offs = [i for i in range(len(x)) if x[i] != y[i]]
        lines.append(f"  entity {e:#x}: bytes {offs[0]}..{offs[-1]}  linux {x.hex()}  windows {y.hex()}")
        for k in sorted({i - i % 4 for i in offs}):
            if k + 4 <= len(x):
                fa, fb = struct.unpack_from("<f", x, k)[0], struct.unpack_from("<f", y, k)[0]
                ua, ub = struct.unpack_from("<I", x, k)[0], struct.unpack_from("<I", y, k)[0]
                lines.append(f"    @{k}: f32 {fa!r} vs {fb!r}, u32 {ua:#x} vs {ub:#x}")
if not lines:
    lines = ["state dumps are identical (the divergence is outside the world section)"]
print("::error title=cross-platform world diff::" + "%0A".join(lines))
