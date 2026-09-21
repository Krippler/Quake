#!/usr/bin/env python3
"""Build a pak holding gfx/pop.lmp, from the pop[] table in common.c.

COM_CheckRegistered decides the game is the registered one by opening
gfx/pop.lmp and comparing it against a 128-entry table compiled into the
engine. Until it succeeds, COM_FindFile refuses any name with a '/' in it when
it reaches a directory -- so a test cannot put a map in maps/ and load it.

Everything needed is already in the source: the table is the check. This reads
it out and writes the file it describes, which unlocks the loose-file path for
the tests without any of id's data. It grants nothing else -- there is no game
content here, only the 256 bytes the check reads.

    python3 tools/make-pop-pak.py WinQuake/common.c out.pak
"""

import re
import struct
import sys


def read_pop(source):
    src = open(source).read()
    m = re.search(r"unsigned short pop\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        raise SystemExit("no pop[] table in %s" % source)

    vals = [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", m.group(1))]
    if len(vals) != 128:
        raise SystemExit("pop[] has %d entries, expected 128" % len(vals))

    # The engine compares against BigShort of what it read, so the file is
    # big-endian shorts.
    return b"".join(struct.pack(">H", v) for v in vals)


def build_pak(files):
    body = b""
    entries = []
    for name, data in files.items():
        entries.append((name, len(body), len(data)))
        body += data

    directory = b""
    for name, off, length in entries:
        directory += name.encode().ljust(56, b"\x00")
        directory += struct.pack("<ii", off + 12, length)

    return (b"PACK" + struct.pack("<ii", 12 + len(body), len(directory))
            + body + directory)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)

    pop = read_pop(sys.argv[1])
    open(sys.argv[2], "wb").write(build_pak({"gfx/pop.lmp": pop}))
    print("wrote %s (gfx/pop.lmp, %d bytes)" % (sys.argv[2], len(pop)))


if __name__ == "__main__":
    main()
