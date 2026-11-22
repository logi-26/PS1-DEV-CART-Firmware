"""TOCA 2 determinism runs, one after the other in one emulator session (research/toca2/NOTES.md).
  python det_run.py TAG:DELAY [TAG:DELAY...] [--lua "extra Lua before each run, e.g. DUMP_AT = 3"] [--attach]
(--attach: use the emulator already running, and leave it running)
Needs research/dd/.redux_t/prerace.sst (car setup, player 2 on START RACE). Results: research/dd/.redux_t/det_TAG*.
Each run waits for its own "det TAG done" line written after its command (gives up after 4 minutes)."""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DD = os.path.join(HERE, "..", "dd")
D = os.path.join(DD, ".redux_t")
LOG = os.path.join(D, "dd.log")
DET = os.path.join(HERE, "toca2_det.lua").replace("\\", "/")


def log_text():
    try:
        return open(LOG).read()
    except OSError:
        return ""


def wait_for(text, start, seconds):
    t0 = time.time()
    while time.time() - t0 < seconds:
        if text in log_text()[start:]:
            return True
        time.sleep(1)
    return False


def main():
    args = sys.argv[1:]
    extra = ""
    if "--lua" in args:
        i = args.index("--lua")
        extra = args[i + 1]
        del args[i:i + 2]
    attach = "--attach" in args
    if attach:
        args.remove("--attach")
    else:
        subprocess.run([sys.executable, "dd_run.py", "t", "--game", "toca2"], cwd=DD, check=True)
        # the state is loaded during the BIOS's start: loading it later from the intro / the menus (FRONTEND.EXE)
        # closes Redux; from a race (GAME.EXE) it works
        if not wait_for("dd_auto ready", 0, 60):
            sys.exit("the emulator didn't start")
        time.sleep(3)                           # (at once, or after the BIOS: Redux closes)
    for n, run in enumerate(args):
        tag, delay = run.split(":")
        start = len(log_text())
        with open(os.path.join(D, "ctl.lua"), "w") as f:
            f.write("-- run %d\n%s\nTAG, DELAY = '%s', %s; dofile([[%s]])\n" % (n, extra, tag, delay, DET))
        if not wait_for("det %s: delay" % tag, start, 60):
            sys.exit("run %s didn't start" % tag)
        t0 = time.time()
        ok = wait_for("det %s done" % tag, start, 240)
        print(tag, "done" if ok else "TIMED OUT", "%.0f s" % (time.time() - t0), flush=True)
        if not ok:
            break
    if not attach:
        subprocess.run([sys.executable, "dd_run.py", "--stop", "t"], cwd=DD)


if __name__ == "__main__":
    main()
