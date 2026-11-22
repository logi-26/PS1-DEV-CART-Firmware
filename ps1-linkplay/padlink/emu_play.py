"""The CTR pad play driver (padlink.c) on N emulators: each PCSX-Redux gets the driver installed at the main menu by
padlink.lua (doing what the computer does for a console), research/ctr/hub.py carries the frames between them.
  python emu_play.py [--players 2] [--setup "{mode='vs', ...}"] [--pad] [--input-delay 2] [--delay 30] [--jitter 20]
                     [--minutes 10] [--hash 900] [--extra "{{race, frame, player, 'START'}}"]
  python emu_play.py --stop
Emulators a, b, c, d = players 1..N; with --pad emulator a's own pad 1 is player 1 (someone playing), else all are
scripted. Logs: research/dd/.redux_X/dd.log, ls_hash.txt (the driver's state hash per frame)."""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RESEARCH = os.path.join(HERE, "..", "research")
DD = os.path.join(RESEARCH, "dd")
CTR = os.path.join(RESEARCH, "ctr")
NAMES = "abcd"
OFFSETS = (0, 37, 80, 120)


def lua_path(*p):
    return os.path.join(*p).replace("\\", "/")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--players", type=int, default=2)
    ap.add_argument("--setup", default="")
    ap.add_argument("--pad", action="store_true")
    ap.add_argument("--input-delay", type=int, default=2)
    ap.add_argument("--delay", type=float, default=30)
    ap.add_argument("--jitter", type=float, default=20)
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--hash", type=int, default=900)
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--extra", default="")
    ap.add_argument("--stop", action="store_true")
    a = ap.parse_args()
    run = [sys.executable, os.path.join(DD, "dd_run.py")]
    if a.stop:
        for n in NAMES:
            subprocess.run(run + ["--stop", n], cwd=DD)
        return
    n = a.players
    setup = a.setup or ("{mode='vs', players=%d, chars={%s}, level=3, laps=3}" % (n, ",".join(str(i) for i in range(n))))
    names = NAMES[:n]
    for name in names:
        subprocess.run(run + [name, "--game", "ctr"], cwd=DD, check=True)
    dirs = [os.path.join(DD, ".redux_" + x) for x in names]
    hub = subprocess.Popen([sys.executable, os.path.join(CTR, "hub.py")] + dirs +
                           ["--delay", str(a.delay), "--jitter", str(a.jitter), "--seconds", str(a.minutes * 60 + 180)])
    time.sleep(1)
    for i, (name, d) in enumerate(zip(names, dirs)):
        source = "pad" if (a.pad and i == 0) else "script"
        with open(os.path.join(d, "ctl.lua"), "w") as f:
            f.write('TAG, ME, N, DELAY, INPUT_DELAY, SOURCE, SEED, HASH_FRAMES = "%s", %d, %d, %d, %d, "%s", %d, %d\n'
                    % (name.upper(), i + 1, n, OFFSETS[i], a.input_delay, source, a.seed, a.hash))
            f.write("SETUP = %s\nEXTRA = %s\n" % (setup, a.extra or "{}"))
            f.write("PL_LUA = [[%s]]\n" % lua_path(HERE, "padlink_ctr.lua"))
            f.write("dofile([[%s]])\ndofile([[%s]])\n" % (lua_path(CTR, "ctr_setup.lua"), lua_path(HERE, "padlink.lua")))
            f.write("return '%s ready %f'\n" % (name, time.time()))
    print("%d emulators with the padlink driver: %s" % (n, setup), flush=True)
    try:
        hub.wait()
    except KeyboardInterrupt:
        hub.terminate()


if __name__ == "__main__":
    main()
