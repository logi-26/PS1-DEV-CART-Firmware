"""Watch Red Alert Retaliation's link variables in both emulators (web servers 8081, 8082) while a link game is set
up: every change logged with its time. The connect routine (0x80114F20) keeps its two sio: handles at gp+0xE00 and
gp+0xE04 and its role at gp+0xDF4 (gp = 0x801B485C); also the game mode and the frame counter. python linkvars.py SECONDS"""
import struct, sys, time, urllib.request
GP = 0x1B485C
WATCH = {"handles": (GP + 0xE00, 8), "role": (GP + 0xDF4, 4), "dfc": (GP + 0xDFC, 4), "frame": (0x1B4EB8, 4),
         "mode 5198": (0x1B5198, 4), "hdr_tx": (0x1B55C4, 6), "hdr_rx": (0x1B55CC, 6)}
t0, last = time.time(), {}
log = open("linkvars.log", "w", buffering=1)
while time.time() - t0 < float(sys.argv[1]):
    for port, name in ((8081, "A"), (8082, "B")):
        try:
            d = urllib.request.urlopen("http://127.0.0.1:%d/api/v1/cpu/ram/raw" % port, timeout=3).read()
        except OSError as e:
            continue
        for k, (a, n) in WATCH.items():
            v = d[a:a + n].hex()
            if k == "frame":
                v = struct.unpack_from("<I", d, a)[0] // 25        # (once a second)
            if last.get((name, k)) != v:
                last[(name, k)] = v
                log.write("%7.2f s %s %-9s %s\n" % (time.time() - t0, name, k, v))
