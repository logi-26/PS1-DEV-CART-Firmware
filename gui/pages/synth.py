"""
PS1 DEV CART GUI: Synth page. The PS1 as a 16 slot sample synth (ps1-tools/synth/synth.c), driven by "ps1transfer -synth":

- 16 slots, slot n plays on MIDI channel n. A slot takes a VAG or a WAV file (encoded to VAG here, vag.py), sent
  to the SPU RAM while the synth plays.
- Settings of each slot (root note, volume, pan, envelope, reverb, fine tune), changed live.
- An on-screen keyboard (mouse, or the computer keyboard), MIDI input from external gear (with mido and
  python-rtmidi), and samples received as SysEx (sysex.py) or from .syx files.
- Kits: the 16 slots (files and settings) saved to a file; the last kit is sent again when the synth starts empty.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import json
import os
import queue
import tempfile
import threading
import time
import tkinter as tk
from tkinter import filedialog

import ttkbootstrap as tb

import sysex
import vag
from pages.base import Page

try:
    import mido
except ImportError:     # MIDI input is optional
    mido = None

SLOTS = 16
PARAMS = ["root", "volume", "pan", "attack", "decay", "sustain", "release", "reverb", "tune"]   # synth.h order
PARAM_RANGE = {"root": 127, "volume": 127, "pan": 127, "attack": 99, "decay": 99, "sustain": 99, "release": 99,
               "reverb": 1, "tune": 100}
DEFAULTS = {"root": 60, "volume": 100, "pan": 64, "attack": 0, "decay": 0, "sustain": 99, "release": 20, "reverb": 0,
            "tune": 50}
SAMPLE_TYPES = [("Samples", "*.wav *.vag"), ("All files", "*.*")]
KIT_TYPES = [("PS1 DEV CART synth kit", "*.ps1kit"), ("All files", "*.*")]
SYX_TYPES = [("SysEx", "*.syx"), ("All files", "*.*")]
RATES = ["Original", "44100", "32000", "22050", "16000", "11025", "8000"]
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
# Computer keyboard: two rows of keys from the keyboard's first C (like a tracker)
KEYS = {k: i for i, k in enumerate(["z", "s", "x", "d", "c", "v", "g", "b", "h", "n", "j", "m", "comma", "l", "period",
                                    "semicolon", "slash"])}
KEYS.update({k: 12 + i for i, k in enumerate(["q", "2", "w", "3", "e", "r", "5", "t", "6", "y", "7", "u", "i", "9",
                                              "o", "0", "p"])})
WHITE = [0, 2, 4, 5, 7, 9, 11]
OCTAVES = 3


def note_name(n):
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 1)


def work_dir():
    path = os.path.join(tempfile.gettempdir(), "ps1devcart-synth")
    os.makedirs(path, exist_ok=True)
    return path


class SynthPage(Page):
    title = "Synth"

    def build(self):
        self.slots = [{"size": 0, "name": "", "loop": 0, "playing": 0, "rate": 0, **DEFAULTS} for _ in range(SLOTS)]
        self.kit = self._load_kit_setting()
        self.vag_files = [None] * SLOTS     # VAG file sent to each slot (for .syx export)
        self.selected = 0
        self.connected = False
        self.expect = []                # Answers awaited from the bridge, in order ("state" or a command label)
        self.state_lines = []
        self.events = queue.Queue()     # From other threads: encoded samples, MIDI input
        self.edit_until = 0             # The editor isn't overwritten by the state while the user changes it
        self.updating = False
        self.send_after = {}
        self.held = {}                  # Notes shown on the keyboard: note -> count
        self.keys_down = set()
        self.base = 48                  # First note of the keyboard (C3)
        self.midi_port = None
        self.receiver = sysex.Receiver()
        self.restore = False

        head = tb.Frame(self)
        head.pack(fill="x")
        tb.Label(head, text="SPU synth", font=("", 16, "bold")).pack(side="left")
        self.status = tb.Label(head, text="Not connected", bootstyle="secondary")
        self.status.pack(side="left", padx=16)
        tb.Label(self, text="The PS1 as a sample synth: 16 slots, slot n plays on MIDI channel n. Samples are loaded "
                            "while it plays.", bootstyle="secondary").pack(anchor="w")

        bar = tb.Frame(self)
        bar.pack(fill="x", pady=6)
        tb.Button(bar, text="Start the synth", bootstyle="primary", command=self.start).pack(side="left")
        tb.Button(bar, text="Disconnect", bootstyle="secondary-outline", command=self.app.stop_task).pack(side="left",
                                                                                                          padx=4)
        tb.Button(bar, text="Clear all slots", bootstyle="secondary-outline", command=self.reset).pack(side="left")
        tb.Button(bar, text="All notes off", bootstyle="danger-outline", command=self.panic).pack(side="left", padx=4)
        for text, cmd in (("Slot to .syx...", self.export_syx), ("Open .syx...", self.open_syx),
                          ("Save kit...", self.save_kit), ("Open kit...", self.open_kit)):
            tb.Button(bar, text=text, bootstyle="secondary-outline", command=cmd).pack(side="right", padx=(4, 0))

        body = tb.Frame(self)
        body.pack(fill="both", expand=True)
        self._build_slots(body)
        self._build_editor(body)
        self._build_keyboard()
        self._show_editor()
        self._refresh_grid()
        self.after(100, self._tick)

    # ---------------------------------------------------------------- layout

    def _build_slots(self, master):
        frame = self.section("Slots (click: select, double click: load, right click: menu)", master)
        frame.pack(side="left", fill="both", expand=True)
        self.cells = []
        for i in range(SLOTS):
            cell = tb.Frame(frame, padding=(6, 3), bootstyle="secondary")
            cell.grid(row=i // 4, column=i % 4, sticky="nsew", padx=2, pady=2)
            name = tb.Label(cell, text="", font=("", 10, "bold"), width=16, bootstyle="inverse-secondary")
            name.pack(anchor="w")
            info = tb.Label(cell, text="", font=("", 8), bootstyle="inverse-secondary")
            info.pack(anchor="w")
            for w in (cell, name, info):
                w.bind("<Button-1>", lambda e, s=i: self.select(s))
                w.bind("<Double-Button-1>", lambda e, s=i: self.load_dialog(s))
                w.bind("<Button-3>", lambda e, s=i: self._menu(e, s))
            self.cells.append((cell, name, info))
        for c in range(4):
            frame.columnconfigure(c, weight=1)
        for r in range(4):
            frame.rowconfigure(r, weight=1)

    def _build_editor(self, master):
        frame = self.section("Selected slot", master)
        frame.pack(side="left", fill="y", padx=(8, 0))
        self.ed_title = tb.Label(frame, text="", font=("", 10, "bold"))
        self.ed_title.grid(row=0, column=0, columnspan=3, sticky="w")
        self.ed_file = tb.Label(frame, text="", bootstyle="secondary")
        self.ed_file.grid(row=1, column=0, columnspan=3, sticky="w", pady=(0, 2))
        self.vars, self.value_labels = {}, {}
        for r, p in enumerate(PARAMS):
            tb.Label(frame, text=p.capitalize()).grid(row=2 + r, column=0, sticky="w")
            var = tk.IntVar(value=DEFAULTS[p])
            self.vars[p] = var
            if p == "reverb":
                tb.Checkbutton(frame, variable=var, bootstyle="round-toggle",
                               command=lambda p=p: self._param_changed(p)).grid(row=2 + r, column=1, sticky="w")
            else:
                tb.Scale(frame, from_=0, to=PARAM_RANGE[p], variable=var, length=170,
                         command=lambda v, p=p: self._param_changed(p)).grid(row=2 + r, column=1, sticky="we", padx=6)
            self.value_labels[p] = tb.Label(frame, text="", width=5)
            self.value_labels[p].grid(row=2 + r, column=2, sticky="w")
        row = 2 + len(PARAMS)
        opts = tb.Frame(frame)
        opts.grid(row=row, column=0, columnspan=3, sticky="w", pady=(6, 0))
        tb.Label(opts, text="WAV rate").pack(side="left")
        self.rate = tb.Combobox(opts, values=RATES, width=8, state="readonly")
        self.rate.set(self.app.settings.field("synth.rate", "22050"))
        self.rate.pack(side="left", padx=4)
        self.rate.bind("<<ComboboxSelected>>", lambda e: self.app.settings.set_field("synth.rate", self.rate.get()))
        self.loop = tk.BooleanVar(value=False)
        tb.Checkbutton(opts, text="Loop from", variable=self.loop).pack(side="left", padx=(6, 2))
        self.loop_start = tb.Entry(opts, width=4)
        self.loop_start.insert(0, "0")
        self.loop_start.pack(side="left")
        tb.Label(opts, text="s").pack(side="left", padx=2)
        buttons = tb.Frame(frame)
        buttons.grid(row=row + 1, column=0, columnspan=3, sticky="w", pady=(6, 0))
        tb.Button(buttons, text="Load sample...", bootstyle="primary-outline",
                  command=lambda: self.load_dialog(self.selected)).pack(side="left")
        tb.Button(buttons, text="Clear", bootstyle="secondary-outline",
                  command=lambda: self.clear(self.selected)).pack(side="left", padx=4)
        tb.Button(buttons, text="Defaults", bootstyle="secondary-outline", command=self.defaults).pack(side="left")

    def _build_keyboard(self):
        frame = self.section("Keyboard: plays the selected slot (click it, then the Z S X ... and Q 2 W ... keys)")
        frame.pack(fill="x", pady=(6, 0))
        top = tb.Frame(frame)
        top.pack(fill="x")
        tb.Button(top, text="<", width=2, bootstyle="secondary-outline", command=lambda: self.octave(-12)).pack(
            side="left")
        self.range_label = tb.Label(top, text="", width=9, anchor="center")
        self.range_label.pack(side="left")
        tb.Button(top, text=">", width=2, bootstyle="secondary-outline", command=lambda: self.octave(12)).pack(
            side="left")
        tb.Label(top, text="Velocity").pack(side="left", padx=(12, 4))
        self.velocity = tk.IntVar(value=100)
        tb.Scale(top, from_=1, to=127, variable=self.velocity, length=110).pack(side="left")
        # MIDI input (external gear through a MIDI interface)
        tb.Label(top, text="MIDI in").pack(side="left", padx=(16, 4))
        if mido:
            self.port = tb.Combobox(top, width=24, state="readonly")
            self.port.pack(side="left")
            tb.Button(top, text="\u21bb", width=2, bootstyle="secondary-outline", command=self.refresh_ports).pack(
                side="left", padx=2)
            self.port_button = tb.Button(top, text="Connect", bootstyle="success-outline", command=self.toggle_port)
            self.port_button.pack(side="left")
            self.midi_mode = tb.Combobox(top, values=["Channel n = slot n", "All to the selected slot"], width=20,
                                         state="readonly")
            self.midi_mode.set(self.app.settings.field("synth.midi_mode", "Channel n = slot n"))
            self.midi_mode.bind("<<ComboboxSelected>>",
                                lambda e: self.app.settings.set_field("synth.midi_mode", self.midi_mode.get()))
            self.midi_mode.pack(side="left", padx=4)
            self.refresh_ports()
        else:
            tb.Label(top, text="needs: pip install mido python-rtmidi", bootstyle="warning").pack(side="left")
        self.piano = tk.Canvas(frame, height=64, highlightthickness=1, takefocus=1, bg="#202020")
        self.piano.pack(fill="x", pady=(6, 0))
        self.piano.bind("<Configure>", lambda e: self._draw_piano())
        self.piano.bind("<ButtonPress-1>", self._piano_press)
        self.piano.bind("<B1-Motion>", self._piano_drag)
        self.piano.bind("<ButtonRelease-1>", self._piano_release)
        self.piano.bind("<KeyPress>", self._key_press)
        self.piano.bind("<KeyRelease>", self._key_release)
        self.piano.bind("<FocusOut>", lambda e: self._all_keys_up())
        self.mouse_note = None

    # ---------------------------------------------------------------- bridge

    def args(self):
        return ["-synth"]

    def start(self):
        if self.connected:
            return
        self.expect, self.state_lines = [], []
        self.status.configure(text="Starting...", bootstyle="warning")
        self.app.run(["-synth"], "Synth", long_running=True, stdin=True, on_line=self._line, on_done=self._done,
                     log_filter=self._log_line)

    @staticmethod
    def _log_line(line):
        return not (line.startswith("slot ") or line.startswith("spu ") or line in ("end", "ok", "ready"))

    def _send(self, line, expect=None):
        if not self.connected or not self.app.runner.send(line):
            return False
        if expect:
            self.expect.append(expect)
        return True

    def _command(self, line, label):
        if not self.connected:
            self.app.error("Start the synth first.")
            return False
        return self._send(line, label)

    def _line(self, line):
        if line == "ready":
            self.connected = True
            self.restore = True     # Once the first state shows the slots
            self.status.configure(text="Connected", bootstyle="success")
            self._request_state()
        elif line.startswith("slot ") or line.startswith("spu "):
            self.state_lines.append(line)
        elif line == "end":
            if self.expect and self.expect[0] == "state":
                self.expect.pop(0)
            self._apply_state(self.state_lines)
            self.state_lines = []
        elif line == "ok" or line.startswith("error"):
            label = self.expect.pop(0) if self.expect else ""
            if line.startswith("error"):
                self.app.log.append("Synth: %s: %s\n" % (label, line[6:]))

    def _done(self, code, output):
        self.connected = False
        self.expect = []
        self.status.configure(text="Not connected (the synth keeps running on the PS1)" if code == 0 or
                              self.app.task_stopped else "Not connected", bootstyle="secondary")

    def _request_state(self):
        if self.connected and "state" not in self.expect and len(self.expect) < 4:
            self._send("state", "state")

    def _apply_state(self, lines):
        for line in lines:
            f = line.split()
            if f[0] == "slot":
                s = self.slots[int(f[1]) - 1]
                for kv in f[2:]:
                    k, _, v = kv.partition("=")
                    s[k] = v if k == "name" else int(v or 0)
            elif f[0] == "spu":
                info = dict(kv.split("=", 1) for kv in f[1:])
                self.status.configure(text="Connected, SPU RAM free: %dK of %dK, MIDI messages: %s" % (
                    int(info.get("free", 0)) // 1024, int(info.get("total", 0)) // 1024, info.get("midi", "0")))
        self._refresh_grid()
        if time.time() > self.edit_until:
            self._show_editor()
        if self.restore:
            self.restore = False
            if not any(s["size"] for s in self.slots) and any(self.kit):
                self.app.log.append("Synth: sending the last kit\n")
                self.send_kit()

    def _tick(self):
        """Encoded samples and MIDI input from the other threads, state refresh."""
        try:
            while True:
                event = self.events.get_nowait()
                kind = event[0]
                if kind == "encoded":
                    self._encoded(*event[1:])
                elif kind == "error":
                    self.app.log.append("Synth: %s\n" % event[1])
                elif kind == "note":
                    self._show_note(event[1], event[2])
                elif kind == "sysex":
                    self._sysex(event[1])
        except queue.Empty:
            pass
        if self.connected and self.winfo_ismapped():
            self._request_state()
        self.after(250 if self.connected else 500, self._tick)

    # ---------------------------------------------------------------- slots

    def select(self, slot):
        self.selected = slot
        self.edit_until = 0
        self._refresh_grid()
        self._show_editor()

    def _menu(self, event, slot):
        self.select(slot)
        menu = tk.Menu(self, tearoff=0)
        menu.add_command(label="Load sample...", command=lambda: self.load_dialog(slot))
        menu.add_command(label="Clear", command=lambda: self.clear(slot))
        menu.add_command(label="Save as .syx...", command=self.export_syx)
        menu.tk_popup(event.x_root, event.y_root)

    def _refresh_grid(self):
        for i, (cell, name, info) in enumerate(self.cells):
            s = self.slots[i]
            style = "primary" if i == self.selected else ("success" if s["playing"] else "secondary")
            cell.configure(bootstyle=style)
            for w in (name, info):
                w.configure(bootstyle="inverse-" + style)
            name.configure(text="%d  %s" % (i + 1, s["name"] or "-"))
            if s["size"]:
                info.configure(text="%dK  %s Hz%s%s" % ((s["size"] + 1023) // 1024, s["rate"],
                                                        "  loop" if s["loop"] else "",
                                                        "  \u266a%d" % s["playing"] if s["playing"] else ""))
            else:
                info.configure(text="empty")

    def _show_editor(self):
        s = self.slots[self.selected]
        entry = self.kit[self.selected]
        self.ed_title.configure(text="Slot %d (MIDI channel %d): %s" % (self.selected + 1, self.selected + 1,
                                                                       s["name"] or "empty"))
        self.ed_file.configure(text=os.path.basename(entry["file"]) if entry else "")
        self.updating = True
        for p in PARAMS:
            self.vars[p].set(s[p])
            self._value_text(p)
        self.updating = False

    def _value_text(self, p):
        v = int(float(self.vars[p].get()))
        text = note_name(v) if p == "root" else ("%+d" % (v - 50) if p == "tune" else
                                                ("L%d" % (64 - v) if p == "pan" and v < 64 else
                                                 ("R%d" % (v - 64) if p == "pan" and v > 64 else
                                                  ("C" if p == "pan" else str(v)))))
        self.value_labels[p].configure(text=text)

    def _param_changed(self, p):
        if self.updating:
            return
        v = int(float(self.vars[p].get()))
        self.vars[p].set(v)
        self._value_text(p)
        slot = self.selected
        self.slots[slot][p] = v
        self.edit_until = time.time() + 1.5
        # Sent once the scale stops moving for a moment (fewer commands)
        key = (slot, p)
        if key in self.send_after:
            self.after_cancel(self.send_after[key])
        self.send_after[key] = self.after(40, lambda: self._send_param(slot, p, v))

    def _send_param(self, slot, p, value):
        # (value kept: a state read meanwhile still has the old one)
        self.send_after.pop((slot, p), None)
        self.slots[slot][p] = value
        self._send("set %d %s %d" % (slot + 1, p, value), "set %s" % p)
        if self.kit[slot]:
            self._save_kit_setting()

    def defaults(self):
        for p in PARAMS:
            if p != "root":
                self.vars[p].set(DEFAULTS[p])
                self._param_changed(p)

    def load_dialog(self, slot):
        self.select(slot)
        path = filedialog.askopenfilename(filetypes=SAMPLE_TYPES, parent=self,
                                          initialdir=self.app.settings.field("synth.dir", "") or None)
        if path:
            self.app.settings.set_field("synth.dir", os.path.dirname(path))
            self.load(slot, os.path.normpath(path))

    def _encode_options(self):
        rate = self.rate.get()
        try:
            loop_start = float(self.loop_start.get() or 0)
        except ValueError:
            loop_start = 0.0
        return {"rate": None if rate == "Original" else int(rate), "loop": self.loop.get(), "loop_start": loop_start}

    def load(self, slot, path, options=None, params=None):
        """Loads a WAV (encoded in a thread) or VAG file into a slot."""
        if not self.connected:
            return self.app.error("Start the synth first.")
        if not os.path.isfile(path):
            return self.app.error("File not found: %s" % path)
        options = options or self._encode_options()
        name = os.path.splitext(os.path.basename(path))[0][:15]
        self.kit[slot] = {"file": path, "options": options, "params": dict(params or {})}
        if not path.lower().endswith(".wav"):
            return self._encoded(slot, path, name)
        self.app.log.append("Synth: encoding %s...\n" % os.path.basename(path))
        out = os.path.join(work_dir(), "slot%d.vag" % (slot + 1))

        def encode():
            try:
                vag.wav_to_vag(path, out, options["rate"], options["loop"], options["loop_start"], name)
                self.events.put(("encoded", slot, out, name))
            except Exception as e:  # noqa: BLE001 (any bad WAV file)
                self.events.put(("error", "%s: %s" % (os.path.basename(path), e)))
        threading.Thread(target=encode, daemon=True).start()

    def _encoded(self, slot, vag_path, name):
        self.vag_files[slot] = vag_path
        if self._command("load %d %s\t%s" % (slot + 1, vag_path, name), "load %s" % name):
            # The slot settings of a kit, after the sample (loading keeps the settings of the slot)
            for p, v in (self.kit[slot] or {}).get("params", {}).items():
                if p in PARAMS:
                    self.slots[slot][p] = v
                    self._send("set %d %s %d" % (slot + 1, p, v), "set %s" % p)
            self._save_kit_setting()
            self.edit_until = 0
            self._show_editor()

    def clear(self, slot):
        if self._command("clear %d" % (slot + 1), "clear"):
            self.kit[slot] = None
            self.vag_files[slot] = None
            self._save_kit_setting()

    def reset(self):
        if self._command("reset", "reset"):
            self.kit = [None] * SLOTS
            self.vag_files = [None] * SLOTS
            self._save_kit_setting()

    def panic(self):
        self.held.clear()
        self._draw_piano()
        for ch in range(SLOTS):
            self._send("midi %02x 7b 00 %02x 40 00" % (0xB0 | ch, 0xB0 | ch))

    # ---------------------------------------------------------------- kits

    def _load_kit_setting(self):
        kit = self.app.settings.field("synth.kit", [])
        return (list(kit) + [None] * SLOTS)[:SLOTS] if isinstance(kit, list) else [None] * SLOTS

    def _save_kit_setting(self):
        for i, entry in enumerate(self.kit):
            if entry:
                entry["params"] = {p: self.slots[i][p] for p in PARAMS}
        self.app.settings.set_field("synth.kit", self.kit)

    def send_kit(self):
        for slot, entry in enumerate(self.kit):
            if entry and os.path.isfile(entry.get("file", "")):
                self.load(slot, entry["file"], entry.get("options"), entry.get("params"))

    def save_kit(self):
        path = filedialog.asksaveasfilename(filetypes=KIT_TYPES, defaultextension=".ps1kit", parent=self)
        if path:
            self._save_kit_setting()
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"version": 1, "slots": self.kit}, f, indent=1)

    def open_kit(self):
        path = filedialog.askopenfilename(filetypes=KIT_TYPES, parent=self)
        if not path:
            return
        try:
            with open(path, encoding="utf-8") as f:
                slots = json.load(f)["slots"]
        except (OSError, ValueError, KeyError) as e:
            return self.app.error("Can't read the kit: %s" % e)
        if not self.connected:
            return self.app.error("Start the synth first.")
        self._command("reset", "reset")
        self.kit = (list(slots) + [None] * SLOTS)[:SLOTS]
        self.send_kit()

    # ---------------------------------------------------------------- SysEx

    def open_syx(self):
        path = filedialog.askopenfilename(filetypes=SYX_TYPES, parent=self)
        if path:
            with open(path, "rb") as f:
                for msg in sysex.split_syx(f.read()):
                    self._sysex(msg)

    def export_syx(self):
        slot = self.selected
        vag_path = self.vag_files[slot]
        if not vag_path or not os.path.isfile(vag_path):
            return self.app.error("Load a sample into slot %d first." % (slot + 1))
        path = filedialog.asksaveasfilename(filetypes=SYX_TYPES, defaultextension=".syx", parent=self,
                                            initialfile="slot%d.syx" % (slot + 1))
        if path:
            with open(vag_path, "rb") as f:
                data = sysex.vag_to_syx(f.read(), slot)
            with open(path, "wb") as f:
                f.write(data)
            self.app.log.append("Synth: %s saved (%d bytes)\n" % (path, len(data)))

    def _sysex(self, msg):
        result = self.receiver.feed(msg)
        if not result:
            return
        kind = result[0]
        if kind == "error":
            self.app.log.append("Synth: %s\n" % result[1])
        elif kind == "load":
            _, slot, adpcm, rate, name = result
            path = os.path.join(work_dir(), "sysex%d.vag" % (slot + 1))
            with open(path, "wb") as f:
                f.write(vag.vag_bytes(adpcm, rate, name))
            self.app.log.append("Synth: SysEx sample %s for slot %d\n" % (name, slot + 1))
            self.kit[slot] = None       # Not a file of this computer: not in the kit
            self._encoded(slot, path, name or "sysex")
        elif kind == "set" and result[2] < len(PARAMS):
            _, slot, param, value = result
            p = PARAMS[param]
            self.slots[slot][p] = min(value, PARAM_RANGE[p])
            self._send("set %d %s %d" % (slot + 1, p, self.slots[slot][p]), "set %s" % p)
        elif kind == "clear":
            self.clear(result[1])

    # ---------------------------------------------------------------- MIDI input

    def refresh_ports(self):
        try:
            names = mido.get_input_names()
        except Exception as e:  # noqa: BLE001 (no backend)
            names = []
            self.app.log.append("MIDI: %s\n" % e)
        self.port.configure(values=names)
        wanted = self.app.settings.field("synth.midi_port", "")
        self.port.set(wanted if wanted in names else (names[0] if names else ""))

    def toggle_port(self):
        if self.midi_port:
            self.midi_port.close()
            self.midi_port = None
            self.port_button.configure(text="Connect", bootstyle="success-outline")
            return
        name = self.port.get()
        if not name:
            return self.app.error("No MIDI input. Connect the MIDI interface, then Refresh.")
        try:
            self.midi_port = mido.open_input(name, callback=self._midi_in)
        except Exception as e:  # noqa: BLE001 (port busy, ...)
            return self.app.error("Can't open %s: %s" % (name, e))
        self.app.settings.set_field("synth.midi_port", name)
        self.port_button.configure(text="Disconnect", bootstyle="danger-outline")

    def _midi_in(self, msg):
        """MIDI input thread: messages sent to the synth at once (lowest latency), SysEx and display via events."""
        if msg.type == "sysex":
            self.events.put(("sysex", bytes([0xF0] + list(msg.data) + [0xF7])))
            return
        data = msg.bytes()
        if not data or data[0] < 0x80 or data[0] >= 0xF0:
            return
        if self.midi_mode.get().startswith("All"):
            data[0] = (data[0] & 0xF0) | self.selected
        self._send("midi " + " ".join("%02x" % b for b in data))
        if msg.type in ("note_on", "note_off"):
            self.events.put(("note", msg.note, msg.type == "note_on" and msg.velocity > 0))

    # ---------------------------------------------------------------- keyboard

    def octave(self, step):
        self._all_keys_up()
        self.base = max(0, min(108 - 12 * OCTAVES, self.base + step))
        self._draw_piano()

    def note_on(self, note):
        if 0 <= note < 128:
            self._send("midi %02x %02x %02x" % (0x90 | self.selected, note, self.velocity.get()))
            self._show_note(note, True)

    def note_off(self, note):
        if 0 <= note < 128:
            self._send("midi %02x %02x 00" % (0x80 | self.selected, note))
            self._show_note(note, False)

    def _show_note(self, note, on):
        self.held[note] = max(0, self.held.get(note, 0) + (1 if on else -1))
        self._draw_piano()

    def _key_geometry(self):
        """(note, x0, y0, x1, y1, black) of each key, black keys last (drawn on top)."""
        w = max(self.piano.winfo_width(), 200)
        h = int(self.piano.cget("height"))
        white_w = w / (7 * OCTAVES + 1)
        keys, blacks = [], []
        for i in range(7 * OCTAVES + 1):
            note = self.base + (i // 7) * 12 + WHITE[i % 7]
            keys.append((note, i * white_w, 0, (i + 1) * white_w, h, False))
            if i % 7 not in (2, 6) and i < 7 * OCTAVES:
                x = (i + 1) * white_w - white_w * 0.3
                blacks.append((note + 1, x, 0, x + white_w * 0.6, h * 0.6, True))
        return keys + blacks

    def _draw_piano(self):
        c = self.piano
        c.delete("all")
        for note, x0, y0, x1, y1, black in self._key_geometry():
            on = self.held.get(note, 0) > 0
            fill = "#f0b030" if on else ("#202020" if black else "#f4f4f4")
            c.create_rectangle(x0, y0, x1, y1, fill=fill, outline="#606060")
            if not black and note % 12 == 0:
                c.create_text((x0 + x1) / 2, y1 - 10, text=note_name(note), fill="#606060", font=("", 7))
        self.range_label.configure(text="%s - %s" % (note_name(self.base), note_name(self.base + 12 * OCTAVES)))

    def _note_at(self, x, y):
        for note, x0, y0, x1, y1, black in reversed(self._key_geometry()):
            if x0 <= x < x1 and y0 <= y < y1:
                return note
        return None

    def _piano_press(self, event):
        self.piano.focus_set()
        self.mouse_note = self._note_at(event.x, event.y)
        if self.mouse_note is not None:
            self.note_on(self.mouse_note)

    def _piano_drag(self, event):
        note = self._note_at(event.x, event.y)
        if note != self.mouse_note:
            if self.mouse_note is not None:
                self.note_off(self.mouse_note)
            self.mouse_note = note
            if note is not None:
                self.note_on(note)

    def _piano_release(self, event):
        if self.mouse_note is not None:
            self.note_off(self.mouse_note)
            self.mouse_note = None

    def _key_press(self, event):
        key = event.keysym.lower()
        if key in KEYS and key not in self.keys_down:      # (auto repeat sends more key presses)
            self.keys_down.add(key)
            self.note_on(self.base + KEYS[key])

    def _key_release(self, event):
        key = event.keysym.lower()
        if key in self.keys_down:
            self.keys_down.discard(key)
            self.note_off(self.base + KEYS[key])

    def _all_keys_up(self):
        for key in list(self.keys_down):
            self.note_off(self.base + KEYS[key])
        self.keys_down.clear()
        if self.mouse_note is not None:
            self.note_off(self.mouse_note)
            self.mouse_note = None
