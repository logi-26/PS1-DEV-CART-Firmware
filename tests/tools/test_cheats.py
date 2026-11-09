"""
Cheats: the cartridge's code engine (kernel_hook.c ApplyCheats, applied at every interrupt) and the firmware's cheat
screen (cheatmenu.h: the cheats in the flash, the ones chosen put in the engine's table), compiled for the PC from
their source, against a reference engine written here from the codes' description (usb.h); the Manager's side: the
table written (codeengine.py), the cheat database's converters (POPStarter, Xploder, GameShark CD records) and the
cheats put in the flash (cheatdb.rom_cheats, read back by the firmware's own code).

    python -m unittest tests/tools/test_cheats.py -v

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import random
import sqlite3
import struct
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cbuild                                                   # noqa: E402
from toolhost import tmp_path, write, read                      # noqa: E402

import cheatdb                                                  # noqa: E402
import codeengine                                               # noqa: E402

RAM = 0x200000
MAGIC, MAX = 0x53544843, 216

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define	CHEAT_TABLE_MAGIC	0x53544843
#define	CHEAT_MAX			216
typedef struct {uint32_t code; uint16_t value; uint16_t reserved;} CheatCode;
typedef struct {uint32_t magic; uint32_t count; CheatCode codes[CHEAT_MAX];} CheatTable;

static CheatTable	host_table;
static uint8_t		host_ram[0x200000 + 0x20000] __attribute__((aligned(16)));
static uint8_t		host_db[0x5000 + 0x1000];

/* kernel_hook.c */
@APPLY@

/* cheatmenu.h: the cheats of a game and the table filled with the ones on */
@ROMLE@
#define	CHEAT_DB_SIZE		0x5000
#define	CHEAT_DB_MAGIC		0x42444843
#define	CHEAT_DB_SERIAL		12
#define	CHEAT_DB_TITLE		32
#define	CHEATS_SHOWN		64
static const volatile uint8_t	*CheatDb(void) {return (host_db);}
@FIND@
static void	ChosenToTable(const volatile uint8_t **cheats, int n, const uint8_t *on)
{
	int	i;
@TABLE@
}

static void	*Load(const char *path, void *buf, size_t max)
{
	FILE	*f = fopen(path, "rb");
	size_t	n;

	if (!f)
		exit(2);
	n = fread(buf, 1, max, f);
	fclose(f);
	return (buf);
}

int	main(int argc, char **argv)
{
	if (!strcmp(argv[1], "apply"))		// apply TABLE RAM_IN RAM_OUT TIMES
	{
		int	k, times = atoi(argv[5]);

		Load(argv[2], &host_table, sizeof(host_table));
		Load(argv[3], host_ram, 0x200000);
		for (k = 0; k < times; k++)
			ApplyCheats();
		{
			FILE	*f = fopen(argv[4], "wb");

			fwrite(host_ram, 1, sizeof(host_ram), f);
			fclose(f);
		}
		return (0);
	}
	if (!strcmp(argv[1], "menu"))		// menu DB SERIAL ON (hex mask of the cheats chosen, cheat i: bit i)
	{
		static const volatile uint8_t	*cheats[CHEATS_SHOWN];
		static uint8_t					on[CHEATS_SHOWN];
		char							title[CHEAT_DB_TITLE + 8];
		unsigned long long				mask = strtoull(argv[4], NULL, 16);
		int								n, i, k;

		memset(host_db, 0xEE, sizeof(host_db));
		Load(argv[2], host_db, 0x5000);
		n = CheatFind(argv[3], cheats, title);
		printf("FOUND %d %s\n", n, n ? title : "");
		for (i = 0; i < n; i++)
		{
			const volatile uint8_t	*c = cheats[i];

			on[i] = (mask >> i) & 1;
			printf("CHEAT %.*s|", c[0], (const char *)c + 1);
			c += 1 + c[0];
			for (k = 0; k < c[0]; k++)
				printf("%08X %04X;", RomLe(c + 1 + k * 6, 4), RomLe(c + 5 + k * 6, 2));
			printf("\n");
		}
		memset(&host_table, 0, sizeof(host_table));
		if (n)
			ChosenToTable(cheats, n, on);
		printf("TABLE %08X %u\n", host_table.magic, host_table.count);
		for (i = 0; i < (int)host_table.count; i++)
			printf("CODE %08X %04X\n", host_table.codes[i].code, host_table.codes[i].value);
		return (0);
	}
	return (1);
}
"""


