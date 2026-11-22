"""Two PCSX-Redux running Red Alert Retaliation, linked by the virtual link cable (vlink.c / vlink.lua) instead of
the emulated serial port: each emulator's Lua script patches the game's serial library and passes the bytes and
lines through two files (a2b_<time>.bin, b2a_<time>.bin, new for each start). Logs: .vlink_a/vlink.log, .vlink_b/vlink.log.

  python run_pair.py [--iso CUE] [--redux EXE] [--only A]
  python run_pair.py --stop          (closes the emulators it started, and only those)
"""
import argparse, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ISO = os.path.expanduser(r"~\Desktop\ps1usb\Command & Conquer - Red Alert - Retaliation (Europe) (Disc 1) (Allies)"
       r"\Command & Conquer - Red Alert - Retaliation (Europe) (Disc 1) (Allies).cue")


def find_key(obj, key):
    if isinstance(obj, dict):
        if key in obj:
            return obj
        for v in obj.values():
            r = find_key(v, key)
            if r is not None:
                return r
    return None


def make_settings(dest_dir, pos, web_port, mute):
    base = os.path.join(os.environ["APPDATA"], "pcsx-redux", "pcsx.json")
    with open(base, encoding="utf-8") as f:
        s = json.load(f)
    dbg = find_key(s, "SIO1Server")
    dbg.update(SIO1Server=False, SIO1Client=False, WebServer=True, WebServerPort=web_port, GdbServer=False,
               Debug=True, PCdrv=False)
    emu = find_key(s, "Dynarec")
    emu.update(Dynarec=False)
    w = find_key(s, "WindowPosX")
    if w is not None:
        w.update(WindowPosX=pos[0], WindowPosY=pos[1], WindowMaximized=False)
    if mute:
        spu = find_key(s, "Mute")
        if spu is not None:
            spu.update(Mute=True)
    os.makedirs(dest_dir, exist_ok=True)
    with open(os.path.join(dest_dir, "pcsx.json"), "w", encoding="utf-8") as f:
        json.dump(s, f, indent=2)


PIDS = os.path.join(HERE, "pids.txt")


def stop_pair():
    """Close the emulators this script started last (by their process IDs only)."""
    try:
        with open(PIDS) as f:
            pids = f.read().split()
    except OSError:
        return
    for pid in pids:            # (/T: the emulator itself, pcsx-redux.main, is a child of pcsx-redux.exe)
        subprocess.run(["taskkill", "/PID", pid, "/T", "/F"], capture_output=True)
    os.remove(PIDS)


def children(pids):
    """The pcsx-redux.main started by these launchers (pcsx-redux.exe)."""
    ps = ("Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'pcsx-redux.main*' -and "
          "@(%s) -contains $_.ParentProcessId } | ForEach-Object { $_.ProcessId }" % ",".join(pids))
    r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True)
    return r.stdout.split()


def lua_str(path):
    return "[[" + path + "]]"


def main():
    if "--stop" in sys.argv:
        stop_pair()
        return
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default=ISO)
    ap.add_argument("--redux", default=r"C:\pcsx-redux-23726-20260420-7\pcsx-redux.exe")
    ap.add_argument("--only", choices=("A", "B"), help="start one emulator only (tests)")
    ap.add_argument("--hw", action="store_true", help="the PS1 DEV CART build (vlink_hw.json) instead of vlink.bin")
    a = ap.parse_args()
    stop_pair()                 # (never two pairs at once: the last one started by this script is closed first)
    # Files of this run only (an emulator left from another run can't write into them); older ones removed
    for old in os.listdir(HERE):
        if old.startswith(("a2b", "b2a")) and old.endswith(".bin"):
            try:
                os.remove(os.path.join(HERE, old))
            except OSError:
                pass            # (still open: an emulator not closed)
    run = time.strftime("%H%M%S")
    a2b, b2a = os.path.join(HERE, "a2b_%s.bin" % run), os.path.join(HERE, "b2a_%s.bin" % run)
    for p in (a2b, b2a):
        open(p, "wb").close()
    procs = []
    for name, pos, port, out, inp in (("A", (40, 40), 8081, a2b, b2a), ("B", (900, 40), 8082, b2a, a2b)):
        if a.only and name != a.only:
            continue
        d = os.path.join(HERE, ".vlink_" + name.lower())
        make_settings(d, pos, port, mute=(name == "B"))
        start = os.path.join(d, "start.lua")
        with open(start, "w") as f:
            if a.hw:
                with open(os.path.join(HERE, "vlink_hw.json")) as j:
                    m = json.load(j)
                gaps = ", ".join("{ adrs = %d, hex = '%s' }" % (g["adrs"], g["hex"]) for g in m["gaps"])
                e = m["entries"]
                words = ", ".join("{ adrs = %d, orig = %d, new = %d }" % (w["adrs"], w["orig"], w["new"])
                                  for w in m.get("words", []))
                f.write("VLINK_HW = { buf = %d, rx_size = %d, tx_size = %d, service = %d, control = %d, read = %d, "
                        "write = %d, gaps = { %s }, words = { %s } }\n" % (m["buf"], m["rx_size"], m["tx_size"],
                                                         m["service"], e["vlink_control"]["target"],
                                                         e["vlink_read"]["target"], e["vlink_write"]["target"], gaps,
                                                         words))
            f.write("VLINK_NAME = '%s'\nVLINK_BIN = %s\nVLINK_OUT = %s\nVLINK_IN = %s\nVLINK_LOG = %s\ndofile(%s)\n" % (
                name, lua_str(os.path.join(HERE, "vlink.bin")), lua_str(out), lua_str(inp),
                lua_str(os.path.join(d, "vlink.log")), lua_str(os.path.join(HERE, "vlink.lua"))))
        procs.append(subprocess.Popen([a.redux, "-portable", d, "-loadiso", a.iso, "-dofile", start, "-run"], cwd=d,
                                      creationflags=0x00000008 | 0x00000200))
        time.sleep(2)
    launchers = [str(p.pid) for p in procs]
    time.sleep(3)
    pids = launchers + children(launchers)     # (both: closing a launcher may not close its emulator)
    with open(PIDS, "w") as f:
        f.write(" ".join(pids))
    print("started pids %s (python run_pair.py --stop closes them)" % " ".join(pids), flush=True)


if __name__ == "__main__":
    main()
