"""Listen to a console's link port through a USB-serial adapter (FT232R, 3.3 V) and log what it does.

The port is opened with RTS and DTR low (nothing asserted towards the console). Logged with the time: every change of
the lines the console drives (CTS, DSR, CD, RI: which ones depends on the wiring), every byte received. The PC's own
RTS / DTR can be changed while it runs: write e.g. "rts 1" or "dtr 0" into lines.txt next to this script.

  python serial_listen.py [--port COM8] [--baud 2000000] [--seconds 300]
"""
import argparse, os, time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
CONTROL = os.path.join(HERE, "lines.txt")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--baud", type=int, default=2000000)       # (FT232R: nearest to the game's 2,073,600)
    ap.add_argument("--seconds", type=float, default=300)
    a = ap.parse_args()
    log = open(os.path.join(HERE, "serial_listen.log"), "w", buffering=1)
    t0 = time.time()

    def say(text):
        log.write("%9.3f s  %s\n" % (time.time() - t0, text))

    s = serial.Serial()
    s.port, s.baudrate, s.bytesize, s.parity, s.stopbits = a.port, a.baud, 8, "N", 2
    s.timeout, s.rtscts, s.dsrdtr = 0, False, False
    s.rts, s.dtr = False, False                 # (before open: held low from the start)
    s.open()
    s.rts, s.dtr = False, False
    with open(CONTROL, "w") as f:
        f.write("")
    say("open %s at %d baud, 8N2; PC RTS 0, DTR 0" % (a.port, a.baud))
    last, control_seen, total, auto = None, "", 0, b""
    lines_reply, rts_down_at = False, 0
    while time.time() - t0 < a.seconds:
        lines = (s.cts, s.dsr, s.cd, s.ri)
        if lines != last:
            say("console lines: CTS %d DSR %d CD %d RI %d" % lines)
            last = lines
        n = s.in_waiting
        if n:
            data = s.read(n)
            total += len(data)
            say("received %d: %s" % (len(data), data[:64].hex(" ") + (" ..." if len(data) > 64 else "")))
            # (auto reply: sent at once when the console's message starts with AF BE, as a console would answer)
            if auto and data[:2] == b"\xaf\xbe":
                s.write(auto)
                s.flush()
                say("auto reply sent %d: %s" % (len(auto), auto.hex(" ")))
            # (line reply, as the emulated other console did: DTR up at once, RTS down 160 ms later)
            if lines_reply and data[:2] == b"\xaf\xbe":
                s.dtr = True
                say("line reply: PC DTR -> 1")
                rts_down_at = time.time() + 0.16
        if rts_down_at and time.time() >= rts_down_at:
            s.rts = False
            rts_down_at = 0
            say("line reply: PC RTS -> 0")
        try:
            with open(CONTROL) as f:
                c = f.read().strip()
        except OSError:
            c = ""
        if c and c != control_seen:
            control_seen = c
            for cmd in c.split(";"):
                parts = cmd.split()
                if len(parts) == 2 and parts[0] in ("rts", "dtr"):
                    setattr(s, parts[0], parts[1] == "1")
                    say("PC %s -> %s" % (parts[0].upper(), parts[1]))
                elif len(parts) == 2 and parts[0] == "linereply":
                    lines_reply = parts[1] == "1"
                    say("line reply: %s" % ("on" if lines_reply else "off"))
                elif len(parts) == 2 and parts[0] == "auto":
                    auto = bytes.fromhex(parts[1]) if parts[1] != "off" else b""
                    say("auto reply: %s" % (auto.hex(" ") or "off"))
                elif len(parts) == 2 and parts[0] == "send":
                    # (flow control: the console takes bytes while its RTS, our CTS, is up: waited for, 10 s at most)
                    data = bytes.fromhex(parts[1])
                    w0 = time.time()
                    while not s.cts and time.time() - w0 < 10:
                        n = s.in_waiting
                        if n:
                            say("received %d: %s" % (n, s.read(n).hex(" ")))
                        time.sleep(0.0001)
                    s.write(data)
                    s.flush()
                    say("sent %d: %s (CTS %d, waited %.3f s)" % (len(data), data.hex(" "), s.cts, time.time() - w0))
        time.sleep(0.0005)
    say("done: %d bytes received" % total)
    s.close()


if __name__ == "__main__":
    main()
