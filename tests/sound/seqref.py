"""
Reference SEQ / SEP player for the sound tests: what the console should play, written from Sony's File Formats and
Run-Time Library Overview (not from the tester's code), in plain Python with exact times (no frames).

SEQ: "SEQp" (or "pQES"), version 1 (32-bit, or 16-bit in some tools' files), resolution (16-bit), tempo (24-bit, us
per quarter note), rhythm (2 bytes), then MIDI events: delta time (variable length), status (running status, also
after a meta event: Sony's meta events have no length byte: FF 51 tt tt tt, FF 2F 00 the end). SEP: version 0
(16-bit), then SEQs: number (16-bit), resolution, tempo, rhythm, size (32-bit), data.
Loops: NRPN 99 = 20 a loop start, the count in the next data entry (CC 6, or CC 98 in converted files): 127 endless,
n: the loop end jumps back n times (no count: read on); NRPN 99 = 30 the loop end. A jump goes back to just after the
loop start (its count isn't read again), with the running status there; the tempo stays as it is.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""


class SeqError(Exception):
    pass


def be(b):
    return int.from_bytes(b, "big")


def tracks(d):
    """[(resolution, tempo, data)] of a SEQ or SEP file."""
    if len(d) < 16 or d[:4] not in (b"SEQp", b"pQES"):
        raise SeqError("not a SEQ")
    if be(d[4:8]) == 1:
        return [(be(d[8:10]), be(d[10:13]), d[15:])]
    if be(d[4:6]) == 1:
        return [(be(d[6:8]), be(d[8:11]), d[13:])]
    out, p = [], 6
    while p + 13 <= len(d) and len(out) < 16:
        size = be(d[p + 9:p + 13])
        out.append((be(d[p + 2:p + 4]), be(d[p + 4:p + 7]), d[p + 13:p + 13 + size]))
        p += 13 + size
    return out


def events(data):
    """The events in file order: [(delta ticks, kind, args)]. kinds: note (ch, note, vel), off (ch, note), cc (ch, n,
    v), prog (ch, p), bend (ch, value -8192..8191), tempo (us), loopstart, loopcount (n), loopend, end. The data's end
    without FF 2F: an end there."""
    out, p, status, pending = [], 0, 0, False

    def varlen():
        nonlocal p
        v = 0
        for _ in range(4):
            if p >= len(data):
                raise IndexError
            b = data[p]
            p += 1
            v = (v << 7) | (b & 0x7F)
            if not b & 0x80:
                return v
        return v

    def byte():
        nonlocal p
        if p >= len(data):
            raise IndexError
        p += 1
        return data[p - 1]

    try:
        while True:
            delta = varlen()
            s = data[p] if p < len(data) else None
            if s is None:
                raise IndexError
            if s & 0x80:
                p += 1
            elif status:
                s = status
            else:
                raise SeqError("data without a status at byte %d" % p)
            if s == 0xFF:
                status = 0xFF
                kind = byte()
                if kind == 0x2F:
                    out.append((delta, "end", ()))
                    return out
                if kind == 0x51:
                    t = (byte() << 16) | (byte() << 8) | byte()
                    out.append((delta, "tempo", (t or 500000,)))
                else:
                    n = varlen()
                    p += n
                    out.append((delta, "meta", (kind,)))
                continue
            status = s
            ch, hi = s & 15, s & 0xF0
            if hi == 0x90:
                n, v = byte(), byte()
                out.append((delta, "note" if v else "off", (ch, n, v) if v else (ch, n)))
            elif hi == 0x80:
                n, _ = byte(), byte()
                out.append((delta, "off", (ch, n)))
            elif hi == 0xB0:
                n, v = byte(), byte()
                if n == 99 and v == 20:
                    out.append((delta, "loopstart", ()))
                    pending = True
                elif n in (6, 98) and pending:
                    out.append((delta, "loopcount", (v,)))
                    pending = False
                elif n == 99 and v == 30:
                    out.append((delta, "loopend", ()))
                    pending = False
                else:
                    out.append((delta, "cc", (ch, n, v)))
            elif hi == 0xC0:
                out.append((delta, "prog", (ch, byte())))
            elif hi == 0xE0:
                lo, hi7 = byte(), byte()
                out.append((delta, "bend", (ch, ((hi7 << 7) | lo) - 8192)))
            elif hi == 0xA0:
                byte(), byte()
                out.append((delta, "other", ()))
            elif hi == 0xD0:
                byte()
                out.append((delta, "other", ()))
            else:
                out.append((delta, "end", ()))      # (an unknown status: the music ends there)
                return out
    except IndexError:
        out.append((0, "end", ()))                  # (cut short)
        return out


def _us(ticks, tempo, res):
    return ticks * tempo / res


def length_us(res, tempo, evs):
    """The music's length read straight through (loops once), us."""
    res, tempo, t = res or 480, tempo or 500000, 0.0
    for delta, kind, args in evs:
        t += _us(delta, tempo, res)
        if kind == "tempo":
            tempo = args[0]
        if kind == "end":
            break
    return t


