"""
Smaller parts of the tools:
- Run PSX / Run Net Yaroze (gui/programs.py): the kind of a program file; the files a Net Yaroze auto script loads
  (nested scripts, Windows paths, the depth limit, missing files) as ps1transfer -auto reads them; the name shown.
- Firmware page: the ROM checked before flashing (pages/firmware.describe_rom); the built ROM (cartrom/ps1devcart.rom):
  valid, its cheats area free; the cheats put in a ROM change nothing else (cheatdb.rom_with_cheats).
- Remote Controller: a multitap slot's reply (the buttons in the pad protocol's bytes, active low; the sticks), the
  code engine's codes writing it, applied by the console's engine (kernel_hook.c, as test_cheats builds it): the bytes
  in the game's buffer; the multitap buffer found in RAM and its port.
- Settings (gui/settings.py): a damaged settings file (cut, not JSON, the wrong kinds): the defaults, no exception; a
  save replaces the file whole.

    python -m unittest tests/tools/test_misc.py -v

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import json
import os
import random
import struct
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from toolhost import ROOT, TMP, read, tmp_path, write       # noqa: E402

import cheatdb                                              # noqa: E402
import programs                                             # noqa: E402


class Programs(unittest.TestCase):

    def test_kinds(self):
        self.assertEqual("psx", programs.file_kind(write(tmp_path("a.exe"), b"PS-X EXE" + bytes(100))))
        self.assertEqual("ecoff", programs.file_kind(write(tmp_path("main"), b"\x62\x01" + bytes(100))))
        self.assertEqual("auto", programs.file_kind(write(tmp_path("auto"), b"local load main\ngo\n")))
        self.assertEqual("auto", programs.file_kind(os.path.join(TMP, "no_such_file")))
        self.assertTrue(programs.needs_libps(write(tmp_path("main"), b"\x62\x01" + bytes(10))))
        self.assertFalse(programs.needs_libps(write(tmp_path("p.exe"), b"PS-X EXE" + bytes(10))))

    def test_auto_scripts(self):
        game = os.path.join(TMP, "yaroze", "MYGAME")
        os.makedirs(os.path.join(game, "data"), exist_ok=True)
        os.makedirs(os.path.join(game, "sub"), exist_ok=True)
        write(os.path.join(game, "sub", "auto2"), b"local dload ..\\data\\more.tim 80100000\n")
        write(os.path.join(game, "sub", "loop"), b"auto loop\n")
        auto = write(os.path.join(game, "auto"), b"local load main\r\nlocal dload data\\tex.tim 80090000\n"
                     b"dload data/snd.vh 800a0000\nauto sub/auto2\nauto sub\\loop\n  \nlocal  load  main2\ngo\n"
                     b"load\ndload x\n")
        files = programs.auto_files(auto)
        j = os.path.join
        self.assertEqual([auto, j(game, "main"), j(game, "data", "tex.tim"), j(game, "data", "snd.vh"),
                          j(game, "sub", "auto2"), j(game, "sub", "..", "data", "more.tim")], files[:6])
        self.assertIn(j(game, "main2"), files, "spaces between the words")
        self.assertLessEqual(len([f for f in files if f.endswith("loop")]), programs.AUTO_MAX_DEPTH + 1,
                             "a script calling itself: the depth limit")
        self.assertEqual("MYGAME", programs.program_name(auto))
        self.assertEqual("main", programs.program_name(write(tmp_path("main.exe"), b"PS-X EXE")).split("_")[-1])
        self.assertEqual([j(game, "missing")], programs.auto_files(j(game, "missing")))


class Firmware(unittest.TestCase):

    ROM = os.path.join(ROOT, "cartrom", "ps1devcart.rom")

    def test_rom_checks(self):
        from pages.firmware import describe_rom
        if not os.path.isfile(self.ROM):
            self.skipTest("no built ROM")
        ok, text = describe_rom(self.ROM)
        self.assertTrue(ok, text)
        self.assertIn("firmware", text)
        self.assertIn("debug monitor v", text)
        rom = read(self.ROM)
        for what, data in (("short", rom[:-1]), ("long", rom + b"\0"), ("no header", rom[:0x84] + bytes(44) + rom[0x84 + 44:]),
                           ("empty", b"")):
            self.assertFalse(describe_rom(write(tmp_path("x.rom"), data))[0], what)
        self.assertFalse(describe_rom(os.path.join(TMP, "no_such.rom"))[0])

    def test_cheats_area(self):
        """The built ROM's cheats area (0xB000-0xFFFF) holds no firmware: empty, or cheats; cheats put in change
        nothing outside it."""
        if not os.path.isfile(self.ROM):
            self.skipTest("no built ROM")
        from pages.firmware import describe_rom
        rom = read(self.ROM)
        area = rom[cheatdb.ROM_CHEATS:cheatdb.ROM_CHEATS + cheatdb.ROM_CHEATS_SIZE]
        self.assertTrue(area[:4] == b"CHDB" or not any(area) or all(b == 0xFF for b in area),
                        "the cheats area holds something else")
        blob = b"CHDB" + bytes(100)
        new = cheatdb.rom_with_cheats(rom, blob)
        self.assertEqual(rom[:cheatdb.ROM_CHEATS], new[:cheatdb.ROM_CHEATS])
        self.assertEqual(rom[cheatdb.ROM_CHEATS + cheatdb.ROM_CHEATS_SIZE:], new[cheatdb.ROM_CHEATS + cheatdb.ROM_CHEATS_SIZE:])
        self.assertTrue(describe_rom(write(tmp_path("c.rom"), new))[0])
        with self.assertRaises(ValueError):
            cheatdb.rom_with_cheats(rom, bytes(cheatdb.ROM_CHEATS_SIZE + 1))
        with self.assertRaises(ValueError):
            cheatdb.rom_with_cheats(rom[:-2], blob)


class Controller(unittest.TestCase):

    # The pad protocol: the first button byte (bit 0 first) Select, L3, R3, Start, Up, Right, Down, Left; the second
    # L2, R2, L1, R1, Triangle, Circle, Cross, Square; 0 = pressed
    FIRST = ("Select", "L3", "R3", "Start", "Up", "Right", "Down", "Left")
    SECOND = ("L2", "R2", "L1", "R1", "Triangle", "Circle", "Cross", "Square")

    def bits_of(self, names):
        from pages import controller as c
        table = {b[0]: b[1] for b in c.BUTTONS}
        table.update(dict(c.STICK_BUTTONS))
        return sum(table[n] for n in names)

    def test_slot_reply(self):
        from pages import controller as c
        rng = random.Random(1)
        for k in range(200):
            names = [n for n in self.FIRST + self.SECOND if rng.random() < 0.3]
            dev = rng.choice([d[0] for d in c.DEVICES if d[2] != "mouse"])
            axes = tuple(rng.randrange(256) for _ in range(4))
            data = c.slot_data(dev, self.bits_of(names), axes)
            want1 = 0xFF & ~sum(1 << i for i, n in enumerate(self.FIRST) if n in names)
            want2 = 0xFF & ~sum(1 << i for i, n in enumerate(self.SECOND) if n in names)
            self.assertEqual(bytes((0, c.DEVICE[dev][1], want1, want2)) + bytes(axes), data, (dev, names))
        self.assertEqual(b"\xff" * 8, c.slot_data(None, 0, None))

    def test_multitap_codes_on_the_engine(self):
        """The codes for a slot, applied by the console's code engine: the game's multitap buffer as the reply."""
        from pages import controller as c
        import test_cheats
        exe = test_cheats.harness()
        for buf in (0x80123450, 0x80123451):                 # (an odd buffer: 8-bit writes)
            ram = bytearray(0x200000)
            ram[buf - 0x80000000:buf - 0x80000000 + 34] = b"\x00\x80" + b"\x00\x41\xff\xff" + b"\xff" * 4 + b"\xff" * 24
            codes, want = [], bytearray(ram)
            for tap, (dev, names) in enumerate((("DualShock (analog)", ["Cross", "L3"]), ("NeGcon", ["Start"]),
                                                ("Digital pad", ["Up", "Square"]), (None, []))):
                axes = (1, 2, 3, 4) if dev and dev != "Digital pad" else None
                codes += c.slot_codes(buf, tap, dev, self.bits_of(names), axes)
                o = buf - 0x80000000 + 2 + tap * 8
                want[o:o + 8] = c.slot_data(dev, self.bits_of(names), axes)
            t = write(tmp_path("t.bin"), test_cheats.table_bytes(codes))
            rin, rout = write(tmp_path("r.bin"), bytes(ram)), tmp_path("o.bin")
            subprocess.run([exe, "apply", t, rin, rout, "1"], check=True, timeout=30)
            got = read(rout)[:0x200000]
            self.assertEqual(bytes(want[buf - 0x80000000:buf - 0x80000000 + 34]),
                             got[buf - 0x80000000:buf - 0x80000000 + 34], "buffer at %08X" % buf)

    def test_find_multitap(self):
        from pages import controller as c
        ram = bytearray(random.Random(2).randrange(256) for _ in range(0x10000))
        buf1 = 0x4000
        ram[buf1:buf1 + 34] = b"\x00\x80" + b"\x00\x73" + bytes(6) + b"\xff" * 24
        struct.pack_into("<II", ram, 0x9000, 0x80000000 + buf1, 0x80000000 + buf1 + 0x22)   # InitPAD(buf1, .., buf2)
        found = c.find_multitap(bytes(ram))
        self.assertIn(0x80000000 + buf1, found)
        self.assertEqual(0, c.multitap_port(bytes(ram), 0x80000000 + buf1))


class SettingsFile(unittest.TestCase):

    def settings(self, content):
        import settings
        os.environ["APPDATA"] = os.path.join(TMP, "appdata")
        os.makedirs(settings.config_dir(), exist_ok=True)
        path = os.path.join(settings.config_dir(), "settings.json")
        if content is None:
            if os.path.exists(path):
                os.remove(path)
        else:
            write(path, content)
        return settings.Settings(), path

    def test_damaged(self):
        old = os.environ.get("APPDATA")
        try:
            for content in (b"", b"{", b"null", b"[1, 2]", b'"text"', b'{"fields": null, "recent": 5}',
                            b'{"fields": {"a": 1}, "theme": 3}', b"\xff\xfe\x00", None):
                with self.subTest(content=content):
                    s, path = self.settings(content)
                    s.field("x")
                    s.set_field("x", 1)
                    s.add_recent("r", "file")
                    self.assertEqual(["file"], s.recent("r"))
                    s.get("theme")
                    s.save()
                    self.assertIsInstance(json.loads(read(path)), dict)
            s, path = self.settings(b'{"fields": {"a": 1}, "device": "BILL"}')
            self.assertEqual((1, "BILL"), (s.field("a"), s.get("device")), "a good file read")
            self.assertFalse(os.path.exists(path + ".tmp"), "saved in place")
        finally:
            if old is None:
                os.environ.pop("APPDATA", None)
            else:
                os.environ["APPDATA"] = old


if __name__ == "__main__":
    unittest.main()
