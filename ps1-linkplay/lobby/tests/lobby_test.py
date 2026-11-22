"""Lobby test on one PC: the lobby server on 7031; BEN's and BILL's halves in lobby mode (standby with their
consoles); when BEN offers a game and BILL's half lists it, BILL joins it (its tune file, as the page will).
Stop: "stop 1" in stop.txt (then both halves and the server end)."""
import os, subprocess, sys, time

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "gui"))
import netplay  # noqa: E402

EXE = os.path.join(REPO, "ps1transfer", "ps1transfer.exe")
OUT = os.path.dirname(os.path.abspath(__file__))
STOP = os.path.join(OUT, "stop.txt")
open(STOP, "w").write("")
patches = netplay.load_patches()
pkg = None
for serial in ("BEN", "BILL"):
    inst = netplay.Installer(EXE, serial)
    try:
        while True:
            state, text, disc = inst.check(patches)
            print("%s %s: %s" % (time.strftime("%H:%M:%S"), serial, text), flush=True)
            if state == "ok":
                pkg = netplay.patch_for(disc, patches)
                break
            if state == "no":
                sys.exit(1)
            time.sleep(1)
    finally:
        inst.close()

server_log = open(os.path.join(OUT, "lobby_server.log"), "w", buffering=1)
server = subprocess.Popen([sys.executable, "-u", os.path.join(REPO, "ps1-linkplay", "lobby", "netplay_server.py"),
                           "--port", "7031"], stdout=server_log, stderr=subprocess.STDOUT)
time.sleep(1)
halves = {}
for serial in ("BEN", "BILL"):
    d = os.path.join(OUT, "lobby_" + serial)
    os.makedirs(d, exist_ok=True)
    tune = os.path.join(d, "tune.txt")
    open(tune, "w").write("pcopy 1\ntrace 400\n")
    if os.path.exists(os.path.join(d, "vlink_trace.txt")):
        os.remove(os.path.join(d, "vlink_trace.txt"))
    log = open(os.path.join(d, "relay.log"), "w", buffering=1)
    halves[serial] = (subprocess.Popen([EXE, "-n", serial, "-vlinklobby", "127.0.0.1:7031", "-vlinkname", serial,
                                        "-vlinkgame", pkg["title"], "-vlinksym", netplay.sym_words(pkg),
                                        "-vlinktune", tune], cwd=d, stdout=log, stderr=subprocess.STDOUT,
                                       stdin=subprocess.DEVNULL), tune, os.path.join(d, "relay.log"))
    print("%s %s in the lobby" % (time.strftime("%H:%M:%S"), serial), flush=True)
    time.sleep(1.5)      # (the two cartridges not opened at the same moment)
print("%s choose LINK GAME on BEN" % time.strftime("%H:%M:%S"), flush=True)
joined = False
while True:
    if "stop" in open(STOP).read():
        for p, tune, _ in halves.values():
            open(tune, "a").write("stop 1\n")
        break
    if not joined:
        for line in open(halves["BILL"][2]):
            if line.startswith("VLINK GAMES ") and " BEN" in line:
                gid = line.split()[2]
                open(halves["BILL"][1], "a").write("join %s\n" % gid)
                print("%s BILL joins game %s (BEN's): choose LINK GAME on BILL" % (time.strftime("%H:%M:%S"), gid),
                      flush=True)
                joined = True
                break
    time.sleep(0.3)
for p, _, _ in halves.values():
    p.wait()
server.terminate()
print("%s ended" % time.strftime("%H:%M:%S"), flush=True)
