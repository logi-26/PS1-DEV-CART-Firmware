"""Read-only live sampler of Red Alert Retaliation's link buffers on two real consoles (via ps1transfer -memlive).

Addresses are for SLES_013.43 / SLES_113.43 (same layout on both):
  0x801B55C4  header sent (6 bytes: 0xDABD, payload length, 0xDEAF, little-endian 16-bit)
  0x801B55CC  header received
  0x801B5DA8  payload sent   (4-byte checksum, then the turn record: type 0x16, turn number...)
  0x801B6DB8  payload received
A sample is logged only when its bytes changed. Output: samples.log (t_ms console region hex).
Stop with Ctrl+C, a file named STOP next to this script, or --seconds.
"""
import argparse, os, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, '..', '..', '..', 'ps1transfer', 'ps1transfer.exe')
REGIONS = [('hdr_tx', 0x801B55C4, 6), ('hdr_rx', 0x801B55CC, 6),
           ('pay_tx', 0x801B5DA8, 64), ('pay_rx', 0x801B6DB8, 64)]


def worker(name, log, lock, t0, stop, stats):
    """Reads a console until stopped; if ps1transfer ends (the USB device not opened: another program had it), the
    reason is logged and it starts again a second later."""
    last = {}
    while not stop.is_set():
        reason = read_console(name, log, lock, t0, stop, stats, last)
        if stop.is_set():
            break
        with lock:
            log.write('%8d %s # reader stopped (%s): again in 1 s\n' % (int((time.time() - t0) * 1000), name, reason))
            log.flush()
        time.sleep(1)


def read_console(name, log, lock, t0, stop, stats, last):
    p = subprocess.Popen([EXE, '-n', name, '-memlive'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, bufsize=1)
    said = []
    while not stop.is_set():
        try:
            for r, a, n in REGIONS:
                p.stdin.write('read %08X %d\n' % (a, n))
            p.stdin.flush()
            got = 0
            while got < len(REGIONS):
                line = p.stdout.readline()
                if not line:
                    p.terminate()
                    return ' / '.join(said[-2:]) or 'ended'
                if not line.startswith('MEMREAD'):
                    said.append(line.strip())
                if line.startswith('MEMREAD'):
                    _, addr, hx = line.split()
                    r = next(x[0] for x in REGIONS if x[1] == int(addr, 16))
                    got += 1
                    stats[name] = stats.get(name, 0) + 1
                    if last.get(r) != hx:
                        last[r] = hx
                        with lock:
                            log.write('%8d %s %s %s\n' % (int((time.time() - t0) * 1000), name, r, hx))
                            log.flush()
        except (OSError, ValueError) as e:
            p.terminate()
            return 'error: %s / %s' % (e, ' / '.join(said[-2:]))
    try:
        p.stdin.write('quit\n'); p.stdin.flush()
    except OSError:
        pass
    p.terminate()
    return 'stopped'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seconds', type=int, default=900)
    ap.add_argument('--consoles', nargs='+', default=['BEN', 'BILL'])
    a = ap.parse_args()
    stop_file = os.path.join(HERE, 'STOP')
    if os.path.exists(stop_file):
        os.remove(stop_file)
    log = open(os.path.join(HERE, 'samples.log'), 'w')
    lock, stop, stats, t0 = threading.Lock(), threading.Event(), {}, time.time()
    ts = [threading.Thread(target=worker, args=(n, log, lock, t0, stop, stats), daemon=True) for n in a.consoles]
    for t in ts:
        t.start()
    try:
        while time.time() - t0 < a.seconds and not os.path.exists(stop_file) and any(t.is_alive() for t in ts):
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    stop.set()
    time.sleep(1)
    log.write('# reads per console: %s\n' % stats)
    log.close()


if __name__ == '__main__':
    main()
