"""
PS1 DEV CART GUI: the asset scanner of the Asset Extractor page. Finds the PlayStation's standard file formats in a block of
data: a snapshot of a running game (ps1transfer -snapshot: its RAM, VRAM and SPU RAM), or a file of a disc (an
archive: the formats are often stored inside a game's own data files, one after the other).

Found by their headers, each one checked before it is kept (a header alone is often just data):
- TIM images: ID 0x10, flags (4/8/16/24-bit, CLUT), the CLUT and image blocks: lengths matching their sizes, inside
  the 1024x512 VRAM.
- VAB sound banks: "pBAV", version 5-7, programs, tones and VAGs counts, the VH size; with its VB if it follows (each
  sample's last ADPCM block has its end flag). A VH alone (the samples already sent to the SPU, the VB freed): its
  samples are looked for in the SPU RAM of the snapshot (the VAG sizes of the VH, one after the other, each ending on
  an end flag), and the VAB put together again.
- SEQ music ("pQES", version 1) up to its end of track, SEP ("pQES", version 0) with its SEQs.
- VAG samples ("VAGp" and the data size).
- TMD models: ID 0x41, the object table (vertices, normals and primitives inside the file).
- The samples in SPU RAM (spu.py's map), as VAG files.
- Games' music in other formats, each saved as found and converted for the sound tester (SEQ + VH + VB):
  - SQ: Sony's later format ("SSsq" music, "SShd" bank and its BD samples: Ape Escape, Alundra 2...; sshd.py), the
    music with the bank found near it.
  - SND: Crystal Dynamics' banks ("DNSa", with their "PMSa" samples: Gex 3, Akuji...; aad.py), stored as they are or
    in a Gex-style BIGFILE.DAT (its compression undone); each bank with its musics.
  - SFX: Crystal Dynamics' sound effect sets ("FNSa" + "FMSa": Soul Reaver), as a VAB with a SEQ playing each one.
  - AKAO: Square's sequences and sample sets ("AKAO": FF7, FF8, Vagrant Story...; akao.py), also inside FF7's LZS
    files; a sequence converted when the sample set it names is in the same file (FF7's instruments are in
    SOUND/INSTR.ALL: the Game music scan of the disc converts those).
Other compressed data isn't found: a game's own compression has to be undone first.

The VRAM of a snapshot is shown as 16-bit pixels, or as 4/8-bit texture pages with a CLUT picked in it; a part of it is
saved as a PNG or a TIM.

Command line: python assetscan.py FILE_OR_SNAPSHOT_FOLDER [OUT_FOLDER]  (lists, or writes every asset to OUT_FOLDER)

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import os
import struct
import sys

import numpy as np

RAM_BASE = 0x80000000
VRAM_W, VRAM_H = 1024, 512
DISC_ID_ADRS = 0xDEE0               # DiscId in RAM (usb.h): the executable of the disc the firmware booted
DISC_ID_MAGIC = 0x43534944
VH_HEADER, VH_PROGRAMS, VH_TONES = 32, 128 * 16, 16 * 32
SPU_RAM_SIZE, SPU_REGS_SIZE = 0x80000, 512
NIBBLE_SWAP = bytes(((b & 15) << 4) | (b >> 4) for b in range(256))     # 4-bit pixels: low nibble first
MIN_SAMPLE = 256                    # SPU RAM samples smaller than this, played by no voice: left out
KINDS = ("Texture", "TIM", "VAB", "VH", "SEQ", "SEP", "VAG", "TMD", "Sample", "SQ", "SND", "SFX", "AKAO")
TESTER_FILES = (".vab", ".vh", ".vb", ".seq", ".sep", ".vag")     # (what the sound tester plays)


class Asset:
    """A file found: kind (KINDS), where (source name: "RAM", "SPU RAM", a file), offset in it, the file's bytes,
    info (a short description), ext (file extension), parts: other files of the same asset {ext: bytes} (VH + VB)."""
    __slots__ = ("kind", "where", "offset", "data", "info", "ext", "parts")

    def __init__(self, kind, where, offset, data, info, ext, parts=None):
        self.kind, self.where, self.offset, self.data, self.info, self.ext = kind, where, offset, data, info, ext
        self.parts = parts or {}

    @property
    def size(self):
        return len(self.data)

    def name(self, base=""):
        """FILE_kind_OFFSET.ext: an asset of a disc file named after its file (assets of several files: no clash),
        one of a snapshot after base (the game)."""
        if self.where not in ("RAM", "VRAM", "SPU RAM"):
            base = os.path.splitext(self.where)[0]
        prefix = (base + "_") if base else ""
        return "%s%s_%06X%s" % (prefix, self.kind.lower(), self.offset, self.ext)

    def save(self, folder, base=""):
        """Writes the file(s) in folder: the paths written."""
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, self.name(base))
        with open(path, "wb") as f:
            f.write(self.data)
        out = [path]
        for ext, data in self.parts.items():
            p = os.path.splitext(path)[0] + ext
            with open(p, "wb") as f:
                f.write(data)
            out.append(p)
        return out


def u16(d, o):
    return d[o] | (d[o + 1] << 8)


def u32(d, o):
    return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24)


def be(b):
    return int.from_bytes(b, "big")


# ---------------------------------------------------------------------------------------------------- TIM

def _tim_block(d, o, clut, longer=False):
    """A TIM block at o: (length, x, y, w, h) if it is valid, else None. longer: the length may be more than the
    pixels need (up to 4 times: some early games' TIMs, e.g. Ridge Racer's TMS archives)."""
    if o + 12 > len(d):
        return None
    length, x, y, w, h = struct.unpack_from("<IHHHH", d, o)
    need = 12 + w * h * 2
    if not w or not h or x + w > VRAM_W or y + h > VRAM_H or o + length > len(d):
        return None
    if length != need and not (longer and need < length <= 12 + w * h * 8 and not length & 3):
        return None
    if clut and (x & 15 or w > 256 * 4):
        return None
    return length, x, y, w, h


def tim_at(d, o):
    """The TIM at offset o: (size, mode, image block, CLUT block or None), or None."""
    if o + 8 > len(d) or u32(d, o) != 0x10:
        return None
    flags = u32(d, o + 4)
    mode = flags & 7
    if flags & ~0xF or mode > 3:
        return None
    p = o + 8
    clut = None
    if flags & 8:
        clut = _tim_block(d, p, True)
        if not clut:
            return None
        p += clut[0]
    elif mode < 2:          # 4/8-bit without a CLUT: not a texture a game would load
        return None
    img = _tim_block(d, p, False, longer=clut is not None)    # (with an exact CLUT block: a longer image block too)
    if not img:
        return None
    if mode == 3 and (img[3] * 2) % 3:
        return None
    return p + img[0] - o, mode, img, clut


def tim_info(d):
    t = tim_at(d, 0)
    _, mode, img, clut = t
    w = (img[3] * 4, img[3] * 2, img[3], img[3] * 2 // 3)[mode]
    text = "%dx%d %d-bit, at %d,%d in VRAM" % (w, img[4], (4, 8, 16, 24)[mode], img[1], img[2])
    if clut:
        text += ", CLUT %d,%d (%d color%s x %d)" % (clut[1], clut[2], clut[3], "s" if clut[3] > 1 else "", clut[4])
    return text


def ps_to_rgb(v):
    return ((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, ((v >> 10) & 31) * 255 // 31)


def _palette(colors):
    pal = []
    for v in colors:
        pal += ps_to_rgb(v)
    return pal + [0] * (768 - len(pal))


def tim_image(d, row=0):
    """PIL image of a TIM file's bytes (its first CLUT row, or row)."""
    from PIL import Image
    size, mode, img, clut = tim_at(d, 0)
    p = 8 + (clut[0] if clut else 0)
    w, h = img[3], img[4]
    raw = bytes(d[p + 12:p + 12 + w * h * 2])
    if mode == 2:
        return Image.frombytes("RGB", (w, h), raw, "raw", "RGB;15")
    if mode == 3:
        return Image.frombytes("RGB", (w * 2 // 3, h), raw, "raw", "RGB", w * 2)
    if mode == 0:
        im = Image.frombytes("P", (w * 4, h), raw.translate(NIBBLE_SWAP), "raw", "P;4")
    else:
        im = Image.frombytes("P", (w * 2, h), raw, "raw", "P")
    if clut:
        cw = clut[3]
        r = min(row, clut[4] - 1)
        colors = struct.unpack_from("<%dH" % cw, d, 8 + 12 + r * cw * 2)
        im.putpalette(_palette(colors))
    return im.convert("RGB")


# ---------------------------------------------------------------------------------------------------- VAB

def vh_at(d, o):
    """The VH at offset o: (vh size, ps, ts, vs, file size of the VAB) or None."""
    if d[o:o + 4] != b"pBAV" or o + VH_HEADER > len(d):
        return None
    version = u32(d, o + 4)
    fsize = u32(d, o + 12)
    ps, ts, vs = u16(d, o + 18), u16(d, o + 20), u16(d, o + 22)
    if version not in (5, 6, 7) or not 1 <= ps <= 128 or not 1 <= ts <= ps * 16 or not 1 <= vs <= 254:
        return None
    size = VH_HEADER + VH_PROGRAMS + ps * VH_TONES + 512
    if o + size > len(d) or fsize < size:
        return None
    return size, ps, ts, vs, fsize


def loaded_vh(vh):
    """A VH as its file had it: the 2 reserved words of each program record (bytes 8-15) hold the sound library's own
    values once the bank is loaded (libsnd); in the files they are 0xFFFFFFFF."""
    vh = bytearray(vh)
    for p in range(128):
        vh[VH_HEADER + p * 16 + 8:VH_HEADER + p * 16 + 16] = b"\xff" * 8
    return bytes(vh)


def vag_sizes(vh, size, vs):
    """The VAG sizes (bytes) of a VH, VAG 1 to vs."""
    base = size - 512
    return [u16(vh, base + i * 2) * 8 for i in range(1, vs + 1)]


def _end_flags(buf):
    """Per byte address: the ADPCM block there has its end flag (numpy bool array)."""
    a = np.frombuffer(bytes(buf), np.uint8)
    flags = np.zeros(len(a), bool)
    flags[:-1] = (a[1:] & 1).astype(bool)
    return flags


def samples_at(buf, sizes, starts, flags=None):
    """The starts (array) where the samples of sizes, one after the other, each end on a block with the end flag."""
    if flags is None:
        flags = _end_flags(buf)
    starts = np.asarray(starts)
    ok = np.ones(len(starts), bool)
    pos = 0
    for s in sizes:
        if s < 16:
            return starts[:0]
        pos += s
        idx = starts + pos - 16
        good = idx < len(flags)
        ok &= good
        ok[good] &= flags[idx[good]]
        if not ok.any():
            break
    return starts[ok]


def find_vb_in_spu(vh, size, vs, spu_ram, flags=None):
    """SPU RAM address of the VAB's samples (sizes from the VH, sent one after the other), or None. Several matches:
    the first one."""
    sizes = vag_sizes(vh, size, vs)
    total = sum(sizes)
    if not total or total > SPU_RAM_SIZE - 0x1000:
        return None
    starts = np.arange(0x1000, SPU_RAM_SIZE - total + 1, 8)
    for s in samples_at(spu_ram, sizes, starts, flags):     # (the end flags only), then each block checked
        if samples_valid(spu_ram, int(s), sizes):
            return int(s)
    return None


def samples_valid(buf, start, sizes):
    """Every block of the samples is ADPCM (filter 0-4, shift 0-12, flags 0-7), with the end flag on its last block
    only (a zero block anywhere: silence)."""
    a = np.frombuffer(bytes(buf[start:start + sum(sizes)]), np.uint8).reshape(-1, 16)
    head, flag = a[:, 0], a[:, 1]
    if ((head >> 4) > 4).any() or ((head & 15) > 12).any() or (flag > 7).any():
        return False
    last = np.cumsum([s // 16 for s in sizes]) - 1
    ends = np.flatnonzero(flag & 1)
    return set(last.tolist()) <= set(ends.tolist()) and len(np.setdiff1d(ends, last)) == 0


# ---------------------------------------------------------------------------------------------------- SEQ

def seq_end(d, p):
    """Offset after the end of track of the SEQ events starting at p, or None (bad data or no end)."""
    status, n, limit = 0, 0, min(len(d), p + 0x100000)

    def varlen():
        nonlocal p
        v = 0
        for _ in range(4):
            if p >= limit:
                return None
            b = d[p]
            p += 1
            v = (v << 7) | (b & 0x7F)
            if not b & 0x80:
                return v
        return None
    if varlen() is None:
        return None
    while p + 1 < limit:
        n += 1
        s = d[p]
        if s & 0x80:
            p += 1
        else:
            s = status
        if s == 0xFF:
            status = 0xFF
            typ = d[p]
            p += 1
            if typ == 0x2F:
                return p + 1 if p < len(d) and d[p] == 0 else p
            if typ == 0x51:
                p += 3
            else:
                return None     # (SEQ files hold no other meta events)
        else:
            if s < 0x80 or s >= 0xF0:
                return None
            status = s
            p += 1 if s & 0xF0 in (0xC0, 0xD0) else 2
        if varlen() is None:
            return None
    return None


def seq_at(d, o):
    """A SEQ or SEP at o: (size, kind, description) or None."""
    if d[o:o + 4] not in (b"pQES", b"SEQp") or o + 16 > len(d):
        return None
    if be(d[o + 4:o + 8]) == 1 or be(d[o + 4:o + 6]) == 1:
        h = o + (8 if be(d[o + 4:o + 8]) == 1 else 6)
        res, tempo = be(d[h:h + 2]), be(d[h + 2:h + 5])
        if not res or not tempo:
            return None
        end = seq_end(d, h + 7)
        if not end:
            return None
        return end - o, "SEQ", "%d ticks a beat, %d BPM" % (res, round(60000000 / tempo))
    if be(d[o + 4:o + 6]) != 0:
        return None
    p, n = o + 6, 0
    while p + 13 <= len(d) and n < 16:
        if be(d[p:p + 2]) != n:
            break
        size = be(d[p + 9:p + 13])
        if not size or p + 13 + size > len(d) or not seq_end(d, p + 13) or seq_end(d, p + 13) > p + 13 + size + 1:
            break
        p += 13 + size
        n += 1
    if not n:
        return None
    return p - o, "SEP", "%d SEQ%s" % (n, "s" if n > 1 else "")


# ---------------------------------------------------------------------------------------------------- TMD

def tmd_at(d, o):
    """A TMD at o (offsets relative to its object table): (size, objects, vertices, primitives) or None."""
    if o + 12 > len(d) or u32(d, o) != 0x41:
        return None
    flags, nobj = u32(d, o + 4), u32(d, o + 8)
    if flags != 0 or not 1 <= nobj <= 1024:
        return None
    table = o + 12
    if table + nobj * 28 > len(d):
        return None
    end, nv, npr = table + nobj * 28, 0, 0
    for i in range(nobj):
        vt, vn, nt, nn, pt, pn, scale = struct.unpack_from("<6Ii", d, table + i * 28)
        for top, count, unit in ((vt, vn, 8), (nt, nn, 8)):
            if top & 3 or not count or count > 65536 or top < nobj * 28 or table + top + count * unit > len(d):
                return None
            end = max(end, table + top + count * unit)
        if pt & 3 or not pn or pn > 65536 or pt < nobj * 28 or table + pt + 4 > len(d):
            return None
        nv += vn
        npr += pn
    return end - o, nobj, nv, npr


# ---------------------------------------------------------------------------------------------------- scan

def scan_bytes(d, where="RAM", spu_ram=None, progress=None):
    """The assets in d (bytes): [Asset]. spu_ram: the SPU RAM of the same snapshot (a VH's samples)."""
    found = []
    flags = _end_flags(spu_ram) if spu_ram is not None else None
    a = np.frombuffer(bytes(d[:len(d) & ~3]), "<u4")
    candidates = []
    for magic, kind in ((0x10, "TIM"), (0x56414270, "VH"), (0x53455170, "SEQ"), (0x70514553, "SEQ"),
                        (0x70474156, "VAG"), (0x41, "TMD")):
        for i in np.flatnonzero(a == magic):
            candidates.append((int(i) * 4, kind))
    candidates.sort()
    covered = 0                     # (assets inside an asset already found: skipped)
    for n, (o, kind) in enumerate(candidates):
        if progress and not n % 2000:
            progress(n, len(candidates))
        if o < covered and kind in ("TIM", "TMD"):
            continue
        if kind == "TIM":
            t = tim_at(d, o)
            if t:
                data = bytes(d[o:o + t[0]])
                found.append(Asset("TIM", where, o, data, tim_info(data), ".tim"))
                covered = o + t[0]
        elif kind == "VH":
            v = vh_at(d, o)
            if not v:
                continue
            size, ps, ts, vs, fsize = v
            vh = loaded_vh(d[o:o + size]) if where == "RAM" else bytes(d[o:o + size])   # (a file's VH: as it is)
            sizes = vag_sizes(vh, size, vs)
            total = sum(sizes)
            text = "%d programs, %d tones, %d samples (%d KB)" % (ps, ts, vs, total // 1024)
            vb_here = o + size + total <= len(d) and len(samples_at(d[o + size:o + size + total], sizes, [0])) == 1
            if vb_here:
                data = vh + bytes(d[o + size:o + size + total])
                found.append(Asset("VAB", where, o, data, text + ", with its samples", ".vab"))
                covered = o + size + total
                continue
            adrs = find_vb_in_spu(vh, size, vs, spu_ram, flags) if spu_ram is not None else None
            if adrs is not None:
                vb = bytes(spu_ram[adrs:adrs + total])
                found.append(Asset("VAB", where, o, vh, text + ", samples found in SPU RAM at 0x%05X" % adrs, ".vh",
                                   {".vb": vb}))
            else:
                found.append(Asset("VH", where, o, vh, text + ", samples not found (VH only)", ".vh"))
            covered = o + size
        elif kind == "SEQ":
            s = seq_at(d, o)
            if s:
                found.append(Asset(s[1], where, o, bytes(d[o:o + s[0]]), s[2], ".seq" if s[1] == "SEQ" else ".sep"))
                covered = o + s[0]
        elif kind == "VAG":
            if o + 48 > len(d):
                continue
            size, rate = be(d[o + 12:o + 16]), be(d[o + 16:o + 20])
            if not 16 <= size <= SPU_RAM_SIZE or size & 15 or not 4000 <= rate <= 96000 or o + 48 + size > len(d):
                continue
            name = bytes(d[o + 32:o + 48]).split(b"\0")[0].decode("ascii", "replace")
            found.append(Asset("VAG", where, o, bytes(d[o:o + 48 + size]),
                               "%d Hz, %.2f s%s" % (rate, size / 16 * 28 / rate, (" \"%s\"" % name) if name else ""), ".vag"))
            covered = o + 48 + size
        elif kind == "TMD":
            t = tmd_at(d, o)
            if t:
                found.append(Asset("TMD", where, o, bytes(d[o:o + t[0]]),
                                   "%d object%s, %d vertices, %d primitives" % (t[1], "s" if t[1] > 1 else "", t[2], t[3]),
                                   ".tmd"))
                covered = o + t[0]
    found += game_music(d, where)
    return found


def _seconds(info):
    minutes, seconds = divmod(int(round(info["seconds"])), 60)
    return "%d:%02d" % (minutes, seconds)


def game_music(d, where="RAM"):
    """The SQ/HD musics and Crystal Dynamics banks in d: [Asset] ("SQ", "SND"), each with its original files and the
    tester's (parts: .seq, .vh, .vb; an SND's musics _N.seq). A file of another kind: none (never an exception)."""
    import aad
    import sshd
    found, d = [], d if isinstance(d, bytes) else bytes(d)
    try:
        musics = sshd.scan(d)
    except Exception:                   # (damaged data: the other assets are still listed)
        musics = []
    for m in musics:
        try:
            seq, info = sshd.to_seq(m["sq"])
            parts, text = {".seq": seq}, "SQ/HD music, %s, %d notes" % (_seconds(info), info["notes"])
            if m["hd"]:
                vh, vb, vinfo = sshd.to_vab(m["hd"], m["bd_bytes"])
                parts.update({".hd": m["hd_bytes"], ".bd": m["bd_bytes"], ".vh": vh, ".vb": vb})
                text += "; its bank: %d programs, %d samples (%d KB)" % (len(vinfo["programs"]), vinfo["samples"],
                                                                          len(vb) // 1024)
            else:
                text += "; no bank found (SEQ only)"
            found.append(Asset("SQ", where, m["at"], m["sq_bytes"], text, ".sq", parts))
        except Exception:
            continue
    try:
        banks = aad.scan(d)
    except Exception:
        banks = []
    for b in banks:
        try:
            snd = b["snd"]
            name = b["name"]
            musics = [(k, s) for k, s in enumerate(snd.sequences) if s.notes]
            text = "Crystal Dynamics bank%s, %d programs, %d music%s" % (
                "" if name.startswith("at 0x") else " (archive file %s)" % name, len(snd.programs), len(musics),
                "" if len(musics) == 1 else "s")
            parts = {}
            if b["smp"] is not None:
                vh, vb, vinfo = aad.to_vab(snd, b["smp"])
                parts.update({".smp": b["smp_file"], ".vh": vh, ".vb": vb})
                text += "; samples: %d (%d KB); reverb: %s" % (vinfo["samples"], len(vb) // 1024, vinfo["reverb"])
            else:
                text += "; samples not found (SEQs only)"
            for k, s in musics:
                parts["_%d.seq" % k] = aad.to_seq(s)[0]
            found.append(Asset("SND", where, b["at"], b["data"], text, ".snd", parts))     # (as stored)
        except Exception:
            continue
    try:
        sets = aad.scan_sfx(d)
    except Exception:
        sets = []
    for s in sets:
        try:
            vh, vb, info = aad.sfx_to_vab(s["sfx"])
            seq, seconds = aad.sfx_demo_seq(s["sfx"])
            found.append(Asset("SFX", where, s["at"], s["snf"], "Crystal Dynamics sound effects: %d effects, %d samples "
                               "(%d KB); the SEQ plays each once" % (info["effects"], info["samples"], len(vb) // 1024),
                               ".snf", {".smf": s["smf"], ".vh": vh, ".vb": vb, ".seq": seq}))
        except Exception:
            continue
    found += akao_music(d, where)
    return found


def akao_music(d, where="RAM"):
    """Square's AKAO sequences and sample sets in d (or in d decompressed, an LZS file of FF7: their place then 0):
    [Asset] ("AKAO"). A sequence with its instruments in d (a sample set it names) also as the tester's .seq + .vh +
    .vb; FF7's (instruments in SOUND/INSTR.ALL) as stored: the Game music scan of the disc converts those."""
    import akao
    found, blobs = [], [(d, False)]
    try:
        if d[:4] != akao.MAGIC and akao._LZS_AKAO.search(d):
            u = akao.unlzs(d)
            if u and akao.MAGIC in u:
                blobs.append((u, True))
    except Exception:
        pass
    seqs, sets = [], []
    for data, packed in blobs:
        p = data.find(akao.MAGIC)
        while p >= 0:
            at = 0 if packed else p
            try:
                if data[p + 6:p + 8] == b"\0\0":
                    n, size = akao.u32(data, p + 0x1C), akao.u32(data, p + 0x14)
                    s = akao.SampleSet(data[p:p + 0x40 + n * 16 + size])
                    sets.append((at, s, data[p:p + 0x40 + n * 16 + size]))
                else:
                    seqs.append((at, akao.Sequence(data[p:p + akao.SEQ_WINDOW])))
            except Exception:
                pass
            p = data.find(akao.MAGIC, p + 1)
    for at, s, raw in sets:
        found.append(Asset("AKAO", where, at, raw, "AKAO sample set %d: articulations %d-%d, %d KB of samples" % (
            s.id, s.first, s.first + s.count - 1, s.size // 1024), ".akao"))
    for at, s in seqs:
        try:
            text = "AKAO music (version %s, ID %d), %d tracks" % (s.version, s.id, len(s.tracks))
            parts = {}
            arts, names = akao.articulations_for(s, [("", x) for _, x, _ in sets])
            if arts and s.version != akao.V1_0:
                seq, vh, vb, info = akao.convert(s, arts)
                parts = {".seq": seq, ".vh": vh, ".vb": vb}
                text += ", %s, %d notes; instruments: %s" % (_seconds(info), info["notes"], names)
            else:
                text += "; its instruments aren't in this file (the Game music scan of the disc converts it)"
            found.append(Asset("AKAO", where, at, s.data, text, ".akao", parts))
        except Exception:
            continue
    return found


def scan_spu(dump):
    """The samples in an SPU dump (RAM + registers, ps1transfer -spu) as VAG assets."""
    import spu as spumod
    import vag as vagmod
    s = spumod.Spu(dump)
    out = []
    for r in s.regions:
        if r.kind != "sample":
            continue
        rate = 22050
        users = [v for v in s.voices if r.start <= v.start < r.end]
        if r.end - r.start < MIN_SAMPLE and not users:    # (a few blocks left over: not a sound)
            continue
        if users and users[0].pitch:
            rate = min(48000, max(4000, round(users[0].pitch * 44100 / 4096)))
        data = vagmod.vag_bytes(bytes(s.ram[r.start:r.end]), rate, "spu_%05X" % r.start)
        text = "%.2f s at %d Hz%s%s" % ((r.end - r.start) / 16 * 28 / rate, rate,
                                       " (the voice's pitch)" if users else " (rate guessed)",
                                       ", loops" if r.loops else "")
        out.append(Asset("Sample", "SPU RAM", r.start, data, text, ".vag"))
    return out


# ---------------------------------------------------------------------------------------------------- textures in use

# The GPU packets the game built in RAM for its last frames (libgpu / PsyQ: each one a tag word, its length in words
# and the address of the next one, then GP0 commands) tell which textures it draws: a textured polygon has its texture
# page and CLUT, a sprite the page of the draw mode set before it. Their UV areas, cut from VRAM with their CLUT, are the
# textures as the game shows them (whatever its files and compression).

POLY_WORDS = {False: {False: 4, True: 7}, True: {False: 6, True: 9}}        # [gouraud][textured] for a triangle
DISC_READER_MAGIC = 0x56525344      # The disc reader's mailbox (ps1-tools/discread/discserver.h) at 0x100000


def _gp0_words(words, i):
    """Words of the GP0 command at words[i] (None: not a drawing command of a packet)."""
    c = words[i] >> 24
    if 0x20 <= c <= 0x3F:
        n = 1 + (4 if c & 8 else 3) * (1 + bool(c & 4)) + ((4 if c & 8 else 3) - 1 if c & 0x10 else 0)
        return n
    if 0x40 <= c <= 0x5F:
        return 3 + (1 if c & 0x10 else 0) if not c & 8 else None     # (polylines: not counted)
    if 0x60 <= c <= 0x7F:
        return 2 + (1 if c & 4 else 0) + (1 if (c >> 3) & 3 == 0 else 0)
    if c == 0x02:
        return 3
    if 0xE1 <= c <= 0xE6 or c == 0x00:
        return 1
    return None


def _packet(words, i, n):
    """The commands of the packet whose tag is words[i] (n words): [(command, its words)], None if they don't fill it
    exactly."""
    out, j, end = [], i + 1, i + 1 + n
    while j < end:
        k = _gp0_words(words, j)
        if not k or j + k > end:
            return None
        out.append(words[j:j + k])
        j += k
    return out


def _uv_box(cmd, ws):
    """A textured command's (clut, tpage or None, u0, v0, u1, v1) (inclusive texel area)."""
    c = cmd
    if 0x20 <= c <= 0x3F:
        verts, step = (4 if c & 8 else 3), (3 if c & 0x10 else 2)
        us, vs = [], []
        clut = tpage = 0
        for v in range(verts):
            w = int(ws[2 + v * step])     # (flat: xy, uv per vertex; gouraud: color, xy, uv, the first color in the command)
            us.append(w & 0xFF)
            vs.append((w >> 8) & 0xFF)
            if v == 0:
                clut = w >> 16
            elif v == 1:
                tpage = w >> 16
        return clut, tpage, min(us), min(vs), max(us), max(vs)
    size = {1: (1, 1), 2: (8, 8), 3: (16, 16)}.get((c >> 3) & 3)
    w = int(ws[2])
    if size is None:
        size = (int(ws[3]) & 0x3FF, (int(ws[3]) >> 16) & 0x1FF)
    u, v = w & 0xFF, (w >> 8) & 0xFF
    return w >> 16, None, u, v, min(255, u + max(size[0], 1) - 1), min(255, v + max(size[1], 1) - 1)


def textures_in_use(ram, vram, progress=None):
    """The textures the GPU packets in RAM draw, cut from VRAM: [Asset] (kind "Texture": a TIM with its CLUT, and the
    same as PNG). Grouped by texture page, colors and CLUT; the areas that touch or overlap merged."""
    words = np.frombuffer(bytes(ram[:len(ram) & ~3]), "<u4")
    nw = len(words)
    lens, nxt = words >> 24, words & 0xFFFFFF
    # (from 1 word: the draw mode packets the sprites use, libgpu's DR_TPAGE (1) and DR_MODE (2): with 3 at least,
    # every sprite's texture page was unknown, the sprites left out)
    cand = np.flatnonzero((lens >= 1) & (lens <= 16) & ((nxt == 0xFFFFFF) | ((nxt < len(ram)) & ((nxt & 3) == 0))))
    cand = cand[cand + 1 + lens[cand] <= nw]
    tex = ((words[np.minimum(cand + 1, nw - 1)] >> 24) & 0xE4) == 0x24                  # Textured polygon
    sprite = (((words[np.minimum(cand + 1, nw - 1)] >> 24) & 0xE4) == 0x64)              # Textured rectangle
    mode = ((words[np.minimum(cand + 1, nw - 1)] >> 24) == 0xE1)                        # Draw mode (first)
    cand = cand[tex | sprite | mode]
    packets = {}                    # Word index -> its commands
    for n_, i in enumerate(cand):
        if progress and not n_ % 5000:
            progress(n_, len(cand))
        cmds = _packet(words, int(i), int(lens[i]))
        if not cmds:
            continue
        nx = int(nxt[i])
        if nx != 0xFFFFFF and (words[nx >> 2] >> 24) > 16:        # (the next one: a tag too)
            continue
        packets[int(i)] = cmds
    # Sprites: the draw mode before them in the drawing order (a packet's predecessor: the tag pointing at it)
    prev = {}
    ptr = np.flatnonzero(((lens <= 16) & (nxt < len(ram)) & ((nxt & 3) == 0)))
    targets = set(packets)
    for i in ptr:
        t = int(nxt[i]) >> 2
        if t in targets and t != i:
            prev.setdefault(t, int(i))

    def mode_before(i):
        for _ in range(256):
            i = prev.get(i)
            if i is None:
                return None
            for ws in reversed(packets.get(i, [])):
                if (int(ws[0]) >> 24) == 0xE1:
                    return int(ws[0]) & 0x7FF
            if i not in packets and (lens[i] > 0):    # (a tag of another kind of packet: its draw modes unknown)
                continue
        return None

    # Only the packets linked both ways (a tag points at them, they point at another tag or end the list): data that
    # happens to look like a packet doesn't, the game's packets are in their ordering table's lists
    linked = {i: c for i, c in packets.items() if i in prev}
    uses = {}                       # (page x, page y, bits, clut x, clut y) -> [[x0, y0, x1, y1, count]] in VRAM units
    for i, cmds in linked.items():
        tpage_state = None
        for ws in cmds:
            c = int(ws[0]) >> 24
            if c == 0xE1:
                tpage_state = int(ws[0]) & 0x7FF
                continue
            if not c & 4 or not (0x20 <= c <= 0x3F or 0x60 <= c <= 0x7F):
                continue
            clut, tpage, u0, v0, u1, v1 = _uv_box(c, ws)
            if tpage is None:
                tpage = tpage_state if tpage_state is not None else mode_before(i)
                if tpage is None:
                    continue
            bits = (4, 8, 16, 16)[(tpage >> 7) & 3]
            px, py = (tpage & 15) * 64, ((tpage >> 4) & 1) * 256
            cx, cy = (clut & 63) * 16, (clut >> 6) & 511
            div = {4: 4, 8: 2, 16: 1}[bits]
            x0, x1 = px + u0 // div, px + u1 // div + 1
            y0, y1 = py + v0, py + v1 + 1
            if x1 > VRAM_W or y1 > VRAM_H or (bits < 16 and cx + (16 if bits == 4 else 256) > VRAM_W):
                continue
            key = (px, py, bits) + ((cx, cy) if bits < 16 else (0, 0))
            uses.setdefault(key, []).append([x0, y0, x1, y1, 1])
    out = []
    for (px, py, bits, cx, cy), rects in uses.items():
        merged = []
        for r in sorted(rects):
            for m in merged:
                if r[0] <= m[2] and m[0] <= r[2] and r[1] <= m[3] and m[1] <= r[3]:     # Touching or overlapping
                    m[0], m[1], m[2], m[3] = min(m[0], r[0]), min(m[1], r[1]), max(m[2], r[2]), max(m[3], r[3])
                    m[4] += 1
                    break
            else:
                merged.append(list(r))
        again = True
        while again:                # (merging may join the merged ones)
            again = False
            for a in range(len(merged)):
                for b in range(a + 1, len(merged)):
                    m, r = merged[a], merged[b]
                    if r[0] <= m[2] and m[0] <= r[2] and r[1] <= m[3] and m[1] <= r[3]:
                        m[0], m[1], m[2], m[3] = min(m[0], r[0]), min(m[1], r[1]), max(m[2], r[2]), max(m[3], r[3])
                        m[4] += r[4]
                        del merged[b]
                        again = True
                        break
                if again:
                    break
        for x0, y0, x1, y1, count in merged:
            rect = (x0, y0, x1 - x0, y1 - y0)
            tim = texture_tim(vram, bits, rect, (cx, cy))
            mul = {4: 4, 8: 2, 16: 1}[bits]
            text = "%dx%d %d-bit, at %d,%d in VRAM (page %d)%s, drawn by %d primitive%s" % (
                rect[2] * mul, rect[3], bits, x0, y0, px // 64 + (py // 256) * 16,
                (", CLUT %d,%d" % (cx, cy)) if bits < 16 else "", count, "" if count == 1 else "s")
            a = Asset("Texture", "VRAM", y0 * VRAM_W + x0, tim, text, ".tim")
            a.parts = {".png": _png(texture_rgb(vram, bits, rect, (cx, cy)))}
            out.append(a)
    out.sort(key=lambda a: -a.size)
    return out


def _png(rgb):
    import io
    from PIL import Image
    b = io.BytesIO()
    Image.fromarray(rgb, "RGB").save(b, "PNG")
    return b.getvalue()


def running_program(ram):
    """What ran when the snapshot was taken: "game" (booted from its disc by the firmware), "disc reader", or ""."""
    if game_id(ram):
        return "game"
    if len(ram) > 0x100004 and u32(ram, 0x100000) == DISC_READER_MAGIC:
        return "disc reader"
    return ""


def game_id(ram):
    """The executable of the disc the firmware booted ("SLES_015.84"), or ""."""
    if len(ram) >= DISC_ID_ADRS + 32 and u32(ram, DISC_ID_ADRS) == DISC_ID_MAGIC:
        return bytes(ram[DISC_ID_ADRS + 4:DISC_ID_ADRS + 32]).split(b"\0")[0].decode("ascii", "replace")
    return ""


class Snapshot:
    """A snapshot folder (ram.bin, vram.bin, spu.bin)."""

    def __init__(self, folder):
        self.folder = folder
        self.ram = self._read("ram.bin")
        self.vram = self._read("vram.bin")
        spu = self._read("spu.bin")
        self.spu = spu if spu and len(spu) == SPU_RAM_SIZE + SPU_REGS_SIZE else None
        if self.ram is None and self.vram is None and self.spu is None:
            raise ValueError("not a snapshot folder (no ram.bin, vram.bin or spu.bin)")
        self.game = game_id(self.ram) if self.ram else ""
        self.program = running_program(self.ram) if self.ram else ""

    def _read(self, name):
        p = os.path.join(self.folder, name)
        if not os.path.isfile(p):
            return None
        with open(p, "rb") as f:
            return f.read()

    def scan(self, progress=None):
        out = []
        if self.ram and self.vram:
            out += textures_in_use(self.ram, self.vram, progress)
        if self.ram:
            out += scan_bytes(self.ram, "RAM", self.spu[:SPU_RAM_SIZE] if self.spu else None, progress)
        if self.spu:
            out += scan_spu(self.spu)
        return out


# ---------------------------------------------------------------------------------------------------- VRAM

def vram_words(vram):
    return np.frombuffer(bytes(vram[:VRAM_W * VRAM_H * 2]), "<u2").reshape(VRAM_H, VRAM_W)


def vram_rgb(words):
    """RGB array (h, w, 3) of 16-bit VRAM words."""
    r = (words & 31).astype(np.uint16) * 255 // 31
    g = ((words >> 5) & 31).astype(np.uint16) * 255 // 31
    b = ((words >> 10) & 31).astype(np.uint16) * 255 // 31
    return np.stack([r, g, b], axis=-1).astype(np.uint8)


def texture_rgb(vram, mode, rect, clut):
    """RGB array of a part of VRAM read as a texture: mode 16 (rect as it is), 8 or 4 (rect in VRAM units: each word
    holds 2 or 4 pixels) with the CLUT at clut (x, y: 256 or 16 colors)."""
    w16 = vram_words(vram)
    x, y, w, h = rect
    part = w16[y:y + h, x:x + w]
    if mode == 16:
        return vram_rgb(part)
    if mode == 8:
        idx = np.stack([part & 0xFF, part >> 8], axis=-1).reshape(h, w * 2)
        n = 256
    else:
        idx = np.stack([part & 15, (part >> 4) & 15, (part >> 8) & 15, part >> 12], axis=-1).reshape(h, w * 4)
        n = 16
    cx, cy = clut
    colors = w16[cy, cx:cx + n]
    if len(colors) < n:
        colors = np.concatenate([colors, np.zeros(n - len(colors), np.uint16)])
    return vram_rgb(colors)[idx]


def texture_tim(vram, mode, rect, clut):
    """A TIM file of a part of VRAM (rect in VRAM units) read as mode 16, 8 or 4 bits, with its CLUT for 4/8."""
    w16 = vram_words(vram)
    x, y, w, h = rect
    flags = {4: 0, 8: 1, 16: 2}[mode]
    out = struct.pack("<II", 0x10, flags | (8 if mode != 16 else 0))
    if mode != 16:
        n = 256 if mode == 8 else 16
        cx, cy = clut
        colors = w16[cy, cx:cx + n].astype("<u2").tobytes().ljust(n * 2, b"\0")
        out += struct.pack("<IHHHH", 12 + n * 2, cx, cy, n, 1) + colors
    out += struct.pack("<IHHHH", 12 + w * h * 2, x, y, w, h) + w16[y:y + h, x:x + w].astype("<u2").tobytes()
    return out


# ---------------------------------------------------------------------------------------------------- command line

def main():
    if len(sys.argv) < 2:
        print(__doc__.strip().split("\n\n")[-2])
        return 1
    src = sys.argv[1]
    if os.path.isdir(src):
        snap = Snapshot(src)
        assets = snap.scan()
        base = snap.game.replace(".", "") if snap.game else "snapshot"
        if snap.game:
            print("Game: %s" % snap.game)
    else:
        with open(src, "rb") as f:
            assets = scan_bytes(f.read(), os.path.basename(src))
        base = os.path.splitext(os.path.basename(src))[0]
    for a in assets:
        print("%-6s %-8s 0x%06X %8d  %s" % (a.kind, a.where, a.offset, a.size, a.info))
    print("%d found" % len(assets))
    if len(sys.argv) > 2:
        for a in assets:
            a.save(sys.argv[2], base)
        print("Written to %s" % sys.argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
