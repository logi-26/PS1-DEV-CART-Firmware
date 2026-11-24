"""
PicoStation Manager (pctools/picostation_manager.py) and the tools it uses (picostation_ids.py, picostation_covers.py):
- .cht files: read (on / off at boot, comments and lines it doesn't know left out), written (CRLF, read back the
  same), the PicoStation's limits (6 KB, 256 cheats).
- .cov files: an image converted (16896 bytes: 256 colors, 128x128), read back; the game ID after the image ("PSID",
  "PSIX" for a blank cover made for it), written once.
- The game ID read from a disc image made here (ISO 9660, Mode 2 and Mode 1, SYSTEM.CNF's BOOT, PSX.EXE without it,
  damaged images).
- The card: a folder of several .cue files is one game of several discs, the others one game each.
- The window (if Tk can open one): the PS1 DEV CART icon; nothing moves when other games are shown (a long title, a
  game of several discs, a game without an ID, a cover being chosen, the list searched); a cover and cheats saved.

    python -m unittest tests/tools/test_picostation.py -v

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import io
import os
import shutil
import sqlite3
import sys
import tempfile
import time
import tkinter
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from toolhost import ROOT, TMP                                # noqa: E402

PCTOOLS = os.path.join(ROOT, "pctools")
sys.path.insert(0, PCTOOLS)
# The app's settings: a file of the tests', never the user's
os.environ["PICOSTATION_SETTINGS"] = os.path.join(TMP, "picostation_settings.json")

from PIL import Image                                         # noqa: E402
import ttkbootstrap as tb                                     # noqa: E402

import picostation_covers as covers                           # noqa: E402
import picostation_ids as ids                                 # noqa: E402
import picostation_manager as manager                         # noqa: E402

RAW = 2352
BACKSLASH = bytes([92])


# ------------------------------------------------------------------------------------------------- disc images

def dir_record(name, lba, size, folder=False):
    rec = bytearray(33 + len(name) + (1 - len(name) % 2))       # (padded to an even length)
    rec[0] = len(rec)
    rec[2:6] = lba.to_bytes(4, "little")
    rec[6:10] = lba.to_bytes(4, "big")
    rec[10:14] = size.to_bytes(4, "little")
    rec[14:18] = size.to_bytes(4, "big")
    rec[25] = 2 if folder else 0
    rec[32] = len(name)
    rec[33:33 + len(name)] = name
    return bytes(rec)


def make_disc(folder, name, serial="SLUS_008.18", mode=2, cnf=True, iso=True):
    """A data track (name.bin, raw 2352-byte sectors) and its .cue: an ISO 9660 volume whose SYSTEM.CNF boots
    cdrom:\\<serial>;1 (lower case: as some discs have it). The .cue's path."""
    sectors = [bytes(2048)] * 24
    if iso:
        boot = b"BOOT = cdrom:" + BACKSLASH + serial.lower().encode() + b";1\r\nTCB = 4\r\nEVENT = 10\r\n"
        files = [(b"SYSTEM.CNF;1", 20, boot)] if cnf else []
        files.append((b"PSX.EXE;1", 21, b"PS-X EXE" + bytes(2040)))
        root = dir_record(b"\0", 18, 2048, True) + dir_record(b"\1", 18, 2048, True)
        for fname, lba, data in files:
            root += dir_record(fname, lba, len(data))
            sectors[lba] = data.ljust(2048, b"\0")
        sectors[18] = root.ljust(2048, b"\0")
        pvd = bytearray(2048)
        pvd[0], pvd[1:6], pvd[6] = 1, b"CD001", 1
        pvd[156:156 + 34] = dir_record(b"\0", 18, 2048, True)
        sectors[16] = bytes(pvd)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, name + ".bin"), "wb") as f:
        for lba, data in enumerate(sectors):
            raw = bytearray(RAW)
            raw[1:11] = b"\xff" * 10
            raw[15] = mode
            start = 16 if mode == 1 else 24
            raw[start:start + 2048] = data
            f.write(raw)
    cue = os.path.join(folder, name + ".cue")
    with open(cue, "w") as f:
        f.write('FILE "%s.bin" BINARY\n  TRACK 01 MODE%d/2352\n    INDEX 01 00:00:00\n' % (name, mode))
    return cue


def new_dir(name):
    path = tempfile.mkdtemp(prefix=name + "_", dir=TMP)
    return path


# ------------------------------------------------------------------------------------------------- files