def harness():
    apply = cbuild.function("kernel_hook/kernel_hook.c", "ApplyCheats")
    for a, b in (("(volatile CheatTable *)CHEAT_TABLE_ADRS", "(volatile CheatTable *)&host_table"),
                 ("(volatile uint8_t *)(0x80000000 | (code & 0x1FFFFF))", "(volatile uint8_t *)(host_ram + (code & 0x1FFFFF))"),
                 ("base = 0x80000000 | (next & 0x1FFFFF)", "base = (uint32_t)host_ram + (next & 0x1FFFFF)")):
        if a not in apply:
            raise RuntimeError("ApplyCheats changed: %s (update the test)" % a)
        apply = apply.replace(a, b)
    table = cbuild.block("cheatmenu.h", "\t\tvolatile CheatTable\t*t = (volatile CheatTable *)CHEAT_TABLE_ADRS;",
                         "\tFillRect(0, UI_HELP_Y")
    table = table.replace("(volatile CheatTable *)CHEAT_TABLE_ADRS", "(volatile CheatTable *)&host_table")
    table = "\t{\n" + table.rstrip()
    code = HARNESS.replace("@APPLY@", apply).replace("@ROMLE@", cbuild.function("cheatmenu.h", "RomLe"))
    code = code.replace("@FIND@", cbuild.function("cheatmenu.h", "CheatFind")).replace("@TABLE@", table)
    return cbuild.build("cheats_host", code)


# ---------------------------------------------------------------------------------------------------- reference

def reference(codes, ram, times=1):
    """The codes applied to the RAM (bytearray), as described: 80 16-bit write, 30 8-bit write, D0-D3 / E0-E3 the
    next code only if the 16 / 8-bit value is equal, not equal, less, greater; 50 serial: the next code's write
    repeated (count, address step, value increment); C0 the codes after it only if equal. Other types, and 16-bit
    codes on odd addresses, skipped."""
    for _ in range(times):
        i = 0
        n = min(len(codes), MAX)
        while i < n:
            code, v = codes[i]
            t, a = code >> 24, code & 0x1FFFFF
            wide = t == 0x80 or t >> 4 == 0xD or t == 0xC0
            if wide and a & 1:
                i += 1
                continue
            if t == 0x80:
                ram[a:a + 2] = struct.pack("<H", v)
            elif t == 0x30:
                ram[a] = v & 0xFF
            elif t >> 4 in (0xD, 0xE) and not t & 0x0C:
                cur = struct.unpack_from("<H", ram, a)[0] if t >> 4 == 0xD else ram[a]
                w = v if t >> 4 == 0xD else v & 0xFF
                ok = (cur == w, cur != w, cur < w, cur > w)[t & 3]
                if not ok:
                    i += 1
            elif t == 0x50 and i + 1 < n:
                count, step = (code >> 8) & 0xFF, code & 0xFF
                nc, w = codes[i + 1]
                nt, base = nc >> 24, nc & 0x1FFFFF
                i += 1
                for k in range(count):
                    adr = (base + k * step)
                    if adr + (2 if nt == 0x80 else 1) > len(ram):
                        break
                    if nt == 0x30:
                        ram[adr] = w & 0xFF
                    elif nt == 0x80 and not adr & 1:
                        ram[adr:adr + 2] = struct.pack("<H", w & 0xFFFF)
                    w = (w + v) & 0xFFFFFFFF
            elif t == 0xC0 and struct.unpack_from("<H", ram, a)[0] != v:
                break
            i += 1
    return ram


