"""One PCSX-Redux with Destruction Derby (SCUS_943.02) and dd_auto.lua (research: pad play's determinism).
  python dd_run.py [NAME]        start Redux NAME (default "a"; folder .redux_NAME), closing the one started before
  python dd_run.py --stop [NAME] close it
Files in .redux_NAME: dd.log, ctl.lua (commands, run each time it changes), screenshots and dumps."""
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "virtual"))
import run_pair  # noqa: E402

DISCS = os.environ.get("PS1_DISCS") or os.path.expanduser(r"~\Desktop\ps1usb")   # Folder with the disc images
GAMES = {"dd": (os.path.join(DISCS, r"Destruction Derby\Destruction Derby.cue"), ""),
         # CTR: no pad hook yet (PAD_READ = 0: the emulator's pads)
         "ctr": (os.path.join(DISCS, r"CTR_ Crash Team Racing\CTR - Crash Team Racing (USA).cue"),
                 "PAD_READ = 0\n"),
         # TOCA 2 Touring Cars (Europe, SLES_015.47): pad play research (research/toca2)
         "toca2": (os.path.join(DISCS, r"TOCA 2 Touring Cars\TOCA 2 Touring Cars.cue"), "PAD_READ = 0\n")}
REDUX = r"C:\pcsx-redux-23726-20260420-7\pcsx-redux.exe"


def folder(name):
    return os.path.join(HERE, ".redux_" + name)


def stop(name):
    pids_file = os.path.join(folder(name), "pids.txt")
    try:
        pids = open(pids_file).read().split()
    except OSError:
        return
    for pid in pids:
        subprocess.run(["taskkill", "/PID", pid, "/T", "/F"], capture_output=True)
    os.remove(pids_file)


def main():
    game = sys.argv[sys.argv.index("--game") + 1] if "--game" in sys.argv else "dd"
    args = [a for a in sys.argv[1:] if not a.startswith("--") and a != game]
    iso, setup = GAMES[game]
    name = args[0] if args else "a"
    stop(name)
    if "--stop" in sys.argv:
        return
    d = folder(name)
    os.makedirs(d, exist_ok=True)
    run_pair.make_settings(d, (40 if name == "a" else 700, 40), 8090 + ord(name[0]) % 10, mute=True)
    # Both pads connected (two-player modes need pad 2 present to be offered)
    cfg = os.path.join(d, "pcsx.json")
    with open(cfg, encoding="utf-8") as f:
        s = json.load(f)
    for pad in run_pair.find_key(s, "pads")["pads"]:
        pad["Connected"] = True
    with open(cfg, "w", encoding="utf-8") as f:
        json.dump(s, f, indent=2)
    start = os.path.join(d, "start.lua")
    with open(start, "w") as f:
        f.write("DD_DIR = [[%s]]\n%sdofile([[%s]])\n" % (d.replace("\\", "/"), setup,
                                                     os.path.join(HERE, "dd_auto.lua").replace("\\", "/")))
    with open(os.path.join(d, "ctl.lua"), "w") as f:
        f.write("-- nothing yet\n")
    p = subprocess.Popen([REDUX, "-portable", d, "-loadiso", iso, "-dofile", start, "-run"], cwd=d,
                         creationflags=0x00000008 | 0x00000200)
    time.sleep(5)
    pids = [str(p.pid)] + run_pair.children([str(p.pid)])
    with open(os.path.join(d, "pids.txt"), "w") as f:
        f.write(" ".join(pids))
    print("started", name, " ".join(pids))


if __name__ == "__main__":
    main()
