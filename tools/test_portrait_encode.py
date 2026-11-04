#!/usr/bin/env python3
"""portrait_encode.py: indices that already match the FE CLUT stay uncrushed.

    python3 -m unittest tools/test_portrait_encode.py
"""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import portrait_encode as pe


ROOT = pe.ROOT


def _clut():
    return pe.load_clut(pe.CLUT_FILE)


class MiamiModelOrder(unittest.TestCase):
    def test_mask_is_the_roster_order(self):
        # d1citymodels.h: 0x71E is MIAMI. Slots fill low bit to high bit.
        with open(os.path.join(ROOT, "d1citymodels.h"), encoding="utf-8") as f:
            header = f.read()
        self.assertIn("0x71E", header)
        self.assertIn("MIAMI", header)
        self.assertEqual(pe.miami_models(), (1, 2, 3, 4, 8, 9, 10))
        self.assertEqual(len(pe.MIAMI_PAGES), 7)
        # model 0 is not on the roster, and 5/6/7 are the D2 gaps
        self.assertNotIn(0, pe.miami_models())
        self.assertFalse(set(pe.miami_models()) & {5, 6, 7})


class PackAuthored(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.clut, cls.clut_raw = _clut()

    def test_clut_is_sixteen_rgb555_words(self):
        self.assertEqual(len(self.clut), 16)
        self.assertEqual(len(self.clut_raw), 32)
        # entry 0 is the black the background file actually stores (STP set)
        word0 = struct.unpack_from("<H", self.clut_raw, 0)[0]
        self.assertEqual(word0, 0x8000)
        self.assertEqual(self.clut[0], (0, 0, 0))

    def test_every_shipped_png_packs_without_moving_indices(self):
        pages = [os.path.join(pe.MIAMI_SRC, name) for name in pe.MIAMI_PAGES]
        pages.append(pe.FALLBACK_PNG)

        for path in pages:
            im = pe.open_image(path)
            pe.require_canvas(im, path)
            entries = pe.palette_entries(im)
            self.assertTrue(pe.palette_matches_clut(entries, self.clut), path)
            page, folded = pe.pack_indexed(im, self.clut)
            self.assertEqual(len(page), pe.PAGE_BYTES, path)
            got = pe.decode_indices(page)
            src = pe._pixels(im)

            for i, index in enumerate(src):
                shown = pe.trunc_rgb555(entries[index])
                self.assertEqual(self.clut[got[i]], shown, "%s texel %d" % (path, i))

                if index < 16:
                    self.assertEqual(got[i], index, path)

            # the pad under the 219th row is index 0, and it is not uploaded
            tail = got[pe.PAGE_W * pe.SHOWN_H:]
            self.assertTrue(all(v == 0 for v in tail))
            self.assertGreaterEqual(folded, 0)

    def test_a_stray_high_index_does_not_keep_its_low_nibble(self):
        # MCARS.RAW_0 uses index 83 for a few near-black texels. 83 & 0xF is 3,
        # an olive. Folding on the RGB555 match must land on black, not olive.
        path = os.path.join(pe.MIAMI_SRC, "MCARS.RAW_0.RAW.png")
        im = pe.open_image(path)
        entries = pe.palette_entries(im)
        src = pe._pixels(im)
        self.assertIn(83, src)
        page, _folded = pe.pack_indexed(im, self.clut)
        got = pe.decode_indices(page)

        for i, index in enumerate(src):
            if index == 83:
                self.assertEqual(got[i], 0)
                self.assertNotEqual(got[i], index & 0x0F)
                self.assertEqual(self.clut[got[i]], pe.trunc_rgb555(entries[index]))
                break
        else:
            self.fail("index 83 was not in the image")

    def test_installed_pages_match_a_fresh_pack(self):
        for page_name, model in zip(pe.MIAMI_PAGES, pe.miami_models()):
            im = pe.open_image(os.path.join(pe.MIAMI_SRC, page_name))
            fresh, _folded = pe.pack_indexed(im, self.clut)

            for dest in (
                os.path.join(pe.MIAMI_SRC, "%d.raw" % model),
                os.path.join(pe.MIAMI_INSTALL, "%d.raw" % model),
            ):
                with open(dest, "rb") as f:
                    self.assertEqual(f.read(), fresh, dest)

            clut_side = os.path.join(pe.MIAMI_INSTALL, "%d.clut" % model)
            self.assertFalse(os.path.exists(clut_side), clut_side)

        im = pe.open_image(pe.FALLBACK_PNG)
        fresh, _folded = pe.pack_indexed(im, self.clut)
        with open(pe.FALLBACK_RAW, "rb") as f:
            self.assertEqual(f.read(), fresh)

    def test_packing_matches_fourbpp_when_the_bake_tree_is_present(self):
        four = os.path.join(ROOT, "..", "bake", "tools", "common")

        if not os.path.isdir(four):
            self.skipTest("bake tools are not beside carhacks")

        sys.path.insert(0, four)
        import fourbpp

        im = pe.open_image(os.path.join(pe.MIAMI_SRC, pe.MIAMI_PAGES[0]))
        page, _folded = pe.pack_indexed(im, self.clut)
        indices = pe.decode_indices(page)
        self.assertEqual(fourbpp.encode_indices(indices), page)
        self.assertEqual(fourbpp.decode_indices(page), indices)


class RemapScreenshot(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.clut, _raw = _clut()

    def _image(self, rgb_rows):
        from PIL import Image

        im = Image.new("RGB", (pe.PAGE_W, pe.SHOWN_H))
        im.putdata(rgb_rows)
        return im

    def test_a_clut_colour_is_not_stippled(self):
        color = self.clut[10]
        rows = [color] * (pe.PAGE_W * pe.SHOWN_H)
        page, how = pe.encode_image(self._image(rows), self.clut, "remap", "bayer")
        self.assertIn("bayer", how)
        got = pe.decode_indices(page)
        shown = got[:pe.PAGE_W * pe.SHOWN_H]
        self.assertTrue(all(i == 10 or self.clut[i] == color for i in shown))

    def test_a_midpoint_dithers_and_nearest_does_not(self):
        a = self.clut[5]    # (8, 8, 8)
        b = self.clut[1]    # (248, 248, 248)
        mid = tuple((a[c] + b[c]) // 2 for c in range(3))
        rows = [mid] * (pe.PAGE_W * pe.SHOWN_H)
        dithered, _how = pe.encode_image(self._image(rows), self.clut, "remap", "bayer")
        flat, _how = pe.encode_image(self._image(rows), self.clut, "remap", "nearest")
        d_idx = set(pe.decode_indices(dithered)[:pe.PAGE_W * pe.SHOWN_H])
        f_idx = set(pe.decode_indices(flat)[:pe.PAGE_W * pe.SHOWN_H])
        self.assertGreater(len(d_idx), 1)
        self.assertEqual(len(f_idx), 1)

    def test_remap_refuses_the_wrong_size(self):
        from PIL import Image

        im = Image.new("RGB", (32, 32), self.clut[1])

        with self.assertRaises(SystemExit):
            pe.require_canvas(im, "tiny.png")

    def test_pack_refuses_a_foreign_palette(self):
        from PIL import Image

        im = Image.new("P", (pe.PAGE_W, pe.SHOWN_H))
        im.putpalette([255, 0, 0] * 16 + [0, 0, 0] * 240)

        with self.assertRaises(ValueError):
            pe.pack_indexed(im, self.clut)


if __name__ == "__main__":
    unittest.main()
