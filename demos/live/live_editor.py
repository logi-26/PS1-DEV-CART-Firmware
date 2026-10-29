#!/usr/bin/env python3
"""
PS1 LIVE editor: edits the level of live.ps-exe while it runs on the PlayStation. Every change is on the TV at once.

    python live_editor.py          (editor.bat)

The level is one block in the PlayStation's RAM (live.h): the editor reads it when it connects, then writes only the
bytes that change, through the PS1 DEV CART ("ps1transfer -memlive": the running program's memory read and written
while it runs) or PCSX-Redux's GDB server. Ten times a second it reads where the player and the enemies are, for the
map. The tile set is a PNG (256 x 16: 16 tiles of 16 x 16, the player is the 9th, the enemy the 10th): saved from any
paint program, it is sent at once (hot reload).

Only Python 3 with Tkinter (part of the usual Windows installer).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""

import base64
import json
import os
import queue
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import tkinter as tk
from tkinter import colorchooser, filedialog, messagebox, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
PS1TRANSFER = os.path.join(HERE, "..", "..", "ps1transfer", "ps1transfer.exe" if os.name == "nt" else "ps1transfer")
GAME = os.path.join(HERE, "live.ps-exe")

# live.h
LEVEL_ADRS, STATE_ADRS = 0x801C0000, 0x801BF000
MAGIC = 0x4556494C
W, H, TILE, ENEMIES, TYPES = 64, 14, 16, 16, 8
SHEET_W, SHEET_H = 256, 16
OFF_VERSION, OFF_TILES, OFF_START, OFF_ECOUNT, OFF_ENEMIES = 4, 8, 904, 908, 912
OFF_SKY_TOP, OFF_SKY_BOTTOM, OFF_PLAYER_TINT, OFF_TINT = 1104, 1108, 1112, 1116
OFF_PHYSICS, OFF_TILES_VERSION, OFF_SHEET = 1148, 1156, 1160
LEVEL_SIZE = OFF_SHEET + SHEET_W * SHEET_H * 2
STATE_SIZE = 56
TILE_NAMES = ["Empty", "Ground", "Brick", "Block", "Spikes", "Coin", "Goal", "Spring"]
SPRITE_PLAYER, SPRITE_ENEMY = 8, 9
PHYSICS = [("Gravity", 10, 300), ("Jump", 400, 3000), ("Run speed", 128, 1024), ("Fastest fall", 400, 3000)]


# ---------------------------------------------------------------------------------------------------------------------
# Connections: the PS1 DEV CART (ps1transfer -memlive) or PCSX-Redux (GDB server)

class CartLink:
    """ps1transfer -memlive: "read ADDR SIZE" / "write ADDR HEX" on its input, the answers on its output."""
    name = "PS1 DEV CART"

    def __init__(self, serial=""):
        args = [PS1TRANSFER] + (["-n", serial] if serial else []) + ["-memlive"]
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, bufsize=1, creationflags=flags)
        self.answers = queue.Queue()
        self.alive = True
        threading.Thread(target=self._reader, daemon=True).start()
        line = self.answers.get(timeout=10)
        if not line.startswith("MEMLIVE ready"):
            raise OSError(line)

    def _reader(self):
        for line in self.p.stdout:
            line = line.strip()
            if line.startswith(("MEMREAD", "MEMLIVE", "MEMERR", "MEMSCREEN")):
                self.answers.put(line)
            if line.startswith("MEMLIVE disconnected"):
                self.alive = False
        self.alive = False
        self.answers.put("MEMLIVE disconnected")

    def write(self, adrs, data):
        for i in range(0, len(data), 2048):
            self.p.stdin.write("write %08X %s\n" % (adrs + i, data[i:i + 2048].hex().upper()))
        self.p.stdin.flush()

    def read(self, adrs, size):
        out = b""
        for i in range(0, size, 4096):
            n = min(4096, size - i)
            self.p.stdin.write("read %08X %d\n" % (adrs + i, n))
            self.p.stdin.flush()
            while True:
                line = self.answers.get(timeout=5)
                if line.startswith("MEMREAD"):
                    out += bytes.fromhex(line.split()[2])
                    break
                if line.startswith(("MEMERR", "MEMLIVE disconnected")):
                    raise OSError(line)
        return out

    def screen(self):
        """The frame being drawn on the PlayStation: (width, height, 16-bit pixels)."""
        path = os.path.join(tempfile.gettempdir(), "ps1live_screen.bin")
        self.p.stdin.write("screen %s\n" % path)
        self.p.stdin.flush()
        while True:
            line = self.answers.get(timeout=5)
            if line.startswith("MEMSCREEN"):
                w, h = int(line.split()[3]), int(line.split()[4])
                with open(path, "rb") as f:
                    return w, h, f.read()
            if line.startswith(("MEMERR", "MEMLIVE disconnected")):
                raise OSError(line)

    def close(self):
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
            self.p.wait(3)
        except Exception:      # noqa: BLE001
            self.p.kill()


class ReduxLink:
    """PCSX-Redux's GDB server: the emulator paused for a moment, memory read/written, running again."""
    name = "PCSX-Redux"

    def __init__(self, port):
        for _ in range(40):
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                time.sleep(0.5)
        else:
            raise OSError("PCSX-Redux's GDB server doesn't answer on port %d" % port)
        self.buf = b""
        self.alive = True
        self.lock = threading.Lock()
        self._command(b"?")
        self._continue()

    def _packet(self):
        while True:
            i = self.buf.find(b"$")
            j = self.buf.find(b"#", i)
            if i >= 0 and j >= 0 and len(self.buf) >= j + 3:
                body, self.buf = self.buf[i + 1:j], self.buf[j + 3:]
                self.s.sendall(b"+")
                return body
            data = self.s.recv(65536)
            if not data:
                self.alive = False
                raise OSError("PCSX-Redux closed")
            self.buf += data

    def _command(self, body):
        self.s.sendall(b"$" + body + b"#%02x" % (sum(body) & 255))
        return self._packet()

    def _continue(self):
        self.s.sendall(b"$c#63")

    def _pause(self):
        self.s.sendall(b"\x03")
        while self._packet()[:1] not in (b"S", b"T"):
            pass

    def write(self, adrs, data):
        with self.lock:
            self._pause()
            try:
                for i in range(0, len(data), 1024):
                    part = data[i:i + 1024]
                    self._command(b"M%x,%x:%s" % (adrs + i, len(part), part.hex().encode()))
            finally:
                self._continue()

    def read(self, adrs, size):
        with self.lock:
            self._pause()
            try:
                out = b""
                for i in range(0, size, 1024):
                    out += bytes.fromhex(self._command(b"m%x,%x" % (adrs + i, min(1024, size - i))).decode())
                return out
            finally:
                self._continue()

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def find_redux():
    """PCSX-Redux: PCSX_REDUX, on the PATH, or the copy of the PSX.Dev extension for VS Code."""
    for path in (os.environ.get("PCSX_REDUX"), shutil.which("pcsx-redux"),
                 os.path.join(os.environ.get("APPDATA", ""), "Code", "User", "globalStorage", "grumpycoders.psx-dev",
                              "pcsx-redux", "pcsx-redux.exe")):
        if path and os.path.exists(path):
            return path
    return None


