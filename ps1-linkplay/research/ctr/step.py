"""Research driver for one Redux running CTR (dd_run.py a --game ctr) with ctr_mt.lua (fake multitap, scripted pads).
  python step.py [--dir .redux_a] [--wait N] [--no-dump] NAME "LUA"
LUA: any Lua run in the emulator first (e.g. 'seq({{1,"DOWN"},{1,"X"}})', 'mt(2)'); then, once the sequence is over
and N vertical blanks (default 60) later, a screenshot NAME.png and a RAM dump NAME.ram in the folder."""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DD = os.path.join(HERE, "..", "dd")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=".redux_a")
    ap.add_argument("--wait", type=int, default=60)
    ap.add_argument("--no-dump", action="store_true")
    ap.add_argument("--setup", default="", help="a setup{} table (ctr_setup.lua): applied once the main menu shows")
    ap.add_argument("name")
    ap.add_argument("lua", nargs="?", default="")
    a = ap.parse_args()
    d = os.path.join(DD, a.dir)
    mt = os.path.join(HERE, "ctr_mt.lua").replace("\\", "/")
    st = os.path.join(HERE, "ctr_setup.lua").replace("\\", "/")
    log = os.path.join(d, "dd.log")
    start = os.path.getsize(log)
    if a.setup:
        a.lua += "\nSEQ_DONE = false\nSETUP_PENDING = %s\n" % a.setup
    with open(os.path.join(d, "ctl.lua"), "w") as f:
        f.write("""-- %s %f
dofile([[%s]])
dofile([[%s]])
%s
local t_done = nil
VSYNC_HOOK = function()
  MT_HOOK()
  if SETUP_PENDING and vs >= 3400 and at_main_menu() then
    say('%%s', setup(SETUP_PENDING)); SETUP_PENDING = nil; SEQ_DONE = true
  end
  if SEQ_DONE and not t_done then t_done = vs end
  if t_done and vs == t_done + %d then
    shot('%s'); %s say('STEP %s'); VSYNC_HOOK = function() MT_HOOK() end
  end
end
return 'step %s'
""" % (a.name, time.time(), mt, st, a.lua, a.wait, a.name, "" if a.no_dump else "dump('%s');" % a.name, a.name, a.name))
    t0 = time.time()
    while time.time() - t0 < 300:
        with open(log) as f:
            f.seek(start)
            text = f.read()
        if "STEP %s" % a.name in text:
            break
        if "hook:" in text or "ctl: " in text:
            print(text); sys.exit(1)
        time.sleep(0.3)
    else:
        print("timeout"); print(text); sys.exit(1)
    subprocess.run([sys.executable, os.path.join(DD, "png.py"), d, a.name], check=True)
    print(os.path.join(d, a.name + ".png"))


if __name__ == "__main__":
    main()
