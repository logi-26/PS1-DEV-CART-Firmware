"""Two PCSX-Redux joined through a relay that can delay the link, as the internet would.

  B (SIO1 client) --> relay (port 6702) --> A (SIO1 server, port 6701)

The relay forwards everything in both directions (Redux's protocol mode 0: the data and the RTS/DTR lines), each
chunk held back DELAY ms first (one way; a round trip adds it twice). The delay is read from delay.txt next to this
script every half second (a number of ms; change it while the game runs). Each emulator has its web server on
(A: 8081, B: 8082) so the frame counter can be read (frames.py). Log: relay.log.

  python relay.py --iso "path\\to\\game.cue" [--redux C:\\pcsx-redux\\pcsx-redux.exe] [--delay 0]
"""
import argparse, json, os, queue, socket, subprocess, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
DELAY_FILE = os.path.join(HERE, "delay.txt")


def find_key(obj, key):
    if isinstance(obj, dict):
        if key in obj:
            return obj
        for v in obj.values():
            r = find_key(v, key)
            if r is not None:
                return r
    return None


def make_settings(base_path, dest_dir, pos, sio, web_port, debug=False):
    os.makedirs(dest_dir, exist_ok=True)
    with open(base_path, encoding="utf-8") as f:
        s = json.load(f)
    holder = find_key(s, "SIO1Server") or s.setdefault("emulator", {}).setdefault("Debug", {})
    holder.update(sio)
    holder.update(WebServer=True, WebServerPort=web_port, GdbServer=False, Debug=debug)
    w = find_key(s, "WindowPosX")
    if w is not None:
        w.update(WindowPosX=pos[0], WindowPosY=pos[1], WindowMaximized=False)
    with open(os.path.join(dest_dir, "pcsx.json"), "w", encoding="utf-8") as f:
        json.dump(s, f, indent=2)


class Log:
    def __init__(self):
        self.f = open(os.path.join(HERE, "relay.log"), "w", buffering=1)
        self.t0 = time.time()
        self.lock = threading.Lock()
        self.bytes = {"A>B": 0, "B>A": 0}

    def note(self, text):
        with self.lock:
            self.f.write("%8d ms  %s\n" % (int((time.time() - self.t0) * 1000), text))


def delay_ms():
    try:
        with open(DELAY_FILE) as f:
            return max(0.0, float(f.read().strip() or 0))
    except (OSError, ValueError):
        return 0.0


def pump(src, dst, direction, log, state):
    """src -> dst, each chunk sent DELAY ms after it came (in order)."""
    q = queue.Queue()

    def sender():
        while True:
            item = q.get()
            if item is None:
                break
            due, data = item
            wait = due - time.time()
            if wait > 0:
                time.sleep(wait)
            try:
                dst.sendall(data)
            except OSError:
                break
    t = threading.Thread(target=sender, daemon=True)
    t.start()
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            log.bytes[direction] += len(data)
            log.note("%s %4d  %s" % (direction, len(data), data[:96].hex(" ") + (" ..." if len(data) > 96 else "")))
            q.put((time.time() + state["delay"] / 1000.0, data))
    except OSError:
        pass
    q.put(None)
    log.note("# %s closed" % direction)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--redux", default=r"C:\pcsx-redux-23726-20260420-7\pcsx-redux.exe")
    ap.add_argument("--delay", type=float, default=0)
    ap.add_argument("--direct", action="store_true", help="B connects to A itself (no relay), to compare")
    ap.add_argument("--watch", help="a Lua script for A only (its debug mode on: breakpoints)")
    a = ap.parse_args()
    with open(DELAY_FILE, "w") as f:
        f.write(str(a.delay))
    base = os.path.join(os.environ["APPDATA"], "pcsx-redux", "pcsx.json")
    dir_a, dir_b = os.path.join(HERE, ".relay_a"), os.path.join(HERE, ".relay_b")
    make_settings(base, dir_a, (40, 40), dict(SIO1Server=True, SIO1ServerPort=6701, SIO1Client=False, SIO1Mode=0), 8081,
                  debug=bool(a.watch))
    make_settings(base, dir_b, (900, 40), dict(SIO1Server=False, SIO1Client=True, SIO1Clienthost="127.0.0.1",
                                              SIO1ClientPort=6701 if a.direct else 6702, SIO1Mode=0), 8082)
    log = Log()
    state = {"delay": a.delay}

    def watch_delay():
        while True:
            d = delay_ms()
            if d != state["delay"]:
                state["delay"] = d
                log.note("# delay now %g ms one way" % d)
            time.sleep(0.5)
    threading.Thread(target=watch_delay, daemon=True).start()
    listen = socket.socket()
    listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listen.bind(("127.0.0.1", 6702))
    listen.listen(1)
    flags = 0x00000008 | 0x00000200
    lua = ["-dofile", os.path.abspath(a.watch)] if a.watch else []     # (A only: in both, they didn't link)
    pa = subprocess.Popen([a.redux, "-portable", dir_a, "-loadiso", a.iso, "-run"] + lua, cwd=dir_a,
                          creationflags=flags)
    time.sleep(3)
    pb = subprocess.Popen([a.redux, "-portable", dir_b, "-loadiso", a.iso, "-run"], cwd=dir_b,
                          creationflags=flags)
    print("A (server) pid %d, B (client) pid %d; delay %g ms one way (delay.txt)" % (pa.pid, pb.pid, a.delay),
          flush=True)
    while pa.poll() is None and pb.poll() is None:
        listen.settimeout(1.0)
        try:
            cb, _ = listen.accept()
        except socket.timeout:
            continue
        try:
            ca = socket.create_connection(("127.0.0.1", 6701), timeout=5)
        except OSError as e:
            log.note("# can't reach A: %s" % e)
            cb.close()
            continue
        for s in (ca, cb):
            s.settimeout(None)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        log.note("# linked")
        ts = [threading.Thread(target=pump, args=(cb, ca, "B>A", log, state), daemon=True),
              threading.Thread(target=pump, args=(ca, cb, "A>B", log, state), daemon=True)]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        for s in (ca, cb):
            s.close()
        log.note("# unlinked; bytes %s" % log.bytes)
    log.note("# an emulator closed")


if __name__ == "__main__":
    main()
