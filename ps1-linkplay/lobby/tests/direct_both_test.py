"""Net Play network test on one PC: install the driver on both consoles (the Manager's installer), then two separate
relay halves over TCP: BEN hosts on port 7030, BILL joins 127.0.0.1:7030. Stop: "stop 1" in the tune file."""
import os, subprocess, sys, time

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "gui"))
import netplay  # noqa: E402

EXE = os.path.join(REPO, "ps1transfer", "ps1transfer.exe")
OUT = os.path.dirname(os.path.abspath(__file__))
TUNE = os.path.join(OUT, "net_tune.txt")
open(TUNE, "w").write("hold 1\npcopy 1\n")
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
procs = []
for serial, role in (("BEN", ["-vlinkhost", "7030"]), ("BILL", ["-vlinkjoin", "127.0.0.1:7030"])):
    log = open(os.path.join(OUT, "net_%s.log" % serial), "w", buffering=1)
    procs.append(subprocess.Popen([EXE, "-n", serial] + role + ["-vlinksym", words, "-vlinktune", TUNE],
                                  stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
    print("%s %s half started (%s)" % (time.strftime("%H:%M:%S"), serial, " ".join(role)), flush=True)
    time.sleep(1)
for p in procs:
    p.wait()
print("%s both halves ended: %s" % (time.strftime("%H:%M:%S"), [p.returncode for p in procs]), flush=True)
