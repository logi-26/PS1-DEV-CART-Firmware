"""shot2png.py NAME: .menu/NAME.raw (+ .txt: width height bpp) from menu_auto.lua's shot() -> .menu/NAME.png"""
import os, sys
from PIL import Image
D = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".menu")
n = sys.argv[1]
t = open(os.path.join(D, n + ".txt")).read().split(); w, h, bpp = t[0], t[1], t[-1]
w, h = int(w), int(h)
raw = open(os.path.join(D, n + ".raw"), "rb").read()
if len(raw) >= w * h * 3 and bpp != "0":
    img = Image.frombytes("RGB", (w, h), raw[:w * h * 3])
else:                       # 15-bit BGR
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            v = raw[(y * w + x) * 2] | raw[(y * w + x) * 2 + 1] << 8
            px[x, y] = ((v & 31) << 3, (v >> 5 & 31) << 3, (v >> 10 & 31) << 3)
img.save(os.path.join(D, n + ".png"))
print(w, h, bpp, len(raw))
