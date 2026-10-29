#!/usr/bin/env python3
"""
PS1 SLI hub: two, three or more PlayStations (and PCSX-Redux) drawing one scene across their screens.

    python sli_hub.py --screens 3 [--port 7040]          (run.bat starts it, with the consoles' bridges)

Each screen connects through its own bridge (a console: "ps1transfer -n CART -net 127.0.0.1:7040"; PCSX-Redux: its
serial port bridged by redux.ps1). Once all of them have said hello, the hub gives each one its place (the one its
script asked for, else the next free one) and starts them together. Then, frame after frame, it takes each screen's
buttons for frame N and, once it has all of them, sends every screen frame N with all the buttons together: they
all simulate the same world (lockstep), the hub keeping the time. Each screen also sends a check of its world: if
they ever differ, the hub starts them all again. A screen that goes away stops the others until it is back.

Packets (as sli.c): sync 0xA5, frame (2), buttons (2), world check (1), 2 more bytes, CRC-16 (CCITT) of the first 8.
Frame 0xFFFF: hello (buttons: the screen's id, check: the place asked for, 0xFF any; from the hub: paused by "freeze");
0xFFFD: start (to a screen: buttons = screens << 8 | place).

Only Python 3, nothing to install.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""

import argparse
import asyncio
import socket
import struct
import time

PKT, SYNC, HELLO, RESTART, START = 10, 0xA5, 0xFFFF, 0xFFFE, 0xFFFD
DELAY = 4                    # As sli.c with the hub: frames 0-3 have no buttons


def crc(p):
    c = 0xFFFF
    for b in p[:8]:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else (c << 1)
            c &= 0xFFFF
    return c


def packet(frame, buttons, check=0):
    p = bytes((SYNC, frame & 255, frame >> 8, buttons & 255, buttons >> 8, check, 0, 0))
    return p + struct.pack("<H", crc(p))


class Screen:
    def __init__(self, writer, name):
        self.writer, self.name = writer, name
        self.hello = False
        self.wanted = 0xFF
        self.place = None
        self.inputs = {}            # Frame -> (buttons, check)


class Hub:
    def __init__(self, count, log):
        self.count, self.log = count, log
        self.screens = []
        self.running = False
        self.next = DELAY           # Next frame to send
        self.frames = 0
        self.restarts = 0
        self.frozen = False         # "freeze" typed: no new frames (every screen stops on the same one), "go": on

    def start(self):
        """Places (the ones asked for, then the free ones in the order they came), then all from frame 0."""
        free = list(range(self.count))
        for s in self.screens:
            s.place = s.wanted if s.wanted in free else None
            if s.place is not None:
                free.remove(s.place)
        for s in self.screens:
            if s.place is None:
                s.place = free.pop(0)
            s.inputs = {}
            s.writer.write(packet(START, (self.count << 8) | s.place))
        self.running, self.next = True, DELAY
        self.log("Started: " + ", ".join("%s on screen %d" % (s.name, s.place + 1)
                                         for s in sorted(self.screens, key=lambda s: s.place)))

    def received(self, s, p):
        f, buttons, check = p[1] | (p[2] << 8), p[3] | (p[4] << 8), p[5]
        if f == HELLO:
            if not s.hello:
                s.hello, s.wanted = True, p[5]
                self.log("%s said hello (place asked for: %s)" % (s.name, "any" if s.wanted == 0xFF else s.wanted + 1))
                if not self.running and sum(x.hello for x in self.screens) == self.count:
                    self.start()
            return
        if not self.running or f >= START:
            return
        s.inputs[f] = (buttons, check)
        # Every screen's buttons for the next frame: sent to all
        while not self.frozen and all(self.next in x.inputs for x in self.screens):
            ins = [x.inputs.pop(self.next) for x in self.screens]
            if len(set(c for _, c in ins)) > 1:
                self.restarts += 1
                self.log("The screens' worlds differ at frame %d: all started again" % self.next)
                self.start()
                return
            combined = 0
            for b, _ in ins:
                combined |= b
            data = packet(self.next, combined)
            for x in self.screens:
                x.writer.write(data)
            self.next = (self.next + 1) & 0xFFFF
            if self.next >= START:
                self.next = 0
            self.frames += 1

    async def handle(self, reader, writer):
        sock = writer.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if len(self.screens) >= self.count:
            self.log("Refused a connection: already %d screens" % self.count)
            writer.close()
            return
        s = Screen(writer, "%s:%d" % writer.get_extra_info("peername")[:2])
        self.screens.append(s)
        self.log("%s connected (%d of %d screens)" % (s.name, len(self.screens), self.count))
        buf = b""
        try:
            while True:
                data = await reader.read(4096)
                if not data:
                    break
                buf += data
                while len(buf) >= PKT:
                    if buf[0] == SYNC and crc(buf[:PKT]) == struct.unpack_from("<H", buf, 8)[0]:
                        self.received(s, buf[:PKT])
                        buf = buf[PKT:]
                    else:
                        buf = buf[1:]
        except (ConnectionError, OSError):
            pass
        self.screens.remove(s)
        writer.close()
        if self.running:
            self.running = False
            self.log("%s left: the others wait for it" % s.name)
            for x in self.screens:
                x.hello = False      # (they start again together once it is back)

    async def commands(self):
        """Typed in the hub's window: freeze (every screen stops on the same frame, e.g. for a photo), go."""
        loop = asyncio.get_running_loop()
        while True:
            line = await loop.run_in_executor(None, input)
            if line.strip() == "freeze":
                self.frozen = True
                self.log("Frozen at frame %d" % self.next)
                for s in self.screens:       # (a hello from the hub: paused, no waiting message on the screens)
                    s.writer.write(packet(HELLO, 0))
            elif line.strip() == "go":
                self.frozen = False
                self.log("Going on")
                for s in self.screens:       # (the frames waiting for it)
                    if s.inputs:
                        self.received(s, packet(min(s.inputs), *s.inputs.pop(min(s.inputs))))
                        break

    async def report(self):
        while True:
            await asyncio.sleep(5)
            if self.running:
                self.log("%d screens, %.1f frames a second%s" % (len(self.screens), self.frames / 5,
                                                                 ", %d restarts" % self.restarts if self.restarts else ""))
            self.frames = 0


async def main():
    ap = argparse.ArgumentParser(description="PS1 SLI hub")
    ap.add_argument("--screens", type=int, default=3)
    ap.add_argument("--port", type=int, default=7040)
    args = ap.parse_args()

    def log(text):
        print(time.strftime("%H:%M:%S ") + text, flush=True)

    hub = Hub(args.screens, log)
    server = await asyncio.start_server(hub.handle, "127.0.0.1", args.port)
    log("PS1 SLI hub on 127.0.0.1:%d, waiting for %d screens" % (args.port, args.screens))
    log('Type "freeze" here to stop every screen on the same frame, "go" to go on')
    await asyncio.gather(server.serve_forever(), hub.report(), hub.commands())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
