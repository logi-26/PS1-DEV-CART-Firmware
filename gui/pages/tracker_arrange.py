"""
PS1 DEV CART GUI: the Music Tracker's arranger (pages/tracker.py): the song's tracks as lanes, its riffs as blocks on
them over time (clips). Click on a track to put the riff picked on the left there, drag a block to move it (to another
track too), its right end to make it longer (the riff plays again), right click for its menu. The ruler: click to play
from there, right click to set the loop.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import tkinter as tk
from tkinter import simpledialog

import ttkbootstrap as tb

import tracker

HEAD_W = 140
RULER_H = 20
LANE_H = 38
EDGE = 7
MIN_BARS = 32                   # Bars shown at least (room to put more after the song's end)
COLORS = dict(bg="#16191c", lane="#1b1f23", lane_alt="#1f2328", bar="#3a4048", beat="#252a30", head="#2a2f35",
              head_text="#d8dade", muted="#7a2a2a", ruler="#2a2f35", ruler_text="#c8ccd0", play="#ffb347",
              loop="#2f6f4f", sel="#ffffff", hint="#7a8088")
PALETTE = ("#2f6fb0", "#a8443d", "#3f8a4c", "#7a52a8", "#b0702a", "#2a8a8a", "#a8457a", "#5a7a2a", "#8a6a2a",
           "#3a5aa8")


class Arranger(tb.Frame):
    def __init__(self, parent, page):
        super().__init__(parent)
        self.page = page
        self.beat_w = 24                # Pixels a beat
        self.sel = None                 # The clip selected (its dict in song["clips"])
        self.mode = None
        self.drag_from = None
        self.orig = None
        self.undo_pushed = False
        self.head = tk.Canvas(self, width=HEAD_W, highlightthickness=0, background=COLORS["head"])
        self.ruler = tk.Canvas(self, height=RULER_H, highlightthickness=0, background=COLORS["ruler"])
        self.lanes = tk.Canvas(self, highlightthickness=1, highlightcolor="#4fc1ff", highlightbackground="#30353b",
                               background=COLORS["bg"], takefocus=1)
        self.vbar = tb.Scrollbar(self, orient="vertical", command=self.yview)
        self.hbar = tb.Scrollbar(self, orient="horizontal", command=self.xview)
        corner = tk.Frame(self, width=HEAD_W, height=RULER_H, background=COLORS["ruler"])
        corner.grid(row=0, column=0, sticky="nsew")
        tk.Label(corner, text="Tracks", background=COLORS["ruler"], foreground=COLORS["ruler_text"],
                 font=("", 8, "bold")).place(x=6, y=2)
        self.ruler.grid(row=0, column=1, sticky="ew")
        self.head.grid(row=1, column=0, sticky="ns")
        self.lanes.grid(row=1, column=1, sticky="nsew")
        self.vbar.grid(row=1, column=2, sticky="ns")
        self.hbar.grid(row=2, column=1, sticky="ew")
        self.rowconfigure(1, weight=1)
        self.columnconfigure(1, weight=1)
        self.lanes.configure(yscrollcommand=self.vbar.set, xscrollcommand=self.hbar.set)
        c = self.lanes
        c.bind("<ButtonPress-1>", self.press)
        c.bind("<B1-Motion>", self.motion)
        c.bind("<ButtonRelease-1>", self.release)
        c.bind("<Double-Button-1>", self.double)
        c.bind("<ButtonPress-3>", self.right_click)
        c.bind("<Motion>", self.hover)
        c.bind("<MouseWheel>", self.wheel)
        c.bind("<Key>", self.key)
        c.bind("<Configure>", lambda e: self.draw())
        self.head.bind("<ButtonPress-1>", self.head_click)
        self.head.bind("<Double-Button-1>", self.head_double)
        self.head.bind("<ButtonPress-3>", self.head_menu)
        self.head.bind("<MouseWheel>", self.wheel)
        self.ruler.bind("<ButtonPress-1>", self.ruler_click)
        self.ruler.bind("<ButtonPress-3>", self.ruler_menu)

    # ---------------------------------------------------------------- view

    @property
    def row_w(self):
        return self.beat_w / self.page.song["lpb"]

    def yview(self, *args):
        self.lanes.yview(*args)
        self.head.yview_moveto(self.lanes.yview()[0])

    def xview(self, *args):
        self.lanes.xview(*args)
        self.ruler.xview_moveto(self.lanes.xview()[0])

    def wheel(self, event):
        step = -1 if event.delta > 0 else 1
        if event.state & 0x4:
            self.zoom(1 if step < 0 else -1)
        elif event.state & 0x1:
            self.yview("scroll", step, "units")
        else:
            self.xview("scroll", step * 3, "units")
        return "break"

    def zoom(self, step):
        self.beat_w = max(6, min(96, self.beat_w * (1.5 if step > 0 else 1 / 1.5)))
        self.draw()

    def at(self, event):
        """(row, track) under the mouse (track: may be past the last one), and the x."""
        x, y = self.lanes.canvasx(event.x), self.lanes.canvasy(event.y)
        return max(0, int(x // self.row_w)), int(y // LANE_H), x

    def snap(self, row):
        lpb = self.page.song["lpb"]
        return int(round(row / lpb)) * lpb

    def clip_at(self, row, track):
        hits = [c for c in self.page.song["clips"] if c["track"] == track and c["start"] <= row <
                c["start"] + c["length"]]
        return max(hits, key=lambda c: c["start"]) if hits else None

    # ---------------------------------------------------------------- drawing

    def draw(self):
        c, head, ruler = self.lanes, self.head, self.ruler
        if c.winfo_width() < 10:
            return
        for w in (c, head, ruler):
            w.delete("all")
        song = self.page.song
        lpb, rw = song["lpb"], self.row_w
        bar_rows = lpb * 4
        bars = max(MIN_BARS, tracker.song_length(song) // bar_rows + 8)
        width = bars * bar_rows * rw
        ntracks = len(song["tracks"])
        height = max(ntracks * LANE_H, c.winfo_height() - 4)
        for w, region in ((c, (0, 0, width, height)), (head, (0, 0, HEAD_W, height)), (ruler, (0, 0, width, RULER_H))):
            w.configure(scrollregion=region)
        try:
            bases = tracker.track_channels(song)[0]
        except tracker.SongError:
            bases = None
        for t, track in enumerate(song["tracks"]):
            y = t * LANE_H
            c.create_rectangle(0, y, width, y + LANE_H, fill=COLORS["lane_alt" if t % 2 else "lane"], outline="")
            muted = t in self.page.muted_tracks
            head.create_rectangle(0, y, HEAD_W, y + LANE_H - 1, fill=COLORS["muted"] if muted else COLORS["head"],
                                  outline="#1b1f23")
            head.create_text(8, y + 11, anchor="w", text=track["name"], fill=COLORS["head_text"], font=("", 9, "bold"))
            if bases and bases[t][1]:
                head.create_text(8, y + 27, anchor="w", fill=COLORS["hint"], font=("", 7),
                                 text="channel%s %d%s" % ("s" if bases[t][1] > 1 else "", bases[t][0] + 1,
                                                          "-%d" % (bases[t][0] + bases[t][1]) if bases[t][1] > 1
                                                          else ""))
            head.create_rectangle(HEAD_W - 26, y + 8, HEAD_W - 8, y + 26, outline=COLORS["head_text"],
                                  fill="#c03030" if muted else "")
            head.create_text(HEAD_W - 17, y + 17, text="M", fill=COLORS["head_text"], font=("", 8, "bold"))
        loop = song["loop"]
        end = tracker.song_length(song)
        if loop is not None and loop < end:
            ruler.create_rectangle(loop * rw, RULER_H - 6, end * rw, RULER_H, fill=COLORS["loop"], outline="")
            ruler.create_text(loop * rw + 14, RULER_H - 3, anchor="sw", text="↻ loop", fill="#9fe0bf", font=("", 7))
        for r in range(0, bars * bar_rows + 1, lpb):
            x = r * rw
            if r % bar_rows == 0:
                c.create_line(x, 0, x, height, fill=COLORS["bar"])
                ruler.create_text(x + 3, 7, anchor="w", text=str(r // bar_rows + 1), fill=COLORS["ruler_text"],
                                  font=("", 8, "bold"))
            elif self.beat_w >= 12:
                c.create_line(x, 0, x, height, fill=COLORS["beat"])
        if end:
            c.create_line(end * rw, 0, end * rw, height, fill="#c0c4c8", dash=(3, 3))
        ends = tracker.clip_ends(song)
        for clip in sorted(song["clips"], key=lambda k: k["start"]):
            self.draw_clip(clip, ends[id(clip)])
        if not song["clips"]:
            c.create_text(20, LANE_H * 0.5, anchor="w", fill=COLORS["hint"], font=("", 10),
                          text="Pick a riff on the left, then click on a track to put it there.")
        self.draw_play()

    def draw_clip(self, clip, cut):
        c, song, rw = self.lanes, self.page.song, self.row_w
        pat = song["patterns"][clip["pattern"]]
        x0, x1 = clip["start"] * rw, (clip["start"] + clip["length"]) * rw
        y0 = clip["track"] * LANE_H + 2
        y1 = y0 + LANE_H - 4
        color = PALETTE[clip["pattern"] % len(PALETTE)]
        is_sel = clip is self.sel
        c.create_rectangle(x0 + 1, y0, x1 - 1, y1, fill=color, outline=COLORS["sel"] if is_sel else "#0d1013",
                           width=2 if is_sel else 1)
        if cut < clip["start"] + clip["length"]:        # (cut by the next clip: the rest not heard)
            c.create_rectangle(cut * rw, y0, x1 - 1, y1, fill="#000000", stipple="gray50", outline="")
        # The riff's notes in small, at each time it plays
        notes = tracker.pattern_notes(pat)
        if notes and x1 - x0 > 12:
            lo = min(n["note"] for n in notes)
            hi = max(n["note"] for n in notes)
            span = max(12, hi - lo + 1)
            h = (y1 - y0 - 14)
            items = 0
            for rep in range(clip["start"], clip["start"] + clip["length"], pat["rows"]):
                if rep > clip["start"]:
                    c.create_line(rep * rw, y0 + 12, rep * rw, y1, fill="#0d1013", dash=(2, 2))
                for n in notes:
                    s = rep + n["row"]
                    e = min(rep + n["end"], clip["start"] + clip["length"])
                    if s >= e:
                        continue
                    y = y1 - 2 - (n["note"] - lo) * h / span
                    c.create_line(s * rw, y, max(s * rw + 1, e * rw - 1), y, fill="#e8ecf0", width=2)
                    items += 1
                if items > 3000:
                    break
        c.create_text(x0 + 5, y0 + 6, anchor="w", text=pat["name"], fill="#ffffff", font=("", 8, "bold"))

    def draw_play(self):
        self.lanes.delete("play")
        self.ruler.delete("play")
        row = self.page.song_play_row()
        if row is not None:
            x = row * self.row_w
            self.lanes.create_line(x, 0, x, len(self.page.song["tracks"]) * LANE_H, fill=COLORS["play"], width=2,
                                   tags="play")
            self.ruler.create_polygon(x - 6, 0, x + 6, 0, x, 9, fill=COLORS["play"], tags="play")
            if self.page.follow.get():  # (kept in view)
                left, right = self.lanes.xview()
                total = float(self.lanes.cget("scrollregion").split()[2] or 1)
                if not left * total <= x <= right * total - 20:
                    self.xview("moveto", max(0.0, (x - 40) / total))

    def hover(self, event):
        row, track, x = self.at(event)
        clip = self.clip_at(row, track)
        self.lanes.configure(cursor="sb_h_double_arrow" if clip and x > (clip["start"] + clip["length"]) * self.row_w
                             - EDGE else ("fleur" if clip else ("plus" if track < len(self.page.song["tracks"])
                                                                else "")))

    # ---------------------------------------------------------------- editing

    def undo_once(self):
        if not self.undo_pushed:
            self.page.push_undo(song=True)
            self.undo_pushed = True

    def press(self, event):
        self.lanes.focus_set()
        row, track, x = self.at(event)
        song, page = self.page.song, self.page
        self.undo_pushed = False
        clip = self.clip_at(row, track)
        if clip:
            self.sel = clip
            self.mode = "resize" if x > (clip["start"] + clip["length"]) * self.row_w - EDGE else "move"
            page.edit_pattern(clip["pattern"])
        elif track < len(song["tracks"]):
            index = page.brush()
            if index is None:
                return
            self.undo_once()
            clip = tracker.new_clip(track, self.snap_down(row), index, song["patterns"][index]["rows"])
            song["clips"].append(clip)
            self.sel = clip
            self.mode = "resize"
            page.edit_pattern(index)
            page.changed(redraw=False)
        else:
            self.sel = None
            self.mode = None
        self.drag_from = (row, track)
        self.orig = dict(self.sel) if self.sel else None
        self.draw()

    def snap_down(self, row):
        lpb = self.page.song["lpb"]
        return row // lpb * lpb

    def motion(self, event):
        clip, o = self.sel, self.orig
        if not self.mode or not clip or not o:
            return
        row, track, x = self.at(event)
        song = self.page.song
        if self.mode == "move":
            start = max(0, o["start"] + self.snap(row - self.drag_from[0]))
            new_track = max(0, min(len(song["tracks"]) - 1, o["track"] + track - self.drag_from[1]))
            if (start, new_track) == (clip["start"], clip["track"]):
                return
            self.undo_once()
            clip["start"], clip["track"] = start, new_track
        else:
            length = max(song["lpb"], self.snap(x / self.row_w - clip["start"]))
            if length == clip["length"]:
                return
            self.undo_once()
            clip["length"] = length
        self.page.changed(redraw=False)
        self.draw()

    def release(self, event):
        self.mode = None

    def double(self, event):
        if self.sel:
            self.page.focus_editor()

    def right_click(self, event):
        row, track, x = self.at(event)
        clip = self.clip_at(row, track)
        if not clip:
            return
        self.sel = clip
        self.page.edit_pattern(clip["pattern"])
        self.draw()
        menu = tk.Menu(self, tearoff=0)
        menu.add_command(label="Edit this riff", command=self.page.focus_editor)
        menu.add_command(label="Repeat it after (Ctrl+D)", command=self.duplicate)
        menu.add_command(label="Make it a new riff (a copy to change)", command=self.make_unique)
        menu.add_separator()
        menu.add_command(label="Remove from the song (Delete)", command=self.delete)
        menu.tk_popup(event.x_root, event.y_root)

    def delete(self):
        if self.sel in self.page.song["clips"]:
            self.page.push_undo(song=True)
            self.page.song["clips"].remove(self.sel)
            self.sel = None
            self.page.changed(redraw=False)
            self.draw()

    def duplicate(self):
        clip = self.sel
        if clip not in self.page.song["clips"]:
            return
        self.page.push_undo(song=True)
        new = tracker.new_clip(clip["track"], clip["start"] + clip["length"], clip["pattern"], clip["length"])
        self.page.song["clips"].append(new)
        self.sel = new
        self.page.changed(redraw=False)
        self.draw()

    def make_unique(self):
        clip = self.sel
        if clip not in self.page.song["clips"]:
            return
        self.page.push_undo(song=True)
        old = self.page.song["patterns"][clip["pattern"]]
        pat = tracker.clone_pattern(old, old["name"] + " copy")
        self.page.song["patterns"].append(pat)
        clip["pattern"] = len(self.page.song["patterns"]) - 1
        self.page.edit_pattern(clip["pattern"])
        self.page.changed(lists=True, redraw=False)
        self.draw()

    def key(self, event):
        ks, ctrl = event.keysym, bool(event.state & 0x4)
        if ks in ("Delete", "BackSpace"):
            self.delete()
        elif ctrl and ks.lower() == "d":
            self.duplicate()
        elif ctrl and ks.lower() in ("z", "y"):
            (self.page.undo if ks.lower() == "z" else self.page.redo)()
        elif ks == "space":
            self.page.space(song=True)
        else:
            return None
        return "break"

    # ---------------------------------------------------------------- tracks, ruler

    def track_at(self, event):
        t = int(self.head.canvasy(event.y) // LANE_H)
        return t if 0 <= t < len(self.page.song["tracks"]) else None

    def head_click(self, event):
        t = self.track_at(event)
        if t is not None and event.x >= HEAD_W - 30:
            self.page.toggle_track_mute(t)

    def head_double(self, event):
        t = self.track_at(event)
        if t is not None and event.x < HEAD_W - 30:
            self.rename_track(t)

    def rename_track(self, t):
        track = self.page.song["tracks"][t]
        name = simpledialog.askstring("Track", "Name of the track:", initialvalue=track["name"], parent=self)
        if name and name.strip() and name.strip() != track["name"]:
            self.page.push_undo(song=True)
            track["name"] = name.strip()[:24]
            self.page.changed(redraw=False)
            self.draw()

    def head_menu(self, event):
        t = self.track_at(event)
        menu = tk.Menu(self, tearoff=0)
        if t is not None:
            menu.add_command(label="Rename...", command=lambda: self.rename_track(t))
            menu.add_command(label="Mute / unmute", command=lambda: self.page.toggle_track_mute(t))
            menu.add_command(label="Remove this track", command=lambda: self.page.remove_track(t))
            menu.add_separator()
        menu.add_command(label="Add a track", command=self.page.add_track)
        menu.tk_popup(event.x_root, event.y_root)

    def ruler_row(self, event):
        return self.snap_down(max(0, int(self.ruler.canvasx(event.x) // self.row_w)))

    def ruler_click(self, event):
        self.page.play("song", self.ruler_row(event))

    def ruler_menu(self, event):
        row = self.ruler_row(event)
        bar = row // (self.page.song["lpb"] * 4) * self.page.song["lpb"] * 4
        menu = tk.Menu(self, tearoff=0)
        menu.add_command(label="Play from here", command=lambda: self.page.play("song", row))
        menu.add_command(label="Loop back to here at the end (bar %d)" % (bar // (self.page.song["lpb"] * 4) + 1),
                         command=lambda: self.page.set_loop(bar))
        menu.add_command(label="No loop (the song plays once)", command=lambda: self.page.set_loop(None))
        menu.tk_popup(event.x_root, event.y_root)
