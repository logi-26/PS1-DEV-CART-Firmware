"""
PS1 DEV CART GUI: Model viewer page (TMD model viewer with textures and TOD animations). The TIM and VRAM viewer has
its own page (pages/tim.py).

"Show on the PS1" starts the console's model viewer (ps1transfer -m ... -tod ... -mdllive, see ps1transfer/mdllive.c).
Its controls are then here too, linked live with the console: the TOD animation played, its frame (play, pause, step,
the frame slider), loop and speed, and the view (rotation, zoom); a change made with the controller shows here, and the
other way round.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import math
import os
import time
import tkinter as tk

import ttkbootstrap as tb

from pages.base import Page
from widgets import FileField, FileList

TIM_TYPES = [("TIM images", "*.tim"), ("All files", "*.*")]
TOD_TYPES = [("TOD animations", "*.tod"), ("All files", "*.*")]
MAX_TODS = 16
PROTOCOL = ("MDLLIVE", "MDLINFO", "MDLSTATE")   # Answers of ps1transfer -mdllive (not in the log)
SPEEDS = (10, 25, 50, 100, 200, 400)
QUIET = 0.6             # s: a control changed here is not set back by the console's state meanwhile
SEND_MS = 40            # Slider changes sent together
STATE_KEYS = ("tod", "frame", "first", "last", "playing", "loop", "speed", "rx", "ry", "rz", "dist", "objects",
              "polygons", "skipped")


def to_degrees(r):
    """Viewer rotation (4096 = 360 degrees, 0..4095) to -180..180 degrees."""
    d = round(r * 360 / 4096)
    return d - 360 if d > 180 else d


class ViewersPage(Page):
    title = "Model & Animation Viewer"

    def build(self):
        self.live = False
        self.disconnected = False
        self.state = {}
        self.changed = {}           # Control -> time changed here
        self.dist_range = (1, 1)
        self.jobs = {}              # Pending sends of the sliders
        self.updating = False
        self.tod_names = []

        tb.Label(self, text=self.title, font=("", 16, "bold")).pack(anchor="w", pady=(0, 4))
        tb.Label(self, text="A TMD model with its TIM textures (loaded at their VRAM position) and TOD animations. "
                            "Once it runs, the controls below and the console's controller act on the same viewer: "
                            "play, pause and step the animations, rotate and zoom.",
                 wraplength=1000, justify="left", bootstyle="secondary").pack(anchor="w", pady=(0, 6))

        box = self.section("Model, textures and animations")
        box.pack(fill="both", expand=True, pady=(0, 6))     # (the lists get the page's spare height)
        self.tmd = FileField(box, self.app, "view.tmd", "Model (TMD)", width=30,
                             filetypes=[("TMD models", "*.tmd"), ("All files", "*.*")])
        self.tmd.pack(fill="x", pady=2)
        self.textures = FileList(box, self.app, "view.textures", "Textures (TIM)", filetypes=TIM_TYPES, max_files=8,
                                 height=2, compact=True)
        self.textures.pack(fill="both", expand=True, pady=(4, 0))
        self.tods = FileList(box, self.app, "view.tods", "Animations (TOD)", filetypes=TOD_TYPES, max_files=MAX_TODS,
                             height=3, compact=True)
        self.tods.pack(fill="both", expand=True, pady=(4, 0))
        bar = tb.Frame(box)
        bar.pack(fill="x", pady=(6, 0))
        tb.Button(bar, text="Show on the PS1", bootstyle="success", command=self.model).pack(side="left")
        tb.Button(bar, text="Stop", bootstyle="danger-outline", width=8, command=self.app.stop_task).pack(
            side="left", padx=6)
        self.status = tb.Label(bar, text="Not running.", bootstyle="secondary")
        self.status.pack(side="left", padx=12)

        body = tb.Frame(self)
        body.pack(fill="x")
        self.anim_controls = []     # Enabled while linked, with animations
        self.view_controls = []     # Enabled while linked

        # Animation: TOD, transport, frame, loop, speed
        box = self.section("Animation", body)
        box.pack(side="left", fill="both", expand=True, padx=(0, 8))
        row = tb.Frame(box)
        row.pack(fill="x")
        tb.Label(row, text="TOD", width=7).pack(side="left")
        b = tb.Button(row, text="◀", bootstyle="secondary-outline", width=3, command=lambda: self.step_tod(-1))
        b.pack(side="left")
        self.anim_controls.append(b)
        self.tod_choice = tb.Combobox(row, state="readonly", width=28)
        self.tod_choice.pack(side="left", padx=4, fill="x", expand=True)
        self.tod_choice.bind("<<ComboboxSelected>>", lambda e: self.set_tod(self.tod_choice.current()))
        self.anim_controls.append(self.tod_choice)
        b = tb.Button(row, text="▶", bootstyle="secondary-outline", width=3, command=lambda: self.step_tod(1))
        b.pack(side="left")
        self.anim_controls.append(b)

        row = tb.Frame(box)
        row.pack(fill="x", pady=(8, 0))
        # First frame, previous frame (L2), play/pause (Circle), next frame (R2), last frame
        for text, command in (("|◀◀", lambda: self.set_frame(self.state.get("first", 0))),
                              ("◀", lambda: self.step_frame(-1)),
                              (None, None),
                              ("▶", lambda: self.step_frame(1)),
                              ("▶▶|", lambda: self.set_frame(self.state.get("last", 0)))):
            if text is None:
                self.play_button = tb.Button(row, text="▶  Play", bootstyle="primary", width=10,
                                             command=self.toggle_play)
                self.play_button.pack(side="left", padx=4)
                self.anim_controls.append(self.play_button)
                continue
            b = tb.Button(row, text=text, bootstyle="secondary-outline", width=4, command=command)
            b.pack(side="left", padx=2)
            self.anim_controls.append(b)
        self.loop = tk.BooleanVar(value=True)
        b = tb.Checkbutton(row, text="Loop", variable=self.loop, bootstyle="round-toggle", command=self.send_loop)
        b.pack(side="left", padx=(16, 0))
        self.anim_controls.append(b)
        tb.Label(row, text="Speed").pack(side="left", padx=(16, 4))
        self.speed_choice = tb.Combobox(row, state="readonly", width=6, values=["%d%%" % s for s in SPEEDS])
        self.speed_choice.current(SPEEDS.index(100))
        self.speed_choice.pack(side="left")
        self.speed_choice.bind("<<ComboboxSelected>>", lambda e: self.set_speed())
        self.anim_controls.append(self.speed_choice)

        row = tb.Frame(box)
        row.pack(fill="x", pady=(10, 0))
        self.frame_var = tk.IntVar(value=0)
        self.frame_scale = tb.Scale(row, from_=0, to=1, variable=self.frame_var,
                                    command=lambda v: self.frame_moved())
        self.frame_scale.pack(side="left", fill="x", expand=True)
        self.anim_controls.append(self.frame_scale)
        self.frame_label = tb.Label(row, text="Frame -", width=16, anchor="e")
        self.frame_label.pack(side="left", padx=(8, 0))
        self.info = tb.Label(box, text="", bootstyle="secondary")
        self.info.pack(anchor="w", pady=(8, 0))
        tb.Label(box, text="On the PS1: Circle play/pause, L2/R2 frame, Square next TOD.", bootstyle="secondary").pack(
            anchor="w")

        # View: rotation, zoom
        box = self.section("View", body)
        box.pack(side="left", fill="y")
        self.rot_vars, self.rot_labels = [], []
        for i, axis in enumerate(("X", "Y", "Z")):
            row = tb.Frame(box)
            row.pack(fill="x", pady=1)
            tb.Label(row, text="Rotate %s" % axis, width=9).pack(side="left")
            var = tk.IntVar(value=0)
            scale = tb.Scale(row, from_=-180, to=180, variable=var, length=200, command=lambda v: self.rot_moved())
            scale.pack(side="left")
            self.view_controls.append(scale)
            label = tb.Label(row, text="0°", width=5, anchor="e")
            label.pack(side="left")
            self.rot_vars.append(var)
            self.rot_labels.append(label)
        row = tb.Frame(box)
        row.pack(fill="x", pady=(6, 1))
        tb.Label(row, text="Zoom", width=9).pack(side="left")
        self.zoom_var = tk.IntVar(value=50)
        scale = tb.Scale(row, from_=0, to=100, variable=self.zoom_var, length=200, command=lambda v: self.zoom_moved())
        scale.pack(side="left")
        self.view_controls.append(scale)
        b = tb.Button(box, text="Reset the view", bootstyle="secondary-outline", command=self.reset_view)
        b.pack(anchor="w", pady=(8, 0))
        self.view_controls.append(b)
        tb.Label(box, text="On the PS1: D-pad, L1/R1 rotate, Triangle/Cross zoom, Start reset.",
                 bootstyle="secondary", wraplength=300).pack(anchor="w", pady=(6, 0))

        self.enable()

    # ---------------------------------------------------------------- console

    def args(self):
        if not self.tmd.get():
            return None
        a = ["-m", self.tmd.get()]
        for path in self.textures.files():
            a += ["-t", path]
        for path in self.tods.files():
            a += ["-tod", path]
        return a + ["-mdllive"]

    def model(self):
        if not os.path.isfile(self.tmd.get()):
            return self.app.error("Choose the TMD model.")
        self.tmd.remember()
        self.tod_names = [os.path.basename(p) for p in self.tods.files()]
        self.tod_choice.configure(values=self.tod_names or ["(no animation)"])
        self.live = False
        self.disconnected = False
        self.status.configure(text="Starting the console's model viewer...")
        self.app.run(self.args(), "Model viewer", long_running=True, stdin=True,
                     cwd=os.path.dirname(self.tmd.get()), on_line=lambda line: self.after(0, self.line, line),
                     log_filter=lambda line: not line.startswith(PROTOCOL),
                     on_done=lambda code, out: self.after(0, self.stopped))
        return None

    def stopped(self):
        self.live = False
        self.status.configure(text="Console disconnected." if self.disconnected else "Stopped.")
        self.disconnected = False
        self.enable()

    def send(self, line):
        return self.live and self.app.runner.send(line)

    def line(self, line):
        fields = line.split()
        if line.startswith("MDLLIVE ready"):
            self.live = True
            self.status.configure(text="Live: the controls here and the controller act on the console's viewer.")
            self.enable()
        elif line.startswith("MDLLIVE disconnected"):
            self.disconnected = True
        elif line.startswith("MDLINFO") and len(fields) >= 4:
            self.dist_range = (max(1, int(fields[2])), max(2, int(fields[3])))
            self.enable()
        elif line.startswith("MDLSTATE") and len(fields) >= 15:
            try:
                self.console_state(dict(zip(STATE_KEYS, (int(v) for v in fields[1:15]))))
            except ValueError:
                pass
        elif line.startswith("MDLERR"):
            self.status.configure(text=line[7:])

    def quiet(self, name):
        return time.time() - self.changed.get(name, 0) < QUIET

    def console_state(self, s):
        """The console's state (changed with its controller, or applied from here): shown, except a control just
        changed here (the console may not have applied it yet)."""
        self.state = s
        self.updating = True
        try:
            if s["tod"] >= 0 and not self.quiet("tod") and s["tod"] < len(self.tod_names):
                self.tod_choice.current(s["tod"])
            if not self.quiet("frame"):
                self.frame_scale.configure(from_=s["first"], to=max(s["last"], s["first"] + 1))
                self.frame_var.set(s["frame"])
            if not self.quiet("loop"):
                self.loop.set(bool(s["loop"]))
            if not self.quiet("speed") and s["speed"] in SPEEDS:
                self.speed_choice.current(SPEEDS.index(s["speed"]))
            if not self.quiet("rot"):
                for var, label, r in zip(self.rot_vars, self.rot_labels, (s["rx"], s["ry"], s["rz"])):
                    var.set(to_degrees(r))
                    label.configure(text="%d°" % to_degrees(r))
            if not self.quiet("zoom"):
                self.zoom_var.set(self.zoom_of(s["dist"]))
        finally:
            self.updating = False
        self.play_button.configure(text="❘❘  Pause" if s["playing"] else "▶  Play")
        if s["tod"] >= 0:
            self.frame_label.configure(text="Frame %d / %d" % (s["frame"], s["last"]))
        else:
            self.frame_label.configure(text="No animation")
        self.info.configure(text="%d objects, %d polygons drawn%s" % (
            s["objects"], s["polygons"],
            ", %d TOD packets not used (light, camera...)" % s["skipped"] if s["skipped"] else ""))
        self.enable()

    def enable(self):
        anim = self.live and self.state.get("tod", -1) >= 0
        for widgets, on in ((self.anim_controls, anim), (self.view_controls, self.live)):
            for w in widgets:
                try:
                    w.configure(state=("readonly" if isinstance(w, tb.Combobox) else "normal") if on else "disabled")
                except tk.TclError:
                    pass

    # ---------------------------------------------------------------- controls

    def later(self, name, fn):
        """A slider's change sent at most every SEND_MS (the last value)."""
        self.changed[name] = time.time()
        if name not in self.jobs:
            self.jobs[name] = self.after(SEND_MS, lambda: (self.jobs.pop(name, None), fn()))

    def set_tod(self, index):
        if self.updating or not 0 <= index < len(self.tod_names):
            return
        self.changed["tod"] = time.time()
        self.tod_choice.current(index)
        self.send("tod %d" % index)

    def step_tod(self, step):
        if self.tod_names:
            self.set_tod((max(self.tod_choice.current(), 0) + step) % len(self.tod_names))

    def toggle_play(self):
        self.send("play %d" % (0 if self.state.get("playing") else 1))

    def set_frame(self, frame):
        self.changed["frame"] = time.time()
        self.frame_var.set(frame)
        self.send("frame %d" % frame)

    def step_frame(self, step):
        self.set_frame(self.frame_var.get() + step)

    def frame_moved(self):
        if not self.updating:
            self.later("frame", lambda: self.send("frame %d" % self.frame_var.get()))

    def send_loop(self):
        self.changed["loop"] = time.time()
        self.send("loop %d" % (1 if self.loop.get() else 0))

    def set_speed(self):
        self.changed["speed"] = time.time()
        self.send("speed %d" % SPEEDS[self.speed_choice.current()])

    def rot_moved(self):
        if self.updating:
            return
        for var, label in zip(self.rot_vars, self.rot_labels):
            label.configure(text="%d°" % var.get())
        self.later("rot", lambda: self.send("rot %d %d %d" % tuple(round(v.get() * 4096 / 360) & 4095
                                                                   for v in self.rot_vars)))

    def zoom_of(self, dist):
        """Camera distance to the zoom slider (0-100, logarithmic between the viewer's limits; 100: nearest)."""
        lo, hi = self.dist_range
        dist = min(max(dist, lo), hi)
        return round(100 - 100 * math.log(dist / lo) / math.log(hi / lo)) if hi > lo else 50

    def dist_of(self, zoom):
        lo, hi = self.dist_range
        return round(lo * (hi / lo) ** ((100 - zoom) / 100))

    def zoom_moved(self):
        if not self.updating:
            self.later("zoom", lambda: self.send("dist %d" % self.dist_of(self.zoom_var.get())))

    def reset_view(self):
        self.changed.pop("rot", None)
        self.changed.pop("zoom", None)
        self.send("view")
