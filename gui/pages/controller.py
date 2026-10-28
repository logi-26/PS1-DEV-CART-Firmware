"""
PS1 DEV CART GUI: Remote Controller page (controller port 1 or 2 driven from this computer, as a digital pad or
another controller: DualShock, analog joystick, NeGcon, mouse, GunCon).

The program must have been started with the remote controller option (Run PSX, Run Net Yaroze, Frame Inspector:
ps1transfer -pad, see monitor/monitor.h): the cartridge's debug monitor writes the controller's reply in the pad
buffer the BIOS fills, for programs reading their pads through the BIOS (PsyQ's PadInit, Net Yaroze, PSn00bSDK's
InitPAD). A digital pad's buttons are added to a real pad's; another device replaces the reply (its ID, buttons and
4 more bytes: sticks, NeGcon twist and analog buttons, GunCon position, mouse motion; debug monitor version 11). The
changes are sent through the live link (ps1transfer -memlive, or the frame capture's session): "pad", "padtype",
"padaxes", "padmove" (see ps1transfer/memlive.c).

Devices, driven like an emulator does: buttons with the mouse or the keyboard on the pad drawing; DualShock / analog
joystick sticks dragged with the mouse; NeGcon twist and I button on a steering area (II and L on sliders); the PC
mouse as the PS1 mouse (click the area to capture it, Esc to let go); the GunCon aimed at the console's picture
(read from VRAM, "screen"): left button the trigger, right A, middle B.

USB controller: a game controller plugged into this computer (e.g. a PS1 controller on a USB adapter, see gamepad.py)
drives the pad too, on any page: its buttons are added to the ones held with the mouse and the keyboard, its sticks
move the DualShock's (NeGcon: the left stick twists, Cross/Square/L1 are I/II/L). "Map buttons..." asks for each PS1
button in turn on the controller; the mapping is saved for that controller (by its USB vendor and product ID).

Recording and playback: "Record" captures every change (buttons, axes, mouse motion) and the time it happened;
"Play" sends them again with the same timing (to the device recorded). A recording can be saved and loaded again.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import json
import os
import struct
import time
import tkinter as tk
from tkinter import filedialog

import ttkbootstrap as tb

import gamepad
from pages.base import Page
from settings import config_dir

# Buttons: name, PsyQ pad bit, keys, shape (kind, x, y, w, h) on the pad drawing, label
BUTTONS = (
    ("L2", 1 << 0, ("1",), ("rect", 40, 8, 70, 20), "L2"),
    ("R2", 1 << 1, ("3",), ("rect", 410, 8, 70, 20), "R2"),
    ("L1", 1 << 2, ("q",), ("rect", 40, 32, 70, 20), "L1"),
    ("R1", 1 << 3, ("e",), ("rect", 410, 32, 70, 20), "R1"),
    ("Triangle", 1 << 4, ("s",), ("circle", 415, 82, 32, 32), "△"),
    ("Circle", 1 << 5, ("c",), ("circle", 451, 118, 32, 32), "○"),
    ("Cross", 1 << 6, ("x",), ("circle", 415, 154, 32, 32), "✕"),
    ("Square", 1 << 7, ("z",), ("circle", 379, 118, 32, 32), "□"),
    ("Select", 1 << 8, ("BackSpace", "Shift_R"), ("rect", 205, 122, 44, 16), "SELECT"),
    ("Start", 1 << 11, ("Return",), ("rect", 271, 122, 44, 16), "START"),
    ("Up", 1 << 12, ("Up",), ("rect", 62, 84, 28, 32), "▲"),
    ("Right", 1 << 13, ("Right",), ("rect", 90, 116, 32, 28), "▶"),
    ("Down", 1 << 14, ("Down",), ("rect", 62, 144, 28, 32), "▼"),
    ("Left", 1 << 15, ("Left",), ("rect", 30, 116, 32, 28), "◀"),
)
STICK_BUTTONS = (("L3", 1 << 9), ("R3", 1 << 10))       # DualShock: the sticks pressed
NEGCON = {"Start": "Start", "Up": "Up", "Right": "Right", "Down": "Down", "Left": "Left", "Circle": "A",
          "Triangle": "B", "R1": "R"}                    # NeGcon buttons (at the pad's bits): pad button -> name
MOUSE_BITS = {"Left": 0x08, "Right": 0x04}               # Mouse: reply byte 3 (bits 0-1 always 0)
GUN_BITS = {"Trigger": 1 << 5, "A": 1 << 11, "B": 1 << 6}
KEYS = {k.lower(): b for b in BUTTONS for k in b[2]}

# Devices: name, ID (reply byte 1, monitor.h PAD_*), kind
DEVICES = (("Digital pad", 0x41, "pad"), ("DualShock (analog)", 0x73, "analog"), ("Analog joystick", 0x53, "analog"),
           ("NeGcon", 0x23, "negcon"), ("Mouse", 0x12, "mouse"), ("GunCon", 0x63, "guncon"))
DEVICE = {d[0]: d for d in DEVICES}

PAD_W, PAD_H = 520, 230
UP, DOWN, LINE = "#3a3f44", "#2b9348", "#9aa0a6"
STATE_POLL_MS = 2000
SEND_MS = 30                # Changes (and mouse motion) sent together at most this often
STICK = 120                 # Stick area (pixels)
GUN_W, GUN_H = 384, 288     # GunCon aiming area
GUN_X, GUN_Y = (77, 461), (25, 248)     # GunCon X/Y over the picture (games calibrate it): NTSC 320 x 240
GUN_OFF = (1, 10)           # GunCon pointed off the screen
SCREEN_MS = 1000            # GunCon picture refreshed this often (auto)
RECORDING_TYPES = [("PS1 DEV CART pad recording", "*.ps1pad"), ("All files", "*.*")]
USB_MS = 16                 # USB controller read this often
USB_RETRY_MS = 2000         # Unplugged: looked for again this often
USB_DEAD = 10               # USB stick: this close to the centre is the centre (worn sticks)
USB_NONE = "None"
# Map buttons: the PS1 buttons asked for in turn, then the sticks (name, text)
LEARN_BUTTONS = ("Up", "Right", "Down", "Left", "Triangle", "Circle", "Cross", "Square", "L1", "R1", "L2", "R2",
                 "Select", "Start", "L3", "R3")
LEARN_STICKS = (("LX", "the left stick to the right"), ("LY", "the left stick down"),
                ("RX", "the right stick to the right"), ("RY", "the right stick down"))


def describe(kind, bits, axes, dx=0, dy=0):
    """Text of a state: buttons pressed, axes, motion."""
    if kind == "mouse":
        names = [n for n, b in MOUSE_BITS.items() if bits & b]
    elif kind == "guncon":
        names = [n for n, b in GUN_BITS.items() if bits & b]
    elif kind == "negcon":
        names = [NEGCON[b[0]] for b in BUTTONS if bits & b[1] and b[0] in NEGCON]
    else:
        names = [b[0] for b in BUTTONS if bits & b[1]] + [n for n, b in STICK_BUTTONS if kind == "analog" and bits & b]
    text = ", ".join(names) or "(released)"
    if axes:
        if kind == "analog":
            text += "  R %d,%d  L %d,%d" % tuple(axes)
        elif kind == "negcon":
            text += "  twist %d  I %d  II %d  L %d" % tuple(axes)
        elif kind == "guncon":
            x, y = struct.unpack("<HH", bytes(axes))
            text += "  at %d,%d" % (x, y)
    if dx or dy:
        text += "  move %+d,%+d" % (dx, dy)
    return text


class ControllerPage(Page):
    title = "Remote Controller"

    def build(self):
        self.pressed = set()        # Buttons held (names)
        self.latched = set()        # Latch mode: buttons toggled on
        self.mouse = None           # Pad button held with the mouse
        self.connected = False      # The live link answers, the state is sent
        self.want = False           # Connection asked, the live link starting
        self.sent = None            # (type, bits, axes) last sent
        self.send_job = None
        self.poll_job = None
        self.pad_state = None       # Last "MEMPAD" answer: (ready, port 1 buffer, port 2 buffer, monitor version)
        self.sticks = [128, 128, 128, 128]  # RX RY LX LY
        self.negcon = [128, 0, 0, 0]        # twist I II L
        self.gun = GUN_OFF          # GunCon X, Y
        self.move = [0, 0]          # Mouse motion not sent yet
        self.captured = False       # The PC mouse drives the PS1 mouse
        self.screen = None          # GunCon picture: (w, h, PIL image)
        self.screen_job = None
        self.screen_file = os.path.join(config_dir(), "guncon_screen.bin")

        self.recording_on = False
        self.recording = []         # [(seconds, bits, axes or None, dx, dy), ...] recorded, or loaded
        self.recording_device = None
        self.record_start = None
        self.last_state = None      # Last state recorded
        self.playing = False
        self.want_play = False
        self.play_index = 0
        self.play_start = None
        self.play_state = None      # (bits, axes) played back (overrides the controls while playing)
        self.play_job = None

        self.usb_pads = []          # USB controllers found (gamepad.Gamepad)
        self.usb = None             # The one used
        self.usb_key = ""           # Its key (USB vendor:product), kept when it is unplugged
        self.usb_map = None         # {"buttons": {PS1 button: input}, "sticks": {LX/LY/RX/RY: axis}}
        self.usb_held = set()       # PS1 buttons held on it
        self.usb_axes = None        # Its sticks (RX RY LX LY) last applied
        self.usb_job = None
        self.usb_auto = True        # A digital pad becomes a DualShock when the USB controller shows it is analog
        self.learn = None           # Map buttons: [(name, is a stick), ...] still to ask, None when not mapping
        self.learn_map = None
        self.learn_rest = None      # The controller at rest (inputs pressed are the ones that change from it)
        self.learn_wait = False     # Waiting for the controller to be released

        tb.Label(self, text=self.title, font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="A controller driven from this computer, on port 1 or port 2: a digital pad, or another "
                            "controller the program sees instead (DualShock, analog joystick, NeGcon, mouse, GunCon), "
                            "driven with the mouse and the keyboard like in an emulator. Start the program with the "
                            "remote controller option (Run PSX, Run Net Yaroze, Frame Inspector). Works with programs "
                            "reading their pads through the BIOS (PsyQ's PadInit, Net Yaroze, PSn00bSDK's InitPAD).",
                 wraplength=1000, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 8))
        bar = tb.Frame(self)
        bar.pack(fill="x", pady=(0, 8))
        self.connect_button = tb.Button(bar, text="Connect", bootstyle="success", width=12, command=self.toggle)
        self.connect_button.pack(side="left")
        tb.Label(bar, text="Controller").pack(side="left", padx=(12, 4))
        self.device = tb.Combobox(bar, values=[d[0] for d in DEVICES], state="readonly", width=19)
        name = self.app.settings.field("controller.device", DEVICES[0][0])
        self.device.set(name if name in DEVICE else DEVICES[0][0])
        self.device.bind("<<ComboboxSelected>>", lambda e: self.device_chosen())
        self.device.pack(side="left")
        self.port = tk.IntVar(value=1 if self.app.settings.field("controller.port", 0) else 0)
        tb.Checkbutton(bar, text="Port 2", variable=self.port, onvalue=1, offvalue=0,
                       bootstyle="warning-round-toggle", command=self.port_changed).pack(side="left", padx=(12, 0))
        self.latch = tk.BooleanVar(value=False)
        tb.Checkbutton(bar, text="Latch buttons", variable=self.latch, bootstyle="round-toggle",
                       command=self.latch_changed).pack(side="left", padx=(12, 0))
        tb.Button(bar, text="Release all", bootstyle="secondary-outline", command=self.release_all).pack(
            side="left", padx=12)
        tb.Button(bar, text="Run the control tester", bootstyle="info-outline", command=self.run_tester).pack(
            side="right")
        self.status = tb.Label(self, text="Not connected. (Run the control tester: the console shows what it sees "
                                          "of both ports, to try the controllers here)", bootstyle="secondary")
        self.status.pack(anchor="w", pady=(0, 8))

        box = tb.Frame(self)        # USB controller: one row (and the Map buttons prompt under it while mapping)
        box.pack(fill="x", pady=(0, 8))
        bar = tb.Frame(box)
        bar.pack(fill="x")
        tb.Label(bar, text="USB controller").pack(side="left")
        self.usb_combo = tb.Combobox(bar, state="readonly", width=34)
        self.usb_combo.bind("<<ComboboxSelected>>", lambda e: self.usb_select())
        self.usb_combo.pack(side="left", padx=(6, 0))
        tb.Button(bar, text="Refresh", bootstyle="secondary-outline", command=self.usb_refresh).pack(
            side="left", padx=(6, 0))
        self.map_button = tb.Button(bar, text="Map buttons...", bootstyle="info-outline", command=self.learn_start)
        self.map_button.pack(side="left", padx=(6, 0))
        self.default_button = tb.Button(bar, text="Default mapping", bootstyle="secondary-outline",
                                        command=self.usb_default)
        self.default_button.pack(side="left", padx=(6, 0))
        self.usb_status = tb.Label(bar, text="", bootstyle="secondary")
        self.usb_status.pack(side="left", padx=(12, 0))
        self.learn_bar = tb.Frame(box)
        self.learn_label = tb.Label(self.learn_bar, text="", bootstyle="warning", font=("", 10, "bold"))
        self.learn_label.pack(side="left")
        tb.Button(self.learn_bar, text="Skip", bootstyle="secondary-outline", width=8,
                  command=self.learn_skip).pack(side="left", padx=(12, 0))
        tb.Button(self.learn_bar, text="Cancel", bootstyle="danger-outline", width=8,
                  command=self.learn_cancel).pack(side="left", padx=(6, 0))

        box = self.section("Recording")
        box.pack(fill="x", pady=(0, 8))
        bar = tb.Frame(box)
        bar.pack(fill="x")
        self.record_button = tb.Button(bar, text="● Record", bootstyle="danger-outline", width=12,
                                       command=self.toggle_record)
        self.record_button.pack(side="left")
        self.play_button = tb.Button(bar, text="▶ Play", bootstyle="primary-outline", width=12,
                                     command=self.toggle_play)
        self.play_button.pack(side="left", padx=(6, 0))
        self.loop = tk.BooleanVar(value=False)
        tb.Checkbutton(bar, text="Loop", variable=self.loop, bootstyle="round-toggle").pack(side="left", padx=(12, 0))
        tb.Button(bar, text="Save...", bootstyle="secondary-outline", command=self.save_recording).pack(
            side="left", padx=(12, 0))
        tb.Button(bar, text="Load...", bootstyle="secondary-outline", command=self.load_recording).pack(
            side="left", padx=4)
        tb.Button(bar, text="Clear", bootstyle="secondary-outline", command=self.clear_recording).pack(
            side="left", padx=4)
        self.record_status = tb.Label(box, text="Nothing recorded.", bootstyle="secondary")
        self.record_status.pack(anchor="w", pady=(4, 0))

        body = tb.Frame(self)
        body.pack(fill="both", expand=True)
        self.left = tb.Frame(body)
        self.left.pack(side="left", anchor="n")
        right = tb.Frame(body, padding=(10, 0, 0, 0))
        right.pack(side="left", fill="both", expand=True)
        self._build_panels()
        self.state_label = tb.Label(self.left, text="", bootstyle="secondary", font=("Consolas", 10))
        self.state_label.pack(anchor="w", pady=(6, 0))
        self.hint = tb.Label(self.left, text="", wraplength=PAD_W + 20, justify="left", bootstyle="secondary")
        self.hint.pack(anchor="w", pady=(4, 0))

        tb.Label(right, text="Recorded events", font=("", 10, "bold")).pack(anchor="w")
        frame = tb.Frame(right)
        frame.pack(fill="both", expand=True, pady=(4, 0))
        self.events = tb.Treeview(frame, columns=("t", "state"), show="headings", height=10)
        self.events.heading("t", text="Time", anchor="e")
        self.events.heading("state", text="Controller")
        self.events.column("t", width=70, anchor="e", stretch=False)
        self.events.column("state", width=300, anchor="w")
        scroll = tb.Scrollbar(frame, orient="vertical", command=self.events.yview)
        self.events.configure(yscrollcommand=scroll.set)
        self.events.pack(side="left", fill="both", expand=True)
        scroll.pack(side="left", fill="y")

        self.show_panel()
        self.app.bind_all("<KeyPress>", self.key_press, add="+")
        self.app.bind_all("<KeyRelease>", self.key_release, add="+")
        self.app.live_listen(self)
        self.usb_key = self.app.settings.field("controller.usb", None)     # None: the first one found is used
        self.usb_refresh()

    def _build_panels(self):
        """A panel per device kind (the pad drawing is shared by the digital pad, analog pads and NeGcon)."""
        self.pad_panel = tb.Frame(self.left)
        self.canvas = tk.Canvas(self.pad_panel, width=PAD_W, height=PAD_H, highlightthickness=0,
                                background="#1e2124")
        self.canvas.pack(anchor="w")
        self.canvas.bind("<ButtonPress-1>", self.mouse_press)
        self.canvas.bind("<ButtonRelease-1>", self.mouse_release)

        # Analog sticks
        self.analog_panel = tb.Frame(self.left)
        row = tb.Frame(self.analog_panel)
        row.pack(anchor="w", pady=(6, 0))
        self.stick_canvas = []
        for i, name in enumerate(("Left stick", "Right stick")):
            col = tb.Frame(row)
            col.pack(side="left", padx=(0, 16))
            tb.Label(col, text=name).pack(anchor="w")
            c = tk.Canvas(col, width=STICK, height=STICK, highlightthickness=0, background="#1e2124",
                          cursor="fleur")
            c.pack()
            side = 2 if i == 0 else 0           # axes: RX RY LX LY
            c.bind("<B1-Motion>", lambda e, s=side: self.stick_drag(s, e))
            c.bind("<ButtonPress-1>", lambda e, s=side: self.stick_drag(s, e))
            c.bind("<ButtonRelease-1>", lambda e, s=side: self.stick_release(s))
            self.stick_canvas.append((c, side))
            b = tb.Button(col, text=("L3" if i == 0 else "R3") + " (hold)", bootstyle="secondary-outline", width=10)
            b.pack(pady=(4, 0))
            b.bind("<ButtonPress-1>", lambda e, n="L3" if i == 0 else "R3": self.hold(n, True))
            b.bind("<ButtonRelease-1>", lambda e, n="L3" if i == 0 else "R3": self.hold(n, False))
        self.recenter = tk.BooleanVar(value=True)
        tb.Checkbutton(row, text="Sticks go back\nto the center", variable=self.recenter,
                       bootstyle="round-toggle").pack(side="left", anchor="n", pady=(24, 0))

        # NeGcon: steering (twist) and I on an area, II and L on sliders
        self.negcon_panel = tb.Frame(self.left)
        tb.Label(self.negcon_panel, text="Steer: drag left/right (twist), up for button I (accelerate)").pack(
            anchor="w", pady=(6, 2))
        self.steer = tk.Canvas(self.negcon_panel, width=PAD_W, height=90, highlightthickness=0,
                               background="#1e2124", cursor="sb_h_double_arrow")
        self.steer.pack(anchor="w")
        self.steer.bind("<B1-Motion>", self.steer_drag)
        self.steer.bind("<ButtonPress-1>", self.steer_drag)
        self.steer.bind("<ButtonRelease-1>", self.steer_release)
        self.negcon_vars = []
        for i, name in ((2, "Button II"), (3, "Button L")):
            row = tb.Frame(self.negcon_panel)
            row.pack(anchor="w", pady=(4, 0))
            tb.Label(row, text=name, width=10).pack(side="left")
            var = tk.IntVar(value=0)
            tb.Scale(row, from_=0, to=255, variable=var, length=300,
                     command=lambda v, i=i, var=var: self.negcon_axis(i, int(float(v)))).pack(side="left")
            self.negcon_vars.append((i, var))

        # Mouse: capture area
        self.mouse_panel = tb.Frame(self.left)
        self.mouse_area = tk.Canvas(self.mouse_panel, width=PAD_W, height=PAD_H, highlightthickness=0,
                                    background="#1e2124", cursor="crosshair")
        self.mouse_area.pack(anchor="w")
        self.mouse_area.bind("<ButtonPress-1>", lambda e: self.mouse_button("Left", True))
        self.mouse_area.bind("<ButtonRelease-1>", lambda e: self.mouse_button("Left", False))
        self.mouse_area.bind("<ButtonPress-3>", lambda e: self.mouse_button("Right", True))
        self.mouse_area.bind("<ButtonRelease-3>", lambda e: self.mouse_button("Right", False))
        self.mouse_area.bind("<Motion>", self.mouse_motion)
        row = tb.Frame(self.mouse_panel)
        row.pack(anchor="w", pady=(6, 0))
        tb.Label(row, text="Speed").pack(side="left")
        self.speed = tk.DoubleVar(value=self.app.settings.field("controller.speed", 1.0))
        tb.Scale(row, from_=0.2, to=4, variable=self.speed, length=200).pack(side="left", padx=6)
        self.speed_label = tb.Label(row, text="", width=6)
        self.speed_label.pack(side="left")
        self.speed.trace_add("write", lambda *a: self.speed_label.configure(text="x%.1f" % self.speed.get()))
        self.speed_label.configure(text="x%.1f" % self.speed.get())

        # GunCon: the console's picture to aim at
        self.gun_panel = tb.Frame(self.left)
        self.gun_area = tk.Canvas(self.gun_panel, width=GUN_W, height=GUN_H, highlightthickness=0,
                                  background="#000000", cursor="tcross")
        self.gun_area.pack(anchor="w")
        self.gun_area.bind("<ButtonPress-1>", lambda e: self.gun_press(e, "Trigger"))
        self.gun_area.bind("<B1-Motion>", lambda e: self.gun_aim(e))
        self.gun_area.bind("<ButtonRelease-1>", lambda e: self.hold("Trigger", False))
        self.gun_area.bind("<ButtonPress-3>", lambda e: self.gun_press(e, "A"))
        self.gun_area.bind("<ButtonRelease-3>", lambda e: self.hold("A", False))
        self.gun_area.bind("<ButtonPress-2>", lambda e: self.gun_press(e, "B"))
        self.gun_area.bind("<ButtonRelease-2>", lambda e: self.hold("B", False))
        row = tb.Frame(self.gun_panel)
        row.pack(anchor="w", pady=(6, 0))
        tb.Button(row, text="Refresh picture", bootstyle="secondary-outline", command=self.refresh_screen).pack(
            side="left")
        self.auto_screen = tk.BooleanVar(value=False)
        tb.Checkbutton(row, text="Every second", variable=self.auto_screen, bootstyle="round-toggle",
                       command=self.screen_auto).pack(side="left", padx=(8, 0))
        b = tb.Button(row, text="Shoot off-screen (hold)", bootstyle="warning-outline")
        b.pack(side="left", padx=(12, 0))
        b.bind("<ButtonPress-1>", lambda e: self.gun_offscreen(True))
        b.bind("<ButtonRelease-1>", lambda e: self.gun_offscreen(False))
        row = tb.Frame(self.gun_panel)
        row.pack(anchor="w", pady=(6, 0))
        tb.Label(row, text="Gun X from").pack(side="left")
        self.gun_range = []
        for label, value in (("", GUN_X[0]), ("to", GUN_X[1]), ("  Y from", GUN_Y[0]), ("to", GUN_Y[1])):
            if label:
                tb.Label(row, text=label).pack(side="left", padx=(4, 0))
            sb = tb.Spinbox(row, from_=0, to=1023, width=5)
            sb.set(value)
            sb.pack(side="left", padx=(4, 0))
            self.gun_range.append(sb)
        self.panels = {"pad": [self.pad_panel], "analog": [self.pad_panel, self.analog_panel],
                       "negcon": [self.pad_panel, self.negcon_panel], "mouse": [self.mouse_panel],
                       "guncon": [self.gun_panel]}
        self.app.bind_all("<Escape>", lambda e: self.release_mouse(), add="+")

    # ---------------------------------------------------------------- device

    def kind(self):
        return DEVICE[self.device.get()][2]

    def device_id(self):
        return DEVICE[self.device.get()][1]

    def show_panel(self):
        for frames in self.panels.values():
            for f in frames:
                f.pack_forget()
        self.state_label.pack_forget()
        self.hint.pack_forget()
        for f in self.panels[self.kind()]:
            f.pack(anchor="w")
        self.state_label.pack(anchor="w", pady=(6, 0))
        self.hint.pack(anchor="w", pady=(4, 0))
        keys = "   ".join("%s: %s" % (b[0], "/".join(k if len(k) > 1 else k.upper() for k in b[2])) for b in BUTTONS)
        self.hint.configure(text={
            "pad": "Keyboard (this page shown): " + keys,
            "analog": "Drag the sticks with the mouse. Keyboard for the buttons: " + keys,
            "negcon": "NeGcon buttons on the pad drawing: A = Circle, B = Triangle, R = R1, Start, D-pad.",
            "mouse": "Click the area to use this computer's mouse as the PS1 mouse (the pointer is held in the area): "
                     "left and right buttons as the mouse's. Esc lets the mouse go.",
            "guncon": "Aim at the picture and pull the trigger with the left button (held), A with the right button, "
                      "B with the middle one. The picture is the frame being drawn (Refresh). Games calibrate the gun "
                      "(often a shot at a target): the X/Y ranges below set where the picture's edges are."}[self.kind()])
        self.draw()

    def device_chosen(self):
        self.usb_auto = False       # (chosen by hand: a USB analog controller doesn't change it any more)
        self.device_changed()

    def device_changed(self):
        self.app.settings.set_field("controller.device", self.device.get())
        self.release_mouse()
        self.pressed.clear()
        self.latched.clear()
        self.usb_held = set()
        self.usb_axes = None        # (the USB controller's sticks applied again, to the new device)
        self.move = [0, 0]
        self.show_panel()
        if self.connected:
            self.app.live_send("padtype %02X" % self.device_id())
            self.sent = None
        self.send()
        if self.kind() == "guncon" and self.connected and not self.screen:
            self.refresh_screen()

    # ---------------------------------------------------------------- control tester

    def run_tester(self):
        """The PS1 DEV CART control tester on the console (ps1transfer -padtest: both ports shown as a program sees
        them), then connected to drive it."""
        if self.connected or self.want:
            self.disconnect()
        self.status.configure(text="Starting the control tester on the console...")
        self._start_tester(25)

    def _start_tester(self, tries):
        if self.app.runner.busy and tries:      # (the live link stopping)
            self.after(150, lambda: self._start_tester(tries - 1))
            return
        self.app.run(["-padtest"], "Control tester",
                     on_done=lambda code, output: self.after(0, self._tester_started, code))

    def _tester_started(self, code):
        if code != 0:
            self.status.configure(text="The control tester couldn't be started (see the log below).")
            return
        self.after(1500, lambda: None if (self.connected or self.want) else self.toggle())

    # ---------------------------------------------------------------- live link

    def toggle(self):
        if self.connected or self.want:
            self.disconnect()
            return
        self.want = True
        if self.app.live_task() and self.app.live_ready:
            self.on_connected()
        else:
            self.status.configure(text="Connecting...")
            self.app.live_start()

    def on_connected(self):
        self.want = False
        self.connected = True
        self.sent = None
        self.connect_button.configure(text="Disconnect", bootstyle="danger")
        self.pad_state = None
        self.app.live_send("padport %d" % self.port.get())
        self.app.live_send("padtype %02X" % self.device_id())
        self.send()
        self.poll()
        if self.kind() == "guncon":
            self.refresh_screen()
        if self.want_play:
            self.want_play = False
            self.start_play()

    def disconnect(self):
        if self.playing:
            self.stop_play()
        self.release_mouse()
        if self.connected:
            self.app.live_send("pad off")
        self.connected = self.want = self.want_play = False
        for job in ("poll_job", "screen_job"):
            if getattr(self, job):
                self.after_cancel(getattr(self, job))
                setattr(self, job, None)
        self.connect_button.configure(text="Connect", bootstyle="success")
        self.status.configure(text="Not connected.")
        self.app.live_release()

    def poll(self):
        self.poll_job = None
        if self.connected:
            self.app.live_send("padstate")
            self.poll_job = self.after(STATE_POLL_MS, self.poll)

    def using_live(self):
        return self.connected or self.want

    def live_stopped(self):
        if self.playing:
            self.stop_play()
        self.release_mouse()
        if self.connected or self.want:
            self.connected = self.want = self.want_play = False
            self.connect_button.configure(text="Connect", bootstyle="success")
            self.status.configure(text="Disconnected (the live link stopped).")

    def live_line(self, line):
        if line.startswith(("MEMLIVE ready", "FRMLIVE ready")) and self.want:
            self.on_connected()
        elif line.startswith("MEMPAD") and self.connected:
            fields = line.split()
            try:
                self.pad_state = (fields[1] == "1", int(fields[2], 16), int(fields[3], 16), int(fields[4]))
            except (IndexError, ValueError):
                return
            self.show_state()
        elif line.startswith("MEMSCREEN"):
            self.load_screen(line.split(None, 5))
        elif line.startswith(("MEMERR", "FRMERR")) and self.connected and "controllers" in line:
            self.status.configure(text=line[7:])

    def show_state(self):
        if not self.pad_state:
            return
        ready, buf1, buf2, version = self.pad_state
        port = self.port.get()
        buf = buf2 if port else buf1
        self.status.configure(text=(
            "The program wasn't started with the remote controller option (or the cartridge firmware is too old): "
            "run it again with the option on." if not ready else
            "Port 2 needs the new cartridge firmware (debug monitor version 9, this one is %d): flash it from the "
            "Firmware page. Port 1 works." % version if port and version < 9 else
            "The %s needs the new cartridge firmware (debug monitor version 11, this one is %d): flash it from the "
            "Firmware page. The digital pad works." % (self.device.get(), version)
            if self.kind() != "pad" and version < 11 else
            "Driving controller port %d as a %s (the program's pad buffer at 0x%08X)." % (
                port + 1, self.device.get(), buf) if buf else
            "Waiting for the program to set up its pads (InitPAD)." if not buf1 else
            "The program doesn't read a pad on port 2 (InitPAD without a second buffer)."))

    def port_changed(self):
        self.app.settings.set_field("controller.port", self.port.get())
        if self.connected:
            self.app.live_send("padport %d" % self.port.get())
        self.show_state()
        self.draw()

    # ---------------------------------------------------------------- state, sending

    def bits(self):
        if self.playing and self.play_state:
            return self.play_state[0]
        held = self.pressed | self.latched | self.usb_held
        kind = self.kind()
        if kind == "mouse":
            return 0x0003 | sum(b for n, b in MOUSE_BITS.items() if n in held)
        if kind == "guncon":
            return sum(b for n, b in GUN_BITS.items() if n in held)
        bits = sum(b[1] for b in BUTTONS if b[0] in held and (kind != "negcon" or b[0] in NEGCON))
        if kind == "analog":
            bits |= sum(b for n, b in STICK_BUTTONS if n in held)
        return bits

    def axes(self):
        if self.playing and self.play_state:
            return self.play_state[1]
        kind = self.kind()
        if kind == "analog":
            return tuple(self.sticks)
        if kind == "negcon":
            return tuple(self.negcon)
        if kind == "guncon":
            return tuple(struct.pack("<HH", *self.gun))
        return None

    def send(self):
        """The state shown, recorded, and sent soon (changes sent together, at most every SEND_MS)."""
        bits, axes = self.bits(), self.axes()
        self.state_label.configure(text="%s   (bits 0x%04X)" % (describe(self.kind(), bits, axes), bits))
        self.draw()
        if self.send_job is None:
            self.send_job = self.after(SEND_MS, self.flush)

    def flush(self):
        self.send_job = None
        bits, axes = self.bits(), self.axes()
        dx, dy = (int(round(v)) for v in self.move)
        self.move = [self.move[0] - dx, self.move[1] - dy]
        if self.recording_on and not self.playing and ((bits, axes) != self.last_state or dx or dy):
            self.record_event(bits, axes, dx, dy)
        self.last_state = (bits, axes)
        if not self.connected:
            return
        state = (self.device_id(), bits, axes)
        if self.sent is None or self.sent[1] != bits or self.sent[0] != state[0]:
            self.app.live_send("pad %04X" % bits)
        if axes is not None and (self.sent is None or self.sent[2] != axes):
            self.app.live_send("padaxes %s" % bytes(axes).hex().upper())
        if dx or dy:
            self.app.live_send("padmove %d %d" % (dx, dy))
        self.sent = state

    # ---------------------------------------------------------------- pad buttons

    def button_at(self, x, y):
        for b in BUTTONS:
            kind, bx, by, w, h = b[3]
            if bx <= x < bx + w and by <= y < by + h:
                return b[0]
        return None

    def hold(self, name, on):
        if self.playing:
            return
        if on:
            self.pressed.add(name)
        else:
            self.pressed.discard(name)
        self.send()

    def mouse_press(self, event):
        if self.playing:
            return
        name = self.button_at(event.x, event.y)
        if not name:
            return
        if self.latch.get():
            self.latched ^= {name}
        else:
            self.mouse = name
            self.pressed.add(name)
        self.send()

    def mouse_release(self, event):
        if self.playing:
            return
        if self.mouse:
            self.pressed.discard(self.mouse)
            self.mouse = None
            self.send()

    def key_press(self, event):
        if self.playing or self.kind() in ("mouse", "guncon"):
            return
        b = KEYS.get(event.keysym.lower())
        if b and self.app.current is self and b[0] not in self.pressed:
            self.pressed.add(b[0])
            self.send()

    def key_release(self, event):
        if self.playing:
            return
        b = KEYS.get(event.keysym.lower())
        if b and b[0] in self.pressed and b[0] != self.mouse:
            self.pressed.discard(b[0])
            self.send()

    def latch_changed(self):
        self.latched.clear()
        self.send()

    def release_all(self):
        if self.playing:
            self.stop_play()
        self.release_mouse()
        self.pressed.clear()
        self.latched.clear()
        self.mouse = None
        self.sticks = [128, 128, 128, 128]
        self.negcon = [128, 0, 0, 0]
        for i, var in self.negcon_vars:
            var.set(0)
        self.send()

    # ---------------------------------------------------------------- sticks, NeGcon

    def stick_drag(self, side, event):
        if self.playing:
            return
        x = max(0, min(255, event.x * 256 // STICK))
        y = max(0, min(255, event.y * 256 // STICK))
        self.sticks[side], self.sticks[side + 1] = x, y
        self.send()

    def stick_release(self, side):
        if self.recenter.get() and not self.playing:
            self.sticks[side] = self.sticks[side + 1] = 128
            self.send()

    def steer_drag(self, event):
        if self.playing:
            return
        self.negcon[0] = max(0, min(255, event.x * 256 // PAD_W))
        self.negcon[1] = max(0, min(255, (90 - event.y) * 256 // 90))
        self.send()

    def steer_release(self, event):
        if self.recenter.get() and not self.playing:
            self.negcon[0], self.negcon[1] = 128, 0
            self.send()

    def negcon_axis(self, i, v):
        if not self.playing:
            self.negcon[i] = v
            self.send()

    # ---------------------------------------------------------------- mouse

    def mouse_button(self, name, on):
        if not self.captured:
            if on and name == "Left":
                self.capture_mouse()
            return
        self.hold(name, on)

    def capture_mouse(self):
        self.captured = True
        self.mouse_area.configure(cursor="none")
        self._center()
        self.draw()

    def release_mouse(self):
        if self.captured:
            self.captured = False
            self.mouse_area.configure(cursor="crosshair")
            self.pressed -= set(MOUSE_BITS)
            self.draw()

    def _center(self):
        self.mouse_area.event_generate("<Motion>", warp=True, x=PAD_W // 2, y=PAD_H // 2)

    def mouse_motion(self, event):
        if not self.captured or self.playing:
            return
        dx, dy = event.x - PAD_W // 2, event.y - PAD_H // 2
        if not dx and not dy:              # (our own warp to the center)
            return
        self.move[0] += dx * self.speed.get()
        self.move[1] += dy * self.speed.get()
        self._center()
        self.send()

    # ---------------------------------------------------------------- GunCon

    def gun_xy(self, event):
        """The gun's X/Y for a point of the aiming area (the picture's edges at the X/Y ranges)."""
        try:
            x0, x1, y0, y1 = (int(sb.get()) for sb in self.gun_range)
        except ValueError:
            (x0, x1), (y0, y1) = GUN_X, GUN_Y
        left, top, w, h = self.picture_box()
        u = (event.x - left) / max(1, w)
        v = (event.y - top) / max(1, h)
        if not (0 <= u < 1 and 0 <= v < 1):
            return GUN_OFF
        return int(x0 + u * (x1 - x0)), int(y0 + v * (y1 - y0))

    def picture_box(self):
        w, h = (self.screen[0], self.screen[1]) if self.screen else (320, 240)
        scale = min(GUN_W / w, GUN_H / h)
        return (GUN_W - w * scale) / 2, (GUN_H - h * scale) / 2, w * scale, h * scale

    def gun_aim(self, event):
        if self.playing:
            return
        self.gun = self.gun_xy(event)
        self.aim_point = (event.x, event.y)
        self.send()

    def gun_press(self, event, name):
        if self.playing:
            return
        self.gun_aim(event)
        self.hold(name, True)

    def gun_offscreen(self, on):
        if self.playing:
            return
        if on:
            self.gun = GUN_OFF
            self.aim_point = None
        self.hold("Trigger", on)

    def refresh_screen(self):
        if self.connected:
            self.app.live_send("screen %s" % self.screen_file)

    def screen_auto(self):
        if self.screen_job:
            self.after_cancel(self.screen_job)
            self.screen_job = None
        if self.auto_screen.get() and self.kind() == "guncon":
            self.refresh_screen()
            self.screen_job = self.after(SCREEN_MS, self.screen_auto)

    def load_screen(self, fields):
        from PIL import Image
        try:
            w, h = int(fields[3]), int(fields[4])
            with open(fields[5], "rb") as f:
                raw = f.read(w * h * 2)
            self.screen = (w, h, Image.frombytes("RGB", (w, h), raw, "raw", "RGB;15"))
        except (IndexError, ValueError, OSError):
            return
        self.draw()

    # ---------------------------------------------------------------- USB controller

    def usb_refresh(self):
        """The USB controllers plugged in; the one used last is chosen again."""
        self.usb_stop()
        for pad in self.usb_pads:
            pad.close()
        try:
            self.usb_pads = gamepad.find()
        except Exception:  # noqa: BLE001 (no joystick API here: the page works without it)
            self.usb_pads = []
        self.usb_combo.configure(values=[USB_NONE] + [p.name for p in self.usb_pads])
        pad = next((p for p in self.usb_pads if p.key == self.usb_key), None)
        if self.usb_key is None and self.usb_pads:
            pad = self.usb_pads[0]
        self.usb_combo.set(pad.name if pad else USB_NONE)
        self.usb_select(save=False)

    def usb_select(self, save=True):
        self.usb_stop()
        self.learn_cancel()
        name = self.usb_combo.get()
        self.usb = next((p for p in self.usb_pads if p.name == name), None)
        if save:
            self.usb_key = self.usb.key if self.usb else USB_NONE     # (None chosen: not used, even if one is found)
            self.app.settings.set_field("controller.usb", self.usb_key)
        for b in (self.map_button, self.default_button):
            b.configure(state="normal" if self.usb else "disabled")
        if not self.usb:
            self.usb_status.configure(text=(
                "Not used: choose one to drive the pad with it (on any page)." if self.usb_pads else
                "None found (e.g. a PS1 controller on a USB adapter): plug one in, then Refresh."))
            return
        self.usb_map = self.app.settings.field("controller.usb_maps", {}).get(self.usb.key) or \
            gamepad.default_mapping(self.usb.key)
        self.usb_axes = None
        self.usb_status.configure(text="Nothing held. (Map buttons if they don't match.)")
        self.usb_poll()

    def usb_stop(self):
        if self.usb_job:
            self.after_cancel(self.usb_job)
            self.usb_job = None
        if self.usb_held:
            self.usb_held = set()
            self.send()

    def usb_poll(self):
        self.usb_job = None
        if not self.usb:
            return
        state = self.usb.read()
        if state is None:
            self.usb_stop()
            self.learn_cancel()
            self.usb_status.configure(text="Unplugged: used again once plugged back in.")
            self.usb_job = self.after(USB_RETRY_MS, self.usb_replug)
            return
        if self.learn is not None:
            self.learn_step(state)
        elif not self.playing:
            self.usb_apply(state)
        self.usb_job = self.after(USB_MS, self.usb_poll)

    def usb_replug(self):
        self.usb_job = None
        try:
            pads = gamepad.find()
        except Exception:  # noqa: BLE001
            pads = []
        back = any(p.key == self.usb_key for p in pads)
        for pad in pads:
            pad.close()
        if back:
            self.usb_refresh()
        else:
            self.usb_job = self.after(USB_RETRY_MS, self.usb_replug)

    def usb_stick(self, state, spec):
        v = state.axis(spec) if spec else 128
        return 128 if abs(v - 128) <= USB_DEAD else v

    def usb_apply(self, state):
        """The PS1 buttons held on the USB controller, and its sticks (DualShock / analog joystick, NeGcon)."""
        kind = self.kind()
        held, axes = set(), None
        if kind not in ("mouse", "guncon"):
            held = {n for n, spec in self.usb_map["buttons"].items() if state.active(spec)}
            sticks = [self.usb_stick(state, self.usb_map.get("sticks", {}).get(n)) for n in ("RX", "RY", "LX", "LY")]
            if kind == "pad" and self.usb_analog(held, sticks):
                return

            if kind == "analog":
                axes = sticks
            elif kind == "negcon":
                axes = [sticks[2]] + [255 if n in held else 0 for n in ("Cross", "Square", "L1")]
        changed = False
        if held != self.usb_held:
            self.usb_held = held
            changed = True
            self.usb_status.configure(text="Held: " + (", ".join(n for n in LEARN_BUTTONS if n in held) or "nothing"))
        if axes is not None and axes != self.usb_axes:
            self.usb_axes = axes
            changed = True
            if kind == "analog":
                self.sticks = list(axes)
            else:
                self.negcon = list(axes)
        if changed:
            self.send()

    def usb_analog(self, held, sticks):
        """An analog controller in analog mode (L3/R3, or a stick moved) while the program sees a digital pad: it
        sees a DualShock instead (as when the Analog button is pressed on a real one). Not when the device was chosen
        by hand, while recording or playing, or for a stick axis that also works as buttons (a D-pad on the axes)."""
        if not self.usb_auto or self.recording_on or self.playing:
            return False
        as_buttons = {spec.rstrip("+-") for spec in self.usb_map["buttons"].values() if spec.startswith("a")}
        moved = [n for n, v in zip(("RX", "RY", "LX", "LY"), sticks)
                 if v != 128 and self.usb_map.get("sticks", {}).get(n, "").rstrip("-") not in as_buttons]
        if not (moved or held & {"L3", "R3"}):
            return False
        self.device.set(DEVICES[1][0])
        self.device_changed()
        self.usb_status.configure(text="Analog controller: the program sees a DualShock (Controller to change it).")
        return True

    def usb_default(self):
        if not self.usb:
            return
        self.learn_cancel()
        maps = self.app.settings.field("controller.usb_maps", {})
        maps.pop(self.usb.key, None)
        self.app.settings.set_field("controller.usb_maps", maps)
        self.usb_map = gamepad.default_mapping(self.usb.key)
        self.usb_status.configure(text="Default mapping.")

    # Map buttons: each PS1 button asked for in turn, the input that changes from the controller at rest taken

    def learn_start(self):
        if not self.usb:
            return None
        state = self.usb.read()
        if state is None:
            return self.app.error("The USB controller doesn't answer (unplugged?): Refresh.")
        self.learn = [(n, False) for n in LEARN_BUTTONS] + [(n, True) for n, _ in LEARN_STICKS]
        self.learn_map = {"buttons": {}, "sticks": {}}
        self.learn_rest = state         # (nothing held when Map buttons was clicked)
        self.learn_wait = False
        self.usb_held = set()
        self.usb_axes = None
        self.send()
        self.learn_bar.pack(anchor="w", pady=(6, 0))
        self.learn_prompt()
        return None

    def learn_prompt(self):
        name, stick = self.learn[0]
        total = len(LEARN_BUTTONS) + len(LEARN_STICKS)
        step = total - len(self.learn) + 1
        if stick:
            text = "Push %s on the USB controller (%d of %d)" % (dict(LEARN_STICKS)[name], step, total)
        else:
            text = "Press %s on the USB controller (%d of %d)" % (name, step, total)
        self.learn_label.configure(text=text)

    def learn_step(self, state):
        if self.learn_wait:             # The input just taken: released before the next one
            if state.at_rest(self.learn_rest):
                self.learn_wait = False
                self.learn_prompt()
            return
        name, stick = self.learn[0]
        found = state.changes(self.learn_rest)
        if stick:
            found = [f for f in found if f[0] == "a"]
        if not found:
            return
        spec = found[0]
        if stick:
            self.learn_map["sticks"][name] = spec.rstrip("+")
        else:
            self.learn_map["buttons"][name] = spec
        self.learn_label.configure(text="%s: %s. Release it." % (name, gamepad.describe(spec)))
        self.learn_wait = True
        self.learn_next()

    def learn_skip(self):
        """This one stays as it was (e.g. no L3 on a digital controller)."""
        if self.learn:
            self.learn_wait = False
            self.learn_next()
            if self.learn:
                self.learn_prompt()

    def learn_next(self):
        self.learn.pop(0)
        if self.learn:
            return
        mapping = {"buttons": dict(self.usb_map["buttons"]), "sticks": dict(self.usb_map.get("sticks", {}))}
        mapping["buttons"].update(self.learn_map["buttons"])
        mapping["sticks"].update(self.learn_map["sticks"])
        maps = self.app.settings.field("controller.usb_maps", {})
        maps[self.usb.key] = mapping
        self.app.settings.set_field("controller.usb_maps", maps)
        self.usb_map = mapping
        self.learn = None
        self.learn_bar.pack_forget()
        self.usb_status.configure(text="Mapping saved (%d buttons, %d stick axes)." % (
            len(self.learn_map["buttons"]), len(self.learn_map["sticks"])))

    def learn_cancel(self):
        self.learn_bar.pack_forget()
        if self.learn is not None:
            self.learn = None
            if self.usb:
                self.usb_status.configure(text="Mapping not changed.")

    # ---------------------------------------------------------------- drawing

    def draw(self):
        kind = self.kind()
        if kind in ("pad", "analog", "negcon"):
            self.draw_pad(kind)
        if kind == "analog":
            for c, side in self.stick_canvas:
                c.delete("all")
                c.create_oval(4, 4, STICK - 4, STICK - 4, outline=LINE)
                c.create_line(STICK // 2, 4, STICK // 2, STICK - 4, fill="#343a40")
                c.create_line(4, STICK // 2, STICK - 4, STICK // 2, fill="#343a40")
                axes = self.axes()
                x, y = axes[side] * STICK / 256, axes[side + 1] * STICK / 256
                c.create_oval(x - 10, y - 10, x + 10, y + 10, fill=DOWN, outline="")
                c.create_text(6, STICK - 8, text="%d,%d" % (axes[side], axes[side + 1]), anchor="w",
                              fill="#868e96", font=("", 8))
        if kind == "negcon":
            c = self.steer
            c.delete("all")
            axes = self.axes()
            c.create_line(PAD_W // 2, 0, PAD_W // 2, 90, fill="#343a40")
            x = axes[0] * PAD_W / 256
            c.create_rectangle(min(x, PAD_W // 2), 34, max(x, PAD_W // 2), 56, fill="#1c7ed6", outline="")
            c.create_rectangle(4, 90 - axes[1] * 86 / 256, 16, 90, fill=DOWN, outline="")
            c.create_text(24, 8, text="twist %d   I %d" % (axes[0], axes[1]), anchor="nw", fill="#adb5bd")
        if kind == "mouse":
            c = self.mouse_area
            c.delete("all")
            c.create_text(PAD_W // 2, PAD_H // 2 - 10, fill="#adb5bd", font=("", 11),
                          text="Mouse captured: move it, click the buttons (Esc: let go)" if self.captured else
                          "Click here to use this computer's mouse as the PS1 mouse")
            held = self.pressed | self.latched
            for i, (name, x) in enumerate((("Left", PAD_W // 2 - 60), ("Right", PAD_W // 2 + 10))):
                c.create_rectangle(x, PAD_H // 2 + 20, x + 50, PAD_H // 2 + 60, outline=LINE,
                                   fill=DOWN if name in held or (self.playing and self.bits() & MOUSE_BITS[name])
                                   else UP)
                c.create_text(x + 25, PAD_H // 2 + 40, text=name, fill="#ffffff")
        if kind == "guncon":
            c = self.gun_area
            c.delete("all")
            left, top, w, h = self.picture_box()
            if self.screen:
                from PIL import ImageTk
                self.photo = ImageTk.PhotoImage(self.screen[2].resize((max(1, int(w)), max(1, int(h)))))
                c.create_image(left, top, image=self.photo, anchor="nw")
            else:
                c.create_rectangle(left, top, left + w, top + h, outline="#343a40")
                c.create_text(GUN_W // 2, GUN_H // 2, text="Refresh picture: the console's frame", fill="#6c757d")
            point = getattr(self, "aim_point", None)
            if point and self.axes() != tuple(struct.pack("<HH", *GUN_OFF)):
                x, y = point
                color = "#ff6b6b" if self.bits() & GUN_BITS["Trigger"] else "#ffd43b"
                c.create_oval(x - 9, y - 9, x + 9, y + 9, outline=color, width=2)
                c.create_line(x - 14, y, x + 14, y, fill=color)
                c.create_line(x, y - 14, x, y + 14, fill=color)

    def draw_pad(self, kind):
        c = self.canvas
        c.delete("all")
        c.create_oval(10, 50, 200, 225, fill="#2e3236", outline=LINE)
        c.create_oval(320, 50, 510, 225, fill="#2e3236", outline=LINE)
        c.create_rectangle(105, 60, 415, 180, fill="#2e3236", outline="")
        c.create_line(105, 60, 415, 60, fill=LINE)
        c.create_text(260, 92, text="NeGcon" if kind == "negcon" else "PlayStation", fill="#6c757d",
                      font=("", 10, "italic"))
        c.create_text(260, 160, text="PORT %d" % (self.port.get() + 1), fill="#f0ad4e" if self.port.get() else
                      "#5bc0de", font=("", 12, "bold"))
        bits = self.bits()
        for name, bit, keys, (shape, x, y, w, h), label in BUTTONS:
            if kind == "negcon" and name not in NEGCON:
                continue
            fill = DOWN if bits & bit else UP
            if shape == "circle":
                c.create_oval(x, y, x + w, y + h, fill=fill, outline=LINE)
            else:
                c.create_rectangle(x, y, x + w, y + h, fill=fill, outline=LINE)
            if kind == "negcon" and NEGCON[name] in ("A", "B", "R"):
                label = NEGCON[name]
            c.create_text(x + w / 2, y + h / 2, text=label, fill="#ffffff",
                          font=("", 7 if len(label) > 2 else 11, "bold"))

    # ---------------------------------------------------------------- recording

    def toggle_record(self):
        if self.playing:
            return
        if self.recording_on:
            self.recording_on = False
            self.record_button.configure(text="● Record", bootstyle="danger-outline")
            self.play_button.configure(state="normal")
            self.update_record_status()
            return
        self.recording = []
        self.recording_device = self.device.get()
        self.record_start = None
        self.last_state = None
        self.events.delete(*self.events.get_children())
        self.recording_on = True
        self.record_button.configure(text="■ Stop recording", bootstyle="danger")
        self.play_button.configure(state="disabled")
        self.record_status.configure(text="Recording the %s..." % self.device.get())

    def record_event(self, bits, axes, dx=0, dy=0):
        now = time.time()
        if self.record_start is None:
            self.record_start = now
        t = now - self.record_start
        self.recording.append((t, bits, axes, dx, dy))
        self.events.insert("", "end", values=("%.2fs" % t, describe(self.kind(), bits, axes, dx, dy)))
        self.events.see(self.events.get_children()[-1])
        self.record_status.configure(text="Recording the %s... %d event(s), %.1f s." % (
            self.recording_device, len(self.recording), t))

    def update_record_status(self):
        if not self.recording:
            self.record_status.configure(text="Nothing recorded.")
        else:
            self.record_status.configure(text="%s: %d event(s), %.1f s." % (
                self.recording_device, len(self.recording), self.recording[-1][0]))

    def fill_events(self):
        kind = DEVICE.get(self.recording_device, DEVICES[0])[2]
        self.events.delete(*self.events.get_children())
        for t, bits, axes, dx, dy in self.recording:
            self.events.insert("", "end", values=("%.2fs" % t, describe(kind, bits, axes, dx, dy)))

    def clear_recording(self):
        if self.recording_on:
            self.toggle_record()
        if self.playing:
            self.stop_play()
        self.recording = []
        self.events.delete(*self.events.get_children())
        self.update_record_status()

    def save_recording(self):
        if not self.recording:
            return self.app.error("Record something first (or load a recording).")
        path = filedialog.asksaveasfilename(filetypes=RECORDING_TYPES, defaultextension=".ps1pad", parent=self)
        if not path:
            return None
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"version": 2, "device": self.recording_device,
                       "events": [[round(t, 3), bits, list(axes) if axes else None, dx, dy]
                                  for t, bits, axes, dx, dy in self.recording]}, f, indent=1)
        self.record_status.configure(text="Saved %d event(s) to %s" % (len(self.recording), os.path.basename(path)))
        return None

    def load_recording(self):
        path = filedialog.askopenfilename(filetypes=RECORDING_TYPES, parent=self)
        if not path:
            return None
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
            events = []
            for e in data["events"]:     # Version 1: [t, bits] (a digital pad)
                axes = tuple(int(v) for v in e[2]) if len(e) > 2 and e[2] else None
                events.append((float(e[0]), int(e[1]), axes, int(e[3]) if len(e) > 3 else 0,
                               int(e[4]) if len(e) > 4 else 0))
            device = data.get("device", DEVICES[0][0])
        except (OSError, ValueError, KeyError, TypeError, IndexError) as e:
            return self.app.error("Can't read %s: %s" % (path, e))
        if self.recording_on:
            self.toggle_record()
        self.recording = events
        self.recording_device = device if device in DEVICE else DEVICES[0][0]
        self.fill_events()
        self.record_status.configure(text="Loaded %d event(s) of the %s from %s" % (
            len(events), self.recording_device, os.path.basename(path)))
        return None

    # ---------------------------------------------------------------- playback

    def toggle_play(self):
        if self.playing:
            self.stop_play()
            return None
        if self.recording_on:
            return None
        if not self.recording:
            return self.app.error("Nothing to play: record something, or load a recording.")
        if self.recording_device and self.recording_device != self.device.get():
            self.device.set(self.recording_device)      # (the device recorded)
            self.device_changed()
        if self.connected:
            self.start_play()
        else:
            self.want_play = True
            self.toggle()
        return None

    def start_play(self):
        self.release_mouse()
        self.playing = True
        self.play_index = 0
        self.play_start = time.time()
        self.record_button.configure(state="disabled")
        self.play_button.configure(text="■ Stop", bootstyle="danger")
        self._play_step()

    def stop_play(self):
        self.playing = False
        self.want_play = False
        if self.play_job:
            self.after_cancel(self.play_job)
            self.play_job = None
        self.play_state = None
        self.events.selection_remove(*self.events.selection())
        self.record_button.configure(state="normal")
        self.play_button.configure(text="▶ Play", bootstyle="primary-outline")
        self.send()

    def _play_step(self):
        if not self.playing:
            return
        if self.play_index >= len(self.recording):
            if self.loop.get() and self.recording:
                self.play_index = 0
                self.play_start = time.time()
            else:
                self.stop_play()
                return
        offset = self.recording[self.play_index][0]
        delay = max(0, offset - (time.time() - self.play_start))
        self.play_job = self.after(int(delay * 1000), self._play_event)

    def _play_event(self):
        if not self.playing:
            return
        t, bits, axes, dx, dy = self.recording[self.play_index]
        self.play_state = (bits, axes)
        self.move[0] += dx
        self.move[1] += dy
        rows = self.events.get_children()
        if self.play_index < len(rows):
            self.events.selection_set(rows[self.play_index])
            self.events.see(rows[self.play_index])
        self.play_index += 1
        self.send()
        self._play_step()