class ChtFiles(unittest.TestCase):

    def test_read(self):
        text = ("# a comment\r\n; another\r\n[Infinite Health]\r\n80012345 0063\r\nD0012346 0001\r\n"
                "[*Moon Jump]\r\n800ABCDE ffff\r\nnot a code\r\n[No codes]\r\n[]\r\n30000000 0001\r\n")
        cheats = manager.parse_cht(text)
        self.assertEqual(["Infinite Health", "Moon Jump", "Cheat"], [c.name for c in cheats])
        self.assertEqual([True, False, True], [c.on for c in cheats])
        self.assertEqual([(0x80012345, 0x63), (0xD0012346, 1)], cheats[0].codes)
        self.assertEqual([(0x800ABCDE, 0xFFFF)], cheats[1].codes)

    def test_written_read_back(self):
        cheats = [manager.Cheat("Health", [(0x80012345, 0x63)], True),
                  manager.Cheat("Lives", [(0x80010000, 9), (0x80010002, 9)], False)]
        text = manager.cht_text(cheats, "A Game", "SLUS_008.18")
        self.assertTrue(text.startswith("# A Game (SLUS_008.18)"))
        self.assertNotIn("\n", text.replace("\r\n", ""))
        self.assertIn("[Health]\r\n80012345 0063\r\n[*Lives]\r\n80010000 0009\r\n", text)
        back = manager.parse_cht(text)
        self.assertEqual([(c.name, c.codes, c.on) for c in cheats], [(c.name, c.codes, c.on) for c in back])

    def test_limits(self):
        few = [manager.Cheat("C%d" % i, [(0x80010000 + i * 2, i)]) for i in range(10)]
        size, problem = manager.cht_limits(few, "Game", "SLUS_008.18")
        self.assertEqual("", problem)
        self.assertEqual(len(manager.cht_text(few, "Game", "SLUS_008.18")), size)
        big = [manager.Cheat("Cheat %d" % i, [(0x80010000 + j * 2, j) for j in range(20)]) for i in range(30)]
        self.assertIn("too big", manager.cht_limits(big, "Game", "SLUS_008.18")[1])
        many = [manager.Cheat("%d" % i, [(0x80010000, 1)]) for i in range(257)]
        many_text = manager.cht_text(many, "G", "S")
        self.assertLessEqual(len(many_text), manager.CHT_SIZE)
        self.assertIn("too many", manager.cht_limits(many, "G", "S")[1])


class CovFiles(unittest.TestCase):

    def test_converted_read_back(self):
        src = Image.new("RGB", (300, 200), (0, 0, 0))
        src.paste((200, 40, 40), (0, 0, 150, 200))
        src.paste((40, 200, 40), (150, 0, 300, 200))
        data = covers.to_cov(src)
        self.assertEqual(ids.COVER_IMAGE, len(data))
        img = manager.cov_image(data)
        self.assertEqual((128, 128), img.size)
        # Fitted in the square: black above and below, the two colors side by side (5 bits a component)
        self.assertEqual((0, 0, 0), img.getpixel((64, 2)))
        for (x, y), want in (((20, 64), (200, 40, 40)), ((108, 64), (40, 200, 40))):
            self.assertTrue(all(abs(a - b) <= 8 for a, b in zip(img.getpixel((x, y)), want)), img.getpixel((x, y)))
        stretched = manager.cov_image(covers.to_cov(src, stretch=True))
        self.assertNotEqual((0, 0, 0), stretched.getpixel((64, 2)))
        self.assertIsNone(manager.cov_image(b"\0" * 100))

    def test_id_after_the_image(self):
        folder = new_dir("cov")
        path = os.path.join(folder, "game.cov")
        with open(path, "wb") as f:
            f.write(covers.to_cov(Image.new("RGB", (128, 128), (9, 99, 199))))
        self.assertEqual("ID written in its cover", ids.write_cover(path, "SLUS_008.18", True))
        self.assertEqual(ids.COVER_IMAGE, os.path.getsize(path))            # (dry run: nothing written)
        ids.write_cover(path, "SLUS_008.18", False)
        data = open(path, "rb").read()
        self.assertEqual(b"PSID" + b"SLUS_008.18\0", data[ids.COVER_IMAGE:])
        self.assertEqual("already done", ids.write_cover(path, "SLUS_008.18", False))
        ids.write_cover(path, "SLES_013.43", False)                         # A wrong ID: replaced
        self.assertEqual(b"PSIDSLES_013.43\0", open(path, "rb").read()[ids.COVER_IMAGE:])
        blank = os.path.join(folder, "none.cov")
        ids.write_cover(blank, "SCES_000.01", False)
        data = open(blank, "rb").read()
        self.assertEqual(bytes(ids.COVER_IMAGE) + b"PSIXSCES_000.01\0", data)
        small = os.path.join(folder, "small.cov")
        open(small, "wb").write(b"x" * 10)
        self.assertTrue(ids.write_cover(small, "SCES_000.01", False).startswith("cover too small"))
        self.assertEqual(10, os.path.getsize(small))

    def test_ids_and_regions(self):
        self.assertEqual("SLES_01343", manager.plain_id("sles_013.43"))
        self.assertEqual("", manager.plain_id(None))
        self.assertEqual("NTSC-U", covers.id_region("SLUS_008.18"))
        self.assertEqual("PAL", covers.id_region("SCES_000.01"))
        self.assertEqual("NTSC-J", covers.id_region("SLPS_012.34"))
        self.assertIsNone(covers.id_region("PSX.EXE"))


