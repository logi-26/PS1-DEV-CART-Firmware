"""How often Red Alert's link payload changes: one console, one small region, read as fast as -memlive allows."""
import subprocess, sys, time
EXE = r"..\..\..\ps1transfer\ps1transfer.exe"
name, secs = sys.argv[1], float(sys.argv[2])
p = subprocess.Popen([EXE, "-n", name, "-memlive"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
t0, reads, changes, last, stamps = time.time(), 0, 0, None, []
while time.time() - t0 < secs:
    p.stdin.write("read 801B5DA8 12\n"); p.stdin.flush()
    line = p.stdout.readline()
    while line and not line.startswith("MEMREAD"):
        line = p.stdout.readline()
    if not line: break
    reads += 1
    hx = line.split()[2]
    if hx != last:
        changes += 1; stamps.append(time.time() - t0); last = hx
p.stdin.write("quit\n"); p.stdin.flush(); p.terminate()
gaps = sorted(round((b - a) * 1000) for a, b in zip(stamps, stamps[1:]))
print("%s: %d reads (%.1f ms each), %d changes in %.0f s; gaps between changes (ms): median %s, smallest %s" % (
    name, reads, secs * 1000 / max(1, reads), changes, secs, gaps[len(gaps) // 2] if gaps else None, gaps[:5]))
