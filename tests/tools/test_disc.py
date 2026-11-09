"""
Asset Extractor, disc: the disc's file system as ps1transfer reads it (disclive.c ListDir / ReadFileSystem / List /
Get, compiled for the PC, the console's sectors read from a disc image made here from the ISO 9660 standard with the
CD-XA attributes): every file and directory listed with its sector, size and XA attributes (nested directories,
directories of several sectors, names without an extension, ";1" versions, many files), files copied (data: their
bytes; XA / STR: whole 2336-byte sectors), a file looked for with or without its "\\"; damaged images (a directory
looping on itself, records cut or too long, sizes too big): no crash, no hang.

    python -m unittest tests/tools/test_disc.py -v

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import random
import struct
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cbuild                                       # noqa: E402
from toolhost import ROOT, read, tmp_path, write    # noqa: E402

SECTOR = 2048
XA_FORM2, XA_INTERLEAVED = 0x1000, 0x2000


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def record(name, lba, size, is_dir, xa=None):
    """A directory record (name: bytes, b"\\0" "." and b"\\1" ".."), with the XA system use (attributes) if xa."""
    n = len(name)
    rec = bytes((0, 0)) + both32(lba) + both32(size) + bytes((99, 1, 1, 0, 0, 0, 0)) + bytes((2 if is_dir else 0, 0, 0))
    rec += both16(1) + bytes((n,)) + name
    if not n & 1:
        rec += b"\0"
    if xa is not None:
        rec += struct.pack(">HHH", 0, 0, xa) + b"XA" + bytes((1, 0, 0, 0, 0, 0))
    return bytes((len(rec),)) + rec[1:]


class Iso:
    """An ISO 9660 image built here: Iso().add("DIR\\\\FILE.EXT", data, xa) ..., then image(): (bytes, the expected
    listing {path: (lba, size, attr, is_dir)}, the files' data {path: bytes})."""

    def __init__(self):
        self.tree = {}          # dir path ("" root) -> [(name, kind, data, xa)]
        self.tree[""] = []

    def add(self, path, data, xa=0):
        parts = path.split("\\")
        cur = ""
        for p in parts[:-1]:
            sub = cur + "\\" + p
            if sub not in self.tree:
                self.tree[sub] = []
                self.tree[cur].append((p, "dir", None, 0))
            cur = sub
        self.tree[cur].append((parts[-1], "file", data, xa))

    def image(self, version=True, dir_pad=0):
        sectors = {}
        next_lba = [18]
        place = {}

        def alloc(n):
            lba = next_lba[0]
            next_lba[0] += max(1, n)
            return lba

        # Directory sizes: their records, sector by sector (a record doesn't cross a sector)
        def dir_records(path, lba_of):
            recs = [record(b"\0", lba_of(path)[0], lba_of(path)[1], True),
                    record(b"\1", lba_of(path.rsplit("\\", 1)[0] if path else "")[0], 0, True)]
            for name, kind, data, xa in sorted(self.tree[path]):
                full = path + "\\" + name
                fname = name.encode() + (b";1" if kind == "file" and version else b"")
                if kind == "file" and "." not in name and version:
                    fname = name.encode() + b".;1"
                lba, size = lba_of(full)
                recs.append(record(fname, lba, size, kind == "dir", xa if kind == "file" else None))
            for _ in range(dir_pad):
                recs.append(b"")
            out, cur = [], b""
            for r in recs:
                if len(cur) + len(r) > SECTOR:
                    out.append(cur)
                    cur = b""
                cur += r
            out.append(cur)
            return out

        sizes = {p: len(dir_records(p, lambda q: (0, 0))) for p in self.tree}
        for p in sorted(self.tree):
            place[p] = (alloc(sizes[p]), sizes[p] * SECTOR)
        listing, contents = {}, {}
        for p in sorted(self.tree):
            for name, kind, data, xa in self.tree[p]:
                full = p + "\\" + name
                if kind == "file":
                    n = (len(data) + SECTOR - 1) // SECTOR
                    place[full] = (alloc(n), len(data))
                    listing[full] = (place[full][0], len(data), xa, False)
                    contents[full] = data
                else:
                    listing[full] = place[full] + (0, True)
        for p in sorted(self.tree):
            for i, s in enumerate(dir_records(p, lambda q: place[q])):
                sectors[place[p][0] + i] = s
        for full, data in contents.items():
            for i in range(0, max(1, len(data)), SECTOR):
                sectors[place[full][0] + i // SECTOR] = data[i:i + SECTOR]
        pvd = bytearray(SECTOR)
        pvd[0:6] = b"\x01CD001"
        pvd[40:72] = b"TESTDISC".ljust(32)
        pvd[156:156 + 34] = record(b"\0", place[""][0], place[""][1], True)
        sectors[16] = bytes(pvd)
        sectors[17] = b"\xffCD001\x01"
        total = max(sectors) + 1
        img = bytearray(total * SECTOR)
        for lba, s in sectors.items():
            img[lba * SECTOR:lba * SECTOR + len(s)] = s
        return bytes(img), listing, contents


HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "discserver.h"
#define	XA_FORM2			0x1000
#define	XA_INTERLEAVED		0x2000
#define	XA_CDDA				0x4000
#define	MAX_FILES			8192
#define	MAX_PATH_LEN		256
@TYPES@
static uint32_t	last_retries;
static DiscFile	*files;
static int		n_files;
static uint8_t	*image;
static uint32_t	image_sectors;
static long		reads;
static uint32_t	Le32(const uint8_t *p)	{return (p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));}

// The console's reads, from the image (an XA sector: its subheader twice, the 2048 bytes, the rest zeros)
static bool	ReadSectors(uint32_t lba, uint32_t count, int mode, uint8_t *dst, uint32_t *size, uint32_t base, uint32_t total)
{
	uint32_t	ss = (mode == DISCSRV_DATA) ? 2048 : 2336, i;

	if (++reads > 100000)
	{
		printf("HANG too many reads\n");
		exit(3);
	}
	for (i = 0; i < count; i++, lba++, dst += ss)
	{
		memset(dst, 0, ss);
		if (lba >= image_sectors)
			return (false);
		if (mode == DISCSRV_DATA)
			memcpy(dst, image + lba * 2048, 2048);
		else
		{
			dst[0] = dst[4] = 1;
			memcpy(dst + 8, image + lba * 2048, 2048);
		}
	}
	if (size)
		*size = ss;
	return (true);
}
@LIST@

int	main(int argc, char **argv)
{
	FILE	*f = fopen(argv[1], "rb");
	long	n;
	int		i;

	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	image = malloc(n);
	fread(image, 1, n, f);
	fclose(f);
	image_sectors = n / 2048;
	List();
	for (i = 2; i + 1 < argc; i += 2)
		Get(argv[i], argv[i + 1], NULL);
	return (0);
}
"""


def harness():
    types = cbuild.block("ps1transfer/disclive.c", "typedef struct\n{\n\tuint32_t\tlba, size;", "} DiscFile;") + "} DiscFile;\n"
    code = "".join(cbuild.function("ps1transfer/disclive.c", f) for f in ("ListDir", "ReadFileSystem", "List",
                                                                          "EndsWith", "Get"))
    text = HARNESS.replace("@TYPES@", types).replace("@LIST@", code)
    return cbuild.build("disc_host", text, ["-I", os.path.join(ROOT, "ps1-tools", "discread")])


class Disc(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.exe = harness()

    def run_disc(self, img, gets=()):
        args = [self.exe, write(tmp_path("disc.iso"), img)]
        for path, out in gets:
            args += [path, out]
        r = subprocess.run(args, capture_output=True, text=True, timeout=60)
        listing, saved, errors = {}, [], []
        for line in r.stdout.splitlines():
            f = line.split(" ", 4)
            if f[0] == "DISCFILE":
                listing[f[4]] = (int(f[1]), int(f[2]), int(f[3], 16), False)
            elif f[0] == "DISCDIR":
                listing[line[8:]] = None
            elif f[0] == "DISCSAVED":
                saved.append(line)
            elif f[0] in ("DISCERR", "HANG"):
                errors.append(line)
        return r, listing, saved, errors

    def test_listing_and_files(self):
        rng = random.Random(1)
        iso = Iso()
        iso.add("SYSTEM.CNF", b"BOOT = cdrom:\\SLES_123.45;1\r\nTCB = 4\r\n")
        iso.add("SLES_123.45", bytes(rng.randrange(256) for _ in range(70000)))
        iso.add("DATA\\TEX\\LEVEL1.TIM", bytes(rng.randrange(256) for _ in range(5000)))
        iso.add("DATA\\README", b"no extension")
        iso.add("MOVIE\\INTRO.STR", bytes(rng.randrange(256) for _ in range(SECTOR * 30)), XA_FORM2 | XA_INTERLEAVED)
        iso.add("SOUND\\MUSIC.XA", bytes(rng.randrange(256) for _ in range(SECTOR * 12)), XA_FORM2 | XA_INTERLEAVED)
        for i in range(150):                    # a directory of several sectors
            iso.add("MANY\\FILE%03d.BIN" % i, bytes((i,)) * (i * 37 + 1))
        deep = "\\".join("D%d" % i for i in range(10))
        iso.add(deep + "\\DEEP.DAT", b"deep")
        iso.add("EMPTY.DAT", b"")
        img, want, contents = iso.image()
        gets = [("\\SLES_123.45", tmp_path("exe")), ("DATA\\TEX\\LEVEL1.TIM", tmp_path("tim")),
                ("\\MOVIE\\INTRO.STR", tmp_path("str")), ("\\many\\file149.bin", tmp_path("bin")),
                ("\\" + deep + "\\DEEP.DAT", tmp_path("deep")), ("\\NOPE.DAT", tmp_path("no")),
                ("\\DATA\\README", tmp_path("readme"))]
        r, got, saved, errors = self.run_disc(img, gets)
        files = {k: v for k, v in got.items() if v}
        self.assertEqual({k: v for k, v in want.items() if not v[3]}, files)
        self.assertEqual({k for k, v in want.items() if v[3]}, {k for k, v in got.items() if v is None})
        self.assertEqual(contents["\\SLES_123.45"], read(gets[0][1]))
        self.assertEqual(contents["\\DATA\\TEX\\LEVEL1.TIM"], read(gets[1][1]))
        xa = read(gets[2][1])
        self.assertEqual(30 * 2336, len(xa), "an STR: whole 2336-byte sectors")
        self.assertEqual(contents["\\MOVIE\\INTRO.STR"][:SECTOR], xa[8:8 + SECTOR])
        self.assertEqual(contents["\\MANY\\FILE149.BIN"], read(gets[3][1]), "found whatever its case")
        self.assertEqual(b"deep", read(gets[4][1]))
        self.assertEqual(b"no extension", read(gets[6][1]))
        self.assertTrue(any("NOPE" in e for e in errors), errors)

    def test_damaged(self):
        rng = random.Random(2)
        iso = Iso()
        for i in range(40):
            iso.add("D%d\\F%d.BIN" % (i % 5, i), bytes((i,)) * 3000)
        img, want, _ = iso.image()
        problems = []
        cases = []
        # A directory pointing at the root (a loop)
        d = bytearray(img)
        root = struct.unpack_from("<I", d, 16 * SECTOR + 156 + 2)[0]
        rec = d.find(b"D0", root * SECTOR) - 33
        struct.pack_into("<I", d, rec + 2, root)
        cases.append(("loop", bytes(d)))
        for k in range(150):
            d = bytearray(img)
            for _ in range(rng.randint(1, 12)):
                o = rng.randrange(16 * SECTOR, min(len(d), 40 * SECTOR))
                d[o] = rng.choice((0, 1, 2, 33, 34, 255, rng.randrange(256)))
            cases.append(("bytes %d" % k, bytes(d)))
        cases.append(("cut", img[:20 * SECTOR + 100]))
        cases.append(("not iso", bytes(40 * SECTOR)))
        for what, data in cases:
            try:
                r, got, saved, errors = self.run_disc(data, [("\\D1\\F1.BIN", tmp_path("o"))])
                if r.returncode or any(e.startswith("HANG") for e in errors):
                    problems.append("%s: exit %s %s" % (what, r.returncode, errors[-1:]))
            except subprocess.TimeoutExpired:
                problems.append("%s: timeout" % what)
        self.assertEqual([], problems[:6])


if __name__ == "__main__":
    unittest.main()