class GameIds(unittest.TestCase):

    def test_mode2_and_mode1(self):
        folder = new_dir("ids")
        self.assertEqual("SLUS_008.18", ids.game_id(make_disc(folder, "a", "SLUS_008.18")))
        self.assertEqual("SCES_000.01", ids.game_id(make_disc(folder, "b", "SCES_000.01", mode=1)))

    def test_no_system_cnf(self):
        self.assertEqual("PSX.EXE", ids.game_id(make_disc(new_dir("ids"), "a", cnf=False)))

    def test_damaged(self):
        folder = new_dir("ids")
        with self.assertRaises(ValueError):
            ids.game_id(make_disc(folder, "noiso", iso=False))
        cue = make_disc(folder, "cut")
        with open(cue[:-4] + ".bin", "r+b") as f:
            f.truncate(10 * RAW)
        with self.assertRaises(ValueError):
            ids.game_id(cue)
        audio = os.path.join(folder, "audio.cue")
        with open(audio, "w") as f:
            f.write('FILE "audio.bin" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n')
        with self.assertRaises(ValueError):
            ids.game_id(audio)


LONG = ("Long, An Extremely Long Game Name That Goes On And On, Special Collector's Edition, Greatest Hits, "
        "Game Of The Year (USA) (Rev 1)")


def make_card():
    """An SD card's folder: games at the top, in folders of their own, a game of 2 discs; a folder left out."""
    sd = new_dir("sd")
    make_disc(sd, "Alpha (USA)", "SLUS_000.01")
    make_disc(os.path.join(sd, "Beta"), "Beta (Europe)", "SLES_000.02")
    make_disc(os.path.join(sd, "Gamma"), "Gamma (Disc 1)", "SLUS_000.03")
    make_disc(os.path.join(sd, "Gamma"), "Gamma (Disc 2)", "SLUS_000.04")
    make_disc(os.path.join(sd, LONG), "Long", "SLUS_000.05")
    make_disc(os.path.join(sd, "Broken"), "Broken", iso=False)
    make_disc(os.path.join(sd, "System Volume Information"), "Hidden")
    return sd


class Card(unittest.TestCase):

    def test_games(self):
        sd = make_card()
        games = manager.scan(sd)
        self.assertEqual(["Alpha (USA)", "Beta", "Broken", "Gamma", LONG], [g.name for g in games])
        alpha, beta, _broken, gamma, _long = games
        self.assertEqual(1, len(alpha.discs))
        self.assertIsNone(alpha.folder_cov)                     # (at the top: no folder)
        self.assertEqual(os.path.join(sd, "Beta.cov"), beta.folder_cov)
        self.assertEqual(["Gamma (Disc 1).cue", "Gamma (Disc 2).cue"], [os.path.basename(d.cue) for d in gamma.discs])
        self.assertEqual(os.path.join(sd, "Gamma", "Gamma (Disc 2).cht"), gamma.discs[1].cht)


# ------------------------------------------------------------------------------------------------- the window

def open_app(sd):
    """The app on a card (settings not read), once it has read the card; None if Tk can't open a window."""
    try:
        app = manager.App(sd=sd, load=False)
    except Exception as e:  # noqa: BLE001 (no display)
        raise unittest.SkipTest("no window: %s" % e)
    end = time.time() + 30
    while not app.games and time.time() < end:
        app.update()
        time.sleep(0.02)
    app.update()
    return app


