#!/usr/bin/env python3
"""Build an id1/pak0.pak that is enough to start the engine, and nothing more.

The game data is not in this repository and cannot be: id's shareware licence
permits redistributing the shareware release as a whole, by electronic means,
in a compressed format, with the agreement attached -- not a pak file lifted
out of it. So there is nothing here to smoke-test against, and "it compiles"
is not the same claim as "it runs".

This writes a pak holding the files the startup path insists on: the palette,
the colormap, a gfx.wad with every lump Draw_Init, SCR_Init and Sbar_Init ask
for by name, and the console background. The pictures are flat colour and the
font is a legible 8x8 bitmap, so a screenshot of the console shows whether the
palette, the 2D blitter, the character cell layout and the X11 path all agree.
Nothing here is game content and none of it is id's.

It cannot start a map: that needs progs.dat, the models and the BSPs.

    python3 tools/make-test-data.py <destdir>      # writes <destdir>/id1/pak0.pak
"""

import math
import os
import struct
import sys

# --------------------------------------------------------------------------
# Palette.
#
# Quake's own is 8 ramps of 16 plus a fullbright range. This is the same
# shape -- what matters for a smoke test is that index 0 is black, that there
# is a grey ramp for the console text to land in, and that the last 32 entries
# are bright, because that is what the renderer treats as fullbright.
# --------------------------------------------------------------------------
def make_palette():
    pal = bytearray()

    def ramp(n, r0, g0, b0, r1, g1, b1):
        for i in range(n):
            t = i / (n - 1)
            pal.extend((int(r0 + (r1 - r0) * t),
                        int(g0 + (g1 - g0) * t),
                        int(b0 + (b1 - b0) * t)))

    ramp(16, 0, 0, 0, 255, 255, 255)        # 0-15   grey
    ramp(16, 15, 11, 7, 235, 195, 155)      # 16-31  browns
    ramp(16, 7, 7, 15, 175, 175, 235)       # 32-47  blues
    ramp(16, 7, 11, 7, 155, 235, 155)       # 48-63  greens
    ramp(16, 27, 7, 7, 255, 107, 107)       # 64-79  reds
    ramp(16, 19, 19, 7, 255, 255, 111)      # 80-95  yellows
    ramp(16, 27, 7, 27, 235, 131, 235)      # 96-111 magentas
    ramp(16, 7, 27, 27, 131, 235, 235)      # 112-127 cyans
    ramp(16, 43, 43, 43, 211, 211, 211)     # 128-143 another grey
    ramp(16, 63, 39, 15, 255, 183, 71)      # 144-159 fire
    ramp(16, 15, 15, 63, 71, 71, 255)       # 160-175
    ramp(16, 63, 15, 15, 255, 71, 71)       # 176-191
    ramp(16, 15, 63, 15, 71, 255, 71)       # 192-207
    ramp(16, 63, 63, 15, 255, 255, 71)      # 208-223
    ramp(16, 31, 31, 31, 159, 159, 159)     # 224-239
    ramp(16, 255, 255, 255, 255, 63, 63)    # 240-255 fullbright
    assert len(pal) == 768, len(pal)
    return bytes(pal)


# --------------------------------------------------------------------------
# Colormap: 64 rows of 256, row 0 fully lit and row 63 black, which is how the
# renderer reads it. The engine also takes vid.fullbright from four bytes at
# offset 8192 -- row 32, columns 0 to 3 -- so those stay 0, as they are in the
# real file, and fullbright comes out at 256.
# --------------------------------------------------------------------------
def make_colormap(palette):
    """64 shade levels, row 0 fully lit and row 63 black.

    The palette above is sixteen ramps of sixteen, each running from dark to
    light, so dimming a colour is walking down its own ramp -- which is how id's
    own colormap is laid out and why the ramps are there at all. That makes each
    entry a subtraction rather than a nearest-colour search over 224 candidates,
    which in Python is the difference between a second and a minute.

    The engine also takes vid.fullbright from four bytes at offset 8192 -- row
    32, columns 0 to 3 -- so those stay 0, as they are in the real file.
    """
    rows = bytearray()
    for shade in range(64):
        light = 1.0 - shade / 63.0
        for c in range(256):
            if c >= 224:                 # the fullbright range ignores light
                rows.append(c)
                continue
            base = c & 0xf0
            step = c & 0x0f
            rows.append(base + int(step * light + 0.5))
    assert len(rows) == 256 * 64
    rows[8192:8196] = b"\0\0\0\0"
    return bytes(rows)


