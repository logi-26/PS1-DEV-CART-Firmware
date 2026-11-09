"""
PS1 DEV CART GUI: the Music Tracker's piano roll (pages/tracker.py): a riff's notes as bars on a grid, the piano keys on
the left, time to the right. Click to put a note (drag to make it longer), drag a note to move it, its right end to
change its length, right click to remove it. Notes played together go on channels of their own (tracker.add_note).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import tkinter as tk

import ttkbootstrap as tb

import tracker

LOW, HIGH = 24, 108             # Notes shown: C1 to B7
KEY_W = 54
RULER_H = 18
KEY_H = 13
EDGE = 7                        # Pixels at a note's right end that change its length
BLACK = (1, 3, 6, 8, 10)
COLORS = dict(white_row="#1d2125", black_row="#16191c", c_line="#3a4048", bar="#56606b", beat="#30363d",
              row="#23282d", key_white="#d8dade", key_black="#2b2f34", key_text="#30343a", ruler="#2a2f35",
              ruler_text="#c8ccd0", play="#ffb347", sel="#ffffff", hover="#4fc1ff")
# Note colours by instrument
PALETTE = ("#4fc1ff", "#ff7b72", "#7ee787", "#d2a8ff", "#ffa657", "#79c0ff", "#f778ba", "#a5d6ff", "#e3b341",
           "#56d4dd", "#ff9bce", "#b4f08c")


def ins_color(ins):
    return PALETTE[(ins or 0) % len(PALETTE)]


class PianoRoll(tb.Frame):
    def __init__(self, parent, page):
        super().__init__(parent)
        self.page = page
        self.row_w = 22                 # Pixels a row
        self.sel = None                 # (row, channel) of the note selected
        self.mode = None                # Dragging: "move", "resize"
        self.drag_from = None
        self.orig = None
        self.undo_pushed = False
        self.keys = tk.Canvas(self, width=KEY_W, highlightthickness=0, background=COLORS["black_row"])
        self.ruler = tk.Canvas(self, height=RULER_H, highlightthickness=0, background=COLORS["ruler"])
        self.grid_c = tk.Canvas(self, highlightthickness=1, highlightcolor=COLORS["hover"],
                                highlightbackground="#30353b", background=COLORS["black_row"], takefocus=1)
        self.vbar = tb.Scrollbar(self, orient="vertical", command=self.yview)
        self.hbar = tb.Scrollbar(self, orient="horizontal", command=self.xview)
        tk.Frame(self, width=KEY_W, height=RULER_H, background=COLORS["ruler"]).grid(row=0, column=0)
        self.ruler.grid(row=0, column=1, sticky="ew")
        self.keys.grid(row=1, column=0, sticky="ns")
        self.grid_c.grid(row=1, column=1, sticky="nsew")
        self.vbar.grid(row=1, column=2, sticky="ns")
        self.hbar.grid(row=2, column=1, sticky="ew")
        self.rowconfigure(1, weight=1)
        self.columnconfigure(1, weight=1)
        self.grid_c.configure(yscrollcommand=self.vbar.set, xscrollcommand=self.hbar.set)
        g = self.grid_c
        g.bind("<ButtonPress-1>", self.press)
        g.bind("<B1-Motion>", self.motion)
        g.bind("<ButtonRelease-1>", self.release)
        g.bind("<ButtonPress-3>", self.right_click)
        g.bind("<Motion>", self.hover)
        g.bind("<MouseWheel>", self.wheel)
        g.bind("<Key>", self.key)
        g.bind("<Configure>", lambda e: self.draw())
        self.keys.bind("<MouseWheel>", self.wheel)
        self.keys.bind("<ButtonPress-1>", self.key_click)
        self.ruler.bind("<ButtonPress-1>", self.ruler_click)

    # ---------------------------------------------------------------- view

    def yview(self, *args):
        self.grid_c.yview(*args)
        self.keys.yview_moveto(self.grid_c.yview()[0])

    def xview(self, *args):
        self.grid_c.xview(*args)
        self.ruler.xview_moveto(self.grid_c.xview()[0])

    def wheel(self, event):
        step = -1 if event.delta > 0 else 1
        if event.state & 0x4:           # Ctrl: zoom
            self.zoom(1 if step < 0 else -1)
        elif event.state & 0x1:         # Shift: across
            self.xview("scroll", step * 3, "units")
        else:
            self.yview("scroll", step * 3, "units")
        return "break"

    def zoom(self, step):
        self.row_w = max(6, min(60, self.row_w + step * 4))
        self.draw()

    def show_notes(self):
        """Scrolled to the riff's notes (C4 for an empty riff), from its start (once the roll has its size)."""
        self.want_notes = True
        self.draw()

    def scroll_to_notes(self):
        self.want_notes = False
        notes = [n["note"] for n in tracker.pattern_notes(self.page.pat())]
        mid = sorted(notes)[len(notes) // 2] if notes else 60
        total = (HIGH - LOW) * KEY_H
        h = max(1, self.grid_c.winfo_height())
        y = (HIGH - 1 - mid) * KEY_H - h / 2
        self.yview("moveto", max(0.0, y / total))
        self.xview("moveto", 0.0)

    def at(self, event):
        """(row, note) under the mouse, and the x in the riff."""
        x, y = self.grid_c.canvasx(event.x), self.grid_c.canvasy(event.y)
        pat = self.page.pat()
        row = max(0, min(pat["rows"] - 1, int(x // self.row_w)))
        note = max(LOW, min(HIGH - 1, HIGH - 1 - int(y // KEY_H)))
        return row, note, x

    def note_at(self, row, note):
        hits = [n for n in tracker.pattern_notes(self.page.pat()) if n["note"] == note and n["row"] <= row < n["end"]]
        return hits[-1] if hits else None

    def selected(self):
        if not self.sel:
            return None
        return next((n for n in tracker.pattern_notes(self.page.pat()) if (n["row"], n["ch"]) == self.sel), None)

    # ---------------------------------------------------------------- drawing

    def draw(self):
        g, keys, ruler = self.grid_c, self.keys, self.ruler
        if g.winfo_width() < 10:
            return
        for c in (g, keys, ruler):
            c.delete("all")
        page = self.page
        pat, lpb, rw = page.pat(), page.song["lpb"], self.row_w
        rows = pat["rows"]
        width, height = rows * rw, (HIGH - LOW) * KEY_H
        g.configure(scrollregion=(0, 0, width, height))
        keys.configure(scrollregion=(0, 0, KEY_W, height))
        ruler.configure(scrollregion=(0, 0, width, RULER_H))
        for i, note in enumerate(range(HIGH - 1, LOW - 1, -1)):
            y = i * KEY_H
            black = note % 12 in BLACK
            g.create_rectangle(0, y, width, y + KEY_H, fill=COLORS["black_row" if black else "white_row"], outline="")
            if black:
                keys.create_rectangle(0, y, KEY_W * 0.62, y + KEY_H, fill=COLORS["key_black"], outline="#000000")
            else:
                keys.create_rectangle(0, y, KEY_W, y + KEY_H, fill=COLORS["key_white"], outline="#9aa0a6")
            if note % 12 == 0:
                g.create_line(0, y + KEY_H, width, y + KEY_H, fill=COLORS["c_line"])
                keys.create_text(KEY_W - 4, y + KEY_H / 2, anchor="e", text=tracker.note_text(note).replace("-", ""),
                                 fill=COLORS["key_text"], font=("", 8, "bold"))
        for r in range(rows + 1):
            x = r * rw
            if r % (lpb * 4) == 0:
                color = COLORS["bar"]
                ruler.create_text(x + 3, RULER_H / 2, anchor="w", text=str(r // (lpb * 4) + 1),
                                  fill=COLORS["ruler_text"], font=("", 8, "bold"))
            elif r % lpb == 0:
                color = COLORS["beat"]
                ruler.create_line(x, RULER_H - 5, x, RULER_H, fill=COLORS["ruler_text"])
            elif rw >= 10:
                color = COLORS["row"]
            else:
                continue
            g.create_line(x, 0, x, height, fill=color)
        sel = self.sel
        for n in tracker.pattern_notes(pat):
            if not LOW <= n["note"] < HIGH:
                continue
            x0, x1 = n["row"] * rw, n["end"] * rw
            y0 = (HIGH - 1 - n["note"]) * KEY_H
            is_sel = (n["row"], n["ch"]) == sel
            g.create_rectangle(x0 + 1, y0 + 1, x1 - 1, y0 + KEY_H - 1, fill=ins_color(n["ins"]),
                               outline=COLORS["sel"] if is_sel else "#0d1013", width=2 if is_sel else 1)
            if x1 - x0 > 34:
                g.create_text(x0 + 4, y0 + KEY_H / 2, anchor="w", text=tracker.note_text(n["note"]).replace("-", ""),
                              fill="#101418", font=("", 7, "bold"))
        self.draw_play()
        if getattr(self, "want_notes", False) and g.winfo_height() > 40:
            self.after_idle(self.scroll_to_notes)

    def draw_play(self):
        g = self.grid_c
        g.delete("play")
        self.ruler.delete("play")
        row = self.page.play_pattern_row()
        if row is not None:
            x = row * self.row_w
            g.create_line(x, 0, x, (HIGH - LOW) * KEY_H, fill=COLORS["play"], width=2, tags="play")
            self.ruler.create_polygon(x - 5, 0, x + 5, 0, x, 8, fill=COLORS["play"], tags="play")

    def hover(self, event):
        row, note, x = self.at(event)
        n = self.note_at(row, note)
        self.grid_c.configure(cursor="sb_h_double_arrow" if n and x > n["end"] * self.row_w - EDGE else
                              ("fleur" if n else "pencil"))

    # ---------------------------------------------------------------- editing

    def put(self, row, length, note, ins, vol, prefer=None):
        """A note added (more channels for the riff if they are all taken then); its (row, channel), None if not."""
        page = self.page
        pat = page.pat()
        ch = tracker.add_note(pat, row, length, note, ins, vol, page.song["channels"], prefer)
        if ch is None and page.song["channels"] < tracker.MAX_CHANNELS:
            page.set_channels(page.song["channels"] + 1)
            ch = tracker.add_note(pat, row, length, note, ins, vol, page.song["channels"], prefer)
        if ch is None:
            page.status_text("16 notes at once at most.", True)
            return None
        return row, ch

    def undo_once(self):
        if not self.undo_pushed:
            self.page.push_undo()
            self.undo_pushed = True

    def press(self, event):
        g = self.grid_c
        g.focus_set()
        row, note, x = self.at(event)
        n = self.note_at(row, note)
        self.undo_pushed = False
        page = self.page
        if n:
            self.sel = (n["row"], n["ch"])
            self.mode = "resize" if x > n["end"] * self.row_w - EDGE else "move"
            if n["ins"] is not None and n["ins"] < len(page.song["instruments"]):
                page.select_instrument(n["ins"])
            page.preview(n["note"], n["ins"])
        else:
            self.undo_once()
            put = self.put(row, page.note_rows(), note, page.current_instrument(), None)
            if not put:
                return
            self.sel = put
            self.mode = "resize"
            page.preview(note)
            page.changed(redraw=False)
        self.drag_from = (row, note)
        self.orig = self.selected()
        self.draw()

    def motion(self, event):
        o = self.orig
        if not self.mode or not o:
            return
        row, note, x = self.at(event)
        pat = self.page.pat()
        cur = self.selected()
        if not cur:
            return
        if self.mode == "move":
            new_row = max(0, min(pat["rows"] - 1, o["row"] + row - self.drag_from[0]))
            new_note = max(0, min(127, o["note"] + note - self.drag_from[1]))
            length = o["end"] - o["row"]
            if (new_row, new_note) == (cur["row"], cur["note"]):
                return
        else:
            end = max(cur["row"] + 1, min(pat["rows"], int(round(x / self.row_w))))
            new_row, new_note, length = cur["row"], cur["note"], end - cur["row"]
            if end == cur["end"]:
                return
        self.undo_once()
        tracker.remove_note(pat, cur["row"], cur["ch"])
        put = tracker.add_note(pat, new_row, length, new_note, cur["ins"], cur["vol"], self.page.song["channels"],
                               cur["ch"])
        if put is None:                 # (no room there: back where it was)
            put = tracker.add_note(pat, cur["row"], cur["end"] - cur["row"], cur["note"], cur["ins"], cur["vol"],
                                   self.page.song["channels"], cur["ch"])
            new_row = cur["row"]
        self.sel = (new_row, put) if put is not None else None
        if new_note != cur["note"]:
            self.page.preview(new_note, cur["ins"])
        self.page.changed(redraw=False)
        self.draw()

    def release(self, event):
        self.mode = None
        self.page.draw_grid()           # (the note grid shows the same riff)

    def right_click(self, event):
        row, note, x = self.at(event)
        n = self.note_at(row, note)
        if n:
            self.page.push_undo()
            tracker.remove_note(self.page.pat(), n["row"], n["ch"])
            self.sel = None
            self.page.changed(redraw=False)
            self.draw()
            self.page.draw_grid()

    def key_click(self, event):
        y = self.keys.canvasy(event.y)
        self.page.preview(max(LOW, min(HIGH - 1, HIGH - 1 - int(y // KEY_H))))

    def ruler_click(self, event):
        row = int(self.ruler.canvasx(event.x) // self.row_w)
        self.page.play("riff", max(0, min(self.page.pat()["rows"] - 1, row)))

    def key(self, event):
        ks, ctrl, shift = event.keysym, bool(event.state & 0x4), bool(event.state & 0x1)
        page = self.page
        if ctrl and ks.lower() in ("z", "y"):
            (page.undo if ks.lower() == "z" else page.redo)()
            return "break"
        if ks == "space":
            page.space()
            return "break"
        cur = self.selected()
        if not cur:
            return None
        pat = page.pat()
        if ks in ("Delete", "BackSpace"):
            page.push_undo()
            tracker.remove_note(pat, cur["row"], cur["ch"])
            self.sel = None
        elif ks in ("Up", "Down", "Left", "Right"):
            dn = {"Up": 12 if shift else 1, "Down": -12 if shift else -1}.get(ks, 0)
            dr = {"Left": -1, "Right": 1}.get(ks, 0)
            row = max(0, min(pat["rows"] - 1, cur["row"] + dr))
            note = max(0, min(127, cur["note"] + dn))
            page.push_undo()
            tracker.remove_note(pat, cur["row"], cur["ch"])
            ch = tracker.add_note(pat, row, cur["end"] - cur["row"], note, cur["ins"], cur["vol"],
                                  page.song["channels"], cur["ch"])
            if ch is None:
                ch = tracker.add_note(pat, cur["row"], cur["end"] - cur["row"], cur["note"], cur["ins"], cur["vol"],
                                      page.song["channels"], cur["ch"])
                row, note = cur["row"], cur["note"]
            self.sel = (row, ch) if ch is not None else None
            if dn:
                page.preview(note, cur["ins"])
        else:
            return None
        page.changed(redraw=False)
        self.draw()
        page.draw_grid()
        return "break"
