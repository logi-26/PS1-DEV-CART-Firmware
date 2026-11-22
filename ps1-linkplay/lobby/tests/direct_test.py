"""Lobby flow test on one PC, with logs and traces: BEN's half hosts and carries BEN from the start; once BEN offers
a game (LINK GAME chosen on BEN), BILL's half joins 127.0.0.1:7030. Stop: "stop 1" in both tune files."""
import os, subprocess, sys, time

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "gui"))
import netplay  # noqa: E402

EXE = os.path.join(REPO, "ps1transfer", "ps1transfer.exe")
OUT = os.path.dirname(os.path.abspath(__file__))
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
words = netplay.sym_words(pkg)


def half(serial, role):
    d = os.path.join(OUT, "half_" + serial)
    os.makedirs(d, exist_ok=True)
    tune = os.path.join(d, "tune.txt")
    open(tune, "w").write("hold 1\npcopy 1\ntrace 300\n")
    for f in ("vlink_trace.txt",):
        if os.path.exists(os.path.join(d, f)):
            os.remove(os.path.join(d, f))
    log = open(os.path.join(d, "relay.log"), "w", buffering=1)
    p = subprocess.Popen([EXE, "-n", serial] + role + ["-vlinksym", words, "-vlinktune", tune], cwd=d,
                         stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    print("%s %s half started (%s)" % (time.strftime("%H:%M:%S"), serial, " ".join(role)), flush=True)
    return p


ben = half("BEN", ["-vlinkhost", "7030"])
benlog = os.path.join(OUT, "half_BEN", "relay.log")
print("%s choose LINK GAME on BEN now" % time.strftime("%H:%M:%S"), flush=True)
while "VLINK OFFER" not in open(benlog).read():
    if ben.poll() is not None:
        sys.exit("BEN's half ended")
    time.sleep(0.2)
delay = int(sys.argv[1]) if len(sys.argv) > 1 else 0
print("%s BEN offers a game: BILL joins in %d s" % (time.strftime("%H:%M:%S"), delay), flush=True)
time.sleep(delay)
bill = half("BILL", ["-vlinkjoin", "127.0.0.1:7030"])
ben.wait()
bill.wait()
print("%s both halves ended: %s %s" % (time.strftime("%H:%M:%S"), ben.returncode, bill.returncode), flush=True)
