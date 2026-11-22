"""CTR pad play prototype, played by someone: two emulators in lockstep (lockstep.lua) through relay.py.
Emulator A: you, with its pad 1 (Redux's keyboard / controller mapping); emulator B: the scripted player 2.
Both go through the menus by themselves (Battle, 2P, the first level); the lockstep starts with the battle.
  python play.py [--input-delay 2] [--delay 30] [--jitter 20] [--minutes 10]
  python play.py --stop      close both emulators
Click in emulator A's window to play (it needs the keyboard focus). The logs: research/dd/.redux_a/dd.log."""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DD = os.path.join(HERE, "..", "dd")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input-delay", type=int, default=2)
    ap.add_argument("--delay", type=float, default=30)
    ap.add_argument("--jitter", type=float, default=20)
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--stop", action="store_true")
    a = ap.parse_args()
    run = [sys.executable, os.path.join(DD, "dd_run.py")]
    if a.stop:
        for n in ("a", "b"):
            subprocess.run(run + ["--stop", n], cwd=DD)
        return
    for n in ("a", "b"):
        subprocess.run(run + [n, "--game", "ctr"], cwd=DD, check=True)
    da, db = os.path.join(DD, ".redux_a"), os.path.join(DD, ".redux_b")
    relay = subprocess.Popen([sys.executable, os.path.join(HERE, "relay.py"), da, db, "--delay", str(a.delay),
                              "--jitter", str(a.jitter), "--seconds", str(a.minutes * 60 + 120)])
    time.sleep(1)
    lua = os.path.join(HERE, "lockstep.lua").replace("\\", "/")
    frames = int(a.minutes * 60 * 30)
    for d, tag, me, delay, source in ((da, "A", 1, 0, "pad"), (db, "B", 2, 37, "script")):
        with open(os.path.join(d, "ctl.lua"), "w") as f:
            f.write('TAG, ME, DELAY, INPUT_DELAY, SOURCE, FRAMES = "%s", %d, %d, %d, "%s", %d\nreturn dofile([[%s]])\n'
                    % (tag, me, delay, a.input_delay, source, frames, lua))
    print("Two emulators started: play in emulator A (the left window) once the battle starts (about 1.5 minutes).")
    print("Input delay %d frames, network %.0f ms + up to %.0f ms. Close with: python play.py --stop"
          % (a.input_delay, a.delay, a.jitter))
    try:
        relay.wait()
    except KeyboardInterrupt:
        relay.terminate()


if __name__ == "__main__":
    main()
