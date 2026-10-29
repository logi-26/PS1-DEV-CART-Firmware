"""
PS1 DEV CART GUI: common widgets (file fields with recent files, monospace text, image view).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import sys
import tkinter as tk
from tkinter import filedialog

import ttkbootstrap as tb

MONO_FONT = ("Consolas", 10) if sys.platform == "win32" else ("DejaVu Sans Mono", 10)


def _fast_scrollbar_thumbs(center=32):
    """ttkbootstrap 2.2's scrollbar thumb is a 17 px image whose middle (1 px) Tk repeats along the thumb: about one
    blend per pixel of length, 1.4 s to draw a horizontal scrollbar across the window, and some computers are much
    slower. The same thumb with a middle of `center` px (the shortest thumb is then 16 + center px). Nothing changed
    if this ttkbootstrap draws its thumbs another way."""
    try:
        from ttkbootstrap.style.builders import scrollbar as sb
        inset, thick, min_thumb = sb._SCROLLBAR_THUMB_INSET, sb._SCROLLBAR_THICKNESS, sb._SCROLLBAR_MIN_THUMB
        sb._scrollbar_thumb      # noqa: B018 (the function replaced)
    except (ImportError, AttributeError):
        return

    def thumb(builder, color, orient, rounded):
        cross = 0 if orient == "vertical" else 1   # x for vertical, y for horizontal

        def draw(d, w, h):
            margin = round((w, h)[cross] * inset / thick)
            box = [0, 0, w - 1, h - 1]
            box[cross] += margin
            box[cross + 2] -= margin
            if rounded:
                d.rounded_rectangle(box, radius=round(((w, h)[cross] - 2 * margin) / 2), fill=color)
            else:
                d.rectangle(box, fill=color)

        cap = builder.scale_size(min_thumb // 2)
        length = min_thumb + center
        size, border = ((thick, length), (0, cap)) if orient == "vertical" else ((length, thick), (cap, 0))
        return builder.assets.image(size, draw, "scrollbar-thumb-%d" % center, color, orient, rounded), border

    sb._scrollbar_thumb = thumb


_fast_scrollbar_thumbs()    # (before any window: the styles are made with the first widgets)

PS1_EXE_TYPES = [("PS1 executables", "*.exe *.ps-exe *.psx *.cpe"), ("All files", "*.*")]


class FileField(tb.Frame):
    """Label, editable combobox with the recent files, browse button. mode: open, save or dir."""

    def __init__(self, master, app, key, label, mode="open", filetypes=None, width=60, on_change=None):
        super().__init__(master)
        self.app = app
        self.key = key
        self.mode = mode
        self.filetypes = filetypes or [("All files", "*.*")]
        self.var = tk.StringVar(value=app.settings.field(key))
        tb.Label(self, text=label, width=14, anchor="w").pack(side="left")
        self.combo = tb.Combobox(self, textvariable=self.var, values=app.settings.recent(key), width=width)
        self.combo.pack(side="left", fill="x", expand=True, padx=(0, 4))
        self.browse_button = tb.Button(self, text="Browse...", bootstyle="secondary-outline", command=self.browse)
        self.browse_button.pack(side="left")
        if on_change:
            self.var.trace_add("write", lambda *_: on_change())

    def browse(self):
        current = self.var.get()
        folder = os.path.dirname(current) if current else None
        if self.mode == "dir":
            path = filedialog.askdirectory(initialdir=current or None, parent=self)
        elif self.mode == "save":
            path = filedialog.asksaveasfilename(initialdir=folder, initialfile=os.path.basename(current) or None,
                                                filetypes=self.filetypes, parent=self)
        else:
            path = filedialog.askopenfilename(initialdir=folder, filetypes=self.filetypes, parent=self)
        if path:
            self.var.set(os.path.normpath(path))

    def get(self):
        return self.var.get().strip()

    def set(self, value):
        self.var.set(value)

    def remember(self):
        """Saves the value as the field value and in the recent files."""
        value = self.get()
        self.app.settings.set_field(self.key, value)
        self.app.settings.add_recent(self.key, value)
        self.combo.configure(values=self.app.settings.recent(self.key))


class FileList(tb.Frame):
    """List of files (remembered in the settings) with Add, Remove, Up, Down, Clear. max_files: 0 = no limit,
    on_change: called when the list changes, compact: the buttons in the label's row (less height), remember: False:
    the list starts empty each time (only the folder of the last file added is remembered), expand: called with the
    files chosen with Add, returns the files to add (e.g. the TIM files of a Net Yaroze auto script)."""

    def __init__(self, master, app, key, label, filetypes=None, max_files=0, height=6, on_change=None, compact=False,
                 remember=True, expand=None):
        super().__init__(master)
        self.app = app
        self.key = key
        self.on_change = on_change
        self.remember = remember
        self.expand = expand
        self.max_files = max_files
        self.filetypes = filetypes or [("All files", "*.*")]
        head = tb.Frame(self)
        head.pack(fill="x")
        tb.Label(head, text=label).pack(side="left")
        if max_files:
            tb.Label(head, text="(up to %d)" % max_files, bootstyle="secondary").pack(side="left", padx=6)
        row = tb.Frame(self)
        row.pack(fill="both", expand=True, pady=(2, 0))
        self.listbox = tb.Listbox(row, height=height, selectmode="extended", activestyle="none", font=MONO_FONT)
        self.listbox.pack(side="left", fill="both", expand=True)
        scroll = tb.Scrollbar(row, orient="vertical", command=self.listbox.yview)
        scroll.pack(side="left", fill="y")
        self.listbox.configure(yscrollcommand=scroll.set)
        buttons = tb.Frame(head if compact else row)
        buttons.pack(side="right" if compact else "left", fill="y", padx=(6, 0))
        for text, command in (("Add...", self.add), ("Remove", self.remove), ("Up", lambda: self.move(-1)),
                              ("Down", lambda: self.move(1)), ("Clear", self.clear)):
            tb.Button(buttons, text=text, width=8, bootstyle="secondary-outline", command=command).pack(
                side="left" if compact else "top", padx=(4, 0) if compact else 0, pady=1)
        for path in app.settings.field(key, []) if remember else []:
            self.listbox.insert("end", path)

    def files(self):
        return list(self.listbox.get(0, "end"))

    def _save(self):
        if self.remember:
            self.app.settings.set_field(self.key, self.files())
        if self.on_change:
            self.on_change()

    def add(self):
        folder = os.path.dirname(self.files()[-1]) if self.files() else \
            (self.app.settings.field(self.key + ".folder", "") or None)
        paths = filedialog.askopenfilenames(initialdir=folder, filetypes=self.filetypes, parent=self)
        if not paths:
            return
        self.app.settings.set_field(self.key + ".folder", os.path.dirname(os.path.normpath(paths[0])))
        if self.expand:
            paths = self.expand(list(paths))
        for path in paths:
            path = os.path.normpath(path)
            if path in self.files():        # (already in the list)
                continue
            if self.max_files and self.listbox.size() >= self.max_files:
                self.app.error("At most %d files." % self.max_files)
                break
            self.listbox.insert("end", path)
        self._save()

    def remove(self):
        for i in reversed(self.listbox.curselection()):
            self.listbox.delete(i)
        self._save()

    def move(self, step):
        sel = self.listbox.curselection()
        if len(sel) != 1:
            return
        i, j = sel[0], sel[0] + step
        if 0 <= j < self.listbox.size():
            text = self.listbox.get(i)
            self.listbox.delete(i)
            self.listbox.insert(j, text)
            self.listbox.selection_set(j)
            self._save()

    def clear(self):
        self.listbox.delete(0, "end")
        self._save()


def labeled(master, label, widget_class, width=14, field_width=None, **kwargs):
    """A frame with a label and a widget, returns (frame, widget). width: of the label (14: as a FileField's label, so
    the fields start at the same place), field_width: of the widget."""
    frame = tb.Frame(master)
    tb.Label(frame, text=label, width=width, anchor="w").pack(side="left")
    if field_width is not None:
        kwargs["width"] = field_width
    widget = widget_class(frame, **kwargs)
    widget.pack(side="left", fill="x", expand=True)
    return frame, widget


def browse_space(frame, file_field):
    """Empty space at the end of a labeled() row, as wide as the file field's Browse button: the row's field ends where
    the file field's box ends (same length)."""
    space = tb.Frame(frame, width=1, height=1)
    space.pack(side="left", padx=(4, 0))
    frame.after_idle(lambda: space.configure(width=file_field.browse_button.winfo_reqwidth()))
    return space


