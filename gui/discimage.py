"""
PS1 DEV CART GUI: a game's disc image (or any of its files) read where needed, never whole: a disc image is hundreds
of MB, and reading it all (twice, for its sectors' data) took the computer's memory. Used by the music scans (sshd.py,
aad.py).

The image is seen as its data track's sectors' 2048 bytes of data one after the other (a file's bytes follow each
other then):
- .bin / .img: 2352-byte sectors (each with its sync, header and error correction; Mode 2 or Mode 1).
- .iso: 2048-byte sectors (the data alone): read as it is. Any other file is read as it is too.
- .cue: its first data track (MODE1/2048, MODE1/2352, MODE2/2336 or MODE2/2352), in a .bin of every track or of its
  own; a .bin missing beside its .ecm is read from the .ecm.
- .ecm (ECM, Neill Corlett's: a .bin with the sectors' sync, headers and error correction left out where they can
  be made again): decoded where needed through an index of its records made when it is opened. The error correction
  isn't made again (only the sectors' data is read).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import array
import bisect
import os
import re

SECTOR_SYNC = b"\x00" + b"\xFF" * 10 + b"\x00"
RAW, DATA = 2352, 2048
FIND_CHUNK = 8 << 20            # find() reads this much at a time
ECM_MAGIC = b"ECM\x00"


class FormatError(ValueError):
    pass


# ---------------------------------------------------------------------------------------------------- sources of bytes

class _Bytes:
    def __init__(self, data):
        self.data, self.size = bytes(data), len(data)

    def read(self, at, n):
        return self.data[at:at + n]

    def close(self):
        pass


class _File:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        self.size = self.f.tell()

    def read(self, at, n):
        self.f.seek(at)
        return self.f.read(n)

    def close(self):
        self.f.close()


class _Window:
    """Part of another source (a .cue's track in a .bin of every track)."""

    def __init__(self, src, start, size):
        self.src, self.start = src, start
        self.size = max(0, min(size, src.size - start))

    def read(self, at, n):
        n = max(0, min(n, self.size - at))
        return self.src.read(self.start + at, n)

    def close(self):
        self.src.close()


class _Ecm:
    """An ECM file's decoded bytes. Records: a type (0: bytes as they are; 1: Mode 1 sectors, stored as their 3 address
    bytes and 2048 data bytes, 2352 decoded; 2: Mode 2 Form 1 sectors from their subheader, stored as 4 subheader bytes
    and 2048 data bytes, 2336 decoded; 3: Mode 2 Form 2, stored as 4 + 2324 bytes, 2336 decoded) and a count; the
    sectors' sync and header bytes of Mode 2 are bytes as they are."""

    STORED = {1: 3 + 2048, 2: 4 + 2048, 3: 4 + 2324}
    DECODED = {1: 2352, 2: 2336, 3: 2336}

    def __init__(self, src):
        self.src = src
        if src.read(0, 4) != ECM_MAGIC:
            raise FormatError("not an ECM file")
        self.out = array.array("q")             # Each record: where its bytes start decoded, where they are stored,
        self.at = array.array("q")              # its type and count
        self.kind = array.array("b")
        self.count = array.array("q")
        pos, out, buf, buf_at = 4, 0, b"", 4
        while True:
            if pos + 5 > buf_at + len(buf):                     # (read ahead: the record headers in big reads)
                buf, buf_at = src.read(pos, 1 << 16), pos
            if pos >= buf_at + len(buf):
                raise FormatError("ECM file ends without its end record")
            c = buf[pos - buf_at]
            pos += 1
            kind, num, bits = c & 3, (c >> 2) & 0x1F, 5
            while c & 0x80:
                if pos >= buf_at + len(buf):
                    buf, buf_at = src.read(pos, 1 << 16), pos
                    if not buf:
                        raise FormatError("ECM file ends in a record header")
                c = buf[pos - buf_at]
                pos += 1
                if bits > 31:
                    raise FormatError("bad ECM record header")
                num |= (c & 0x7F) << bits
                bits += 7
            if num == 0xFFFFFFFF:
                break
            num += 1
            if num >= 0x80000000:
                raise FormatError("bad ECM record count")
            stored = num if kind == 0 else num * self.STORED[kind]
            if pos + stored > src.size:
                raise FormatError("ECM file ends in a record")
            self.out.append(out)
            self.at.append(pos)
            self.kind.append(kind)
            self.count.append(num)
            pos += stored
            out += num if kind == 0 else num * self.DECODED[kind]
        self.size = out

    def _sector(self, kind, stored):
        if kind == 1:
            return SECTOR_SYNC + stored[:3] + b"\x01" + stored[3:] + bytes(288)
        sub = stored[:4]
        return sub + sub + stored[4:] + bytes(2336 - 8 - len(stored) + 4)

    def _span(self, r, start, end):
        """Record r's decoded bytes start..end: (where the stored bytes for them start and end, the first sector
        stored there (records of sectors))."""
        kind = self.kind[r]
        if kind == 0:
            return self.at[r] + start, self.at[r] + end, 0
        size, stored = self.DECODED[kind], self.STORED[kind]
        first, last = start // size, -(-end // size)
        return self.at[r] + first * stored, self.at[r] + last * stored, first

    def read(self, at, n):
        n = max(0, min(n, self.size - at))
        if not n:
            return b""
        end = at + n
        r0 = bisect.bisect_right(self.out, at) - 1
        r1 = bisect.bisect_right(self.out, end - 1) - 1
        parts = []                          # Each record's part: (record, its decoded start and end)
        for r in range(r0, r1 + 1):
            o = self.out[r]
            parts.append((r, max(at, o) - o, min(end, self.out[r + 1] if r + 1 < len(self.out) else self.size) - o))
        lo = self._span(parts[0][0], parts[0][1], parts[0][2])[0]
        hi = self._span(parts[-1][0], parts[-1][1], parts[-1][2])[1]
        blob = self.src.read(lo, hi - lo)  # (the records' stored bytes follow each other: one read)
        out = bytearray()
        for r, start, stop in parts:
            a, b, first = self._span(r, start, stop)
            if self.kind[r] == 0:
                out += blob[a - lo:b - lo]
                continue
            kind = self.kind[r]
            size, stored = self.DECODED[kind], self.STORED[kind]
            sectors = b"".join(self._sector(kind, blob[p - lo:p - lo + stored]) for p in range(a, b, stored))
            out += sectors[start - first * size:stop - first * size]
        return bytes(out)

    def close(self):
        self.src.close()


def _open_file(path):
    """A file's bytes (an ECM file's decoded; a missing .bin read from the .ecm beside it)."""
    if not os.path.exists(path) and os.path.exists(path + ".ecm"):
        path += ".ecm"
    src = _File(path)
    if src.read(0, 4) == ECM_MAGIC:
        try:
            return _Ecm(src)
        except Exception:
            src.close()
            raise
    return src


# ---------------------------------------------------------------------------------------------------- .cue sheets

CUE_SIZES = {"MODE1/2048": 2048, "MODE1/2352": 2352, "MODE2/2336": 2336, "MODE2/2352": 2352, "AUDIO": 2352,
             "CDG": 2448, "MODE2/2048": 2048}


def cue_tracks(path):
    """A .cue sheet's tracks: [dict(number, mode, file, size (bytes a sector), first (its first INDEX: where it
    starts in its file, its pregap's INDEX 00 if it has one), start (its INDEX 01), in sectors)]."""
    with open(path, encoding="latin-1") as f:
        lines = f.read().splitlines()
    tracks, file = [], None
    for line in lines:
        words = line.strip()
        m = re.match(r'FILE\s+(?:"([^"]+)"|(\S+))', words, re.I)
        if m:
            file = os.path.join(os.path.dirname(os.path.abspath(path)), m.group(1) or m.group(2))
            continue
        m = re.match(r"TRACK\s+(\d+)\s+(\S+)", words, re.I)
        if m:
            mode = m.group(2).upper()
            tracks.append(dict(number=int(m.group(1)), mode=mode, file=file, size=CUE_SIZES.get(mode, 2352),
                               first=None, start=None))
            continue
        m = re.match(r"INDEX\s+(\d+)\s+(\d+):(\d+):(\d+)", words, re.I)
        if m and tracks:
            at = (int(m.group(2)) * 60 + int(m.group(3))) * 75 + int(m.group(4))
            if tracks[-1]["first"] is None:
                tracks[-1]["first"] = at
            if int(m.group(1)) == 1 or tracks[-1]["start"] is None:
                tracks[-1]["start"] = at
    if not tracks or any(t["file"] is None or t["start"] is None for t in tracks):
        raise FormatError("%s: no tracks" % os.path.basename(path))
    return tracks


def _open_cue(path):
    """The first data track of a .cue sheet: (its bytes, its sector size)."""
    tracks = cue_tracks(path)
    data = next((t for t in tracks if t["mode"].startswith("MODE")), None)
    if data is None:
        raise FormatError("%s: no data track (an audio CD)" % os.path.basename(path))
    # Where it starts (its INDEX 01) and ends (the next track's first index) in its file: each track of the file from
    # its first index, in sectors of its size
    same = [t for t in tracks if t["file"] == data["file"]]
    byte, prev, start, ends = 0, None, 0, None
    for t in same:
        if prev is not None:
            byte += (t["first"] - prev["first"]) * prev["size"]
        if t is data:
            start = byte + (t["start"] - t["first"]) * t["size"]
        elif prev is data:
            ends = byte
        prev = t
    src = _open_file(data["file"])
    return _Window(src, start, (ends if ends is not None else src.size) - start), data["size"]


# ---------------------------------------------------------------------------------------------------- the image

class Image:
    """source: a path, or bytes (the tests). len(img), img[a:b] (bytes), img.read(at, n), img.find(magic)."""

    def __init__(self, source):
        size = None
        if isinstance(source, (bytes, bytearray, memoryview)):
            self.src = _Bytes(source)
            if self.src.read(0, 4) == ECM_MAGIC:
                self.src = _Ecm(self.src)
        elif str(source).lower().endswith(".cue"):
            self.src, size = _open_cue(source)
        else:
            self.src = _open_file(source)
        total = self.src.size
        # Sectors: 2352 bytes (sync, header: Mode 2's data after its subheader, Mode 1's after the header), 2336
        # (Mode 2 without sync and header: from a .cue) or 2048 (data alone)
        self.sector, self.data_off = None, 0
        if size in (None, 2352) and total >= RAW and self._raw(0, 12) == SECTOR_SYNC and \
                (total < RAW * 2 or self._raw(RAW, 12) == SECTOR_SYNC):
            self.sector = RAW
            self.data_off = 16 if self._raw(15, 1) == b"\x01" else 24
        elif size == 2336:
            self.sector, self.data_off = 2336, 8
        self.raw = self.sector is not None
        self.size = total // self.sector * DATA if self.raw else total

    def _raw(self, at, n):
        return self.src.read(at, n)

    def read(self, at, n):
        """n bytes of the data from at (fewer at the end)."""
        at = max(0, at)
        n = max(0, min(n, self.size - at))
        if not self.raw:
            return self._raw(at, n)
        sz, do = self.sector, self.data_off
        out = bytearray()
        while len(out) < n:
            sector, offset = divmod(at + len(out), DATA)
            part = min(DATA - offset, n - len(out))
            if offset == 0 and part == DATA:                    # (whole sectors: read together)
                count = min((n - len(out)) // DATA, 512)
                raw = self._raw(sector * sz, count * sz)
                for i in range(count):
                    out += raw[i * sz + do:i * sz + do + DATA]
                continue
            out += self._raw(sector * sz + do + offset, part)
        return bytes(out)

    def __len__(self):
        return self.size

    def __getitem__(self, key):
        if isinstance(key, slice):
            start, stop, step = key.indices(self.size)
            if step != 1:
                raise ValueError("Image slices have a step of 1")
            return self.read(start, max(0, stop - start))
        if not 0 <= key < self.size:
            raise IndexError(key)
        return self.read(key, 1)[0]

    def find(self, magic, start=0, stop=None):
        """The positions of magic in the data (from start to stop), in order (read FIND_CHUNK bytes at a time)."""
        stop = self.size if stop is None else min(stop, self.size)
        at, keep = start, len(magic) - 1
        while at < stop:
            chunk = self.read(at, min(FIND_CHUNK, stop - at) + keep)
            p = chunk.find(magic)
            while p >= 0:
                if p < FIND_CHUNK and at + p + len(magic) <= stop:     # (the overlap's matches: in the next chunk)
                    yield at + p
                p = chunk.find(magic, p + 1)
            at += FIND_CHUNK

    def close(self):
        if self.src:
            self.src.close()
            self.src = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
