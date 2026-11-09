"""
Memory Card Tools: the card image commands of ps1transfer (-mcformat, -mclist with the icons, -mcexport, -mcimport,
-mcdelete, -mccopy; the Memory Card Tools page's save browser runs them), checked against a reference reading of the
memory card format written here: block 0 = header + 15 directory frames (state, size, next block, file name, XOR
checksum) + the broken sector list; the saves' blocks linked; the title frame ("SC", Shift-JIS title, icon CLUT) and
icon. Saves imported, exported (byte for byte), copied, deleted and their blocks reused (also scattered), a full card,
a name already there, DexDrive .gme images, damaged images (no crash, no hang, nothing written on a failure).

    python -m unittest tests/tools/test_memcard.py -v

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import random
import re
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from toolhost import TMP, ps1transfer, read, tmp_path, write    # noqa: E402

CARD, BLOCK, FRAME = 131072, 8192, 128
GME_HEADER = 3904


# ---------------------------------------------------------------------------------------------------- reference

def checksum_ok(frame):
    x = 0
    for b in frame[:127]:
        x ^= b
    return x == frame[127]


def directory(img):
    """[(block, state, size, next, name)] of blocks 1-15."""
    out = []
    for b in range(1, 16):
        f = img[b * FRAME:(b + 1) * FRAME]
        out.append((b, f[0], struct.unpack_from("<I", f, 4)[0], struct.unpack_from("<H", f, 8)[0],
                    f[10:30].split(b"\0")[0].decode("latin-1")))
    return out


def card_problems(img):
    """What's wrong with a card image by the format: header, checksums, saves' chains."""
    p = []
    if img[0:2] != b"MC" or not checksum_ok(img[0:FRAME]):
        p.append("header frame")
    for i in range(1, 36):
        if not checksum_ok(img[i * FRAME:(i + 1) * FRAME]):
            p.append("frame %d checksum" % i)
    d = {b: (s, size, nxt, name) for b, s, size, nxt, name in directory(img)}
    owner = {}
    for b, (s, size, nxt, name) in d.items():
        if s != 0x51:
            continue
        chain, cur = [], b
        while True:
            chain.append(cur)
            if cur in owner:
                p.append("block %d in two saves" % cur)
            owner[cur] = b
            nxt = d[cur][2]
            if nxt == 0xFFFF:
                break
            cur = nxt + 1
            if not 1 <= cur <= 15 or len(chain) > 15:
                p.append("save %d: link out of the card" % b)
                break
            if d[cur][0] not in (0x52, 0x53):
                p.append("save %d: block %d is not a middle/last block (%02X)" % (b, cur, d[cur][0]))
                break
        if d[chain[-1]][0] != 0x53 and len(chain) > 1:
            p.append("save %d: its last block isn't marked last" % b)
        if len(chain) == 1 and d[b][2] != 0xFFFF:
            p.append("save %d: one block, a link" % b)
        if size != len(chain) * BLOCK:
            p.append("save %d: size %d for %d blocks" % (b, size, len(chain)))
        for c in chain:
            if d[c][3] != name and c != b and d[c][3]:
                pass        # (the PS1 writes the name in the first block; the others may have it too)
    for b, (s, size, nxt, name) in d.items():
        if s in (0x52, 0x53) and b not in owner:
            p.append("block %d used (%02X) by no save" % (b, s))
    return p


def save_chain(img, first):
    d = {b: nxt for b, s, size, nxt, name in directory(img)}
    chain = [first]
    while d[chain[-1]] != 0xFFFF and len(chain) < 15:
        chain.append(d[chain[-1]] + 1)
    return chain


def save_data(img, first):
    return b"".join(img[b * BLOCK:(b + 1) * BLOCK] for b in save_chain(img, first))


def sjis_title(text):
    """A title in full-width Shift-JIS (letters, digits, space), as the games write them."""
    out = b""
    for c in text:
        if c.isdigit():
            out += struct.pack(">H", 0x824F + int(c))
        elif "A" <= c <= "Z":
            out += struct.pack(">H", 0x8260 + ord(c) - 65)
        elif "a" <= c <= "z":
            out += struct.pack(">H", 0x8281 + ord(c) - 97)
        else:
            out += b"\x81\x40"
    return out


def make_save(name, blocks, title="TEST SAVE", seed=0, clut=None):
    """An .mcs save: its directory frame + blocks (an SC title frame, an icon, random data)."""
    rng = random.Random(seed)
    data = bytearray(rng.randrange(256) for _ in range(blocks * BLOCK))
    data[0:4] = b"SC\x11" + bytes((blocks,))
    t = sjis_title(title)[:64]
    data[4:68] = t + bytes(64 - len(t))
    clut = clut or [0] + [((i * 2) & 31) | (((31 - i * 2) & 31) << 5) | ((i & 31) << 10) for i in range(1, 16)]
    data[0x60:0x80] = b"".join(struct.pack("<H", c) for c in clut)
    for j in range(128):                    # icon: a pattern of the 16 colours
        data[FRAME + j] = ((j * 3) & 15) | ((((j * 3) + 1) & 15) << 4)
    frame = bytearray(FRAME)
    frame[0] = 0x51
    struct.pack_into("<I", frame, 4, blocks * BLOCK)
    struct.pack_into("<H", frame, 8, 0xFFFF)
    frame[10:10 + len(name)] = name.encode()
    x = 0
    for b in frame[:127]:
        x ^= b
    frame[127] = x
    return bytes(frame) + bytes(data)


def listed(out):
    """-mclist output -> {name: (first block, blocks, title)}."""
    saves = {}
    for m in re.finditer(r"block\s+(\d+): (.{20}) (\d+) block\(s\)\s+\"([^\"]*)\"", out):
        saves[m.group(2).strip()] = (int(m.group(1)), int(m.group(3)), m.group(4))
    return saves


# ---------------------------------------------------------------------------------------------------- tests

class MemoryCard(unittest.TestCase):

    def new_card(self):
        path = tmp_path("card.mcd")
        code, out = ps1transfer("-mcformat", path)
        self.assertEqual(0, code, out)
        return path

    def imp(self, card, mcs_bytes, ok=True):
        mcs = write(tmp_path("save.mcs"), mcs_bytes)
        code, out = ps1transfer("-mcimport", card, mcs)
        self.assertEqual(0 if ok else 1, code, out)
        return out

    def test_format(self):
        img = read(self.new_card())
        self.assertEqual(CARD, len(img))
        self.assertEqual([], card_problems(img))
        self.assertTrue(all(s == 0xA0 and nxt == 0xFFFF for b, s, size, nxt, name in directory(img)))
        code, out = ps1transfer("-mclist", self.new_card())
        self.assertIn("0 save(s), 0/15 blocks used", out)

    def test_import_export_list(self):
        card = self.new_card()
        saves = [("BESLES-01234GAME1", 1, "FIRST GAME"), ("BASLUS-00001SAVE02", 3, "Second 2"),
                 ("BISLPS-12345ABCDEFGH", 5, "LONG NAME 20")]
        for i, (name, n, title) in enumerate(saves):
            self.imp(card, make_save(name, n, title, seed=i))
        img = read(card)
        self.assertEqual([], card_problems(img))
        icons = os.path.join(TMP, "icons")
        os.makedirs(icons, exist_ok=True)
        code, out = ps1transfer("-mclist", card, "-icons", icons)
        got = listed(out)
        self.assertIn("3 save(s), 9/15 blocks used", out)
        for i, (name, n, title) in enumerate(saves):
            self.assertIn(name, got)
            self.assertEqual((n, title.upper() if False else title), got[name][1:], name)
            out_mcs = tmp_path("out.mcs")
            code, o = ps1transfer("-mcexport", card, name, out_mcs)
            self.assertEqual(0, code, o)
            want, exported = make_save(name, n, title, seed=i), read(out_mcs)
            # (the directory frame as on the card: its link to the save's next block, which an import makes again)
            self.assertEqual(want[128:], exported[128:], "exported byte for byte: %s" % name)
            self.assertEqual((want[:8], want[10:127]), (exported[:8], exported[10:127]), "its directory frame")
            self.assertTrue(checksum_ok(exported[:128]))
            code, o = ps1transfer("-mcexport", card, str(got[name][0]), out_mcs)
            self.assertEqual(exported, read(out_mcs), "by its block number")
        # The icon: the save's first frame through its CLUT
        from PIL import Image
        name, n, title = saves[1]
        png = os.path.join(icons, "block%02d_%s.png" % (got[name][0], name))
        im = Image.open(png).convert("RGBA")
        data = make_save(name, n, title, seed=1)[128:]
        for j in (0, 1, 17, 255):
            p = (data[FRAME + j // 2] >> ((j & 1) * 4)) & 15
            c = struct.unpack_from("<H", data, 0x60 + p * 2)[0]
            want = ((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3, 255 if c else 0)
            self.assertEqual(want, im.getpixel((j % 16, j // 16)), "icon pixel %d" % j)

    def test_delete_reuse_scattered(self):
        """Delete in the middle, then a bigger save: its blocks scattered over the card, each save intact."""
        card = self.new_card()
        contents = {}
        for i, (name, n) in enumerate((("A", 3), ("B", 2), ("C", 4), ("D", 1), ("E", 2))):
            contents[name] = make_save(name, n, seed=10 + i)
            self.imp(card, contents[name])
        for name in ("B", "D"):
            code, out = ps1transfer("-mcdelete", card, name)
            self.assertEqual(0, code, out)
            del contents[name]
        self.assertTrue(os.path.isfile(card + ".bak"), "the image before kept")
        img = read(card)
        self.assertEqual([], card_problems(img))
        self.assertEqual({"A", "C", "E"}, set(listed(ps1transfer("-mclist", card)[1])))
        contents["F"] = make_save("F", 5, seed=20)       # 3 free + B's 2 deleted blocks... and D's
        self.imp(card, contents["F"])
        img = read(card)
        self.assertEqual([], card_problems(img))
        got = listed(ps1transfer("-mclist", card)[1])
        for name, mcs in contents.items():
            self.assertEqual(mcs[128:], save_data(img, got[name][0]), "save %s intact" % name)
        chain = save_chain(img, got["F"][0])
        self.assertNotEqual(list(range(chain[0], chain[0] + len(chain))), chain, "F's blocks scattered: %s" % chain)

    def test_full_card_and_duplicates(self):
        card = self.new_card()
        self.imp(card, make_save("BIG", 14, seed=1))
        before = read(card)
        self.imp(card, make_save("TWO", 2, seed=2), ok=False)
        self.assertEqual(before, read(card), "a failed import changes nothing")
        self.imp(card, make_save("BIG", 1, seed=3), ok=False)
        self.assertEqual(before, read(card), "a save of that name already there: nothing changed")
        self.imp(card, make_save("ONE", 1, seed=4))
        self.assertIn("2 save(s), 15/15 blocks used", ps1transfer("-mclist", card)[1])
        self.assertEqual([], card_problems(read(card)))
        self.imp(card, make_save("X", 1, seed=5), ok=False)

    def test_copy(self):
        a, b = self.new_card(), self.new_card()
        mcs = make_save("COPY-ME", 4, seed=7)
        self.imp(a, mcs)
        self.imp(b, make_save("OTHER", 2, seed=8))
        code, out = ps1transfer("-mccopy", a, "COPY-ME", b)
        self.assertEqual(0, code, out)
        img = read(b)
        self.assertEqual([], card_problems(img))
        got = listed(ps1transfer("-mclist", b)[1])
        self.assertEqual(mcs[128:], save_data(img, got["COPY-ME"][0]))
        code, out = ps1transfer("-mccopy", a, "COPY-ME", b)
        self.assertNotEqual(0, code, "copied twice: refused")

    def test_gme(self):
        """A DexDrive .gme image (3904-byte header): read, and still a .gme (its header kept) after a change."""
        card = read(self.new_card())
        head = b"123-456-STD" + bytes(GME_HEADER - 11)
        gme = write(tmp_path("card.gme"), head + card)
        self.imp(gme, make_save("GME", 2, seed=9))
        data = read(gme)
        self.assertEqual(CARD + GME_HEADER, len(data), "the .gme's header kept")
        self.assertEqual(head, data[:GME_HEADER])
        self.assertEqual([], card_problems(data[GME_HEADER:]))
        self.assertIn("GME", listed(ps1transfer("-mclist", gme)[1]))

    def test_bad_inputs(self):
        card = self.new_card()
        for what, data in (("empty", b""), ("frame only", make_save("X", 1)[:128]),
                           ("not a multiple of blocks", make_save("X", 1)[:5000]),
                           ("state not 51", b"\x00" + make_save("X", 1)[1:]),
                           ("16 blocks", make_save("X", 16))):
            with self.subTest(what):
                before = read(card)
                self.imp(card, data, ok=False)
                self.assertEqual(before, read(card))
        for what, data in (("small", bytes(1000)), ("big", bytes(CARD + 1))):
            p = write(tmp_path("bad.mcd"), data)
            code, out = ps1transfer("-mclist", p)
            self.assertIn("not a memory card image", out)
        code, out = ps1transfer("-mcexport", card, "NOPE", tmp_path("x.mcs"))
        self.assertNotEqual(0, code)
        code, out = ps1transfer("-mcdelete", card, "NOPE")
        self.assertNotEqual(0, code)

    def test_damaged_images(self):
        """Damaged directories (links in loops, out of range, states mixed): no crash or hang."""
        rng = random.Random(5)
        base = self.new_card()
        for i, n in enumerate((3, 4, 2, 5)):
            self.imp(base, make_save("S%d" % i, n, seed=i))
        img = read(base)
        failures = []
        for k in range(80):
            d = bytearray(img)
            for _ in range(rng.randint(1, 10)):
                b = rng.randint(0, 15)
                off = b * FRAME + rng.choice((0, 4, 8, 9, 10, 127, rng.randrange(128)))
                d[off] = rng.choice((0x51, 0x52, 0x53, 0xA0, 0xA1, 0xFF, 0x00, rng.randrange(256)))
            p = write(tmp_path("dmg.mcd"), d)
            for args in (("-mclist", p, "-icons", TMP), ("-mcexport", p, "1", tmp_path("d.mcs")),
                         ("-mcdelete", p, "2"), ("-mcimport", p, write(tmp_path("s.mcs"), make_save("NEW", 2))),
                         ("-mccopy", p, "3", self.new_card())):
                code, out = ps1transfer(*args, timeout=20)
                if code is None or code not in (0, 1):
                    failures.append("case %d %s: exit %s %s" % (k, args[0], code, out[-200:]))
        self.assertEqual([], failures[:5])


if __name__ == "__main__":
    unittest.main()
