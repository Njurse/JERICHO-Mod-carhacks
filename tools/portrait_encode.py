#!/usr/bin/env python3
"""portrait_encode.py — car-select portraits that match the frontend CLUT.

The car-select icon is a 256x256 4bpp page. The frontend uploads the top 219
rows and colours them with the 16-entry CLUT `LoadBackgroundFile` leaves at
VRAM (960, 256) from `DATA\\CARS\\CARBACK.RAW` (file offset 11 * 0x8000, the
first 32 bytes of that page). See docs/PORTRAITS.md.

Two ways in:

  * An authored mode-P PNG whose first 16 palette entries already ARE that
    CLUT (Jaret's MIAMI rips). The indices are packed as they stand. Colours
    are not re-quantised, so the page is not crushed when the stock CLUT
    draws it. No per-model `.clut` is written.
  * A screenshot or any other RGB picture. It is remapped onto those same 16
    colours (ordered dither between the two nearest entries) and packed the
    same way.

    python3 tools/portrait_encode.py install
    python3 tools/portrait_encode.py pack shot.png -o 1.raw
    python3 tools/portrait_encode.py encode shot.png -o 1.raw --dither bayer

`install` reads the source PNGs under assets/portraits/ and writes the pages
the game opens: D1CARS/<CITY>.portraits/<model>.raw, plus the shared
assets/portraits/CARHACKS/fallback.raw.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PAGE_W = 256
PAGE_H = 256
SHOWN_H = 219
PAGE_BYTES = PAGE_W * PAGE_H // 2          # 32768, two texels per byte
CLUT_ENTRIES = 16
CLUT_BYTES = CLUT_ENTRIES * 2

# LoadBackgroundFile (FEmain.c) reads page 11 of a background and uploads the
# first row of it to VRAM (960, 256). The sprite's CLUT is the first 16
# pixels of that row.
CARBACK_CLUT_OFFSET = 11 * 0x8000

# 4x4 Bayer, values 0..15. Low nibble of a 4bpp page is the left texel, the
# same order tools/common/fourbpp.py encode_indices uses. This file does not
# import fourbpp: the car bake's RGB555 rounding must stay untouched.
BAYER4 = (
    (0, 8, 2, 10),
    (12, 4, 14, 6),
    (3, 11, 1, 9),
    (15, 7, 13, 5),
)

# chkD1CityModels bit N is in-game model N. Retail MIAMI is 0x71E: models
# 1, 2, 3, 4, 8, 9, 10. chkFillCarTableFromImport writes those into frontend
# slots 0..6 in that order, which is also the order bake.destination_slots
# lays the source cars (D2_COMPLIANT_MODELS). MCARS.RAW_N is page N of that
# order. Page 0 is the cop; the bake pins Miami's cop to model 1.
MIAMI_MODEL_MASK = 0x71E
MIAMI_PAGES = (
    "MCARS.RAW_0.RAW.png",
    "MCARS.RAW_1.RAW.png",
    "MCARS.RAW_2.RAW.png",
    "MCARS.RAW_3.RAW.png",
    "MCARS.RAW_4.RAW.png",
    "MCARS.RAW_5.RAW.png",
    "MCARS.RAW_6.RAW.png",
)

ASSETS = os.path.join(ROOT, "assets", "portraits")
MIAMI_SRC = os.path.join(ASSETS, "MIAMI", "portraits")
CARHACKS_SRC = os.path.join(ASSETS, "CARHACKS")
CLUT_FILE = os.path.join(CARHACKS_SRC, "carback.clut")
FALLBACK_PNG = os.path.join(CARHACKS_SRC, "fallback.png")
FALLBACK_RAW = os.path.join(CARHACKS_SRC, "fallback.raw")
# Where JerFeShowPortrait looks: <dir of the LEV>/<stem>.portraits/<model>.raw
MIAMI_INSTALL = os.path.join(ROOT, "D1CARS", "MIAMI.portraits")

# The host opens this when a custom / Driver 1 city has no <model>.raw and
# the city has no stock CCARS slot. gD1DataFolder is JERICHO\MODS\carhacks\.
FALLBACK_HOST_REL = "assets\\portraits\\CARHACKS\\fallback.raw"


def models_from_mask(mask):
    """Set bits, low to high. The roster order for a generated D1 city."""
    return tuple(i for i in range(16) if mask & (1 << i))


def miami_models():
    return models_from_mask(MIAMI_MODEL_MASK)


def expand_rgb555(word):
    """One little-endian RGB555 word -> the 8-bit colour the engine shows."""
    return (
        ((word >> 0) & 0x1F) << 3,
        ((word >> 5) & 0x1F) << 3,
        ((word >> 10) & 0x1F) << 3,
    )


def trunc_rgb555(rgb):
    """Drop the low 3 bits. This is what a CLUT entry displays, not snap555."""
    return tuple((int(c) >> 3) << 3 for c in rgb[:3])


def load_clut(path):
    """16 expanded RGB triples and the raw 32 bytes.

    A 32-byte file is the CLUT itself. Anything larger is a CARBACK.RAW (or
    the same layout): the portrait CLUT is 32 bytes at CARBACK_CLUT_OFFSET.
    """
    with open(path, "rb") as f:
        blob = f.read()

    if len(blob) == CLUT_BYTES:
        raw = blob
    elif len(blob) >= CARBACK_CLUT_OFFSET + CLUT_BYTES:
        raw = blob[CARBACK_CLUT_OFFSET:CARBACK_CLUT_OFFSET + CLUT_BYTES]
    else:
        raise SystemExit(
            "%s is %d bytes; want a 32-byte CLUT or a CARBACK.RAW "
            "(portrait CLUT at offset 0x%X)" % (path, len(blob), CARBACK_CLUT_OFFSET))

    colors = []

    for i in range(CLUT_ENTRIES):
        word = struct.unpack_from("<H", raw, i * 2)[0]
        colors.append(expand_rgb555(word))

    return tuple(colors), raw


def default_clut_path():
    if os.path.isfile(CLUT_FILE):
        return CLUT_FILE

    raise SystemExit(
        "no portrait CLUT at %s — extract one with:\n"
        "  python3 tools/portrait_encode.py extract-clut "
        "--carback DRIVER2/DATA/CARS/CARBACK.RAW" % CLUT_FILE)


def _pixels(im):
    if hasattr(im, "get_flattened_data"):
        return list(im.get_flattened_data())

    return list(im.getdata())


def open_image(path):
    from PIL import Image

    im = Image.open(path)
    im.load()
    return im


def require_canvas(im, path):
    if im.size != (PAGE_W, SHOWN_H):
        raise SystemExit(
            "%s is %dx%d; a portrait is exactly %dx%d "
            "(the frontend uploads that rectangle)"
            % (path, im.size[0], im.size[1], PAGE_W, SHOWN_H))


def palette_entries(im):
    pal = im.getpalette()

    if not pal:
        return []

    return [tuple(pal[i:i + 3]) for i in range(0, len(pal) - 2, 3)]


def palette_matches_clut(entries, clut):
    """True when indices 0..15 already ARE the frontend CLUT, in order.

    Compared after dropping the low 3 bits. A PNG saver keeps those bits, so
    an entry authored as the CLUT colour is not always byte-identical, but it
    displays as that entry. Order matters: packing the index only works when
    slot N is CLUT N.
    """
    if len(entries) < CLUT_ENTRIES:
        return False

    return tuple(trunc_rgb555(c) for c in entries[:CLUT_ENTRIES]) == tuple(clut)


def encode_indices(indices):
    """One index byte per texel, 256x256 -> 32768-byte 4bpp page.

    Low nibble is the even x. Same layout as fourbpp.encode_indices.
    """
    if len(indices) != PAGE_W * PAGE_H:
        raise ValueError("index map is %d texels, expected %d"
                         % (len(indices), PAGE_W * PAGE_H))

    raw = bytearray(PAGE_BYTES)
    row_bytes = PAGE_W // 2

    for y in range(PAGE_H):
        for xb in range(row_bytes):
            lo = indices[y * PAGE_W + xb * 2] & 0x0F
            hi = indices[y * PAGE_W + xb * 2 + 1] & 0x0F
            raw[y * row_bytes + xb] = lo | (hi << 4)

    return bytes(raw)


def decode_indices(raw):
    if len(raw) != PAGE_BYTES:
        raise ValueError("page is %d bytes, expected %d" % (len(raw), PAGE_BYTES))

    idx = bytearray(PAGE_W * PAGE_H)
    row_bytes = PAGE_W // 2

    for y in range(PAGE_H):
        row = raw[y * row_bytes:(y + 1) * row_bytes]

        for xb, b in enumerate(row):
            idx[y * PAGE_W + xb * 2] = b & 0x0F
            idx[y * PAGE_W + xb * 2 + 1] = (b >> 4) & 0x0F

    return bytes(idx)


def _fold_index(index, entries, clut):
    """Keep an authored index. A stray slot above 15 folds only on an exact
    RGB555 match (a duplicate near-black the PNG saver invented). Anything
    else is not this CLUT and must go through the remap path."""
    if index < CLUT_ENTRIES:
        return index

    shown = trunc_rgb555(entries[index])

    for i, color in enumerate(clut):
        if color == shown:
            return i

    return None


def pack_indexed(im, clut):
    """Pack a mode-P image whose palette is the frontend CLUT. No remap."""
    if im.mode != "P":
        raise ValueError("pack wants a mode-P image, got %s" % im.mode)

    entries = palette_entries(im)

    if not palette_matches_clut(entries, clut):
        raise ValueError(
            "palette is not the CARBACK portrait CLUT in index order; "
            "use encode (remap) instead of pack")

    src = _pixels(im)
    folded = 0
    indices = []

    for index in src:
        mapped = _fold_index(index, entries, clut)

        if mapped is None:
            raise ValueError(
                "index %d is outside the portrait CLUT and is not the same "
                "RGB555 as any entry; use encode (remap)" % index)

        if mapped != index:
            folded += 1

        indices.append(mapped)

    return page_from_rows(indices), folded


def _dist2(a, b):
    return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2


def _nearest_two(rgb, clut):
    best = (10 ** 9, 0)
    second = (10 ** 9, 0)

    for i, color in enumerate(clut):
        d = _dist2(rgb, color)

        if d < best[0]:
            second = best
            best = (d, i)
        elif d < second[0]:
            second = (d, i)

    return best, second


def remap_indices(rgb_rows, clut, dither):
    """RGB texels (length SHOWN_H * PAGE_W) -> one index per texel.

    `bayer` dithers between the two nearest CLUT colours. A texel that
    already is a CLUT colour stays on that index, so flat areas are not
    stippled. `nearest` is the flat posterize, for when the stipple is
    unwanted. Identical CLUT entries (the portrait CLUT repeats black and
    one olive) never dither against each other.
    """
    if dither not in ("bayer", "nearest"):
        raise ValueError("dither must be bayer or nearest, not %r" % dither)

    out = []

    for n, rgb in enumerate(rgb_rows):
        (d0, i0), (d1, i1) = _nearest_two(rgb, clut)
        x = n % PAGE_W
        y = n // PAGE_W

        if (dither == "nearest" or d0 == 0 or clut[i0] == clut[i1]
                or d0 + d1 == 0):
            out.append(i0)
            continue

        # 0 when the texel is the first colour, ~0.5 when it sits halfway.
        t = d0 / float(d0 + d1)
        b = (BAYER4[y & 3][x & 3] + 0.5) / 16.0
        out.append(i1 if b < t else i0)

    return out


def image_rgb(im):
    """RGB triples, alpha composited over black (index 0 is the dark entry)."""
    if im.mode == "P":
        rgb = im.convert("RGB")
    elif im.mode == "RGBA":
        from PIL import Image

        base = Image.new("RGB", im.size, (0, 0, 0))
        base.paste(im, mask=im.getchannel("A"))
        rgb = base
    else:
        rgb = im.convert("RGB")

    return _pixels(rgb)


def page_from_rows(row_indices):
    """219 rows of indices -> a 256-row page, the tail filled with index 0."""
    if len(row_indices) != PAGE_W * SHOWN_H:
        raise ValueError("portrait is %d texels, expected %d"
                         % (len(row_indices), PAGE_W * SHOWN_H))

    full = list(row_indices) + [0] * (PAGE_W * (PAGE_H - SHOWN_H))
    return encode_indices(full)


def encode_image(im, clut, mode, dither):
    """Return (page bytes, how). `mode` is 'auto', 'pack', or 'remap'."""
    entries = palette_entries(im) if im.mode == "P" else []
    matched = im.mode == "P" and palette_matches_clut(entries, clut)

    if mode == "auto":
        mode = "pack" if matched else "remap"

    if mode == "pack":
        page, folded = pack_indexed(im, clut)
        how = "pack indices (folded %d stray exact-RGB555 texels)" % folded
        return page, how

    if mode != "remap":
        raise ValueError("mode must be auto, pack, or remap")

    indices = remap_indices(image_rgb(im), clut, dither)
    return page_from_rows(indices), "remap %s" % dither


def write_page(path, page):
    os.makedirs(os.path.dirname(path), exist_ok=True)

    with open(path, "wb") as f:
        f.write(page)


def extract_clut(carback, out_path):
    _colors, raw = load_clut(carback)
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)

    with open(out_path, "wb") as f:
        f.write(raw)

    return raw


def install_pages(clut, verbose=True):
    """Encode the shipped PNGs and copy Miami's pages beside the LEV."""
    models = miami_models()

    if len(models) != len(MIAMI_PAGES):
        raise SystemExit(
            "MIAMI mask 0x%X has %d models, but %d MCARS pages are listed"
            % (MIAMI_MODEL_MASK, len(models), len(MIAMI_PAGES)))

    written = []

    for page_name, model in zip(MIAMI_PAGES, models):
        src = os.path.join(MIAMI_SRC, page_name)
        im = open_image(src)
        require_canvas(im, src)
        page, how = encode_image(im, clut, "pack", "bayer")
        name = "%d.raw" % model

        for dest_dir in (MIAMI_SRC, MIAMI_INSTALL):
            dest = os.path.join(dest_dir, name)
            write_page(dest, page)
            written.append(dest)

        if verbose:
            print("MIAMI page %s -> model %d (%s)" % (page_name, model, how))

    im = open_image(FALLBACK_PNG)
    require_canvas(im, FALLBACK_PNG)
    page, how = encode_image(im, clut, "pack", "bayer")
    write_page(FALLBACK_RAW, page)
    written.append(FALLBACK_RAW)

    if verbose:
        print("fallback -> %s (%s)" % (FALLBACK_RAW, how))
        print("install -> %s" % MIAMI_INSTALL)

    return written


