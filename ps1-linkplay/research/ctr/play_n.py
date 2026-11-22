"""CTR pad play for 2-4 players on N emulators (research): lockstep_n.lua in each, hub.py between them.
  python play_n.py [--players 3] [--setup "{mode='battle', ...}"] [--pad] [--input-delay 2] [--delay 30]
                   [--jitter 20] [--minutes 10] [--frames 900]
  python play_n.py --stop
Emulators a, b, c, d = players 1..N. Without --pad all players are scripted; with --pad emulator a's own pad 1 is
player 1 (someone playing). Emulators after the first set up later (37, 80, 120 blanks) to give them different timing.
The setup table: see ctr_setup.lua (its chars must have N entries)."""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DD = os.path.join(HERE, "..", "dd")
NAMES = "abcd"
OFFSETS = (0, 37, 80, 120)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--players", type=int, default=3)
    ap.add_argument("--setup", default="")
    ap.add_argument("--pad", action="store_true")
    ap.add_argument("--input-delay", type=int, default=2)
    ap.add_argument("--delay", type=float, default=30)
    ap.add_argument("--jitter", type=float, default=20)
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--frames", type=int, default=900)
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--hash", type=int, default=0, help="log each driver's hash per frame up to this frame")
    ap.add_argument("--dump-at", default="", help="frames to dump RAM at too, e.g. 0,50,100")
    ap.add_argument("--extra", default="", help="extra presses, Lua: {{race, frame, player, 'START'}, ...}")
    ap.add_argument("--stop", action="store_true")
    a = ap.parse_args()
    run = [sys.executable, os.path.join(DD, "dd_run.py")]
    if a.stop:
        for n in NAMES:
            subprocess.run(run + ["--stop", n], cwd=DD)
        return
    n = a.players
    setup = a.setup or ("{mode='battle', players=%d, chars={%s}, level=18}" % (n, ",".join(str(i) for i in range(n))))
    names = NAMES[:n]
    for name in names:
        subprocess.run(run + [name, "--game", "ctr"], cwd=DD, check=True)
    dirs = [os.path.join(DD, ".redux_" + x) for x in names]
    hub = subprocess.Popen([sys.executable, os.path.join(HERE, "hub.py")] + dirs +
                           ["--delay", str(a.delay), "--jitter", str(a.jitter), "--seconds", str(a.minutes * 60 + 180)])
    time.sleep(1)
    files = [os.path.join(HERE, f).replace("\\", "/") for f in ("ctr_mt.lua", "ctr_setup.lua", "lockstep_n.lua")]
    for i, (name, d) in enumerate(zip(names, dirs)):
        source = "pad" if (a.pad and i == 0) else "script"
        with open(os.path.join(d, "ctl.lua"), "w") as f:
            f.write('TAG, ME, N, DELAY, INPUT_DELAY, SOURCE, FRAMES, SEED = "%s", %d, %d, %d, %d, "%s", %d, %d\n'
                    % (name.upper(), i + 1, n, OFFSETS[i], a.input_delay, source, a.frames, a.seed))
            f.write("SETUP = %s\n" % setup)
            f.write("EXTRA = %s\n" % (a.extra or "{}"))
            f.write("HASH_FRAMES = %d\nDUMP_AT = {%s}\n" % (a.hash, ", ".join("[%s] = true" % x for x in a.dump_at.split(",") if x)))
            for lua in files:
                f.write("dofile([[%s]])\n" % lua)
            f.write("return '%s ready'\n" % name)
    print("%d emulators: %s; input delay %d frames, network %.0f ms + up to %.0f ms. Close with: python play_n.py --stop"
          % (n, setup, a.input_delay, a.delay, a.jitter), flush=True)
    try:
        hub.wait()
    except KeyboardInterrupt:
        hub.terminate()


if __name__ == "__main__":
    main()