# --------------------------------------------------------------------------
# The console font: 256 cells of 8x8 in a 128x128 sheet, index 0 transparent.
#
# Quake draws text straight out of this with no glyph metrics, so getting the
# cell layout wrong shows up immediately as garbage -- which is the point of
# having it in a smoke test at all.
# --------------------------------------------------------------------------
GLYPHS = {
    " ": ("        ",) * 8,
    "!": ("   XX   ", "   XX   ", "   XX   ", "   XX   ", "   XX   ", "        ", "   XX   ", "        "),
    "'": ("   XX   ", "   XX   ", "   X    ", "        ", "        ", "        ", "        ", "        "),
    "(": ("     X  ", "    X   ", "   X    ", "   X    ", "   X    ", "    X   ", "     X  ", "        "),
    ")": ("  X     ", "   X    ", "    X   ", "    X   ", "    X   ", "   X    ", "  X     ", "        "),
    "*": ("        ", "  X X X ", "   XXX  ", "  XXXXX ", "   XXX  ", "  X X X ", "        ", "        "),
    "+": ("        ", "   XX   ", "   XX   ", " XXXXXX ", "   XX   ", "   XX   ", "        ", "        "),
    ",": ("        ", "        ", "        ", "        ", "        ", "   XX   ", "   XX   ", "  X     "),
    "-": ("        ", "        ", "        ", " XXXXXX ", "        ", "        ", "        ", "        "),
    ".": ("        ", "        ", "        ", "        ", "        ", "   XX   ", "   XX   ", "        "),
    "/": ("      X ", "     X  ", "    X   ", "   X    ", "  X     ", " X      ", "X       ", "        "),
    "0": ("  XXXX  ", " XX  XX ", " XX XXX ", " XXXXXX ", " XXX XX ", " XX  XX ", "  XXXX  ", "        "),
    "1": ("   XX   ", "  XXX   ", "   XX   ", "   XX   ", "   XX   ", "   XX   ", "  XXXX  ", "        "),
    "2": ("  XXXX  ", " XX  XX ", "     XX ", "    XX  ", "   XX   ", "  XX    ", " XXXXXX ", "        "),
    "3": ("  XXXX  ", " XX  XX ", "     XX ", "   XXX  ", "     XX ", " XX  XX ", "  XXXX  ", "        "),
    "4": ("    XXX ", "   XXXX ", "  XX XX ", " XX  XX ", " XXXXXX ", "     XX ", "     XX ", "        "),
    "5": (" XXXXXX ", " XX     ", " XXXXX  ", "     XX ", "     XX ", " XX  XX ", "  XXXX  ", "        "),
    "6": ("  XXXX  ", " XX  XX ", " XX     ", " XXXXX  ", " XX  XX ", " XX  XX ", "  XXXX  ", "        "),
    "7": (" XXXXXX ", "     XX ", "    XX  ", "   XX   ", "  XX    ", "  XX    ", "  XX    ", "        "),
    "8": ("  XXXX  ", " XX  XX ", " XX  XX ", "  XXXX  ", " XX  XX ", " XX  XX ", "  XXXX  ", "        "),
    "9": ("  XXXX  ", " XX  XX ", " XX  XX ", "  XXXXX ", "     XX ", " XX  XX ", "  XXXX  ", "        "),
    ":": ("        ", "   XX   ", "   XX   ", "        ", "   XX   ", "   XX   ", "        ", "        "),
    "=": ("        ", "        ", " XXXXXX ", "        ", " XXXXXX ", "        ", "        ", "        "),
    "?": ("  XXXX  ", " XX  XX ", "     XX ", "    XX  ", "   XX   ", "        ", "   XX   ", "        "),
    "[": ("   XXX  ", "   XX   ", "   XX   ", "   XX   ", "   XX   ", "   XX   ", "   XXX  ", "        "),
    "]": ("  XXX   ", "   XX   ", "   XX   ", "   XX   ", "   XX   ", "   XX   ", "  XXX   ", "        "),
    "_": ("        ", "        ", "        ", "        ", "        ", "        ", "        ", " XXXXXX "),
    "A": ("  XXXX  ", " XX  XX ", " XX  XX ", " XXXXXX ", " XX  XX ", " XX  XX ", " XX  XX ", "        "),
    "B": (" XXXXX  ", " XX  XX ", " XX  XX ", " XXXXX  ", " XX  XX ", " XX  XX ", " XXXXX  ", "        "),
    "C": ("  XXXX  ", " XX  XX ", " XX     ", " XX     ", " XX     ", " XX  XX ", "  XXXX  ", "        "),
    "D": (" XXXXX  ", " XX  XX ", " XX  XX ", " XX  XX ", " XX  XX ", " XX  XX ", " XXXXX  ", "        "),
    "E": (" XXXXXX ", " XX     ", " XX     ", " XXXXX  ", " XX     ", " XX     ", " XXXXXX ", "        "),
    "F": (" XXXXXX ", " XX     ", " XX     ", " XXXXX  ", " XX     ", " XX     ", " XX     ", "        "),
    "G": ("  XXXX  ", " XX  XX ", " XX     ", " XX XXX ", " XX  XX ", " XX  XX ", "  XXXX  ", "        "),
    "H": (" XX  XX ", " XX  XX ", " XX  XX ", " XXXXXX ", " XX  XX ", " XX  XX ", " XX  XX ", "        "),
    "I": ("  XXXX  ", "   XX   ", "   XX   ", "   XX   ", "   XX   ", "   XX   ", "  XXXX  ", "        "),
    "J": ("   XXXX ", "     XX ", "     XX ", "     XX ", " XX  XX ", " XX  XX ", "  XXXX  ", "        "),
    "K": (" XX  XX ", " XX XX  ", " XXXX   ", " XXX    ", " XXXX   ", " XX XX  ", " XX  XX ", "        "),
    "L": (" XX     ", " XX     ", " XX     ", " XX     ", " XX     ", " XX     ", " XXXXXX ", "        "),
    "M": (" XX  XX ", " XXXXXX ", " XXXXXX ", " XXXXXX ", " XX  XX ", " XX  XX ", " XX  XX ", "        "),
    "N": (" XX  XX ", " XXX XX ", " XXXXXX ", " XXXXXX ", " XX XXX ", " XX  XX ", " XX  XX ", "        "),
    "O": ("  XXXX  ", " XX  XX ", " XX  XX ", " XX  XX ", " XX  XX ", " XX  XX ", "  XXXX  ", "        "),
    "P": (" XXXXX  ", " XX  XX ", " XX  XX ", " XXXXX  ", " XX     ", " XX     ", " XX     ", "        "),
    "Q": ("  XXXX  ", " XX  XX ", " XX  XX ", " XX  XX ", " XX XXX ", " XX  XX ", "  XXXXX ", "        "),
    "R": (" XXXXX  ", " XX  XX ", " XX  XX ", " XXXXX  ", " XXXX   ", " XX XX  ", " XX  XX ", "        "),
    "S": ("  XXXX  ", " XX  XX ", " XX     ", "  XXXX  ", "     XX ", " XX  XX ", "  XXXX  ", "        "),
    "T": (" XXXXXX ", "   XX   ", "   XX   ", "   XX   ", "   XX   ", "   XX   ", "   XX   ", "        "),
    "U": (" XX  XX ", " XX  XX ", " XX  XX ", " XX  XX ", " XX  XX ", " XX  XX ", "  XXXX  ", "        "),
    "V": (" XX  XX ", " XX  XX ", " XX  XX ", " XX  XX ", " XX  XX ", "  XXXX  ", "   XX   ", "        "),
    "W": (" XX  XX ", " XX  XX ", " XX  XX ", " XXXXXX ", " XXXXXX ", " XXXXXX ", " XX  XX ", "        "),
    "X": (" XX  XX ", " XX  XX ", "  XXXX  ", "   XX   ", "  XXXX  ", " XX  XX ", " XX  XX ", "        "),
    "Y": (" XX  XX ", " XX  XX ", " XX  XX ", "  XXXX  ", "   XX   ", "   XX   ", "   XX   ", "        "),
    "Z": (" XXXXXX ", "     XX ", "    XX  ", "   XX   ", "  XX    ", " XX     ", " XXXXXX ", "        "),
}


