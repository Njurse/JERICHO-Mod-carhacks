# Car-select portraits

The icon beside the car-select menu is not in the `.LEV`. The host draws one
32768-byte 4bpp page per **model** (the in-game model, not the roster cursor).

It looks beside the `.LEV`, in this order:

1. `<LEV stem>.RAW` page at offset `model * 0x8000`, when that file exists and
   the page is not all zero. A car-bank bake writes this next to the LEV/LCF.
   Stock `CCARS.RAW` uses the roster cursor; this file uses the model, which
   is the number the menu already passes. The RAW is not a lump.
2. `<directory of the .LEV>/<LEV stem>.portraits/<model>.raw` — the loose page.
   The bake still writes this too. A hand-dropped folder still works.
3. Driver 2 cities 0–3: that model's stock `CCARS` / `HCARS` / `VCARS` /
   `RCARS` slot.
4. The shared CARHACKS fallback, below.

A `<model>.clut` in the portraits folder replaces the CARBACK CLUT for the
draw. The packed RAW does not contain CLUTs. An all-zero RAW page is empty,
so the loose page for that model still shows.

`JERICHO\MODS\carhacks\D1CARS\MIAMI.LEV` opens
`JERICHO\MODS\carhacks\D1CARS\MIAMI.portraits\1.raw` for model 1. There is no
`MIAMI.RAW` in this tree, and these pages do not need to move. The page is
256x256 at 4bpp (128 bytes a row). The frontend uploads the top 219 rows to
VRAM `(896,256)` and draws a 255x219 sprite. Colours come from the portrait
CLUT in `DATA\CARS\CARBACK.RAW`, at VRAM `(960,256)` after
`LoadBackgroundFile`. That CLUT is 16 little-endian RGB555 words, the first
32 bytes at file offset `11 * 0x8000`.

A `<model>.clut` (32 bytes) beside the page replaces that CLUT. Leave it out
when the indices were packed for the stock colours. Shipping one would paint
the page through a different palette and is how a matched portrait gets crushed.

## `handling.json` `portrait`

The bake reads `portrait` from `cars/<id>/handling.json`. The path is relative
to that file. It may be a pre-packed `.raw` of exactly `0x8000` bytes, or a
`.png`, `.tga`, or `.bmp`. A raster is converted into the page at bake. A
mode-P PNG whose first 16 palette entries are already the frontend CARBACK
CLUT, in index order, is packed as those indices and is not re-quantised.
The bake then writes that page at offset `model * 0x8000` in `<stem>.RAW`
and as the loose `<model>.raw`.

## Miami model numbers

`MCARS.RAW_N` is page N of Driver 1's Miami car-select raw, in source-car
order. It is **not** in-game model N.

`d1citymodels.h` gives MIAMI the mask `0x71E`. Bit N is model N, so the city
holds models 1, 2, 3, 4, 8, 9 and 10. `chkFillCarTableFromImport` writes
those into frontend slots 0..6 from low bit to high bit. The bake lays the
source cars onto the same numbers (`D2_COMPLIANT_MODELS`). Model 0 is not in
the mask. Models 5, 6 and 7 are the Driver 2 gaps and are never used.

Page 0 is the cop. The bake pins Miami's cop to destination model 1
(`COP_DEST_MODELS`), which is roster slot 0. That is the check on the mapping:

| `MCARS.RAW_N` | roster slot | in-game model | file |
| --- | --- | --- | --- |
| 0 (cop) | 0 | 1 | `1.raw` |
| 1 | 1 | 2 | `2.raw` |
| 2 | 2 | 3 | `3.raw` |
| 3 | 3 | 4 | `4.raw` |
| 4 | 4 | 8 | `8.raw` |
| 5 | 5 | 9 | `9.raw` |
| 6 | 6 | 10 | `10.raw` |

Those pages are installed at `D1CARS/MIAMI.portraits/<model>.raw`. The same
bytes sit next to the source PNGs under
`assets/portraits/MIAMI/portraits/`. `D1CARS/*.LEV` stays untracked; only the
`.portraits` folders are checked in.

Leave them there. A packed `MIAMI.RAW` would use the **in-game model** column
(`1.raw` at offset `0x8000`, `8.raw` at `8 * 0x8000`), not the roster slot.
The Driver 1 city bake does not write that file, so nothing here moves.

## Already on the frontend CLUT

Jaret's MIAMI PNGs, `fallback.png`, and `template.png` are mode-P, 256x219.
Their first 16 palette entries are the CARBACK portrait CLUT in index order
(a PNG saver keeps the low 3 bits; `(channel >> 3) << 3` is the colour the
CLUT displays). `pack` writes those indices straight into the 4bpp page and
pads rows 219..255 with index 0. It does not re-quantise. A `portrait`
PNG the bake is given in that same shape is packed the same way.

A handful of texels use a palette slot above 15 whose colour is the same
RGB555 black as CLUT entry 0. Those fold to entry 0. Keeping the low nibble
(83 -> 3) would turn them olive. Every other index is stored unchanged, so
the page drawn with the stock CLUT matches the PNG.

`template.png` is the empty plate to paint on. It is not installed.

```
python3 tools/portrait_encode.py pack assets/portraits/MIAMI/portraits/MCARS.RAW_0.RAW.png -o 1.raw
python3 tools/portrait_encode.py install
```

`install` re-encodes every shipped PNG and refreshes both copies. It refuses
a PNG whose palette is not the CLUT.

## A screenshot

Crop or scale the picture to exactly 256x219, then remap it onto the same 16
colours. The default is a 4x4 Bayer stipple between the two nearest CLUT
entries. A texel that already is one of those colours stays there, so a flat
area is not noisier than it has to be. `--dither nearest` is the flat
posterize; on a photo it blows highlights out and drops the grille and
lamps, which is the crushed look this path is avoiding. The result is still
the sepia frontend palette, not the screenshot's own colours.

```
python3 tools/portrait_encode.py encode shot.png -o 3.raw
python3 tools/portrait_encode.py encode shot.png -o 3.raw --dither nearest
```

Drop `3.raw` in that city's `*.portraits` folder (the model number, not the
roster slot). No `.clut` beside it.

The CLUT the tool uses is `assets/portraits/CARHACKS/carback.clut`, extracted
from a real `CARBACK.RAW`:

```
python3 tools/portrait_encode.py extract-clut --carback DRIVER2/DATA/CARS/CARBACK.RAW
```

This tool does not call the car bake's `fourbpp.py`. That module's RGB555
rounding stays as it is. Packing here is the same low-nibble-first layout
`fourbpp.encode_indices` writes; the unit test checks the bytes when the
bake tree is present.

## Fallback

One shared page covers a model with no art: a Driver 1 city other than the
Miami models above, a bank, or any model the stock ten-slot lookup does not
name.

```
JERICHO\MODS\carhacks\assets\portraits\CARHACKS\fallback.raw
```

The host builds that as `<gD1DataFolder>assets\portraits\CARHACKS\fallback.raw`
(`gD1DataFolder` is `JERICHO\MODS\carhacks\`). It is packed from
`fallback.png` the same way as the Miami pages, for the stock CLUT.

Driver 2 cities 0-3 are not covered by it. With no `<stem>.RAW` page and no
`<model>.raw` they keep the `CCARS` / `HCARS` / `VCARS` / `RCARS` slot. A
non-empty page in `<stem>.RAW` wins first. An override page in
`<stem>.portraits` still wins over the stock slot, on those cities too.

A missing fallback file leaves the icon blank and logs once:
`[frontend] portrait: <CITY> model <N> no art, fallback missing (<path>)`.
A hit logs `fallback <- <path>`.