def tick_us(res, tempo, evs):
    """A tick at the music's slowest tempo (us): how close times are."""
    return max([tempo or 500000] + [a[0] for d, k, a in evs if k == "tempo"]) / (res or 480)


def channels_used(evs):
    return {args[0] for d, kind, args in evs if kind == "note"}


def new_state():
    return {c: dict(prog=0, volume=127, expression=127, pan=64, bend=0, sustain=0) for c in range(16)}


def apply(st, kind, args):
    """A channel event on the channels' state."""
    if kind == "prog":
        st[args[0]]["prog"] = args[1]
    elif kind == "bend":
        st[args[0]]["bend"] = args[1]
    elif kind == "cc":
        c, n, v = args
        key = {7: "volume", 11: "expression", 10: "pan"}.get(n)
        if key:
            st[c][key] = v
        elif n == 64:
            st[c]["sustain"] = 1 if v >= 64 else 0


def play(res, tempo, evs, horizon_us, loops=True, start_us=None):
    """Notes played, as [(us from the start, ch, note, vel, channel state dict)], up to horizon_us; and the end time
    (None: still playing at the horizon). loops: the loops followed (False: read straight through, as a seek reads).
    start_us: from there (a seek: the notes before it not played, the state set as they go; the notes at it played)."""
    res, tempo = res or 480, tempo or 500000
    notes, t, i, st = [], 0.0, 0, new_state()
    loop_i, count, guard = None, -1, 0
    while i < len(evs):
        delta, kind, args = evs[i]
        t += _us(delta, tempo, res)
        if t > horizon_us:
            return notes, None
        guard += 1
        if guard > 5000000:
            raise SeqError("more than 5 million events before the horizon")
        apply(st, kind, args)
        if kind == "note":
            # (a note at the time sought: its time added up in floating point can come just short of it, Ape Escape's
            # Coral Cave at 102.5 s: 102499999.99999991 us)
            if start_us is None or t >= start_us - 0.001:
                notes.append((t, args[0], args[1], args[2], dict(st[args[0]])))
        elif kind == "tempo":
            tempo = args[0]
        elif kind == "loopstart":
            loop_i, count = i, -1
        elif kind == "loopcount":
            count = args[0]
        elif kind == "loopend" and loops and loop_i is not None and (count == 127 or count > 0):
            if count != 127:
                count -= 1
            i = loop_i + 1
            if i < len(evs) and evs[i][1] == "loopcount":    # (not read again: its delta time only)
                t += _us(evs[i][0], tempo, res)
                i += 1
            continue
        elif kind == "end":
            return notes, t
        i += 1
    return notes, t


def state_at(res, tempo, evs, at_us):
    """The channels' state after the events before at_us, read straight through as a seek reads (the events at at_us
    are played after it): {ch: dict prog, volume, expression, pan, bend, sustain}."""
    res, tempo, t = res or 480, tempo or 500000, 0.0
    st = new_state()
    for delta, kind, args in evs:
        t += _us(delta, tempo, res)
        if t >= at_us or kind == "end":
            break
        if kind == "tempo":
            tempo = args[0]
        apply(st, kind, args)
    return st