def _cmd_extract(args):
    raw = extract_clut(args.carback, args.out)
    print("wrote %s (%d bytes) from offset 0x%X" % (args.out, len(raw), CARBACK_CLUT_OFFSET))


def _cmd_encode(args, mode):
    clut, _raw = load_clut(args.carback or default_clut_path())
    im = open_image(args.png)
    require_canvas(im, args.png)
    page, how = encode_image(im, clut, mode, args.dither)
    write_page(args.out, page)
    print("%s -> %s (%d bytes, %s)" % (args.png, args.out, len(page), how))


def _cmd_install(args):
    clut_path = args.carback or default_clut_path()
    clut, raw = load_clut(clut_path)

    if args.write_clut:
        os.makedirs(os.path.dirname(CLUT_FILE), exist_ok=True)

        with open(CLUT_FILE, "wb") as f:
            f.write(raw)

        print("wrote %s" % CLUT_FILE)

    install_pages(clut)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    ex = sub.add_parser("extract-clut", help="pull the 16 portrait colours out of CARBACK.RAW")
    ex.add_argument("--carback", required=True, help="DATA/CARS/CARBACK.RAW")
    ex.add_argument("-o", "--out", default=CLUT_FILE)
    ex.set_defaults(func=_cmd_extract)

    enc = sub.add_parser("encode", help="screenshot or RGB art -> .raw on the FE CLUT")
    enc.add_argument("png")
    enc.add_argument("-o", "--out", required=True)
    enc.add_argument("--carback", help="CARBACK.RAW or a 32-byte .clut (default: the shipped clut)")
    enc.add_argument("--dither", choices=("bayer", "nearest"), default="bayer",
                     help="bayer stipples between the two nearest FE colours (default); "
                          "nearest is a flat posterize")
    enc.set_defaults(func=lambda a: _cmd_encode(a, "remap"))

    pk = sub.add_parser("pack", help="mode-P PNG already on the FE CLUT -> .raw, no remap")
    pk.add_argument("png")
    pk.add_argument("-o", "--out", required=True)
    pk.add_argument("--carback", help="CARBACK.RAW or a 32-byte .clut")
    pk.add_argument("--dither", default="bayer", help=argparse.SUPPRESS)
    pk.set_defaults(func=lambda a: _cmd_encode(a, "pack"))

    ins = sub.add_parser("install", help="encode the shipped MIAMI pages and the fallback")
    ins.add_argument("--carback", help="CARBACK.RAW, if carback.clut is not shipped yet")
    ins.add_argument("--write-clut", action="store_true",
                     help="also write assets/portraits/CARHACKS/carback.clut from --carback")
    ins.set_defaults(func=_cmd_install)

    args = p.parse_args(argv)
    args.func(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
