"""The network between N emulators running lockstep_n.lua (research): every line of each one's ls_out.txt copied to
every other one's ls_in.txt after a delay (one way) and a random jitter, in order per sender (as a lobby server
forwarding TCP traffic to everyone would).
  python hub.py DIR... [--delay MS] [--jitter MS] [--seconds N]
Starts all the files empty."""
import argparse
import os
import random
import time


class Sender:
    def __init__(self, path, dests, delay, jitter):
        self.path, self.dests = path, dests
        self.src = None
        self.partial = ""
        self.delay, self.jitter = delay / 1000.0, jitter / 1000.0
        self.queues = [[] for _ in dests]       # per destination: (due, line)
        self.last_due = [0.0] * len(dests)

    def step(self, now):
        if self.src is None and os.path.exists(self.path):
            self.src = open(self.path, "r")
        if self.src:
            data = self.src.read()
            if data:
                lines = (self.partial + data).split("\n")
                self.partial = lines.pop()
                for line in lines:
                    for i in range(len(self.dests)):
                        due = max(now + self.delay + random.uniform(0, self.jitter), self.last_due[i])
                        self.last_due[i] = due
                        self.queues[i].append((due, line))
        for i, q in enumerate(self.queues):
            n = 0
            while n < len(q) and q[n][0] <= now:
                self.dests[i].write(q[n][1] + "\n")
                n += 1
            if n:
                del q[:n]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--delay", type=float, default=30)
    ap.add_argument("--jitter", type=float, default=20)
    ap.add_argument("--seconds", type=float, default=1800)
    a = ap.parse_args()
    for d in a.dirs:
        for n in ("ls_out.txt", "ls_in.txt"):
            open(os.path.join(d, n), "w").close()
    ins = {d: open(os.path.join(d, "ls_in.txt"), "w") for d in a.dirs}
    senders = [Sender(os.path.join(d, "ls_out.txt"), [ins[o] for o in a.dirs if o != d], a.delay, a.jitter)
               for d in a.dirs]
    print("hub: %d emulators, %.0f ms + up to %.0f ms" % (len(a.dirs), a.delay, a.jitter), flush=True)
    end = time.time() + a.seconds
    while time.time() < end:
        now = time.time()
        for s in senders:
            s.step(now)
        for f in ins.values():
            f.flush()
        time.sleep(0.001)


if __name__ == "__main__":
    main()
