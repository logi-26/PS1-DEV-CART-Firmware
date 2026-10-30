"""
PS1 DEV CART GUI: the cheat database (SQLite), for the Cheats page's known cheats of the game booted from its disc.

    python cheatdb.py import FOLDER...      game lists and cheat files into the database (the Manager's settings folder)
    python cheatdb.py game SLES_015.42      the cheats of a game

Games by their disc's executable (SYSTEM.CNF BOOT: the serial number, e.g. SLES_015.42), which the firmware reports
("ps1transfer -info": game=). Cheats as PS1 GameShark codes:
	80aaaaaa vvvv	16-bit write			30aaaaaa 00vv	8-bit write
	D0aaaaaa vvvv	the next line if the 16-bit value at aaaaaa is equal (D1 not equal, D2 less, D3 greater)
	E0aaaaaa 00vv	the same for an 8-bit value (E1, E2, E3)

Imported (a folder may hold several kinds, sub-folders too):
- Game lists: "SERIAL**TITLE" lines (letter headings skipped); the region from the file name (PAL, NTSCU, NTSCJ).
- Cheat files (.cht) of POPStarter (PS1 games on a PS2: Open PS2 Loader): the cheat's name on a line, then its codes,
  PS2 codes on the PS1's RAM at 0x01000000 (0: 8-bit, 1: 16-bit, 2: 32-bit write, D: test, 4: 32-bit slide), turned
  into the codes above; the POPS hook (9, F) left out; a cheat with codes that can't be turned is left out.
- Xploder / Xplorer cheat databases (.xp.txt): the text "xpcrypt -x ROM" prints from a cartridge's ROM
  (https://github.com/mlafeldt/xpcrypt, run separately: GPL): "Game" lines, cheat names, decrypted codes (8: 16-bit,
  3: 8-bit write, 7: test equal, 9: test not equal; the "off by default" bit cleared; the other types left out).
  Games by title only: matched to the game lists' serials of the ROM's region (a "usa" file name: NTSCU, else PAL),
  every version of the title (marked: the codes may be for one of them); a title not found is kept by its title.
- GameShark CD images (.img/.bin with "GameShark" in them: the upgrade and enhancement CDs): their code lists are
  in the audio tracks, the same records many times over. Records: game name \0, cheat count, then for each cheat:
  name \0, a byte (bit 7: on at first, bits 0-6: code count), the codes (4 + 2 bytes). A name's bytes F6-FF are
  words (GS_WORDS). A record is kept when it follows another whole record (one cut by a sector boundary or read at a
  wrong offset isn't followed by one), the most frequent version of each game. The codes are GameShark's already
  (some of them types the Cheats page doesn't apply). Games by title: NTSC-U, else PAL, else NTSC-J game lists.

data/cheats.sqlite: the database the Manager comes with (game lists, POPStarter cheat files, Xploder / Xplorer and
GameShark cheat collections), bundled into the built program and copied to the settings folder at the first start.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import difflib
import os
import re
import shutil
import sqlite3
import sys

from settings import config_dir

SCHEMA = """
CREATE TABLE IF NOT EXISTS games (serial TEXT PRIMARY KEY, title TEXT, region TEXT);
CREATE TABLE IF NOT EXISTS cheats (id INTEGER PRIMARY KEY, serial TEXT NOT NULL, name TEXT NOT NULL, codes TEXT NOT NULL,
                                   source TEXT, by_title INTEGER DEFAULT 0, UNIQUE (serial, codes));
