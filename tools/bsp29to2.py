#!/usr/bin/env python3
"""Rewrite a BSP29 map as BSP2, so the BSP2 loader can be tested without
re-release data.

The two formats differ only in the width of the indices and bounds in six of
the fifteen lumps. Everything else -- planes, vertices, texinfo, surfedges,
lighting, visibility, entities, textures, submodels -- is byte-identical, which
is what makes this worth doing: convert a map the engine already renders
correctly, load the result through the BSP2 path, and the frame should come out
the same. Anything else is a bug in the loader.

    python3 tools/bsp29to2.py in.bsp out.bsp
"""

import struct
import sys

BSPVERSION = 29
BSP2 = b"BSP2"

HEADER_LUMPS = 15
(LUMP_ENTITIES, LUMP_PLANES, LUMP_TEXTURES, LUMP_VERTEXES, LUMP_VISIBILITY,
 LUMP_NODES, LUMP_TEXINFO, LUMP_FACES, LUMP_LIGHTING, LUMP_CLIPNODES,
 LUMP_LEAFS, LUMP_MARKSURFACES, LUMP_EDGES, LUMP_SURFEDGES,
 LUMP_MODELS) = range(HEADER_LUMPS)

# (lump, 29-format, 2-format, field converter). A converter takes the tuple
# unpacked from the BSP29 record and returns the tuple to pack as BSP2.
def node29to2(f):
    pl, c0, c1, n0, n1, n2, x0, x1, x2, ff, nf = f
    return (pl, c0, c1, float(n0), float(n1), float(n2),
            float(x0), float(x1), float(x2), ff, nf)

def leaf29to2(f):
    co, vo, n0, n1, n2, x0, x1, x2, fm, nm, amb = f
    return (co, vo, float(n0), float(n1), float(n2),
            float(x0), float(x1), float(x2), fm, nm, amb)

def face29to2(f):
    return f            # same fields, all widened to int

def clipnode29to2(f):
    return f

def edge29to2(f):
    return f

def marksurface29to2(f):
    return f

CONVERSIONS = [
    (LUMP_NODES,        "<i2h3h3h2H", "<i2i3f3f2I", node29to2),
    (LUMP_LEAFS,        "<ii3h3h2H4s", "<ii3f3f2I4s", leaf29to2),
    (LUMP_FACES,        "<hhihh4si",  "<iiiii4si",  face29to2),
    (LUMP_CLIPNODES,    "<i2h",       "<i2i",       clipnode29to2),
    (LUMP_EDGES,        "<2H",        "<2I",        edge29to2),
    (LUMP_MARKSURFACES, "<H",         "<I",         marksurface29to2),
]


def convert(data):
    version = struct.unpack_from("<i", data, 0)[0]
    if version != BSPVERSION:
        raise SystemExit("not a BSP29 map: version field is %d" % version)

    lumps = [struct.unpack_from("<ii", data, 4 + i * 8)
             for i in range(HEADER_LUMPS)]

    out = {}
    counts = {}
    for lump, (ofs, length) in enumerate(lumps):
        out[lump] = data[ofs:ofs + length]

    for lump, fmt29, fmt2, conv in CONVERSIONS:
        size29 = struct.calcsize(fmt29)
        size2 = struct.calcsize(fmt2)
        raw = out[lump]
        if len(raw) % size29:
            raise SystemExit("lump %d is %d bytes, not a multiple of %d"
                             % (lump, len(raw), size29))
        n = len(raw) // size29
        counts[lump] = n
        built = bytearray()
        for i in range(n):
            fields = struct.unpack_from(fmt29, raw, i * size29)
            built += struct.pack(fmt2, *conv(fields))
        out[lump] = bytes(built)
        assert len(out[lump]) == n * size2

    # Reassemble. Lumps are written in lump order, each 4-byte aligned, which
    # is what every compiler emits and what the loader's offsets expect.
    body = bytearray()
    header = bytearray(BSP2)
    offsets = []
    base = 4 + HEADER_LUMPS * 8
    for lump in range(HEADER_LUMPS):
        while len(body) % 4:
            body += b"\x00"
        offsets.append((base + len(body), len(out[lump])))
        body += out[lump]

    for ofs, length in offsets:
        header += struct.pack("<ii", ofs, length)

    return bytes(header) + bytes(body), counts


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)

    data = open(sys.argv[1], "rb").read()
    converted, counts = convert(data)
    open(sys.argv[2], "wb").write(converted)

    print("%s -> %s  (%d -> %d bytes)"
          % (sys.argv[1], sys.argv[2], len(data), len(converted)))
    for lump, name in ((LUMP_NODES, "nodes"), (LUMP_LEAFS, "leafs"),
                       (LUMP_FACES, "faces"), (LUMP_CLIPNODES, "clipnodes"),
                       (LUMP_EDGES, "edges"),
                       (LUMP_MARKSURFACES, "marksurfaces")):
        print("  %-13s %6d" % (name, counts[lump]))


if __name__ == "__main__":
    main()
