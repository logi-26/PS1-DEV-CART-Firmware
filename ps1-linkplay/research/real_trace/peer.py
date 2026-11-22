"""The PC as the other console on Red Alert Retaliation's link game setup screen (role 0), through a USB-serial adapter
(FT232R, 3.3 V) on the console's link port.

The setup screen's code (loaded from DATA.MIX, 0x80019040-0x80019330 in RAM) each frame:
  1. role 1 sends a 4-byte check word (AF BE ..); role 0 reads it and compares it with its own (a difference: back to
     the menu);
  2. once the screen has faded in, role 1 also sends 4 + 728 + 4 + 4 bytes (the game settings); role 0 reads them;
  3. role 0 sends two 4-byte values (its own settings); role 1 reads them.
A console sends only while the other's RTS (its CTS) is up; it raises its RTS while it waits to read.

What the PC does:
  - while the console waits ("waiting to connect": its RTS up), raise RTS: the console sees the other console and is
    role 1;
  - at the console's first bytes, raise DTR, and keep RTS up (the console may send at any time);
  - read and log everything (the 4-byte words; blocks of the setup's size);
  - each time the console raises its RTS after sending (it waits to read), send REPLY (8 bytes).
At the end, or when the console stops, DTR and RTS are lowered (the menu shows LINK GAME only with DSR low).

  python peer.py [--port COM8] [--reply 0000000000000000] [--seconds 300]
"""
import argparse, os, time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--baud", type=int, default=2000000)
    ap.add_argument("--reply", default="0000000000000000", help="the 8 bytes sent when the console waits to read")
    ap.add_argument("--seconds", type=float, default=300)
    ap.add_argument("--armed", action="store_true", help="play the other console from the start (else: write 'arm' "
                                                         "into peer_ctl.txt)")
    a = ap.parse_args()
    reply = bytes.fromhex(a.reply)
    log = open(os.path.join(HERE, "peer.log"), "w", buffering=1)
    t0 = time.time()

    def say(text):
        log.write("%9.3f s  %s\n" % (time.time() - t0, text))

    s = serial.Serial()
    s.port, s.baudrate, s.bytesize, s.parity, s.stopbits = a.port, a.baud, 8, "N", 2
    s.timeout, s.rtscts, s.dsrdtr = 0, False, False
    s.rts, s.dtr = False, False
    s.open()
    s.rts, s.dtr = False, False
    say("open %s at %d baud; PC RTS 0, DTR 0; reply %s" % (a.port, a.baud, reply.hex(" ")))
    # (the port is opened once and kept open: opening or closing it can glitch the lines, which crashed the game)
    ctl = os.path.join(HERE, "peer_ctl.txt")
    with open(ctl, "w") as f:
        f.write("")
    armed, ctl_seen, ctl_check = a.armed, "", 0
    state = "menu"              # menu -> waiting (console RTS up) -> linked (PC RTS up) -> talking (bytes came)
    last_cts, got, chunk, chunk_t, replies = s.cts, 0, bytearray(), 0, 0
    say("console RTS (PC CTS) %d" % last_cts)
    try:
        while time.time() - t0 < a.seconds:
            cts = s.cts
            if cts != last_cts:
                say("console RTS -> %d" % cts)
                if state == "talking" and cts and not s.in_waiting:
                    s.write(reply)              # (it waits to read: our two values)
                    s.flush()
                    replies += 1
                    if replies <= 5 or replies % 50 == 0:
                        say("sent reply %d: %s" % (replies, reply.hex(" ")))
                last_cts = cts
            if time.time() - ctl_check > 0.2:      # (commands: arm, disarm, reply HEX: one at a time)
                ctl_check = time.time()
                try:
                    with open(ctl) as f:
                        c = f.read().strip()
                except OSError:
                    c = ""
                if c and c != ctl_seen:
                    ctl_seen = c
                    if c == "arm":
                        armed, state = True, "menu"
                        say("armed: the PC answers as the other console when the console waits")
                    elif c == "disarm":
                        armed, state = False, "menu"
                        s.rts, s.dtr = False, False
                        say("disarmed: PC RTS 0, DTR 0 (listening only)")
                    elif c.startswith("reply "):
                        reply = bytes.fromhex(c[6:])
                        say("reply now %s" % reply.hex(" "))
            if not armed:
                state = "menu"
            if armed and state == "menu" and cts:
                state = "waiting"
                say("the console waits for the other console")
            if state == "waiting":
                time.sleep(0.5)                 # (as a person would arrive: then the PC's RTS up)
                s.rts = True
                state = "linked"
                say("PC RTS -> 1 (the other console is here)")
            n = s.in_waiting
            if n:
                data = s.read(n)
                got += len(data)
                if state == "linked":
                    s.dtr = True
                    state = "talking"
                    say("first bytes: PC DTR -> 1 (RTS stays up)")
                if not chunk:
                    chunk_t = time.time()
                chunk += data
            # (a pause of 2 ms ends a chunk: logged as one line)
            if chunk and time.time() - chunk_t > 0.002 and not s.in_waiting:
                c = bytes(chunk)
                say("received %d: %s" % (len(c), c[:24].hex(" ") + (" ... " + c[-8:].hex(" ") if len(c) > 24 else "")))
                chunk = bytearray()
            time.sleep(0.0002)
    finally:
        s.rts, s.dtr = False, False
        say("done: %d bytes received, %d replies; PC RTS 0, DTR 0" % (got, replies))
        s.close()


if __name__ == "__main__":
    main()
