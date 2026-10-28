"""
PS1 DEV CART GUI: GPU commands (GP0) of a captured frame decoded, for the Frame Inspector page: polygons, lines,
rectangles (sprites, tiles), fills, VRAM transfers and the drawing environment commands, with their colors, vertices
(screen coordinates: the drawing offset added), texture page, CLUT and semi-transparency.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
DEPTHS = ("4-bit", "8-bit", "15-bit", "15-bit")
SEMI_MODES = ("B/2+F/2", "B+F", "B-F", "B+F/4")
RECT_SIZES = (None, (1, 1), (8, 8), (16, 16))
ENV_NAMES = {0xE1: "Draw mode", 0xE2: "Texture window", 0xE3: "Draw area top left", 0xE4: "Draw area bottom right",
             0xE5: "Drawing offset", 0xE6: "Mask bit"}


def s11(v):
    """11-bit signed coordinate."""
    v &= 0x7FF
    return v - 0x800 if v & 0x400 else v


def xy(w):
    return s11(w), s11(w >> 16)


def rgb(w):
    return w & 0xFF, (w >> 8) & 0xFF, (w >> 16) & 0xFF


def tpage_text(tp):
    """Texture page: VRAM position, colors, semi-transparency mode."""
    return "page %d,%d %s, semi-transparency %s" % ((tp & 15) * 64, ((tp >> 4) & 1) * 256, DEPTHS[(tp >> 7) & 3],
                                                     SEMI_MODES[(tp >> 5) & 3])


def clut_text(c):
    return "CLUT %d,%d" % ((c & 63) * 16, (c >> 6) & 511)


class Prim:
    """One GPU command: name, kind (poly, line, rect, fill, copy, env, other), points (drawing coordinates, the offset
    not added yet), size, colors, text (details), words."""

    def __init__(self, list_no, adrs, words):
        self.list_no = list_no
        self.adrs = adrs
        self.words = words
        self.cmd = words[0] >> 24
        self.name = "Command %02Xh" % self.cmd
        self.kind = "other"
        self.points = []
        self.colors = []
        self.details = []
        self.semi = False
        self.textured = False
        self.vram_rect = None    # Fill: its VRAM rectangle (no drawing offset)

    def color_text(self):
        if not self.colors:
            return ""
        if len(set(self.colors)) == 1:
            return "#%02X%02X%02X" % self.colors[0]
        return "gouraud"


def decode(list_no, adrs, words):
    """The commands of a packet (several can share one), as Prim objects."""
    prims, i = [], 0
    while i < len(words):
        w = words[i]
        cmd = w >> 24
        p = Prim(list_no, adrs, words[i:])
        n = 1
        if 0x20 <= cmd <= 0x3F:                 # Polygon: color, then per vertex [color] xy [uv]
            gouraud, quad, tex = cmd & 0x10, cmd & 0x08, cmd & 0x04
            verts = 4 if quad else 3
            p.kind, p.semi, p.textured = "poly", bool(cmd & 2), bool(tex)
            p.name = "%s%s%s %s" % ("Gouraud " if gouraud else "Flat ", "textured " if tex else "",
                                    "quad" if quad else "triangle", "(semi-transparent)" if cmd & 2 else "")
            p.colors.append(rgb(w))
            j = i + 1
            for v in range(verts):
                if gouraud and v:
                    p.colors.append(rgb(words[j]) if j < len(words) else (0, 0, 0))
                    j += 1
                if j < len(words):
                    p.points.append(xy(words[j]))
                j += 1
                if tex:
                    if j < len(words):
                        uv = words[j]
                        if v == 0:
                            p.details.append(clut_text(uv >> 16))
                        elif v == 1:
                            p.details.append(tpage_text(uv >> 16))
                        p.details.append("uv%d %d,%d" % (v, uv & 0xFF, (uv >> 8) & 0xFF))
                    j += 1
            if quad and len(p.points) == 4:     # (drawn as two triangles 0-1-2, 1-2-3: outline 0, 1, 3, 2)
                p.points = [p.points[0], p.points[1], p.points[3], p.points[2]]
            n = j - i
        elif 0x40 <= cmd <= 0x5F:               # Line or polyline
            gouraud, poly = cmd & 0x10, cmd & 0x08
            p.kind, p.semi = "line", bool(cmd & 2)
            p.name = "%s%s%s" % ("Gouraud " if gouraud else "", "polyline" if poly else "line",
                                 " (semi-transparent)" if cmd & 2 else "")
            p.colors.append(rgb(w))
            j = i + 1
            while j < len(words):
                if poly and (words[j] & 0xF000F000) == 0x50005000 and len(p.points) >= 2:
                    j += 1
                    break
                if gouraud and p.points:
                    p.colors.append(rgb(words[j]))
                    j += 1
                    if j >= len(words):
                        break
                p.points.append(xy(words[j]))
                j += 1
                if not poly and len(p.points) == 2:
                    break
            n = j - i
        elif 0x60 <= cmd <= 0x7F:               # Rectangle: color, xy, [uv + CLUT], [w, h]
            size, tex = RECT_SIZES[(cmd >> 3) & 3], cmd & 0x04
            p.kind, p.semi, p.textured = "rect", bool(cmd & 2), bool(tex)
            p.colors.append(rgb(w))
            j = i + 1
            x, y = xy(words[j]) if j < len(words) else (0, 0)
            j += 1
            if tex:
                if j < len(words):
                    p.details += [clut_text(words[j] >> 16), "uv %d,%d" % (words[j] & 0xFF, (words[j] >> 8) & 0xFF)]
                j += 1
            if size is None:
                size = ((words[j] & 0x3FF), (words[j] >> 16) & 0x1FF) if j < len(words) else (0, 0)
                j += 1
            p.name = "%s %dx%d%s" % ("Sprite" if tex else "Tile", size[0], size[1],
                                    " (semi-transparent)" if cmd & 2 else "")
            p.points = [(x, y), (x + size[0], y), (x + size[0], y + size[1]), (x, y + size[1])]
            n = j - i
        elif cmd == 0x02:                       # Fill rectangle (VRAM coordinates, no offset)
            p.kind, p.colors = "fill", [rgb(w)]
            if i + 2 < len(words):
                x, y = words[i + 1] & 0x3F0, (words[i + 1] >> 16) & 0x1FF
                fw, fh = ((words[i + 2] & 0x3FF) + 15) & ~15, (words[i + 2] >> 16) & 0x1FF
                p.name = "Fill %dx%d at %d,%d (VRAM)" % (fw, fh, x, y)
                p.vram_rect = (x, y, fw, fh)
            n = 3
        elif 0x80 <= cmd <= 0x9F:
            p.kind, n = "copy", 4
            if i + 3 < len(words):
                p.name = "VRAM copy %dx%d from %d,%d to %d,%d" % (
                    words[i + 3] & 0xFFFF, words[i + 3] >> 16, words[i + 1] & 0xFFFF, words[i + 1] >> 16,
                    words[i + 2] & 0xFFFF, words[i + 2] >> 16)
        elif 0xA0 <= cmd <= 0xBF:               # CPU to VRAM: its data follows
            p.kind = "copy"
            if i + 2 < len(words):
                cw, ch = words[i + 2] & 0xFFFF, words[i + 2] >> 16
                p.name = "Load image %dx%d at %d,%d" % (cw, ch, words[i + 1] & 0xFFFF, words[i + 1] >> 16)
                n = 3 + (cw * ch + 1) // 2
            else:
                n = len(words) - i
        elif 0xC0 <= cmd <= 0xDF:
            p.kind, p.name, n = "copy", "Store image (VRAM to CPU)", 3
        elif cmd in ENV_NAMES:
            p.kind, p.name = "env", ENV_NAMES[cmd]
            if cmd == 0xE1:
                p.details.append(tpage_text(w & 0x7FF))
            elif cmd in (0xE3, 0xE4):
                p.details.append("%d,%d" % (w & 0x3FF, (w >> 10) & 0x3FF))
            elif cmd == 0xE5:      # (x in bits 0-10, y in bits 11-21)
                p.details.append("%d,%d" % (s11(w), s11(w >> 11)))
        elif cmd == 0x00:
            p.kind, p.name = "other", "NOP"
        elif cmd == 0x01:
            p.kind, p.name = "other", "Clear texture cache"
        p.words = words[i:i + max(n, 1)]
        prims.append(p)
        i += max(n, 1)
    return prims


def inside(prim, x, y):
    """Point (drawing coordinates) in the primitive (polygons, rectangles), or near it (lines)."""
    pts = prim.points
    if prim.kind == "line":
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            dx, dy = x1 - x0, y1 - y0
            t = max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / float(dx * dx + dy * dy or 1)))
            if (x0 + t * dx - x) ** 2 + (y0 + t * dy - y) ** 2 <= 9:
                return True
        return False
    if len(pts) < 3:
        return False
    inside_ = False
    for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]):
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / float(y1 - y0):
            inside_ = not inside_
    return inside_