def start_redux(redux, gdb_port):
    """PCSX-Redux with the game and its GDB server, with a copy of your usual settings (.redux next to this file)."""
    base = os.path.join(os.path.dirname(redux), "pcsx.json")
    if not os.path.exists(base):
        base = os.path.join(os.environ.get("APPDATA", ""), "pcsx-redux", "pcsx.json")
    settings = json.load(open(base)) if os.path.exists(base) else {}
    debug = settings.setdefault("emulator", {}).setdefault("Debug", {})
    debug.update({"GdbServer": True, "GdbServerPort": gdb_port, "Debug": True})
    folder = os.path.join(HERE, ".redux")
    os.makedirs(folder, exist_ok=True)
    json.dump(settings, open(os.path.join(folder, "pcsx.json"), "w"), indent=1)
    return subprocess.Popen([redux, "-portable", folder, "-loadexe", GAME, "-run", "-gdb", "-gdb-port", str(gdb_port)],
                            cwd=folder)


# ---------------------------------------------------------------------------------------------------------------------
# The level

class Level:
    def __init__(self, data):
        self.b = bytearray(data)

    def tile(self, x, y):
        return self.b[OFF_TILES + y * W + x]

    def set_tile(self, x, y, t):
        self.b[OFF_TILES + y * W + x] = t

    def start(self):
        return struct.unpack_from("<hh", self.b, OFF_START)

    def set_start(self, x, y):
        struct.pack_into("<hh", self.b, OFF_START, x, y)

    def enemies(self):
        n = min(self.b[OFF_ECOUNT], ENEMIES)
        return [list(struct.unpack_from("<hhhhBBBx", self.b, OFF_ENEMIES + i * 12)) for i in range(n)]

    def set_enemies(self, enemies):
        self.b[OFF_ECOUNT] = len(enemies)
        for i in range(ENEMIES):
            e = enemies[i] if i < len(enemies) else [0, 0, 0, 0, 128, 128, 128]
            struct.pack_into("<hhhhBBBx", self.b, OFF_ENEMIES + i * 12, *e)

    def colour(self, off):
        return tuple(self.b[off:off + 3])

    def set_colour(self, off, rgb):
        self.b[off:off + 3] = bytes(rgb)

    def physics(self):
        return list(struct.unpack_from("<hhhh", self.b, OFF_PHYSICS))

    def set_physics(self, i, v):
        struct.pack_into("<h", self.b, OFF_PHYSICS + i * 2, v)

    def sheet_pixel(self, x, y):
        return struct.unpack_from("<H", self.b, OFF_SHEET + (y * SHEET_W + x) * 2)[0]

    def set_sheet(self, pixels):
        self.b[OFF_SHEET:OFF_SHEET + len(pixels)] = pixels

    def to_json(self):
        return {"tiles": [bytes(self.b[OFF_TILES + y * W:OFF_TILES + (y + 1) * W]).hex() for y in range(H)],
                "start": self.start(), "enemies": self.enemies(),
                "sky_top": self.colour(OFF_SKY_TOP), "sky_bottom": self.colour(OFF_SKY_BOTTOM),
                "player_tint": self.colour(OFF_PLAYER_TINT),
                "tints": [self.colour(OFF_TINT + i * 4) for i in range(TYPES)], "physics": self.physics(),
                "sheet": base64.b64encode(bytes(self.b[OFF_SHEET:])).decode()}

    def from_json(self, j):
        for y, row in enumerate(j["tiles"][:H]):
            self.b[OFF_TILES + y * W:OFF_TILES + y * W + W] = bytes.fromhex(row)[:W]
        self.set_start(*j["start"])
        self.set_enemies(j["enemies"])
        self.set_colour(OFF_SKY_TOP, j["sky_top"])
        self.set_colour(OFF_SKY_BOTTOM, j["sky_bottom"])
        self.set_colour(OFF_PLAYER_TINT, j["player_tint"])
        for i, c in enumerate(j["tints"][:TYPES]):
            self.set_colour(OFF_TINT + i * 4, c)
        for i, v in enumerate(j["physics"][:4]):
            self.set_physics(i, v)
        self.set_sheet(base64.b64decode(j["sheet"]))