def make_conchars():
    """128x128, 16x16 cells of 8x8. Index 0 is the transparent colour."""
    sheet = bytearray(128 * 128)

    def draw(cell, rows, colour):
        cx = (cell % 16) * 8
        cy = (cell // 16) * 8
        for y, row in enumerate(rows):
            for x, ch in enumerate(row):
                if ch != " ":
                    sheet[(cy + y) * 128 + cx + x] = colour

    for code in range(32, 127):
        ch = chr(code)
        rows = GLYPHS.get(ch) or GLYPHS.get(ch.upper())
        if rows is None:
            rows = ("        ", " XXXXXX ", " X    X ", " X    X ",
                    " X    X ", " X    X ", " XXXXXX ", "        ")
        draw(code, rows, 15)
        # The high half is the same font in gold, which is how Quake marks
        # highlighted text; the menus index straight into it.
        draw(code + 128, rows, 175)

    # The cursor Quake blinks at the console prompt lives at 11.
    draw(11, ("        ", " XXXXXX ", " XXXXXX ", " XXXXXX ",
              " XXXXXX ", " XXXXXX ", " XXXXXX ", "        "), 15)
    # 12 and 13 are the "]" prompt and the scrollbar dot the console uses.
    draw(13, GLYPHS["-"], 15)
    return bytes(sheet)


def make_wav(seconds=0.6, freq=440.0, rate=11025):
    """An 8-bit mono RIFF, which is the shape every sound in Quake's pak is.

    A tone rather than silence, so the smoke test can tell "the mixer ran" from
    "the mixer ran and produced nothing".
    """
    n = int(rate * seconds)
    samples = bytearray()
    for i in range(n):
        # A short attack and a long decay, so it sounds like something rather
        # than clicking at both ends.
        env = min(1.0, i / (rate * 0.01)) * (1.0 - i / n)
        v = math.sin(2 * math.pi * freq * i / rate) * env
        samples.append(max(0, min(255, int(128 + v * 100))))

    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate, 1, 8)
    body = (b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt +
            b"data" + struct.pack("<I", len(samples)) + bytes(samples))
    return b"RIFF" + struct.pack("<I", len(body)) + body


