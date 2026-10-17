"""
PS1 DEV CART tim.tfs tool: list/extract the menu TIM images and rebuild the archive from PNG files.

Requires Pillow (pip install pillow).

  python tfs_tool.py list    tim.tfs
  python tfs_tool.py extract tim.tfs out_dir
  python tfs_tool.py pack    tim.tfs [--bg R,G,B] name.tim=image.png@X,Y[:WxH] ... [name.tim ...]

Pack notes:
- Images are loaded by the firmware straight into the 320x240 16-bit framebuffer at X,Y,
  so they are stored as 16bpp TIMs (no CLUT).
- A bare name.tim (no '=') keeps that entry unchanged from the existing archive.
- The optional :WxH scales the PNG to fit that box (aspect ratio kept, centred, white padding).
- Padding and transparent pixels use the --bg colour (default 14,16,28: the menu background, UI_BG of
  devcart_ui.h). Black pixels are stored as 0, transparent in the text sprites (font.tim: white on black).
- The menu (display.h) has the logo at 8,6 (304x40, images/logo_menu.png: images/logo.png with its black background
  lightened to the menu blue), then from y=50 an accent line and the panels: keep images within the logo area.
  python tfs_tool.py pack tim.tfs logo.tim=images/logo_menu.png@8,6:304x40 font.tim=images/font.png@768,0
- The archive is placed at ROM offset 0x10000, it must not exceed 64KB to keep a 128KB ROM,
  and each TIM must not exceed 64KB (it is copied to RAM 0x80010000, the VRAM backup starts at 0x80020000).
- The firmware loads the TIMs listed in tim_files (ps1devcart.c) by name, font.tim must be kept.
"""
import os
import struct
import sys

from PIL import Image

VRAM_W, VRAM_H = 1024, 512
NAME_LEN = 24
MAX_TFS_SIZE = 0x10000
MAX_TIM_SIZE = 0x10000


def read_tfs(path):
    data = open(path, 'rb').read()
    if data[:3] != b'TFS':
        raise ValueError('%s is not a TFS archive' % path)
    files = []
    for i in range(data[3]):
        entry = data[4 + i * 32:4 + (i + 1) * 32]
        name = entry[:NAME_LEN].split(b'\0')[0].decode()
        offset, size = struct.unpack('<II', entry[NAME_LEN:32])
        files.append((name, data[offset:offset + size]))
    return files


def write_tfs(path, files):
    header = bytearray(b'TFS' + bytes([len(files)]))
    offset = 4 + len(files) * 32
    for name, tim in files:
        header += name.encode().ljust(NAME_LEN, b'\0')[:NAME_LEN] + struct.pack('<II', offset, len(tim))
        offset += len(tim)
    out = bytes(header) + b''.join(tim for _, tim in files)
    if len(out) > MAX_TFS_SIZE:
        raise ValueError('archive is %d bytes, max is %d (64KB)' % (len(out), MAX_TFS_SIZE))
    open(path, 'wb').write(out)
    return len(out)


def tim_info(tim):
    flags = struct.unpack_from('<I', tim, 4)[0]
    pos = 8
    if flags & 8:    # CLUT present
        pos += struct.unpack_from('<I', tim, pos)[0]
    length, x, y, w, h = struct.unpack_from('<IHHHH', tim, pos)
    return flags & 3, x, y, w, h, tim[pos + 12:pos + length]


def tim_to_image(tim):
    bpp, x, y, w, h, pixels = tim_info(tim)
    if bpp != 2:
        raise ValueError('only 16bpp TIMs are used by the PS1 DEV CART menu')
    img = Image.new('RGB', (w, h))
    for i, v in enumerate(struct.unpack('<%dH' % (w * h), pixels)):
        r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
        img.putpixel((i % w, i // w), (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2))
    return img, x, y


def image_to_tim(img, x, y):
    w, h = img.size
    if x < 0 or y < 0 or x + w > VRAM_W or y + h > VRAM_H:
        raise ValueError('image %dx%d at %d,%d does not fit in the %dx%d VRAM' % (w, h, x, y, VRAM_W, VRAM_H))
    pixels = bytearray()
    for r, g, b in img.getdata():
        v = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
        # Black is stored as 0 (transparent when drawn as a texture: the menu draws its text with sprites of font.tim,
        # the logo is copied to the screen as it is)
        pixels += struct.pack('<H', v)
    tim = struct.pack('<II', 0x10, 2) + struct.pack('<IHHHH', 12 + len(pixels), x, y, w, h) + bytes(pixels)
    if len(tim) > MAX_TIM_SIZE:
        raise ValueError('TIM is %d bytes, max is %d (64KB)' % (len(tim), MAX_TIM_SIZE))
    return tim


def load_png(path, box, bg):
    img = Image.open(path).convert('RGBA')
    if box:
        img.thumbnail(box, Image.LANCZOS)
        canvas = Image.new('RGBA', box, bg + (255,))
        canvas.alpha_composite(img, ((box[0] - img.width) // 2, (box[1] - img.height) // 2))
        img = canvas
    background = Image.new('RGBA', img.size, bg + (255,))
    return Image.alpha_composite(background, img).convert('RGB')


def parse_pack_arg(arg):
    # name.tim=image.png@X,Y[:WxH]
    name, rest = arg.split('=', 1)
    path, place = rest.rsplit('@', 1)
    box = None
    if ':' in place:
        place, size = place.split(':')
        box = tuple(int(v) for v in size.lower().split('x'))
    x, y = (int(v) for v in place.split(','))
    return name, path, x, y, box


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    cmd, tfs = argv[1], argv[2]
    if cmd == 'list':
        for name, tim in read_tfs(tfs):
            bpp, x, y, w, h, _ = tim_info(tim)
            print('%-12s %6d bytes  %dx%d at %d,%d' % (name, len(tim), w, h, x, y))
    elif cmd == 'extract':
        os.makedirs(argv[3], exist_ok=True)
        for name, tim in read_tfs(tfs):
            img, x, y = tim_to_image(tim)
            out = os.path.join(argv[3], '%s@%d,%d.png' % (os.path.splitext(name)[0], x, y))
            img.save(out)
            print(out)
    elif cmd == 'pack':
        existing = dict(read_tfs(tfs)) if os.path.exists(tfs) else {}
        files = []
        bg = (14, 16, 28)
        args = argv[3:]
        if args[:1] == ['--bg']:
            bg = tuple(int(v) for v in args[1].split(','))
            args = args[2:]
        for arg in args:
            if '=' not in arg:
                files.append((arg, existing[arg]))
                continue
            name, path, x, y, box = parse_pack_arg(arg)
            files.append((name, image_to_tim(load_png(path, box, bg), x, y)))
        print('%s: %d bytes' % (tfs, write_tfs(tfs, files)))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