def table_bytes(codes, magic=MAGIC):
    return (struct.pack("<II", magic, len(codes)) +
            b"".join(struct.pack("<IHH", c, v, 0) for c, v in codes)).ljust(8 + MAX * 8, b"\0")


class Engine(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.exe = harness()

    def console(self, codes, ram, times=1, magic=MAGIC):
        t = write(tmp_path("table.bin"), table_bytes(codes, magic))
        rin, rout = write(tmp_path("ram.bin"), bytes(ram)), tmp_path("ramout.bin")
        r = subprocess.run([self.exe, "apply", t, rin, rout, str(times)], capture_output=True, timeout=30)
        self.assertEqual(0, r.returncode, r.stderr)
        out = read(rout)
        self.assertFalse(any(out[RAM:]), "written past the 2 MB of RAM")
        return bytearray(out[:RAM])

    def check(self, codes, ram=None, times=1):
        ram = ram if ram is not None else bytearray(RAM)
        want = reference(codes, bytearray(ram), times)
        got = self.console(codes, ram, times)
        if want != got:
            diff = [i for i in range(RAM) if want[i] != got[i]][:8]
            self.fail("codes %s: RAM differs at %s (want %s, got %s)" % (
                ["%08X %04X" % c for c in codes], ["%06X" % d for d in diff], [want[d] for d in diff],
                [got[d] for d in diff]))
        return got

    def test_writes(self):
        got = self.check([(0x80001000, 0x1234), (0x30001003, 0xAB), (0x801FFFFE, 0xBEEF), (0x30000000, 0x1FF)])
        self.assertEqual(b"\x34\x12\x00\xAB", got[0x1000:0x1004])
        self.check([(0x80001001, 0x1234)])                      # (odd: skipped)

    def test_conditions(self):
        ram = bytearray(RAM)
        ram[0x2000:0x2002] = struct.pack("<H", 500)
        ram[0x2003] = 7
        for t, v in ((0xD0, 500), (0xD0, 499), (0xD1, 500), (0xD1, 1), (0xD2, 501), (0xD2, 500), (0xD3, 499),
                     (0xD3, 500), (0xE0, 7), (0xE0, 0x107), (0xE1, 7), (0xE2, 8), (0xE3, 6), (0xE3, 7), (0xD4, 0),
                     (0xE5, 0)):
            with self.subTest(type="%02X" % t, value=v):
                a = 0x2003 if t >> 4 == 0xE else 0x2000
                self.check([((t << 24) | a, v), (0x80003000, 0x5555), (0x80003002, 0x6666)], ram)

    def test_serial_and_master(self):
        self.check([(0x50000A04, 0x0002), (0x80004000, 0x0100)])           # 10 writes, every 4 bytes, +2
        self.check([(0x50000301, 0x0001), (0x30004101, 0x00FE)])           # 8-bit, wraps
        self.check([(0x50000403, 0x0001), (0x80004201, 0x0000)])           # 16-bit on odd addresses: skipped
        self.check([(0x50000204, 0x0001)])                                 # no code after it
        ram = bytearray(RAM)
        self.check([(0xC0005000, 0x0001), (0x80005002, 0x1111)], ram)      # master: not equal, nothing after
        ram[0x5000] = 1
        self.check([(0xC0005000, 0x0001), (0x80005002, 0x1111)], ram)

    def test_serial_past_the_ram(self):
        """A serial code running past the RAM's end: the console's engine writes on (on the PS1: the RAM's mirror,
        the kernel at its start). The Manager never sends one (codeengine.safe_codes), nor puts one in the flash."""
        codes = [(0x5000FFFF, 0x0001), (0x801FFF00, 0x0000)]
        t = write(tmp_path("t.bin"), table_bytes(codes))
        rin, rout = write(tmp_path("r.bin"), bytes(RAM)), tmp_path("o.bin")
        subprocess.run([self.exe, "apply", t, rin, rout, "1"], timeout=30)
        self.assertTrue(any(read(rout)[RAM:]), "(the engine itself: past the RAM)")
        self.assertEqual([], codeengine.safe_codes(codes))
        ok = [(0x50000402, 0x0001), (0x801FFFF8, 0x0000)]        # its last write at 0x1FFFFE: in the RAM
        self.assertEqual(ok, codeengine.safe_codes(ok))
        self.check(ok)
        self.assertEqual([(0x80000000, 1)], codeengine.safe_codes([(0x50000402, 1), (0x801FFFFA, 0), (0x80000000, 1)]))

    def test_off_and_limits(self):
        ram = bytearray(RAM)
        self.assertEqual(ram, self.console([(0x80001000, 0x1234)], ram, magic=0))
        codes = [(0x80000000 + i * 2, i) for i in range(MAX + 20)]
        t = table_bytes(codes[:MAX])
        t = t[:4] + struct.pack("<I", 10000) + t[8:]                      # a count too big
        rin, rout = write(tmp_path("r.bin"), bytes(RAM)), tmp_path("o.bin")
        subprocess.run([self.exe, "apply", write(tmp_path("t.bin"), t), rin, rout, "1"], timeout=30)
        self.assertEqual(bytearray(reference(codes[:MAX], bytearray(RAM))), bytearray(read(rout)[:RAM]))

    def test_random_programs(self):
        """2000 random code lists (every type, conditions on values that are and aren't there), against the reference;
        applied 3 times (as at each interrupt: a code's effect on the next ones' tests)."""
        rng = random.Random(11)
        types = [0x80, 0x30, 0xD0, 0xD1, 0xD2, 0xD3, 0xE0, 0xE1, 0xE2, 0xE3, 0x50, 0xC0, 0x10, 0x20, 0xC1, 0xD7, 0x00]
        base = bytearray(rng.randrange(256) for _ in range(256))
        for k in range(200):
            codes = []
            for _ in range(rng.randint(1, 30)):
                t = rng.choice(types)
                a = 0x10000 + rng.randrange(256)
                if t == 0x50:
                    codes.append(((t << 24) | (rng.randrange(1, 20) << 8) | rng.randrange(0, 9), rng.randrange(4)))
                    a = 0x10000 + rng.randrange(256)
                    t = rng.choice((0x80, 0x30))
                codes.append(((t << 24) | a, rng.choice((rng.randrange(0x10000), base[a - 0x10000], 0, 0xFF))))
            ram = bytearray(RAM)
            ram[0x10000:0x10100] = base
            with self.subTest(case=k):
                self.check(codes, ram, times=3)


class Manager(unittest.TestCase):
    """The Manager's side: the table it writes, the converters, the cheats it puts in the flash."""

    def test_table(self):
        class App:
            engine_codes = {}
        app = App()
        self.assertEqual(bytes(8), codeengine.table(app))
        codeengine.set_codes(app, "cheats", [(0x80000000 + i * 2, i) for i in range(300)])
        codeengine.set_codes(app, "multitap", [(0x80100000, 1), (0x80100002, 2)])
        t = codeengine.table(app)
        magic, count = struct.unpack_from("<II", t)
        self.assertEqual((MAGIC, MAX), (magic, count))
        self.assertEqual(8 + MAX * 8, len(t), "fits the console's table (0x6C8 bytes)")
        self.assertEqual((0x80100000, 1), struct.unpack_from("<IH", t, 8), "the multitap's codes first")

    def test_pops(self):
        cases = [
            ([("01012345", "000000FF")], ["30012345 00FF"]),
            ([("11012344", "00001234")], ["80012344 1234"]),
            ([("21012340", "11223344")], ["80012340 3344", "80012342 1122"]),
            ([("D1012000", "01000064"), ("11012344", "00000005")], ["D0012000 0064", "80012344 0005"]),
            ([("D1012000", "02010007"), ("01012344", "00000001"), ("01012345", "00000002")],
             ["E0012000 0007", "30012344 0001", "E0012000 0007", "30012345 0002"]),
            ([("41012340", "00030001"), ("00000010", "00000001")],
             ["80012340 0010", "80012342 0000", "80012344 0011", "80012346 0000", "80012348 0012", "8001234A 0000"]),
            ([("9000FFFF", "12345678"), ("01012345", "00000001")], ["30012345 0001"]),
            ([("01400000", "00000001")], None),                 # outside the PS1's RAM
            ([("51012345", "00000001")], None),                 # a type it can't turn
        ]
        for lines, want in cases:
            with self.subTest(lines=lines):
                self.assertEqual(want, cheatdb.pops_cheat(lines))

    def test_pops_test_of_no_line(self):
        """A POPS test covering 0 lines: it must not cover every line after it."""
        got = cheatdb.pops_cheat([("D1012000", "00000064"), ("11012344", "00000005"), ("11012346", "00000006")])
        self.assertTrue(got is None or got == ["80012344 0005", "80012346 0006"], got)

    def test_xploder(self):
        self.assertEqual(["80012344 1234", "30012345 00AB", "D0012000 0064", "80012344 0001"],
                         cheatdb.xp_cheat([(0x80012344, 0x1234), (0x30012345, 0xAB), (0x70012000, 0x64),
                                           (0x80012344, 1)]))
        self.assertIsNone(cheatdb.xp_cheat([(0x70012000, 0x64)]), "a test with no code after it")
        self.assertIsNone(cheatdb.xp_cheat([(0x80400000, 1)]))

    def test_gameshark_records(self):
        def rec(title, cheats):
            out = title + b"\0" + bytes((len(cheats),))
            for name, codes in cheats:
                out += name + b"\0" + bytes((0x80 | len(codes),))
                out += b"".join(struct.pack(">IH", c, v) for c, v in codes)
            return out
        a = rec(b"GAME ONE", [(b"\xFFLives", [(0x80012344, 0x63)]), (b"Have All", [(0xD0012000, 1), (0x80012000, 2)])])
        b = rec(b"GAME TWO", [(b"\xFC\xFA", [(0x30010000, 0xFF)])])
        data = b"junk GameShark " + bytes(100) + (a + b + a + b) * 3 + bytes(50)
        path = write(tmp_path("gs.bin"), data)
        games = cheatdb.gameshark_games(path)
        self.assertEqual({"GAME ONE", "GAME TWO"}, set(games))
        self.assertEqual([("Infinite Lives", ("80012344 0063",)), ("Have All", ("D0012000 0001", "80012000 0002"))],
                         games["GAME ONE"])
        self.assertEqual([("Unlimited Health", ("30010000 00FF",))], games["GAME TWO"])


class Flash(unittest.TestCase):
    """The cheats put in the flash (cheatdb.rom_cheats) read by the firmware's cheat screen (cheatmenu.h)."""

    @classmethod
    def setUpClass(cls):
        cls.exe = harness()

    def db(self, games):
        db = sqlite3.connect(":memory:")
        db.executescript(cheatdb.SCHEMA)
        for serial, title, cheats in games:
            db.execute("INSERT INTO games VALUES (?, ?, 'PAL')", (serial, title))
            for name, codes in cheats:
                db.execute("INSERT INTO cheats (serial, name, codes) VALUES (?, ?, ?)", (serial, name, "\n".join(codes)))
        return db

    def menu(self, blob, serial, mask=0):
        r = subprocess.run([self.exe, "menu", write(tmp_path("db.bin"), blob), serial, "%X" % mask],
                           capture_output=True, text=True, timeout=30)
        self.assertEqual(0, r.returncode, r.stderr)
        found, cheats, table = None, [], []
        for line in r.stdout.splitlines():
            if line.startswith("FOUND"):
                found = line.split(" ", 2)
            elif line.startswith("CHEAT"):
                name, codes = line[6:].split("|")
                cheats.append((name, [c for c in codes.split(";") if c]))
            elif line.startswith("CODE"):
                table.append(line[5:])
        return int(found[1]), (found[2] if len(found) > 2 else ""), cheats, table

    def test_round_trip(self):
        rng = random.Random(4)
        games = []
        for g in range(12):
            cheats = []
            for c in range(rng.randint(1, 70)):
                codes = ["%02X%06X %04X" % (rng.choice((0x80, 0x30, 0xD0, 0xE1, 0x50, 0xC0)), rng.randrange(0x200000),
                                            rng.randrange(0x10000)) for _ in range(rng.randint(1, 6))]
                cheats.append(("Cheat %d of game %d with a long name to cut" % (c, g), codes))
            cheats.append(("Not for the engine", ["10012345 0001"]))         # (a type the engine doesn't do)
            games.append(("SLES_%03d.%02d" % (100 + g, g), "Game number %d with a long title to be cut" % g, cheats))
        db = self.db(games)
        serials = [g[0] for g in games]
        blob, put, left = cheatdb.rom_cheats(db, serials)
        self.assertLessEqual(len(blob), cheatdb.ROM_CHEATS_SIZE)
        self.assertEqual(serials, put + left)
        self.assertEqual(len(put), len(cheatdb.read_rom_cheats(blob)))
        for serial, title, cheats in games:
            with self.subTest(serial=serial):
                n, shown_title, got, _ = self.menu(blob, serial)
                if serial in left:
                    self.assertEqual(0, n)
                    continue
                engine = [(nm, cs) for nm, cs in cheats if all(int(c[:2], 16) in cheatdb.ENGINE_TYPES for c in cs)]
                self.assertEqual(min(64, len(engine)), n)
                self.assertEqual(title.upper()[:31], shown_title)
                for (name, codes), (gname, gcodes) in zip(engine, got):
                    self.assertEqual(name[:40], gname)
                    self.assertEqual([c.upper() for c in codes], gcodes)
        # The cheats chosen: their codes in the table, in order, up to the table's size
        serial = put[0]
        n, _, got, _ = self.menu(blob, serial)
        mask = sum(1 << i for i in range(n) if i % 3 != 1)
        _, _, _, table = self.menu(blob, serial, mask)
        want = [c for i, (nm, cs) in enumerate(got) if mask >> i & 1 for c in cs][:MAX]
        self.assertEqual(want, table)
        self.assertEqual((0, "", [], []), self.menu(blob, "SLUS_999.99"))
        self.assertEqual(0, self.menu(blob, put[0][:10])[0], "a serial's start isn't the game")

    @unittest.expectedFailure
    def test_damaged_flash(self):
        """A damaged cheats area: the cheat screen must not use cheats it didn't read. KNOWN (firmware): CheatFind
        returns the count in the game's header even when the data ends before (its pointers NULL: the kernel's RAM
        read as cheats, their codes put in the engine). Needs a firmware change (count the cheats read)."""
        db = self.db([("SLES_001.00", "A", [("c%d" % i, ["80012345 %04X" % i]) for i in range(5)])])
        blob, _, _ = cheatdb.rom_cheats(db, ["SLES_001.00"])
        rng = random.Random(9)
        for k in range(200):
            d = bytearray(blob.ljust(cheatdb.ROM_CHEATS_SIZE, b"\xFF"))
            for _ in range(rng.randint(1, 8)):
                d[rng.randrange(min(len(blob) + 8, len(d)))] = rng.randrange(256)
            self.menu(bytes(d), "SLES_001.00", 0xFF)      # (no crash: the reads stay in the flash)
            cheatdb.read_rom_cheats(bytes(d))
        self.assertEqual(bytes(16), cheatdb.rom_cheats(db, [])[0])


if __name__ == "__main__":
    unittest.main()
