"""PS1 DEV CART synth SysEx: samples and slot settings sent over MIDI (from a sequencer, a SysEx librarian, ...).

Every message is F0 7D 50 53 <command> ... F7 (7D: non-commercial manufacturer ID, 50 53: "PS"). All the values
are 7-bit; numbers are little endian, 7 bits per byte.

    01 SAMPLE START  slot(0-15) rate(3 bytes) size(4 bytes) name(16 bytes, ASCII)
    02 SAMPLE DATA   up to 128 bytes of SPU ADPCM (the VAG data without its header), 7 bytes packed in 8:
                     a byte with the high bits of the next 7 (bit 0 = first byte), then their low 7 bits
    03 SAMPLE END    checksum(2 bytes): sum of the ADPCM bytes & 0x3FFF
    04 SET           slot param value (param: synth.h order, root volume pan attack decay sustain release reverb tune)
    05 CLEAR         slot

A sample is loaded once its SAMPLE END arrives with the right checksum.
"""

import struct

HEADER = bytes((0xF0, 0x7D, 0x50, 0x53))
START, DATA, END, SET, CLEAR = 1, 2, 3, 4, 5
CHUNK = 128
NAME_LEN = 16


def _num(value, count):
    return bytes((value >> (7 * i)) & 0x7F for i in range(count))


def _unnum(data):
    return sum(b << (7 * i) for i, b in enumerate(data))


def pack7(data):
    out = bytearray()
    for i in range(0, len(data), 7):
        group = data[i:i + 7]
        out.append(sum(((b >> 7) & 1) << j for j, b in enumerate(group)))
        out += bytes(b & 0x7F for b in group)
    return bytes(out)


def unpack7(data):
    out = bytearray()
    for i in range(0, len(data), 8):
        high = data[i]
        out += bytes(b | (((high >> j) & 1) << 7) for j, b in enumerate(data[i + 1:i + 8]))
    return bytes(out)


def message(command, payload=b""):
    return HEADER + bytes((command,)) + bytes(payload) + b"\xF7"


def sample_messages(slot, adpcm, rate, name=""):
    """SysEx messages that load ADPCM data (VAG without its header) into a slot (0-15)."""
    name_bytes = bytes(c & 0x7F for c in name.encode("ascii", "replace")[:NAME_LEN]).ljust(NAME_LEN, b"\0")
    msgs = [message(START, bytes((slot & 15,)) + _num(rate, 3) + _num(len(adpcm), 4) + name_bytes)]
    for i in range(0, len(adpcm), CHUNK):
        msgs.append(message(DATA, pack7(adpcm[i:i + CHUNK])))
    msgs.append(message(END, _num(sum(adpcm) & 0x3FFF, 2)))
    return msgs


def vag_to_syx(vag, slot):
    """A .syx file (the messages one after the other) for a VAG file's bytes."""
    size = struct.unpack(">I", vag[12:16])[0] or len(vag) - 48
    rate = struct.unpack(">I", vag[16:20])[0] or 44100
    name = vag[32:48].split(b"\0")[0].decode("ascii", "replace")
    return b"".join(sample_messages(slot, vag[48:48 + size], rate, name))


def split_syx(data):
    """The messages of a .syx file."""
    msgs, start = [], None
    for i, b in enumerate(data):
        if b == 0xF0:
            start = i
        elif b == 0xF7 and start is not None:
            msgs.append(bytes(data[start:i + 1]))
            start = None
    return msgs


class Receiver:
    """Assembles the samples. feed() takes a complete SysEx message (with F0 and F7) and returns:
    ("load", slot, adpcm, rate, name), ("set", slot, param, value), ("clear", slot), ("error", text) or None
    (not one of ours, or a sample in progress)."""

    def __init__(self):
        self.sample = None

    def feed(self, msg):
        msg = bytes(msg)
        if len(msg) < 6 or not msg.startswith(HEADER) or msg[-1] != 0xF7:
            return None
        command, payload = msg[4], msg[5:-1]
        if command == START and len(payload) >= 8 + NAME_LEN:
            name = payload[8:8 + NAME_LEN].split(b"\0")[0].decode("ascii", "replace")
            self.sample = {"slot": payload[0] & 15, "rate": _unnum(payload[1:4]), "size": _unnum(payload[4:8]),
                           "name": name, "data": bytearray()}
            return None
        if command == DATA and self.sample:
            self.sample["data"] += unpack7(payload)
            return None
        if command == END and self.sample:
            s, self.sample = self.sample, None
            data = bytes(s["data"][:s["size"]])
            if len(data) != s["size"] or (sum(data) & 0x3FFF) != _unnum(payload[:2]):
                return ("error", "SysEx sample for slot %d: bad size or checksum" % (s["slot"] + 1))
            return ("load", s["slot"], data, s["rate"], s["name"])
        if command == SET and len(payload) >= 3:
            return ("set", payload[0] & 15, payload[1], payload[2])
        if command == CLEAR and payload:
            return ("clear", payload[0] & 15)
        return None
