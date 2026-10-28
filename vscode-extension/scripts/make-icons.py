"""Makes the panel's button icons (media/buttons/*.png) from the drawings in icons/ (grey lines on an off-white
background, .bmp): the background becomes transparent, the lines white (the panel colours them: they are used as
masks), cropped to the drawing, its thin lines thickened a little (they are shrunk 4 times), centred in 96 x 64
(3:2: the wide drawings of two cartridges use the width; shown at 36 x 24 px: sharp on high DPI screens).

    python scripts/make-icons.py
"""
import glob
import os

from PIL import Image, ImageFilter

EXT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SRC = os.path.join(EXT, 'icons')
OUT = os.path.join(EXT, 'media', 'buttons')
WIDTH, HEIGHT = 96, 64
MARGIN = 2          # (pixels around the drawing)
LIGHT, DARK = 200, 135  # grey levels: lighter than LIGHT is background (the faint grid too), darker than DARK the line


def alpha_of(gray):
    if gray >= LIGHT:
        return 0
    if gray <= DARK:
        return 255
    return round(255 * (LIGHT - gray) / (LIGHT - DARK))


def convert(path):
    gray = Image.open(path).convert('L')
    alpha = gray.point(alpha_of)
    box = alpha.point(lambda a: 255 if a > 40 else 0).getbbox()
    alpha = alpha.crop(box).filter(ImageFilter.MaxFilter(3))
    w, h = alpha.size
    scale = min((WIDTH - 2 * MARGIN) / w, (HEIGHT - 2 * MARGIN) / h)
    size = (round(w * scale), round(h * scale))
    alpha = alpha.resize(size, Image.LANCZOS)
    icon = Image.new('RGBA', (WIDTH, HEIGHT), (255, 255, 255, 0))
    white = Image.new('RGBA', size, (255, 255, 255, 255))
    white.putalpha(alpha)
    icon.paste(white, ((WIDTH - size[0]) // 2, (HEIGHT - size[1]) // 2))
    return icon


def main():
    os.makedirs(OUT, exist_ok=True)
    for path in sorted(glob.glob(os.path.join(SRC, '*.bmp'))):
        name = os.path.splitext(os.path.basename(path))[0].replace(' ', '-') + '.png'
        convert(path).save(os.path.join(OUT, name), optimize=True)
        print(name)


if __name__ == '__main__':
    main()
