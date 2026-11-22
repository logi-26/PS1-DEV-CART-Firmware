"""Red Alert Retaliation's game speed in the two emulators of relay.py: the frame counter (0x801B4EB8) read through
each one's web server over a few seconds. python frames.py [seconds]"""
import struct, sys, time, urllib.request

FRAME = 0x801B4EB8 - 0x80000000


def frame(port):
    d = urllib.request.urlopen("http://127.0.0.1:%d/api/v1/cpu/ram/raw" % port, timeout=5).read()
    return struct.unpack_from("<I", d, FRAME)[0]


secs = float(sys.argv[1]) if len(sys.argv) > 1 else 5
t0, f0 = time.time(), {p: frame(p) for p in (8081, 8082)}
time.sleep(secs)
t1, f1 = time.time(), {p: frame(p) for p in (8081, 8082)}
for p, name in ((8081, "A"), (8082, "B")):
    print("%s: frame %d, %.1f frames a second" % (name, f1[p], (f1[p] - f0[p]) / (t1 - t0)))