def rgb15(v):
    return ((v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3)


def hexcolour(rgb):
    return "#%02x%02x%02x" % tuple(max(0, min(255, int(c))) for c in rgb)


# ---------------------------------------------------------------------------------------------------------------------
# The editor

class Editor:
    def __init__(self, root):
        self.root = root
        self.link = None
        self.level = None
        self.sent = None                # What the PlayStation has
        self.state = None
        self.selected_enemy = None
        self.drag = None
        self.push_pending = False
        self.watch_file = None
        self.watch_mtime = 0
        self.redux = None
        root.title("PS1 LIVE editor")
        root.protocol("WM_DELETE_WINDOW", self.quit)
        self.build()
        self.root.after(100, self.poll)
        self.root.after(500, self.watch)

    # -- Window -------------------------------------------------------------------------------------------------------
    def build(self):
        top = ttk.Frame(self.root, padding=6)
        top.pack(fill="x")
        ttk.Button(top, text="Start on the PS1 DEV CART", command=self.start_cart).pack(side="left")
        ttk.Button(top, text="Attach (game running)", command=self.attach_cart).pack(side="left", padx=4)
        ttk.Label(top, text="Cartridge:").pack(side="left", padx=(8, 2))
        self.serial = tk.StringVar()
        ttk.Entry(top, textvariable=self.serial, width=10).pack(side="left")
        ttk.Button(top, text="Start in PCSX-Redux", command=self.start_redux).pack(side="left", padx=(12, 4))
        ttk.Button(top, text="Save level...", command=self.save).pack(side="right")
        ttk.Button(top, text="Load level...", command=self.load).pack(side="right", padx=4)
        self.status = tk.StringVar(value="Not connected: start the game, or attach to it")
        ttk.Label(self.root, textvariable=self.status, padding=(8, 0)).pack(fill="x")

        mapframe = ttk.Frame(self.root, padding=6)
        mapframe.pack(fill="x")
        self.canvas = tk.Canvas(mapframe, width=W * TILE, height=H * TILE, bg="#1c2030", highlightthickness=0,
                                scrollregion=(0, 0, W * TILE, H * TILE))
        bar = ttk.Scrollbar(mapframe, orient="horizontal", command=self.canvas.xview)
        self.canvas.configure(xscrollcommand=bar.set)
        self.canvas.pack(fill="x")
        bar.pack(fill="x")
        for ev in ("<Button-1>", "<B1-Motion>"):
            self.canvas.bind(ev, self.left)
        self.canvas.bind("<ButtonRelease-1>", lambda e: setattr(self, "drag", None))
        for ev in ("<Button-3>", "<B3-Motion>"):
            self.canvas.bind(ev, self.right)

        panels = ttk.Frame(self.root, padding=6)
        panels.pack(fill="both", expand=True)

        tools = ttk.LabelFrame(panels, text="Draw", padding=6)
        tools.pack(side="left", fill="y")
        self.tool = tk.StringVar(value="tile")
        ttk.Radiobutton(tools, text="Tiles (right button: erase)", variable=self.tool, value="tile").pack(anchor="w")
        ttk.Radiobutton(tools, text="Enemies (click: add or pick, drag: move, right: delete)", variable=self.tool,
                        value="enemy").pack(anchor="w")
        ttk.Radiobutton(tools, text="Player start", variable=self.tool, value="start").pack(anchor="w")
        self.palette = ttk.Frame(tools)
        self.palette.pack(anchor="w", pady=6)
        self.tile_choice = tk.IntVar(value=1)

        enemy = ttk.LabelFrame(panels, text="Selected enemy", padding=6)
        enemy.pack(side="left", fill="y", padx=6)
        self.enemy_range = tk.IntVar(value=4)
        self.enemy_speed = tk.IntVar(value=128)
        ttk.Label(enemy, text="Patrol (tiles)").pack(anchor="w")
        ttk.Scale(enemy, from_=0, to=20, variable=self.enemy_range, command=lambda v: self.enemy_changed()).pack(fill="x")
        ttk.Label(enemy, text="Speed").pack(anchor="w")
        ttk.Scale(enemy, from_=0, to=768, variable=self.enemy_speed, command=lambda v: self.enemy_changed()).pack(fill="x")
        ttk.Button(enemy, text="Colour...", command=self.enemy_colour).pack(anchor="w", pady=4)

        colours = ttk.LabelFrame(panels, text="Colours", padding=6)
        colours.pack(side="left", fill="y")
        self.swatches = {}
        names = [("Sky top", OFF_SKY_TOP), ("Sky bottom", OFF_SKY_BOTTOM), ("Player tint", OFF_PLAYER_TINT)] + \
            [("%s tint" % TILE_NAMES[i], OFF_TINT + i * 4) for i in range(1, TYPES)]
        for i, (name, off) in enumerate(names):
            b = tk.Button(colours, text=name, width=13, command=lambda o=off, n=name: self.pick_colour(o, n))
            b.grid(row=i % 5, column=i // 5, sticky="w", padx=2, pady=1)
            self.swatches[off] = b

        phys = ttk.LabelFrame(panels, text="Physics", padding=6)
        phys.pack(side="left", fill="y", padx=6)
        self.phys = []
        for i, (name, lo, hi) in enumerate(PHYSICS):
            v = tk.IntVar()
            ttk.Label(phys, text=name).pack(anchor="w")
            ttk.Scale(phys, from_=lo, to=hi, variable=v, length=160,
                      command=lambda val, i=i, v=v: self.physics_changed(i, v)).pack(fill="x")
            self.phys.append(v)

        sheet = ttk.LabelFrame(panels, text="Tile graphics (hot reload)", padding=6)
        sheet.pack(side="left", fill="y")
        ttk.Button(sheet, text="Export the tile set PNG...", command=self.export_sheet).pack(fill="x")
        ttk.Button(sheet, text="Load a tile set PNG...", command=self.load_sheet).pack(fill="x", pady=4)
        self.watching = tk.StringVar(value="Edit the PNG in any paint program: each save is sent at once")
        ttk.Label(sheet, textvariable=self.watching, wraplength=220).pack(fill="x")
        self.sheet_preview = ttk.Label(sheet)
        self.sheet_preview.pack(pady=4)

        tv = ttk.LabelFrame(panels, text="The TV", padding=6)
        tv.pack(side="left", fill="y", padx=6)
        self.show_tv = tk.BooleanVar(value=False)
        ttk.Checkbutton(tv, text="Show the TV here (PS1 DEV CART, every 2 s)", variable=self.show_tv).pack(anchor="w")
        self.tv_image = tk.PhotoImage(width=160, height=120)
        ttk.Label(tv, image=self.tv_image).pack()
        self.root.after(2000, self.tv)

    # -- Connection ---------------------------------------------------------------------------------------------------
    def connect(self, link, send_ours):
        if self.link:
            self.link.close()
        self.link = link
        try:
            if send_ours and self.level:        # (the game started again: our level, all of it)
                self.sent = bytearray(link.read(LEVEL_ADRS, LEVEL_SIZE))
                self.push(force=True)
            else:
                data = link.read(LEVEL_ADRS, LEVEL_SIZE)
                if struct.unpack_from("<I", data, 0)[0] != MAGIC:
                    raise OSError("no PS1 LIVE level in the PlayStation's memory (is live.ps-exe running?)")
                self.level = Level(data)
                self.sent = bytearray(data)
        except (OSError, queue.Empty) as e:
            self.status.set("Can't read the level: %s" % e)
            self.link.close()
            self.link = None
            return
        self.status.set("Connected (%s): every change is on the TV at once" % link.name)
        self.refresh_panels()
        self.redraw()

    def start_cart(self):
        args = [PS1TRANSFER] + (["-n", self.serial.get()] if self.serial.get() else []) + ["-r", "-e", "-f", GAME]
        self.status.set("Starting the game on the PlayStation...")
        self.root.update()
        if self.link:
            self.link.close()
            self.link = None
        r = subprocess.run(args, capture_output=True, text=True,
                           creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        if "Executing" not in r.stdout:
            self.status.set("Couldn't start it: %s" % (r.stdout.strip().splitlines() or ["?"])[-1])
            return
        time.sleep(1)
        self.attach_cart(send_ours=True)

    def attach_cart(self, send_ours=False):
        try:
            self.connect(CartLink(self.serial.get()), send_ours)
        except (OSError, queue.Empty) as e:
            self.status.set("Can't attach: %s" % e)

    def start_redux(self):
        redux = find_redux() or filedialog.askopenfilename(title="Where is pcsx-redux.exe?",
                                                           filetypes=[("PCSX-Redux", "pcsx-redux.exe")])
        if not redux:
            return
        if self.link:
            self.link.close()
            self.link = None
        if self.redux and self.redux.poll() is None:
            self.redux.kill()
        self.status.set("Starting PCSX-Redux...")
        self.root.update()
        self.redux = start_redux(redux, 3334)
        time.sleep(3)
        try:
            self.connect(ReduxLink(3334), send_ours=True)
        except OSError as e:
            self.status.set("Can't connect to PCSX-Redux: %s" % e)

    # -- Sending -------------------------------------------------------------------------------------------------------
    def changed(self):
        """A change: sent a moment later (a drag sends once a frame or so, not each mouse move)."""
        self.redraw()
        if self.link and not self.push_pending:
            self.push_pending = True
            self.root.after(40, self.push)

    def push(self, force=False):
        self.push_pending = False
        if not (self.link and self.level):
            return
        b, s = self.level.b, self.sent
        try:
            # The bytes that changed, in runs (gaps of up to 16 unchanged bytes inside a run: fewer writes); all of
            # them when forced. The level's data, and the tile set, without the two versions
            runs = []
            for lo, hi in ((OFF_TILES, OFF_TILES_VERSION), (OFF_SHEET, LEVEL_SIZE)):
                if force:
                    runs.append((lo, hi))
                    continue
                i = lo
                while i < hi:
                    if b[i] == s[i]:
                        i += 1
                        continue
                    last = j = i
                    while j < hi and j - last <= 16:
                        if b[j] != s[j]:
                            last = j
                        j += 1
                    runs.append((i, last + 1))
                    i = last + 1
            sheet = any(lo >= OFF_SHEET for lo, _ in runs)
            for lo, hi in runs:
                self.link.write(LEVEL_ADRS + lo, bytes(b[lo:hi]))
            if sheet:   # The tile set's pixels, then its version: the game uploads them to VRAM
                tv = struct.unpack_from("<I", s, OFF_TILES_VERSION)[0] + 1
                struct.pack_into("<I", b, OFF_TILES_VERSION, tv)
                self.link.write(LEVEL_ADRS + OFF_TILES_VERSION, bytes(b[OFF_TILES_VERSION:OFF_TILES_VERSION + 4]))
            if runs:    # Last: the version (the enemies start again from their places)
                v = struct.unpack_from("<I", s, OFF_VERSION)[0] + 1
                struct.pack_into("<I", b, OFF_VERSION, v)
                self.link.write(LEVEL_ADRS + OFF_VERSION, bytes(b[OFF_VERSION:OFF_VERSION + 4]))
            self.sent = bytearray(b)
        except OSError as e:
            self.status.set("Lost the connection: %s" % e)
            self.link = None

    # -- Reading the game ----------------------------------------------------------------------------------------------
    def poll(self):
        if self.link:
            try:
                data = self.link.read(STATE_ADRS, STATE_SIZE)
                if struct.unpack_from("<I", data, 0)[0] == MAGIC:
                    self.state = data
                    px, py, coins, deaths, wins = struct.unpack_from("<hhHHH", data, 8)
                    self.status.set("Connected (%s): every change is on the TV at once.   Player at %d, %d   coins %d"
                                    "   deaths %d   goals %d" % (self.link.name, px, py, coins, deaths, wins))
                    self.draw_actors()
            except (OSError, queue.Empty) as e:
                self.status.set("Lost the connection: %s" % e)
                self.link.close()
                self.link = None
        self.root.after(100, self.poll)

    def tv(self):
        """The PlayStation's picture in the editor, at half size (read from VRAM while the game runs)."""
        if self.show_tv.get() and isinstance(self.link, CartLink):
            try:
                w, h, pixels = self.link.screen()
                rows = []
                for y in range(0, min(h, 240), 2):
                    row = struct.unpack_from("<%dH" % w, pixels, y * w * 2)
                    rows.append("{" + " ".join(hexcolour(rgb15(v)) for v in row[0:w:2]) + "}")
                self.tv_image.put(" ".join(rows))
            except (OSError, queue.Empty, struct.error):
                pass
        self.root.after(2000, self.tv)

    # -- Drawing the map ----------------------------------------------------------------------------------------------
    def tile_images(self):
        """Each tile set entry as an image, tinted like on the PlayStation, transparent pixels as the sky."""
        sky = self.level.colour(OFF_SKY_BOTTOM)
        self.images = []
        for e in range(16):
            if e < TYPES:
                tint = self.level.colour(OFF_TINT + e * 4)
            elif e == SPRITE_PLAYER:
                tint = self.level.colour(OFF_PLAYER_TINT)
            else:
                tint = (128, 128, 128)
            img = tk.PhotoImage(width=16, height=16)
            rows = []
            for y in range(16):
                row = []
                for x in range(16):
                    v = self.level.sheet_pixel(e * 16 + x, y)
                    c = sky if v == 0 else [min(255, ch * t // 128) for ch, t in zip(rgb15(v), tint)]
                    row.append(hexcolour(c))
                rows.append("{" + " ".join(row) + "}")
            img.put(" ".join(rows))
            self.images.append(img)

    def redraw(self):
        if not self.level:
            return
        c = self.canvas
        c.delete("all")
        self.tile_images()
        top, bottom = self.level.colour(OFF_SKY_TOP), self.level.colour(OFF_SKY_BOTTOM)
        for y in range(H):
            k = y / (H - 1)
            c.create_rectangle(0, y * TILE, W * TILE, (y + 1) * TILE, width=0,
                               fill=hexcolour([a + (b - a) * k for a, b in zip(top, bottom)]))
        for y in range(H):
            for x in range(W):
                t = self.level.tile(x, y)
                if 0 < t < TYPES:
                    c.create_image(x * TILE, y * TILE, image=self.images[t], anchor="nw")
        sx, sy = self.level.start()
        c.create_rectangle(sx, sy, sx + 12, sy + 14, outline="#ffffff", dash=(2, 2))
        c.create_text(sx + 6, sy - 6, text="START", fill="#ffffff", font=("Segoe UI", 7))
        for i, e in enumerate(self.level.enemies()):
            x, y, rng = e[0], e[1], e[2]
            c.create_line(x, y + 15, x + rng + 16, y + 15, fill="#ff80ff", width=2)
            c.create_rectangle(x, y, x + 16, y + 16, outline="#ffff00" if i == self.selected_enemy else "#ff80ff")
        self.draw_actors()
        # The palette and the tile set preview
        for w in self.palette.winfo_children():
            w.destroy()
        for t in range(1, TYPES):
            ttk.Radiobutton(self.palette, image=self.images[t], text=TILE_NAMES[t], compound="left",
                            variable=self.tile_choice, value=t).grid(row=(t - 1) % 4, column=(t - 1) // 4, sticky="w")
        self.sheet_image = tk.PhotoImage(width=SHEET_W, height=SHEET_H)
        rows = ["{" + " ".join(hexcolour(rgb15(self.level.sheet_pixel(x, y))) for x in range(SHEET_W)) + "}"
                for y in range(SHEET_H)]
        self.sheet_image.put(" ".join(rows))
        self.sheet_preview.configure(image=self.sheet_image)

    def draw_actors(self):
        """Where the player and the enemies are in the game now (read from the PlayStation)."""
        c = self.canvas
        c.delete("live")
        if not (self.state and self.level):
            return
        px, py = struct.unpack_from("<hh", self.state, 8)
        c.create_image(px - 2, py - 2, image=self.images[SPRITE_PLAYER], anchor="nw", tags="live")
        xs = struct.unpack_from("<%dh" % ENEMIES, self.state, 20)
        for i, e in enumerate(self.level.enemies()):
            c.create_image(xs[i], e[1], image=self.images[SPRITE_ENEMY], anchor="nw", tags="live")

    # -- Mouse ---------------------------------------------------------------------------------------------------------
    def at(self, ev):
        x, y = int(self.canvas.canvasx(ev.x)), int(self.canvas.canvasy(ev.y))
        return x, y, min(W - 1, max(0, x // TILE)), min(H - 1, max(0, y // TILE))

    def left(self, ev):
        if not self.level:
            return
        x, y, tx, ty = self.at(ev)
        tool = self.tool.get()
        if tool == "tile":
            if self.level.tile(tx, ty) != self.tile_choice.get():
                self.level.set_tile(tx, ty, self.tile_choice.get())
                self.changed()
        elif tool == "start":
            self.level.set_start(tx * TILE + 2, ty * TILE + 2)
            self.changed()
        else:
            enemies = self.level.enemies()
            if self.drag is not None:       # Moving the one picked
                e = enemies[self.drag]
                e[0], e[1] = tx * TILE, ty * TILE
                self.level.set_enemies(enemies)
                self.changed()
                return
            for i, e in enumerate(enemies):
                if e[0] <= x < e[0] + 16 and e[1] <= y < e[1] + 16:
                    self.select_enemy(i)
                    self.drag = i
                    return
            if len(enemies) < ENEMIES and ev.type == tk.EventType.ButtonPress:
                enemies.append([tx * TILE, ty * TILE, 4 * TILE, 128, 128, 128, 128])
                self.level.set_enemies(enemies)
                self.select_enemy(len(enemies) - 1)
                self.drag = len(enemies) - 1
                self.changed()

    def right(self, ev):
        if not self.level:
            return
        x, y, tx, ty = self.at(ev)
        if self.tool.get() == "tile":
            if self.level.tile(tx, ty):
                self.level.set_tile(tx, ty, 0)
                self.changed()
        elif self.tool.get() == "enemy" and ev.type == tk.EventType.ButtonPress:
            enemies = self.level.enemies()
            for i, e in enumerate(enemies):
                if e[0] <= x < e[0] + 16 and e[1] <= y < e[1] + 16:
                    del enemies[i]
                    self.level.set_enemies(enemies)
                    self.selected_enemy = None
                    self.changed()
                    return

    # -- Panels --------------------------------------------------------------------------------------------------------
    def refresh_panels(self):
        for i, v in enumerate(self.level.physics()):
            self.phys[i].set(v)
        for off, b in self.swatches.items():
            c = self.level.colour(off)
            b.configure(bg=hexcolour(c), fg="#000000" if sum(c) > 300 else "#ffffff")

    def select_enemy(self, i):
        self.selected_enemy = i
        e = self.level.enemies()[i]
        self.enemy_range.set(e[2] // TILE)
        self.enemy_speed.set(e[3])
        self.redraw()

    def enemy_changed(self):
        if self.level and self.selected_enemy is not None:
            enemies = self.level.enemies()
            if self.selected_enemy < len(enemies):
                e = enemies[self.selected_enemy]
                e[2], e[3] = int(self.enemy_range.get()) * TILE, int(self.enemy_speed.get())
                self.level.set_enemies(enemies)
                self.changed()

    def enemy_colour(self):
        if self.level and self.selected_enemy is not None:
            enemies = self.level.enemies()
            e = enemies[self.selected_enemy]
            rgb, _ = colorchooser.askcolor(hexcolour([c * 2 for c in e[4:7]]), title="Enemy colour")
            if rgb:
                e[4:7] = [min(255, int(c) // 2) for c in rgb]     # (tint: 128 = as drawn)
                self.level.set_enemies(enemies)
                self.changed()

    def pick_colour(self, off, name):
        if not self.level:
            return
        tint = off != OFF_SKY_TOP and off != OFF_SKY_BOTTOM
        cur = self.level.colour(off)
        rgb, _ = colorchooser.askcolor(hexcolour([c * 2 for c in cur] if tint else cur), title=name)
        if rgb:
            self.level.set_colour(off, [min(255, int(c) // 2) if tint else int(c) for c in rgb])
            self.refresh_panels()
            self.changed()

    def physics_changed(self, i, v):
        if self.level and self.level.physics()[i] != int(v.get()):
            self.level.set_physics(i, int(v.get()))
            self.changed()

    # -- Tile set (hot reload) -------------------------------------------------------------------------------------------
    def export_sheet(self):
        if not self.level:
            return
        path = filedialog.asksaveasfilename(defaultextension=".png", initialfile="tiles.png",
                                            filetypes=[("PNG", "*.png")])
        if path:
            img = tk.PhotoImage(width=SHEET_W, height=SHEET_H)
            for y in range(SHEET_H):
                for x in range(SHEET_W):
                    v = self.level.sheet_pixel(x, y)
                    if v:
                        img.put(hexcolour(rgb15(v)), (x, y))
            img.write(path, format="png")
            self.watch_file, self.watch_mtime = path, os.path.getmtime(path)
            self.watching.set("Watching %s: save it from your paint program to send it" % os.path.basename(path))

    def load_sheet(self, path=None):
        path = path or filedialog.askopenfilename(filetypes=[("PNG", "*.png")])
        if not (path and self.level):
            return
        try:
            img = tk.PhotoImage(file=path)
        except tk.TclError as e:
            messagebox.showerror("PS1 LIVE", "Can't read %s: %s" % (path, e))
            return
        pixels = bytearray(SHEET_W * SHEET_H * 2)
        for y in range(min(SHEET_H, img.height())):
            for x in range(min(SHEET_W, img.width())):
                if img.transparency_get(x, y):
                    continue
                r, g, b = img.get(x, y)
                v = 0x8000 | (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
                struct.pack_into("<H", pixels, (y * SHEET_W + x) * 2, v)
        self.level.set_sheet(pixels)
        self.watch_file, self.watch_mtime = path, os.path.getmtime(path)
        self.watching.set("Watching %s: save it from your paint program to send it" % os.path.basename(path))
        self.changed()

    def watch(self):
        try:
            if self.watch_file and os.path.getmtime(self.watch_file) != self.watch_mtime:
                time.sleep(0.2)     # (the paint program finishes writing)
                self.load_sheet(self.watch_file)
                self.status.set("Tile set sent (hot reload)")
        except OSError:
            pass
        self.root.after(500, self.watch)

    # -- Files ---------------------------------------------------------------------------------------------------------
    def save(self):
        if not self.level:
            return
        path = filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("PS1 LIVE level", "*.json")])
        if path:
            json.dump(self.level.to_json(), open(path, "w"), indent=1)

    def load(self):
        path = filedialog.askopenfilename(filetypes=[("PS1 LIVE level", "*.json")])
        if not path:
            return
        if not self.level:
            self.level = Level(bytes(LEVEL_SIZE))
            struct.pack_into("<I", self.level.b, 0, MAGIC)
        self.level.from_json(json.load(open(path)))
        self.refresh_panels()
        self.changed()

    def quit(self):
        if self.link:
            self.link.close()
        self.root.destroy()


def main():
    root = tk.Tk()
    Editor(root)
    root.mainloop()


if __name__ == "__main__":
    main()
