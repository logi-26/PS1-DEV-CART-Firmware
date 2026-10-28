"""
PS1 DEV CART GUI: makes the app icon (icons/ps1devcart.ico for the .exe, icons/ps1devcart-SIZE.png for the windows):
the cartridge of the VS Code extension's panel
icon (vscode-extension/media/devcart.svg), its outline light blue on a transparent background.
Run it again after changing the drawing: python make_icon.py
"""
import os

from PIL import Image, ImageDraw

COLOR = (79, 193, 255, 255)     # #4fc1ff, the light blue of the extension's panel
STROKE = 1.6                    # devcart.svg: 24x24 view box, stroke width 1.6, round joins
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
WINDOW_SIZES = (16, 32, 48, 256)     # The windows' icon (Tk's iconphoto: title bar, taskbar, Alt+Tab)
HERE = os.path.dirname(os.path.abspath(__file__))

# devcart.svg's paths: the cartridge, its label, the connector's pins
OUTLINE = [(5, 3), (19, 3), (19, 15), (17, 17), (17, 21), (7, 21), (7, 17), (5, 15), (5, 3)]
LABEL = [(8, 6), (16, 6), (16, 11), (8, 11), (8, 6)]
PINS = [[(9, 17), (9, 21)], [(12, 17), (12, 21)], [(15, 17), (15, 21)]]


def draw(size):
    """The icon at a size: drawn 8 times bigger, then reduced (smooth edges)."""
    big = size * 8
    scale = big / 24.0
    width = max(1, round(STROKE * scale * (1.25 if size <= 24 else 1.0)))     # (a bit thicker when small)
    im = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for line in [OUTLINE, LABEL] + PINS:
        points = [(x * scale, y * scale) for x, y in line]
        d.line(points, fill=COLOR, width=width, joint="curve")
        r = width / 2.0
        for x, y in points:     # Round joins and ends
            d.ellipse((x - r, y - r, x + r, y + r), fill=COLOR)
    return im.resize((size, size), Image.LANCZOS)


def main():
    out = os.path.join(HERE, "icons")
    os.makedirs(out, exist_ok=True)
    images = [draw(s) for s in SIZES]
    images[-1].save(os.path.join(out, "ps1devcart.ico"), sizes=[(s, s) for s in SIZES], append_images=images[:-1])
    for size, image in zip(SIZES, images):
        if size in WINDOW_SIZES:
            image.save(os.path.join(out, "ps1devcart-%d.png" % size))
    print("icons/ps1devcart.ico, icons/ps1devcart-{%s}.png" % ",".join(map(str, WINDOW_SIZES)))


if __name__ == "__main__":
    main()
