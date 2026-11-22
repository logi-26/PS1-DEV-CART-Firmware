"""One PCSX-Redux with Red Alert Retaliation and menu_auto.lua (research: driving the menus from outside).
  python menu_run.py          start (closes the one this script started before)
  python menu_run.py --stop   close it
Files: .menu/menu.log, .menu/ctl.lua (commands), dumps and savestates in .menu/."""
import os, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "virtual"))
import run_pair  # noqa: E402

D = os.path.join(HERE, ".menu")
PIDS = os.path.join(D, "pids.txt")


def stop():
    try:
        pids = open(PIDS).read().split()
    except OSError:
        return
    for pid in pids:
        subprocess.run(["taskkill", "/PID", pid, "/T", "/F"], capture_output=True)
    os.remove(PIDS)


def main():
    stop()
    if "--stop" in sys.argv:
        return
    os.makedirs(D, exist_ok=True)
    run_pair.make_settings(D, (40, 40), 8083, mute=True)
    start = os.path.join(D, "start.lua")
    with open(start, "w") as f:
        f.write("MENU_DIR = [[%s]]\ndofile([[%s]])\n" % (D.replace("\\", "/"),
                                                         os.path.join(HERE, "menu_auto.lua").replace("\\", "/")))
    with open(os.path.join(D, "ctl.lua"), "w") as f:
        f.write("-- nothing yet\n")
    p = subprocess.Popen([r"C:\pcsx-redux-23726-20260420-7\pcsx-redux.exe", "-portable", D, "-loadiso", run_pair.ISO,
                          "-dofile", start, "-run"], cwd=D, creationflags=0x00000008 | 0x00000200)
    time.sleep(5)
    pids = [str(p.pid)] + run_pair.children([str(p.pid)])
    with open(PIDS, "w") as f:
        f.write(" ".join(pids))
    print("started", " ".join(pids))


if __name__ == "__main__":
    main()
