"""png.py DIR NAME...: DIR/NAME.raw (+ .txt: width height bpp) from dd_auto.lua's shot() -> DIR/NAME.png"""
import os, sys
from PIL import Image
D = sys.argv[1]
for n in sys.argv[2:]:
    t = open(os.path.join(D, n + ".txt")).read().split(); w, h, bpp = int(t[0]), int(t[1]), t[-1]
    raw = open(os.path.join(D, n + ".raw"), "rb").read()
    if len(raw) >= w * h * 3 and bpp != "0":
        img = Image.frombytes("RGB", (w, h), raw[:w * h * 3])
    else:
        img = Image.new("RGB", (w, h)); px = img.load()
        for y in range(h):
            for x in range(w):
                v = raw[(y * w + x) * 2] | raw[(y * w + x) * 2 + 1] << 8
                px[x, y] = ((v & 31) << 3, (v >> 5 & 31) << 3, (v >> 10 & 31) << 3)
    img.save(os.path.join(D, n + ".png"))