def qpic(width, height, fill):
    """A qpic_t: two little-endian ints then width*height palette indices."""
    return struct.pack("<ii", width, height) + bytes([fill]) * (width * height)


def checkerboard(width, height, a, b, cell=8):
    px = bytearray()
    for y in range(height):
        for x in range(width):
            px.append(a if ((x // cell) + (y // cell)) % 2 == 0 else b)
    return struct.pack("<ii", width, height) + bytes(px)


# --------------------------------------------------------------------------
# Which lumps gfx.wad has to contain.
#
# W_GetLumpName calls Sys_Error when a name is missing, and Sbar_Init asks for
# all of these unconditionally at startup -- including the Rogue and Hipnotic
# mission pack names, which the stock gfx.wad does not have either. It gets
# away with it because Sbar_Init only reaches those behind `if (rogue)`; the
# list below is what a plain run actually touches, plus the mission pack names
# so that -rogue and -hipnotic start too.
# --------------------------------------------------------------------------
def wad_lumps():
    lumps = {}

    lumps["conchars"] = (make_conchars(), ord("E"))     # raw, no qpic header
    lumps["disc"] = (qpic(24, 24, 15), ord("B"))
    lumps["backtile"] = (checkerboard(64, 64, 8, 10), ord("B"))

    for name, w, h, fill in (
        ("ram", 32, 32, 79), ("net", 32, 32, 47), ("turtle", 32, 32, 63),
        ("sbar", 320, 24, 4), ("ibar", 320, 24, 6), ("scorebar", 320, 24, 5),
        ("num_colon", 8, 24, 15), ("num_minus", 8, 24, 15),
        ("num_slash", 8, 24, 15), ("anum_minus", 8, 24, 15),
        ("face_invis", 24, 24, 35), ("face_invul2", 24, 24, 69),
        ("face_inv2", 24, 24, 51), ("face_quad", 24, 24, 99),
        ("sb_key1", 16, 16, 85), ("sb_key2", 16, 16, 87),
        ("sb_invis", 16, 16, 35), ("sb_invuln", 16, 16, 69),
        ("sb_suit", 16, 16, 55), ("sb_quad", 16, 16, 99),
        ("sb_shells", 24, 24, 81), ("sb_nails", 24, 24, 49),
        ("sb_rocket", 24, 24, 65), ("sb_cells", 24, 24, 113),
        ("sb_armor1", 24, 24, 53), ("sb_armor2", 24, 24, 83),
        ("sb_armor3", 24, 24, 67),
        ("sb_sigil1", 16, 16, 115), ("sb_sigil2", 16, 16, 116),
        ("sb_sigil3", 16, 16, 117), ("sb_sigil4", 16, 16, 118),
        # Rogue
        ("r_invbar1", 320, 24, 6), ("r_invbar2", 320, 24, 7),
        ("r_lava", 24, 24, 73), ("r_superlava", 24, 24, 74),
        ("r_gren", 24, 24, 50), ("r_multirock", 24, 24, 66),
        ("r_plasma", 24, 24, 114), ("r_shield1", 16, 16, 53),
        ("r_agrav1", 16, 16, 55), ("r_teambord", 32, 32, 45),
        ("r_ammolava", 24, 24, 73), ("r_ammomulti", 24, 24, 66),
        ("r_ammoplasma", 24, 24, 114),
        # Hipnotic
        ("sb_wsuit", 16, 16, 57), ("sb_eshld", 16, 16, 59),
    ):
        lumps[name] = (qpic(w, h, fill), ord("B"))

    for i in range(10):
        lumps["num_%i" % i] = (qpic(24, 24, 15), ord("B"))
        lumps["anum_%i" % i] = (qpic(24, 24, 175), ord("B"))
    for i in range(1, 6):
        lumps["face%i" % i] = (qpic(24, 24, 18 + i), ord("B"))
        lumps["face_p%i" % i] = (qpic(24, 24, 24 + i), ord("B"))

    weapons = ("shotgun", "sshotgun", "nailgun", "snailgun",
               "rlaunch", "srlaunch", "lightng")
    rogue_weapons = ("lava", "prox", "laser", "mjolnir", "gren_prox",
                     "prox_gren")
    for w in weapons + rogue_weapons:
        lumps["inv_%s" % w] = (qpic(48, 16, 8), ord("B"))
        lumps["inv2_%s" % w] = (qpic(48, 16, 14), ord("B"))
        for i in range(1, 6):
            lumps["inva%i_%s" % (i, w)] = (qpic(48, 16, 8 + i), ord("B"))

    return lumps


def build_wad(lumps):
    """WAD2: a 12 byte header, the lump data, then the 32 byte info table."""
    body = bytearray()
    table = bytearray()
    for name, (data, kind) in lumps.items():
        assert len(name) < 16, name
        pos = 12 + len(body)
        body.extend(data)
        while len(body) % 4:
            body.append(0)
        table.extend(struct.pack("<iiiBBBB16s", pos, len(data), len(data),
                                 kind, 0, 0, 0, name.encode()))
    header = struct.pack("<4sii", b"WAD2", len(lumps), 12 + len(body))
    return bytes(header) + bytes(body) + bytes(table)


# Quake's own CRC-16: CCITT polynomial, initialised to 0xffff, not reflected.
# common.c runs it over the pak directory and compares the raw running value
# against PAK0_CRC without the final xor, so this stops where that does.
CRC_TABLE = []
for _n in range(256):
    _c = _n << 8
    for _ in range(8):
        _c = ((_c << 1) ^ 0x1021) & 0xffff if _c & 0x8000 else (_c << 1) & 0xffff
    CRC_TABLE.append(_c)


def quake_crc_from(crc, data):
    for b in data:
        crc = ((crc << 8) & 0xffff) ^ CRC_TABLE[(crc >> 8) ^ b]
    return crc


def quake_crc(data):
    return quake_crc_from(0xffff, data)


# What common.c expects of an untouched shareware pak0.pak.
PAK0_COUNT = 339
PAK0_CRC = 32981


def build_pak(files):
    """PAK: "PACK", dirofs, dirlen, then 64 byte entries of name+pos+len.

    The directory is padded out to the shareware pak's file count and its CRC
    steered to the shareware pak's CRC, because otherwise the engine sets
    com_modified and COM_CheckRegistered refuses to start: "You must have the
    registered version to use modified games". That check is the engine
    noticing its data has been tampered with, and it is right -- this data has
    been. Satisfying it unlocks nothing: gfx/pop.lmp is still absent, so
    static_registered stays 0, the game stays shareware, and episodes 2 to 4
    remain as unreachable as they were. It only stops the smoke test dying
    before it draws a frame.
    """
    body = bytearray()
    entries = []
    for name, data in files.items():
        assert len(name) < 56, name
        entries.append((name, 12 + len(body), len(data)))
        body.extend(data)

    # Filler entries, zero length and pointing nowhere, to reach the count.
    # Two bytes of the last one's name are the free parameter the CRC search
    # turns; the rest carry a name that cannot collide with a real lookup.
    assert len(entries) < PAK0_COUNT, "too many files for a shareware-shaped pak"
    while len(entries) < PAK0_COUNT - 1:
        entries.append(("pad/%04d.pad" % len(entries), 0, 0))

    head = bytearray()
    for name, pos, length in entries:
        head.extend(struct.pack("<56sii", name.encode(), pos, length))

    # The CRC of everything but the last entry, taken once. Only that entry's
    # 64 bytes differ between candidates, so the search runs over those alone
    # rather than over the whole 21 KB directory 65536 times.
    head_crc = quake_crc(head)

    table = None
    for i in range(65536):
        tag = bytes((32 + (i & 63), 32 + ((i >> 6) & 63),
                     32 + ((i >> 12) & 63)))
        tail = struct.pack("<56sii", b"pad/crc%s.pad" % tag, 0, 0)
        if quake_crc_from(head_crc, tail) == PAK0_CRC:
            table = bytes(head) + tail
            break
    if table is None:
        raise SystemExit("no padding hit the shareware pak CRC")

    header = struct.pack("<4sii", b"PACK", 12 + len(body), len(table))
    return bytes(header) + bytes(body) + bytes(table)


def main():
    dest = sys.argv[1] if len(sys.argv) > 1 else "."
    out = os.path.join(dest, "id1")
    os.makedirs(out, exist_ok=True)

    palette = make_palette()
    files = {
        "gfx/palette.lmp": palette,
        "gfx/colormap.lmp": make_colormap(palette),
        "gfx.wad": build_wad(wad_lumps()),
        "gfx/conback.lmp": checkerboard(320, 200, 32, 33, cell=16),
        "gfx/loading.lmp": qpic(128, 24, 15),
        "gfx/pause.lmp": qpic(128, 24, 15),
        "gfx/qplaque.lmp": qpic(24, 96, 5),
        "gfx/ttl_main.lmp": qpic(240, 32, 15),
        "gfx/ttl_sgl.lmp": qpic(240, 32, 15),
        "gfx/ttl_cstm.lmp": qpic(240, 32, 15),
        "gfx/mainmenu.lmp": qpic(232, 112, 6),
        "gfx/sp_menu.lmp": qpic(232, 72, 6),
        "gfx/mp_menu.lmp": qpic(232, 72, 6),
        "gfx/p_option.lmp": qpic(144, 24, 15),
        "gfx/p_load.lmp": qpic(128, 24, 15),
        "gfx/p_save.lmp": qpic(128, 24, 15),
        "gfx/p_multi.lmp": qpic(128, 24, 15),
        "gfx/menuplyr.lmp": qpic(64, 64, 20),
        "gfx/bigbox.lmp": qpic(160, 144, 4),
        "gfx/complete.lmp": qpic(232, 32, 15),
        "gfx/inter.lmp": qpic(160, 144, 4),
        "gfx/finale.lmp": qpic(232, 32, 15),
        "gfx/ranking.lmp": qpic(232, 32, 15),
        # A tone the smoke test can listen for on the audio pipe, and the
        # line that plays it. Neither exists in the real game data.
        "sound/misc/menu1.wav": make_wav(0.6, 440.0),
        "sound/misc/menu2.wav": make_wav(0.3, 660.0),
        "quake.rc": (b"exec default.cfg\nexec config.cfg\nstuffcmds\n"
                     b"volume 1\nplay misc/menu1\n"),
        "default.cfg": (b"bind w +forward\nbind s +back\n"
                        b"bind a +moveleft\nbind d +moveright\n"
                        b"bind SPACE +jump\nbind CTRL +attack\n"
                        b"bind ` toggleconsole\nbind ~ toggleconsole\n"
                        b"bind ESCAPE togglemenu\n"),
    }
    for i in range(1, 7):
        files["gfx/menudot%i.lmp" % i] = qpic(20, 20, 240 + i)
    for i in range(1, 6):
        files["gfx/netmen%i.lmp" % i] = qpic(232, 72, 6)
    for n in ("box_tl", "box_tm", "box_tr", "box_ml", "box_mm", "box_mm2",
              "box_mr", "box_bl", "box_bm", "box_br"):
        files["gfx/%s.lmp" % n] = qpic(8, 8, 4)
    for n in ("dim_drct", "dim_ipx", "dim_modm", "dim_tcp"):
        files["gfx/%s.lmp" % n] = qpic(24, 24, 4)

    pak = os.path.join(out, "pak0.pak")
    with open(pak, "wb") as f:
        f.write(build_pak(files))
    print("wrote %s (%d files, %d bytes)" % (pak, len(files),
                                             os.path.getsize(pak)))


if __name__ == "__main__":
    main()
