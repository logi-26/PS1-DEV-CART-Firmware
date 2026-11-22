"""Run two PCSX-Redux instances with their serial ports (SIO1) joined through a logging proxy.

  A (SIO1 server, port 6701)  <--proxy-->  B (SIO1 server, port 6702); the proxy connects to both

Every chunk that passes is logged with a timestamp to trace.log (hex) and to trace_<dir>.bin (raw bytes).
Usage:
  python trace.py --iso "path\\to\\game.cue" [--redux C:\\pcsx-redux\\pcsx-redux.exe] [--mode 0|1]
--mode 0 = Redux's protocol mode (also carries the control lines), 1 = raw bytes only.
Settings come from your usual pcsx.json (BIOS, controls); your own file is not changed.
"""
import argparse, copy, json, os, socket, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))


def find_key(obj, key):
    """Return the dict (at any depth) that holds `key`, or None."""
    if isinstance(obj, dict):
        if key in obj:
            return obj
        for v in obj.values():
            r = find_key(v, key)
            if r is not None:
                return r
    return None


def make_settings(base_path, dest_dir, pos=None, **sio):
    os.makedirs(dest_dir, exist_ok=True)
    with open(base_path, encoding='utf-8') as f:
        s = json.load(f)
    holder = find_key(s, 'SIO1Server')
    if holder is None:
        sec = s.setdefault('emulator', {}).setdefault('Debug', {})
        holder = sec
    holder.update(sio)
    if pos:
        # Your saved window position would put both emulators in the same place, one hiding the other.
        w = find_key(s, 'WindowPosX')
        if w is not None:
            w.update(WindowPosX=pos[0], WindowPosY=pos[1], WindowSizeX=852, WindowSizeY=706, WindowMaximized=False)
    with open(os.path.join(dest_dir, 'pcsx.json'), 'w', encoding='utf-8') as f:
        json.dump(s, f, indent=2)


class Log:
    def __init__(self, path):
        self.f = open(path, 'w', buffering=1)
        self.t0 = time.time()
        self.lock = threading.Lock()
        self.bins = {}

    def chunk(self, direction, data):
        t = int((time.time() - self.t0) * 1000)
        with self.lock:
            self.f.write('%8d ms  %s  %5d  %s\n' % (t, direction, len(data), data.hex(' ')))
            b = self.bins.setdefault(direction, open(os.path.join(HERE, 'trace_%s.bin' % direction), 'wb'))
            b.write(data)
            b.flush()

    def note(self, text):
        t = int((time.time() - self.t0) * 1000)
        with self.lock:
            self.f.write('%8d ms  # %s\n' % (t, text))


def pump(src, dst, direction, log):
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            log.chunk(direction, data)
            dst.sendall(data)
    except OSError:
        pass
    log.note('%s closed' % direction)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iso', required=True)
    ap.add_argument('--redux', default=r'C:\pcsx-redux-23726-20260420-7\pcsx-redux.exe')
    ap.add_argument('--base', default=os.path.join(os.environ.get('APPDATA', ''), 'pcsx-redux', 'pcsx.json'))
    ap.add_argument('--mode', type=int, default=0)
    ap.add_argument('--port-a', type=int, default=6701)
    ap.add_argument('--port-b', type=int, default=6702)
    a = ap.parse_args()

    dir_a, dir_b = os.path.join(HERE, '.redux_a'), os.path.join(HERE, '.redux_b')
    make_settings(a.base, dir_a, pos=(20, 40), SIO1Server=True, SIO1ServerPort=a.port_a, SIO1Client=False, SIO1Mode=a.mode)
    make_settings(a.base, dir_b, pos=(890, 40), SIO1Server=True, SIO1ServerPort=a.port_b, SIO1Client=False, SIO1Mode=a.mode)

    log = Log(os.path.join(HERE, 'trace.log'))

    def launch(d):
        return subprocess.Popen([a.redux, '-portable', '-loadiso', a.iso, '-run'], cwd=d)

    pa = launch(dir_a)
    pb = launch(dir_b)
    print('Both consoles started. In both windows choose LINK GAME. Close either emulator to stop. Log: trace.log')

    def alive():
        return pa.poll() is None and pb.poll() is None

    # The emulators drop their serial connections when the game (re)starts, so link again each time.
    while alive():
        to_a = None
        while alive() and not to_a:
            try:
                to_a = socket.create_connection(('127.0.0.1', a.port_a))
            except OSError:
                time.sleep(0.5)
        to_b = None
        while alive() and to_a and not to_b:
            try:
                to_b = socket.create_connection(('127.0.0.1', a.port_b))
            except OSError:
                time.sleep(0.5)
        if not (to_a and to_b):
            break
        for s in (to_a, to_b):
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        log.note('linked')
        print('linked')
        ts = [threading.Thread(target=pump, args=(to_a, to_b, 'A>B', log), daemon=True),
              threading.Thread(target=pump, args=(to_b, to_a, 'B>A', log), daemon=True)]
        for t in ts:
            t.start()
        while alive() and all(t.is_alive() for t in ts):
            time.sleep(0.2)
        for s in (to_a, to_b):
            try:
                s.close()
            except OSError:
                pass
        time.sleep(1)
    for p in (pa, pb):
        if p.poll() is None:
            p.terminate()
    return 0


if __name__ == '__main__':
    sys.exit(main())