class Window(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.sd = make_card()
        cls.app = open_app(cls.sd)

    @classmethod
    def tearDownClass(cls):
        cls.app.destroy()
        shutil.rmtree(cls.sd, ignore_errors=True)

    def show(self, name):
        app = self.app
        app.filter_var.set("")
        i = [g.name for g in app.shown].index(name)
        app.tree.selection_set(str(i))
        app.update()
        self.assertEqual(name, app.game.name)

    def places(self):
        """Where the panel's parts are (on the screen, their sizes)."""
        app = self.app
        app.update()
        widgets = {"canvas": app.canvas, "disc": app.disc_box, "title": app.title_label, "cheats": app.cheat_tree,
                   "games": app.tree}
        for name in ("btn_file", "btn_db", "btn_save_cov", "btn_cancel_cov", "btn_png", "btn_add", "btn_delete",
                     "btn_import", "btn_save_cht", "stretch_box", "cover_src", "cover_state", "cheat_note"):
            widgets[name] = getattr(app, name)
        for key, label in app.info.items():
            widgets["info " + key] = label
        return {name: (w.winfo_rootx(), w.winfo_rooty(), w.winfo_width(), w.winfo_height())
                for name, w in widgets.items()}

    def test_icon(self):
        self.assertEqual(len(manager.ICON_SIZES), len(self.app.icons))
        self.assertTrue(os.path.isfile(os.path.join(manager.ICONS, "ps1devcart.ico")))

    def test_card_read(self):
        app = self.app
        self.assertEqual(5, len(app.games))
        broken = [g for g in app.games if g.name == "Broken"][0]
        self.assertIsNone(broken.serial)
        self.assertTrue(broken.discs[0].error)
        self.assertEqual("5 of 5", app.count_label.cget("text"))

    def test_nothing_moves(self):
        app = self.app
        before = self.places()
        for name in ("Alpha (USA)", LONG, "Gamma", "Broken", "Beta", "Alpha (USA)"):
            self.show(name)
            self.assertEqual(before, self.places(), name)
        self.show("Gamma")
        app.select_disc(1)
        self.assertEqual("SLUS_000.04", app.disc.serial)
        self.assertEqual(before, self.places(), "the second disc")
        # A long title and folder: cut to the room there is, the whole text in the tooltip
        self.show(LONG)
        title = app.title_label.cget("text")
        self.assertTrue(title.endswith("…"), title)
        self.assertTrue(app.title_label.tip.text.startswith("Long, An Extremely Long Game Name"))
        # A cover chosen, not saved
        app.set_pending(Image.new("RGB", (64, 90), (10, 120, 220)), "An image with a name long enough to be cut "
                        "short in the panel, a very long name indeed, really.png")
        self.assertEqual(before, self.places(), "a cover chosen")
        self.assertEqual("normal", str(app.btn_save_cov.cget("state")))
        app.cancel_cover()
        # The list searched: the count changes, the list stays where it is
        app.filter_var.set("gamma")
        self.assertEqual(before, self.places(), "searched")
        self.assertEqual("1 of 5", app.count_label.cget("text"))
        app.filter_var.set("")

    def test_save_cheats(self):
        app = self.app
        self.show("Gamma")
        self.assertEqual("disabled", str(app.btn_save_cht.cget("state")))
        app.cheats = [manager.Cheat("Lives", [(0x80010000, 9)], True)]
        app.changed(0)
        self.assertEqual("normal", str(app.btn_save_cht.cget("state")))
        self.assertTrue(app.save_cheats())
        cht = app.game.discs[0].cht
        self.assertEqual(["Lives"], [c.name for c in manager.parse_cht(open(cht).read())])
        self.assertEqual(1, app.game.cheats)
        self.assertEqual("disabled", str(app.btn_save_cht.cget("state")))
        app.cheats = []                                         # No cheats: the file removed
        app.changed()
        self.assertTrue(app.save_cheats())
        self.assertFalse(os.path.exists(cht))

    def test_save_cover(self):
        app = self.app
        self.show("Beta")
        game = app.game
        app.set_pending(Image.new("RGB", (100, 100), (250, 200, 0)), "yellow.png")
        app.save_cover()                                        # (no cover there yet: not asked)
        self.assertIsNone(app.pending)
        for path, serial in app.cover_targets(game):
            data = open(path, "rb").read()
            self.assertEqual(b"PSID" + serial.encode() + b"\0", data[ids.COVER_IMAGE:], path)
        self.assertEqual(("Cover", "Yes"), (game.cover, game.id_state))
        self.assertEqual("disabled", str(app.btn_save_cov.cget("state")))


def make_game_db(folder, covers_of):
    """A PSIO game database (psio_assist.db's tables): games' titles, covers (PNG) for the IDs given."""
    path = os.path.join(folder, "psio_assist.db")
    db = sqlite3.connect(path)
    db.execute("create table games (game_id text, name text)")
    db.execute("create table covers (game_id text, psio blob)")
    for serial, color in covers_of.items():
        png = io.BytesIO()
        Image.new("RGB", (80, 84), color).save(png, "PNG")
        db.execute("insert into games values (?, ?)", (manager.plain_id(serial), "Title of " + serial))
        db.execute("insert into covers values (?, ?)", (manager.plain_id(serial), png.getvalue()))
    db.commit()
    db.close()
    return path


class Dialogs(unittest.TestCase):
    """The app's own windows opened, answered as a user would (the whole card's lists, a cheat's window)."""

    @classmethod
    def setUpClass(cls):
        cls.sd = make_card()
        cls.app = open_app(cls.sd)

    @classmethod
    def tearDownClass(cls):
        cls.app.destroy()
        shutil.rmtree(cls.sd, ignore_errors=True)

    def answer(self, act):
        """act(window) on the dialog the next call opens (it waits for it to close); its title kept."""
        seen = []

        def find():
            wins = [w for w in self.app.winfo_children() if isinstance(w, tkinter.Toplevel) and w.winfo_exists()
                    and w.winfo_viewable()]
            if not wins:
                self.app.after(50, find)
                return
            win = wins[-1]
            seen.append((win.title(), str(win.master)))
            act(win)
        self.app.after(100, find)
        return seen

    @staticmethod
    def widgets(win, kind):
        out, todo = [], [win]
        while todo:
            w = todo.pop(0)
            todo += w.winfo_children()
            if isinstance(w, kind):
                out.append(w)
        return out

    def press(self, win, start):
        """The dialog's button whose text starts with start, pressed (or the window closed if it is disabled)."""
        for b in self.widgets(win, tb.Button):
            if b.cget("text").startswith(start):
                if str(b.cget("state")) != "disabled":
                    b.invoke()
                    return
        win.destroy()

    def test_missing_covers_then_ids(self):
        app = self.app
        app.open_db(make_game_db(new_dir("gamedb"), {"SLUS_000.01": (200, 0, 0), "SLUS_000.03": (0, 0, 200)}))
        self.assertIsNotNone(app.db)
        seen = self.answer(lambda win: self.press(win, "Write"))
        app.fill_covers()
        self.assertEqual([("Missing covers", str(app))], seen)
        by_name = {g.name: g for g in app.games}
        self.assertEqual("Cover", by_name["Alpha (USA)"].cover)
        self.assertEqual("Cover", by_name["Gamma"].cover)
        for d in by_name["Gamma"].discs:                        # (each disc's cover, with its own ID)
            self.assertTrue(os.path.exists(d.cov))
        self.assertTrue(os.path.exists(by_name["Gamma"].folder_cov))
        self.assertEqual("Missing", by_name["Beta"].cover)     # (not in the database)
        # A cover without its ID (made by another tool): the second button adds it, the image kept
        beta = by_name["Beta"]
        image = covers.to_cov(Image.new("RGB", (128, 128), (0, 160, 0)))
        with open(beta.discs[0].cov, "wb") as f:
            f.write(image)
        seen = self.answer(lambda win: self.press(win, "Write"))
        app.write_ids()
        self.assertEqual([("Game IDs", str(app))], seen)
        with open(beta.discs[0].cov, "rb") as f:
            self.assertEqual(image + b"PSIDSLES_000.02\0", f.read())
        # Nothing left to do: the list says so, its button can't be pressed
        seen = self.answer(lambda win: self.press(win, "Write"))
        app.write_ids()
        self.assertEqual([("Game IDs", str(app))], seen)

    def test_cheat_window(self):
        app = self.app

        def fill(win):
            self.widgets(win, tb.Entry)[0].insert(0, "Max money")
            self.widgets(win, tkinter.Text)[0].insert("1.0", "80012345 270F\n80012347 0000")
            self.press(win, "OK")
        seen = self.answer(fill)
        result = app.cheat_dialog(None)
        self.assertEqual([("Cheat", str(app))], seen)
        self.assertEqual(("Max money", [(0x80012345, 0x270F), (0x80012347, 0)], False), result)
        seen = self.answer(lambda win: self.press(win, "Cancel"))
        self.assertIsNone(app.cheat_dialog(manager.Cheat("Lives", [(0x80010000, 9)])))
        self.assertEqual([("Cheat", str(app))], seen)


class Selftest(unittest.TestCase):

    def test_selftest(self):
        try:
            problems = manager.selftest()
        except Exception as e:  # noqa: BLE001
            raise unittest.SkipTest("no window: %s" % e)
        self.assertEqual([], problems)


if __name__ == "__main__":
    unittest.main()
