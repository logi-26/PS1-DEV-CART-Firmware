"""The network between two emulators running lockstep.lua (research): each one's ls_out.txt copied, line by line, to
the other's ls_in.txt after a delay (a one-way network time) and a random jitter, in order (as TCP delivers).
  python relay.py DIR_A DIR_B [--delay MS] [--jitter MS] [--seconds N]
DIR_A / DIR_B: the emulators' folders (research/dd/.redux_a, .redux_b). Starts both in files empty."""
import argparse
import os
import random
import time


class Way:
    def __init__(self, src, dst, delay, jitter):
        self.src_path, self.dst = src, open(dst, "w")
        self.src = None
        self.delay, self.jitter = delay / 1000.0, jitter / 1000.0
        self.queue = []          # (time due, line)
        self.last_due = 0.0
        self.partial = ""
        self.sent = 0

    def step(self, now):
        if self.src is None and os.path.exists(self.src_path):
            self.src = open(self.src_path, "r")
        if self.src:
            data = self.src.read()
            if data:
                data = self.partial + data
                lines = data.split("\n")
                self.partial = lines.pop()
                for line in lines:
                    due = max(now + self.delay + random.uniform(0, self.jitter), self.last_due)
                    self.last_due = due
                    self.queue.append((due, line))
        while self.queue and self.queue[0][0] <= now:
            self.dst.write(self.queue.pop(0)[1] + "\n")
            self.sent += 1
        self.dst.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--delay", type=float, default=30)
    ap.add_argument("--jitter", type=float, default=20)
    ap.add_argument("--seconds", type=float, default=1800)
    a = ap.parse_args()
    for d in (a.a, a.b):
        for n in ("ls_out.txt", "ls_in.txt"):
            open(os.path.join(d, n), "w").close()
    ways = [Way(os.path.join(a.a, "ls_out.txt"), os.path.join(a.b, "ls_in.txt"), a.delay, a.jitter),
            Way(os.path.join(a.b, "ls_out.txt"), os.path.join(a.a, "ls_in.txt"), a.delay, a.jitter)]
    print("relay: %s <-> %s, %.0f ms + up to %.0f ms" % (a.a, a.b, a.delay, a.jitter), flush=True)
    end = time.time() + a.seconds
    last_report = time.time()
    while time.time() < end:
        now = time.time()
        for w in ways:
            w.step(now)
        if now - last_report > 30:
            last_report = now
            print("relay: %d A->B, %d B->A lines" % (ways[0].sent, ways[1].sent), flush=True)
        time.sleep(0.001)


if __name__ == "__main__":
    main()