class MonoText(tb.ScrolledText):
    """Read-only style monospace text area (the program can still append)."""

    def __init__(self, master, height=10, **kwargs):
        super().__init__(master, height=height, wrap="none", hbar=True, font=MONO_FONT, **kwargs)
        self.text.configure(state="disabled")

    def append(self, text, autoscroll=True):
        self.text.configure(state="normal")
        self.text.insert("end", text)
        if int(self.text.index("end-1c").split(".")[0]) > 20000:     # Keep the last lines
            self.text.delete("1.0", "5000.0")
        self.text.configure(state="disabled")
        if autoscroll:
            self.text.see("end")

    def set(self, text):
        self.clear()
        self.append(text, autoscroll=False)

    def clear(self):
        self.text.configure(state="normal")
        self.text.delete("1.0", "end")
        self.text.configure(state="disabled")

    def get_all(self):
        return self.text.get("1.0", "end-1c")


class ImageView(tb.Frame):
    """Shows a PIL image scaled by a whole number to fit (pixels stay sharp)."""

    def __init__(self, master, **kwargs):
        super().__init__(master, **kwargs)
        self.canvas = tk.Canvas(self, highlightthickness=0, background="#000000")
        self.canvas.pack(fill="both", expand=True)
        self.image = None
        self.photo = None
        self.canvas.bind("<Configure>", lambda e: self._draw())

    def show(self, image):
        self.image = image
        self._draw()

    def _draw(self):
        from PIL import Image, ImageTk
        self.canvas.delete("all")
        if self.image is None:
            return
        cw, ch = max(self.canvas.winfo_width(), 1), max(self.canvas.winfo_height(), 1)
        w, h = self.image.size
        scale = max(1, min(cw // w, ch // h))
        img = self.image.resize((w * scale, h * scale), Image.NEAREST) if scale > 1 else self.image
        if img.width > cw or img.height > ch:     # Bigger than the view: shrink
            ratio = min(cw / img.width, ch / img.height)
            img = img.resize((max(1, int(img.width * ratio)), max(1, int(img.height * ratio))), Image.LANCZOS)
        self.photo = ImageTk.PhotoImage(img)
        self.canvas.create_image(cw // 2, ch // 2, image=self.photo, anchor="center")


class ScrollFrame(tb.Frame):
    """A frame whose content (self.inner) scrolls vertically when it is taller than the frame (mouse wheel over it)."""

    def __init__(self, master, **kw):
        super().__init__(master, **kw)
        bg = tb.Style().colors.bg
        self.canvas = tk.Canvas(self, highlightthickness=0, background=bg)
        self.scroll = tb.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self.scroll.set)
        self.canvas.pack(side="left", fill="both", expand=True)
        self.scroll.pack(side="left", fill="y")
        self.inner = tb.Frame(self.canvas)
        self.window = self.canvas.create_window(0, 0, window=self.inner, anchor="nw")
        self.inner.bind("<Configure>", lambda e: self._resize())
        self.canvas.bind("<Configure>", lambda e: self._resize())
        self.bind_all("<MouseWheel>", self._wheel, add="+")

    def _resize(self):
        self.canvas.itemconfigure(self.window, width=max(self.canvas.winfo_width(), self.inner.winfo_reqwidth()))
        self.canvas.configure(scrollregion=(0, 0, self.inner.winfo_reqwidth(), self.inner.winfo_reqheight()))

    def _wheel(self, event):
        w = self.winfo_containing(event.x_root, event.y_root)
        while w is not None and w is not self:
            w = w.master
        if w is self and self.inner.winfo_reqheight() > self.canvas.winfo_height():
            self.canvas.yview_scroll(-1 if event.delta > 0 else 1, "units")
