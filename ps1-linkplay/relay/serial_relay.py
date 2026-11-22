"""PS1 link cable relay: joins two consoles' link ports over a network, each through a USB-serial adapter (FT232R,
3.3 V) on its own PC (or both on one PC, two adapters).

Each side runs this script with its adapter's COM port; one side listens, the other connects:

  python serial_relay.py --port COM8 --listen 7100                 (the first console's PC)
  python serial_relay.py --port COM9 --connect HOST:7100           (the second console's PC)

What passes, both ways:
  - every byte the console sends (written to the other console as it comes);
  - the handshake lines, as the link cable joins them: this console's RTS (the adapter's CTS) becomes the other
    console's CTS (the other adapter's RTS); this console's DTR (the adapter's DSR) becomes the other's DSR (the
    other adapter's DTR).
Red Alert's link code needs both: RTS/CTS for the handshake and flow control, DSR for the link page.

Safety (opening or closing the port with the console running crashed Red Alert): the port is opened once, with RTS
and DTR low, and kept open until the script ends; start the script with the console off or still on its boot
screen, and stop it the same way.

--delay MS holds every message back that long (one way), to try the game with internet-like delays.
The log (relay_<port>.log): connections, line changes and byte counts each second.
"""
import argparse
import os
import queue
import socket
import struct
import threading
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
DATA, LINES = 0, 1                      # Message kinds: [kind, length (16-bit)] + payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="the adapter's COM port")
    ap.add_argument("--baud", type=int, default=2000000, help="FT232R's nearest to Red Alert's 2,073,600")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--listen", type=int, metavar="PORT")
    g.add_argument("--connect", metavar="HOST:PORT")
    ap.add_argument("--delay", type=float, default=0.0, help="ms each message is held back (one way)")
    a = ap.parse_args()
    log = open(os.path.join(HERE, "relay_%s.log" % a.port), "a", buffering=1)
    t0 = time.time()
    lock = threading.Lock()

    def say(text):
        with lock:
            log.write("%s %9.3f  %s\n" % (time.strftime("%H:%M:%S"), time.time() - t0, text))

    s = serial.Serial()
    s.port, s.baudrate, s.bytesize, s.parity, s.stopbits = a.port, a.baud, 8, "N", 2
    s.timeout, s.write_timeout, s.rtscts, s.dsrdtr = 0, 1, False, False
    s.rts, s.dtr = False, False
    s.open()
    s.rts, s.dtr = False, False
    say("opened %s at %d baud, RTS 0 DTR 0" % (a.port, a.baud))

    if a.listen:
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", a.listen))
        srv.listen(1)
        say("waiting for the other side on port %d" % a.listen)
        net, peer = srv.accept()
    else:
        host, port = a.connect.rsplit(":", 1)
        while True:
            try:
                net = socket.create_connection((host, int(port)), timeout=5)
                break
            except OSError:
                time.sleep(1)
        peer = (host, port)
    net.settimeout(None)
    net.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    say("linked with %s:%s" % peer)
    stats = {"to_net": 0, "from_net": 0}
    out = queue.Queue()

    def sender():                       # (the delay: each message sent DELAY ms after it was made)
        while True:
            due, msg = out.get()
            wait = due - time.time()
            if wait > 0:
                time.sleep(wait)
            try:
                net.sendall(msg)
            except OSError:
                return

    def send(kind, payload):
        out.put((time.time() + a.delay / 1000.0, struct.pack("<BH", kind, len(payload)) + payload))

    def receiver():                     # (the other console's bytes and lines, onto this console's port)
        buf = b""
        while True:
            try:
                chunk = net.recv(65536)
            except OSError:
                chunk = b""
            if not chunk:
                say("the other side closed")
                s.rts, s.dtr = False, False
                os._exit(0)
            buf += chunk
            while len(buf) >= 3:
                kind, n = struct.unpack_from("<BH", buf)
                if len(buf) < 3 + n:
                    break
                payload, buf = buf[3:3 + n], buf[3 + n:]
                if kind == DATA:
                    s.write(payload)
                    stats["from_net"] += len(payload)
                elif kind == LINES:
                    rts, dtr = payload[0] & 1, payload[0] >> 1 & 1
                    s.rts, s.dtr = bool(rts), bool(dtr)
                    say("other console: RTS %d DTR %d" % (rts, dtr))

    threading.Thread(target=sender, daemon=True).start()
    threading.Thread(target=receiver, daemon=True).start()
    last_lines, last_report = None, time.time()
    try:
        while True:
            lines = (s.cts, s.dsr)              # (this console's RTS and DTR, as the adapter sees them)
            if lines != last_lines:
                send(LINES, bytes([int(lines[0]) | int(lines[1]) << 1]))
                say("this console: RTS %d DTR %d" % lines)
                last_lines = lines
            n = s.in_waiting
            if n:
                data = s.read(n)
                send(DATA, data)
                stats["to_net"] += len(data)
            if time.time() - last_report >= 1 and (stats["to_net"] or stats["from_net"]):
                say("bytes: sent %(to_net)d, received %(from_net)d" % stats)
                stats["to_net"] = stats["from_net"] = 0
                last_report = time.time()
            time.sleep(0.0001)
    except KeyboardInterrupt:
        pass
    finally:
        s.rts, s.dtr = False, False
        say("stopped")


if __name__ == "__main__":
    main()
