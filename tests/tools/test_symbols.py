"""
Symbol files (gui/symbols.py: the Memory page's variables, the Profiler's and Frame Inspector's functions): ELF
executables read against the toolchain's nm (-S: sizes, kinds), the same programs' GNU ld map files and nm output
read as text, Net Yaroze ECOFF executables (the SDK's tools, if PS1_NETYAROZE is set: their symbols plausible),
damaged files (a ValueError, nothing else).

    python -m unittest tests/tools/test_symbols.py -v

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import glob
import os
import random
import struct
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from toolhost import ROOT, tmp_path, write, read            # noqa: E402

import symbols                                              # noqa: E402

NM = os.path.join(ROOT, "buildtools", "psn00bsdk", "bin", "mipsel-none-elf-nm.exe")


def elfs():
    out = []
    for pattern in ("ps1-tools/*/*.elf", "demos/*/*.elf", "kernel_hook/*.elf", "monitor/*.elf", "ps1transfer/*.elf"):
        out += glob.glob(os.path.join(ROOT, pattern))
    return sorted(set(out))


def nm(path, *flags):
    r = subprocess.run([NM, "-S"] + list(flags) + [path], capture_output=True, text=True)
    return r.stdout


def nm_symbols(path):
    """{(name, address): (size, kind)} of the functions and data nm lists (defined, named, not section symbols)."""
    out = {}
    for line in nm(path).splitlines():
        f = line.split()
        if len(f) == 4:
            adrs, size, letter, name = int(f[0], 16) & 0xFFFFFFFF, int(f[1], 16), f[2], f[3]
        elif len(f) == 3:
            adrs, size, letter, name = int(f[0], 16) & 0xFFFFFFFF, 0, f[1], f[2]
        else:
            continue
        if letter in "UuwvVaANn" or name.startswith((".", "$")):
            continue
        kind = "func" if letter in "Tt" else ("data" if letter in "BbDdGgSsRrCc" else "?")
        out[(name, adrs)] = (size, kind)
    return out


def elf_objects(path):
    """Names of the ELF's data objects (STT_OBJECT), from readelf."""
    readelf = NM.replace("-nm.exe", "-readelf.exe")
    out = subprocess.run([readelf, "-sW", path], capture_output=True, text=True).stdout
    return {f[7] for f in (line.split() for line in out.splitlines()) if len(f) >= 8 and f[3] == "OBJECT"}


class Symbols(unittest.TestCase):

    def test_elf_against_nm(self):
        files = elfs()
        self.assertGreater(len(files), 5)
        for path in files:
            with self.subTest(elf=os.path.relpath(path, ROOT)):
                want = nm_symbols(path)
                got = {(s.name, s.adrs): s for s in symbols.load(path)}
                funcs = {k for k, (size, kind) in want.items() if kind == "func"}
                data = {k for k, (size, kind) in want.items() if kind == "data" and size}
                self.assertEqual(set(), funcs - set(got), "functions nm lists, not read")
                self.assertEqual(set(), data - set(got), "variables nm lists, not read")
                objects = elf_objects(path)
                for k in funcs | data:
                    size, kind = want[k]
                    s = got[k]
                    # (nm's letter is the section's: a data object in the code section is "T"; the ELF's type says)
                    self.assertEqual("data" if k[0] in objects else kind, s.kind, "%s: kind" % k[0])
                    if size:
                        self.assertEqual(size, s.size, "%s: size" % k[0])
                        self.assertFalse(s.estimated)
                order = [(s.adrs, s.name) for s in symbols.load(path)]
                self.assertEqual(sorted(order), order, "sorted by address")

    def test_nm_output_and_map(self):
        """nm output (with and without -S) and the map file of a program: the same symbols as its ELF."""
        for path in elfs():
            with self.subTest(elf=os.path.relpath(path, ROOT)):
                elf = {(s.name, s.adrs): s for s in symbols.load(path) if s.kind in ("func", "data")}
                for flags in ((), ("--no-sort",)):
                    text = write(tmp_path("nm.txt"), nm(path, *flags).encode())
                    got = {(s.name, s.adrs): s for s in symbols.load(text)}
                    for k, s in elf.items():
                        self.assertIn(k, got, "nm output: %s" % k[0])
                        if s.size and not s.estimated:
                            self.assertEqual(s.size, got[k].size, k[0])
                plain = subprocess.run([NM, path], capture_output=True, text=True).stdout
                got = {(s.name, s.adrs) for s in symbols.load(write(tmp_path("nm2.txt"), plain.encode()))}
                self.assertEqual(set(), set(elf) - got, "nm without sizes")
                mp = os.path.splitext(path)[0] + ".map"
                if os.path.isfile(mp) and os.path.getmtime(mp) >= os.path.getmtime(path) - 5:
                    got = {(s.name, s.adrs): s for s in symbols.load(mp)}
                    funcs = {k for k, s in elf.items() if s.kind == "func" and not k[0].startswith("_")}
                    missing = [k[0] for k in funcs if k not in got]
                    # (a map lists the global symbols: the static functions aren't in it)
                    globals_ = {f.split()[-1] for f in nm(path, "-g").splitlines() if " T " in f}
                    self.assertEqual([], [m for m in missing if m in globals_], "map: global functions")
                    objects = elf_objects(path)
                    for k in got:
                        if k in elf and k[0] not in objects:     # (a map has no types: by section)
                            self.assertEqual(elf[k].kind, got[k].kind if got[k].kind != "?" else elf[k].kind)

    def test_net_yaroze(self):
        folder = os.environ.get("PS1_NETYAROZE")
        if not folder:
            self.skipTest("PS1_NETYAROZE not set (the Net Yaroze SDK's folder)")
        files = [p for p in glob.glob(os.path.join(folder, "**", "*"), recursive=True)
                 if os.path.isfile(p) and read(p)[:2] == b"\x62\x01"]
        self.assertTrue(files)
        for path in files:
            with self.subTest(file=os.path.basename(path)):
                try:
                    syms = symbols.load(path)
                except ValueError as e:         # (a stripped executable: said so)
                    self.assertIn("stripped", str(e))
                    continue
                self.assertGreater(len(syms), 10)
                for s in syms:
                    self.assertTrue(0x80000000 <= s.adrs < 0x80200000 or s.kind == "?", (s.name, hex(s.adrs)))
                self.assertIn("main", [s.name for s in syms if s.kind == "func"])

    def test_damaged(self):
        rng = random.Random(2)
        path = elfs()[0]
        data = read(path)
        bad = []
        for k in range(300):
            d = bytearray(data)
            if k % 4 == 0:
                d = d[:rng.randrange(4, len(d))]
            for _ in range(rng.randint(1, 40)):
                d[rng.randrange(len(d))] = rng.randrange(256)
            if k % 7 == 0:
                d[:2] = b"\x62\x01"                                  # (as an ECOFF file)
            p = write(tmp_path("dmg.elf"), d)
            try:
                symbols.load(p)
            except (ValueError, struct.error):     # (what the pages catch: the file said not to be one)
                pass
            except Exception as e:      # noqa: BLE001
                bad.append("%s: %s" % (type(e).__name__, e))
        for text in (b"", b"garbage\n\x00\xff", b"80012345 T\n", b" .text 0x\n   0xZZ main\n"):
            try:
                symbols.load(write(tmp_path("t.txt"), text))
            except ValueError:
                pass
            except Exception as e:      # noqa: BLE001
                bad.append("text %r: %s: %s" % (text, type(e).__name__, e))
        self.assertEqual([], sorted(set(bad))[:10])


if __name__ == "__main__":
    unittest.main()