CREATE INDEX IF NOT EXISTS cheats_serial ON cheats (serial);
"""
SERIAL = re.compile(r"^([A-Z]{4})[_-]?(\d{3})\.?(\d{2})$")
POPS_RAM = 0x01000000
RAM_SIZE = 0x200000


# The database the Manager comes with (data/cheats.sqlite: in manager_files/ in the built program): copied to the
# settings folder at the first start, then the user's copy is used (and imported into)
BUNDLED = os.path.join(getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__))), "data", "cheats.sqlite")


def db_path():
    return os.path.join(config_dir(), "cheats", "cheats.sqlite")


def connect(path=None):
    path = path or db_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if path == db_path() and not os.path.exists(path) and os.path.isfile(BUNDLED):
        shutil.copyfile(BUNDLED, path)
    db = sqlite3.connect(path)
    db.executescript(SCHEMA)
    if "by_title" not in [c[1] for c in db.execute("PRAGMA table_info(cheats)")]:     # (a database of before)
        db.execute("ALTER TABLE cheats ADD COLUMN by_title INTEGER DEFAULT 0")
    return db


def norm_serial(text):
    """'SLES_015.42', 'SLES-01542', 'sles_01542' -> 'SLES_015.42' (as the disc's executable), None if not one."""
    m = SERIAL.match(text.strip().upper())
    return "%s_%s.%s" % m.groups() if m else None


def region_of(name):
    name = name.upper().replace(" ", "")
    for r in ("NTSCJ", "NTSCU", "PAL"):
        if r in name:
            return r
    return None


# ---------------------------------------------------------------- importers

def import_game_list(db, path):
    """'SERIAL**TITLE' lines. Returns the number of games."""
    region, n = region_of(os.path.basename(path)), 0
    with open(path, encoding="latin-1") as f:
        for line in f:
            serial, sep, title = line.strip().partition("**")
            serial = norm_serial(serial) if sep else None
            if serial and title:
                db.execute("INSERT INTO games (serial, title, region) VALUES (?, ?, ?) ON CONFLICT (serial) DO UPDATE "
                           "SET title = excluded.title, region = COALESCE(excluded.region, region)",
                           (serial, title.strip(), region))
                n += 1
    return n


def pops_cheat(lines):
    """The PS2 (POPS) code lines of a cheat as PS1 GameShark lines, None if one can't be turned."""
    out, cond, i = [], [], 0

    def emit(line):
        out.extend(cond[:1])        # (a GameShark test applies to the next line: repeated before each line covered)
        out.append(line)

    def ram(a):
        a -= POPS_RAM
        return a if 0 <= a < RAM_SIZE else None

    while i < len(lines):
        a, v = int(lines[i][0], 16), int(lines[i][1], 16)
        t, adrs = a >> 28, ram(a & 0x0FFFFFFF)
        i += 1
        if t in (9, 0xF):                           # The POPS hook
            continue
        if adrs is None:
            return None
        if t == 0:
            emit("30%06X 00%02X" % (adrs, v & 0xFF))
        elif t == 1:
            emit("80%06X %04X" % (adrs, v & 0xFFFF))
        elif t == 2:
            emit("80%06X %04X" % (adrs, v & 0xFFFF))
            emit("80%06X %04X" % (adrs + 2, v >> 16))
        elif t == 4 and i < len(lines):             # Slide: count, step (words), then value and increment
            count, step = v >> 16, v & 0xFFFF
            value, inc = int(lines[i][0], 16), int(lines[i][1], 16)
            i += 1
            if count > 256 or adrs + count * step * 4 > RAM_SIZE:
                return None
            for k in range(count):
                w = (value + k * inc) & 0xFFFFFFFF
                emit("80%06X %04X" % (adrs + k * step * 4, w & 0xFFFF))
                emit("80%06X %04X" % (adrs + k * step * 4 + 2, w >> 16))
        elif t == 0xD:                              # Test: nn lines, T (0 =, 1 !=, 2 <, 3 >), S (0: 16, 1: 8-bit)
            count, test, size = v >> 24, (v >> 20) & 0xF, (v >> 16) & 0xF
            if test > 3 or size > 1 or cond:
                return None
            cond[:] = ["%s%X%06X %04X" % ("E" if size else "D", test, adrs, v & (0xFF if size else 0xFFFF)), count]
        else:
            return None
        if t != 0xD and cond:                       # (the test covers this many PS2 lines)
            cond[1] -= 1
            if not cond[1]:
                cond.clear()
    return out or None


def import_cht(db, path, source="cht"):
    """A POPStarter cheat file (its name: the game's serial). Returns (cheats added, cheats left out)."""
    serial = norm_serial(os.path.basename(path)[:-4])
    if not serial:
        return 0, 0
    added = skipped = 0
    name, codes = None, []

    def done():
        nonlocal added, skipped
        if name and codes and name.upper() != "POPS ENABLE CODE":
            gs = pops_cheat(codes)
            if gs:
                cur = db.execute("INSERT OR IGNORE INTO cheats (serial, name, codes, source) VALUES (?, ?, ?, ?)",
                                 (serial, name, "\n".join(gs), source))
                added += cur.rowcount
            else:
                skipped += 1

    with open(path, encoding="latin-1") as f:
        for line in f:
            line = line.strip()
            m = re.match(r"^([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})$", line)
            if m:
                codes.append(m.groups())
            elif line:
                done()
                name, codes = line, []
        done()
    return added, skipped


def title_key(title):
    """A title compared loosely: upper case, & as AND, no punctuation or leading THE."""
    t = re.sub(r"[^A-Z0-9 ]", " ", title.upper().replace("&", " AND "))
    t = " ".join(t.split())
    return t[4:] if t.startswith("THE ") else t


def serials_by_title(db, title, region):
    """The game lists' serials of a title in a region: the same title, else the titles starting with it."""
    key = title_key(title)
    rows = db.execute("SELECT serial, title FROM games WHERE region = ? AND serial NOT LIKE '%:%'",
                      (region,)).fetchall()
    same = [s for s, t in rows if title_key(t) == key]
    if same:
        return same
    longer = [s for s, t in rows if title_key(t).startswith(key + " ")]
    if longer:
        return longer
    # Last: the closest titles (spelling, punctuation), the ones as close as the best
    keys = {}
    for s, t in rows:
        keys.setdefault(title_key(t), []).append(s)
    close = difflib.get_close_matches(key, list(keys), n=3, cutoff=0.88)
    return [s for k in close[:1] for s in keys[k]]


def xp_cheat(lines):
    """Xploder codes (decrypted) as PS1 GameShark lines, None if one can't be turned."""
    out = []
    for code, v in lines:
        b0, adrs = code >> 24, code & 0xFFFFFF
        kind = b0 >> 4                              # (bit 3: off by default, bits 0-2: the key, 0 once decrypted)
        if adrs >= RAM_SIZE:
            return None
        if kind == 8:
            out.append("80%06X %04X" % (adrs, v))
        elif kind == 3:
            out.append("30%06X 00%02X" % (adrs, v & 0xFF))
        elif kind in (7, 9):
            out.append("D%d%06X %04X" % (0 if kind == 7 else 1, adrs, v))
        else:
            return None
    return out if out and not out[-1].startswith("D") else None


def import_xp(db, path, source="xploder"):
    """An Xploder / Xplorer database printed by xpcrypt -x. Returns (cheats added, cheats left out, games not
    found in the game lists)."""
    region = "NTSCU" if "usa" in os.path.basename(path).lower() else "PAL"
    added = skipped = 0
    lost = []
    serials, name, codes = [], None, []

    def done():
        nonlocal added, skipped
        if name and codes and serials:
            gs = xp_cheat(codes)
            if not gs:
                skipped += 1
                return
            for serial, by_title in serials:
                cur = db.execute("INSERT OR IGNORE INTO cheats (serial, name, codes, source, by_title) "
                                 "VALUES (?, ?, ?, ?, ?)", (serial, name, "\n".join(gs), source, by_title))
                added += cur.rowcount

    with open(path, encoding="latin-1") as f:
        for line in f:
            line = line.strip()
            m = re.match(r"^([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{4})$", line)
            if m:
                codes.append((int(m.group(1), 16), int(m.group(2), 16)))
            elif line.startswith('"') and line.endswith('"') and len(line) > 2:
                done()
                title, name, codes = line[1:-1].strip(), None, []
                found = serials_by_title(db, title, region)
                if found:
                    serials = [(s, 1) for s in found]
                else:                               # (kept by its title: found by "find one")
                    key = "%s:%s" % (region, title)
                    db.execute("INSERT OR IGNORE INTO games (serial, title, region) VALUES (?, ?, ?)",
                               (key, title, region))
                    serials = [(key, 0)]
                    lost.append(title)
            elif line and not line.startswith("//"):
                done()
                name, codes = line, []
        done()
    return added, skipped, lost


GS_WORDS = {0xF6: "Key", 0xF7: "Have ", 0xF8: "Lives", 0xF9: "Energy", 0xFA: "Health", 0xFB: "Enable ",
            0xFC: "Unlimited ", 0xFD: "Player ", 0xFE: "Always ", 0xFF: "Infinite "}
GS_TYPES = {0x80, 0x30, 0xD0, 0xD1, 0xD2, 0xD3, 0xE0, 0xE1, 0xE2, 0xE3, 0x10, 0x11, 0x20, 0x21, 0x50, 0xC0, 0xC1,
            0xC2, 0x1F, 0x12, 0x22}
GS_NAME = re.compile(rb"[\x20-\x7E\xE0-\xFF]{1,60}\x00")
GS_START = re.compile(rb"[\x20-\x7E\xE0-\xFF]{2,60}\x00[\x01-\x7F]")


def gs_text(name):
    return " ".join("".join(GS_WORDS.get(b, chr(b)) if b >= 0xE0 else chr(b) for b in name).split())


def gs_record(d, i):
    """(title, cheats, end) of the GameShark game record at i, or None."""
    m = GS_NAME.match(d, i)
    if not m or m.end() >= len(d):
        return None
    title, j = d[i:m.end() - 1], m.end()
    count = d[j]
    j += 1
    if not 1 <= count <= 127:
        return None
    cheats = []
    for _ in range(count):
        m = GS_NAME.match(d, j)
        if not m or m.end() >= len(d):
            return None
        name, j = d[j:m.end() - 1], m.end()
        n = d[j] & 0x7F
        j += 1
        codes = []
        for _ in range(n):
            c = d[j:j + 6]
            if len(c) < 6 or c[0] not in GS_TYPES:
                return None
            codes.append("%02X%02X%02X%02X %02X%02X" % tuple(c))
            j += 6
        cheats.append((name, tuple(codes)))
    return title, tuple(cheats), j


def gameshark_games(path):
    """{title: [(name, codes)]} of a GameShark CD image, {} if it isn't one."""
    with open(path, "rb") as f:
        data = f.read()
    if b"GameShark" not in data and b"GAMESHARK" not in data:
        return {}
    votes, tried = {}, set()
    for m in GS_START.finditer(data):
        if m.start() in tried:
            continue
        rec = gs_record(data, m.start())
        if not rec:
            continue
        j = rec[2]
        while True:                                 # (the records after the first are trusted)
            tried.add(j)
            nxt = gs_record(data, j)
            if not nxt:
                break
            v = votes.setdefault(gs_text(nxt[0]), {})
            v[nxt[1]] = v.get(nxt[1], 0) + 1
            j = nxt[2]
    return {t: [(gs_text(n), c) for n, c in max(v, key=v.get) if c] for t, v in votes.items() if t}


def import_gameshark(db, path, source="gameshark"):
    """A GameShark CD image. Returns (cheats added, cheats left out, games not found), or None if it isn't one."""
    games = gameshark_games(path)
    if not games:
        return None
    added, lost = 0, []
    for title, cheats in games.items():
        serials = []
        for region in ("NTSCU", "PAL", "NTSCJ"):
            serials = serials_by_title(db, title, region)
            if serials:
                break
        if not serials:
            key = "GS:%s" % title
            db.execute("INSERT OR IGNORE INTO games (serial, title, region) VALUES (?, ?, NULL)", (key, title))
            serials = [key]
            lost.append(title)
        for serial in serials:
            for name, codes in cheats:
                cur = db.execute("INSERT OR IGNORE INTO cheats (serial, name, codes, source, by_title) VALUES "
                                 "(?, ?, ?, ?, 1)", (serial, name, "\n".join(codes), source))
                added += cur.rowcount
    return added, 0, lost


def import_folder(db, folder, log=print):
    """Every game list (.txt with SERIAL**TITLE lines), cheat file (.cht), xpcrypt database (.xp.txt) and GameShark
    CD image (.img, .bin) in a folder and its sub-folders (the game lists first: the titles are matched to them)."""
    games = cheats = skipped = 0
    lost = []
    paths = [os.path.join(root, f) for root, _dirs, files in os.walk(folder) for f in sorted(files)]
    for path in paths:
        f = path.lower()
        if f.endswith(".txt") and not f.endswith(".xp.txt"):
            games += import_game_list(db, path)
    for path in paths:
        f = path.lower()
        if f.endswith(".cht"):
            a, s = import_cht(db, path)
            cheats, skipped = cheats + a, skipped + s
        elif f.endswith(".xp.txt"):
            a, s, n = import_xp(db, path)
            cheats, skipped, lost = cheats + a, skipped + s, lost + n
        elif f.endswith((".img", ".bin")):
            res = import_gameshark(db, path)
            if res:
                cheats, lost = cheats + res[0], lost + res[2]
                db.commit()
    db.commit()
    log("%s: %d games, %d cheats added%s%s." % (
        folder, games, cheats, ", %d left out (codes that can't be used)" % skipped if skipped else "",
        ", %d titles not in the game lists (kept by title): %s" % (len(lost), ", ".join(lost[:12]) +
                                                                     ("..." if len(lost) > 12 else "")) if lost else ""))
    return games, cheats, skipped


# ---------------------------------------------------------------- the cartridge's cheats (on-console cheat screen)

ROM_CHEATS = 0xB000             # In the firmware ROM (cartrom/rom.s, cheatmenu.h: CHEAT_DB_ROM 0x1F00B000)
ROM_CHEATS_SIZE = 0x5000
ROM_SIZE = 128 * 1024
ENGINE_TYPES = {0x80, 0x30, 0xD0, 0xD1, 0xD2, 0xD3, 0xE0, 0xE1, 0xE2, 0xE3, 0x50, 0xC0}


def rom_cheats(db, serials, size=ROM_CHEATS_SIZE):
    """The cheats of games for the cartridge's flash (cheatmenu.h): the ones the code engine does, the games in the
    order given while they fit. Returns (bytes, [serials put in], [serials left out: no room])."""
    body, put, left = bytearray(), [], []
    for serial in serials:
        title = (game(db, serial)[0] or serial).upper()
        rec = bytearray(serial.encode("ascii", "replace")[:12].ljust(12, b"\0"))
        rec += title.encode("ascii", "replace")[:31].ljust(32, b"\0")
        cheats = bytearray()
        n = 0
        for _id, name, codes, _by_title in cheats_of(db, serial):
            lines = []
            for line in codes.split("\n"):
                m = re.match(r"^([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{4})$", line.strip())
                if not m:
                    break
                lines.append((int(m.group(1), 16), int(m.group(2), 16)))
            else:
                if not lines or len(lines) > 255 or any(c >> 24 not in ENGINE_TYPES for c, _v in lines):
                    continue
                text = name.encode("ascii", "replace")[:40]
                cheats += bytes([len(text)]) + text + bytes([len(lines)])
                cheats += b"".join(c.to_bytes(4, "little") + v.to_bytes(2, "little") for c, v in lines)
                n += 1
                if n == 64:                         # (the cheat screen lists 64 at most)
                    break
        if not n:
            continue
        rec += n.to_bytes(2, "little") + cheats
        if 12 + len(body) + len(rec) > size:
            left.append(serial)
            continue
        body += rec
        put.append(serial)
    head = b"CHDB" + (1).to_bytes(2, "little") + len(put).to_bytes(2, "little") + (12 + len(body)).to_bytes(4, "little")
    return bytes(head + body) if put else bytes(16), put, left


def read_rom_cheats(blob):
    """The games of a cartridge's cheats area (rom_cheats' format): [(serial, title, cheats, bytes)], [] if empty."""
    if len(blob) < 12 or blob[:4] != b"CHDB":
        return []
    games, size = int.from_bytes(blob[6:8], "little"), min(int.from_bytes(blob[8:12], "little"), len(blob))
    out, i = [], 12
    for _ in range(games):
        if i + 46 > size:
            break
        serial = blob[i:i + 12].split(b"\0")[0].decode("ascii", "replace")
        title = blob[i + 12:i + 44].split(b"\0")[0].decode("ascii", "replace")
        n, start = int.from_bytes(blob[i + 44:i + 46], "little"), i
        i += 46
        for _ in range(n):
            if i + 2 > size:
                break
            i += 1 + blob[i]                        # Name
            i += 1 + blob[i] * 6                    # Codes
        out.append((serial, title, n, i - start))
    return out


def rom_with_cheats(rom, blob):
    """A firmware ROM image with these cheats in its cheats area (the rest as it is)."""
    if len(rom) != ROM_SIZE or len(blob) > ROM_CHEATS_SIZE:
        raise ValueError("not a 128 KB firmware ROM, or too many cheats")
    area = blob.ljust(ROM_CHEATS_SIZE, b"\0")
    return rom[:ROM_CHEATS] + area + rom[ROM_CHEATS + ROM_CHEATS_SIZE:]


# ---------------------------------------------------------------- queries

def game(db, serial):
    """(title, region) of a game, or (None, None)."""
    row = db.execute("SELECT title, region FROM games WHERE serial = ?", (serial,)).fetchone()
    return row or (None, None)


def cheats_of(db, serial):
    """[(id, name, codes, by_title)] of a game, in their file's order (by_title: matched by the title only)."""
    return db.execute("SELECT id, name, codes, by_title FROM cheats WHERE serial = ? ORDER BY id", (serial,)).fetchall()


def search_games(db, text, limit=200):
    """Games whose title or serial has the text, those with cheats first: [(serial, title, region, cheats)]."""
    like = "%" + text.strip() + "%"
    return db.execute("SELECT g.serial, g.title, g.region, COUNT(c.id) AS n FROM games g LEFT JOIN cheats c "
                      "ON c.serial = g.serial WHERE g.title LIKE ? OR g.serial LIKE ? GROUP BY g.serial "
                      "ORDER BY n = 0, g.title LIMIT ?", (like, like, limit)).fetchall()


def counts(db):
    return (db.execute("SELECT COUNT(*) FROM games").fetchone()[0],
            db.execute("SELECT COUNT(*) FROM cheats").fetchone()[0],
            db.execute("SELECT COUNT(DISTINCT serial) FROM cheats").fetchone()[0])


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "import":
        db = connect()
        for folder in sys.argv[2:]:
            import_folder(db, folder)
        print("Database %s: %d games, %d cheats for %d games." % ((db_path(),) + counts(db)))
    elif len(sys.argv) == 3 and sys.argv[1] == "game":
        db = connect()
        serial = norm_serial(sys.argv[2]) or sys.argv[2]
        print(serial, *game(db, serial))
        for _id, name, codes, by_title in cheats_of(db, serial):
            print("  " + name + (" (matched by title)" if by_title else ""))
            for line in codes.split("\n"):
                print("    " + line)
    else:
        print(__doc__)
