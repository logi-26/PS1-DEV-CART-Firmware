"""Runs toca2_cycle.lua groups, each in a freshly booted Redux driven into a 2P race (../dd/nav_toca2_race.sh);
a group is retried (up to 3 times) when Redux closes (it does, every few races, with Lua breakpoints).
  python cyc_driver.py GROUPS.json [--lua "FIXED_STEP = true; SEED_SETUP = true"]
GROUPS.json: {"G1": [["M", 30]], "G3": [["F", 30, {"pads": false, "frames": 300}], ["M", 30]]}
Each run's results are moved to ../dd/.redux_t/groups/<group>/cyc_<tag>*."""
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DD = os.path.join(HERE, "..", "dd")
D = os.path.join(DD, ".redux_t")
LOG = os.path.join(D, "dd.log")
CYCLE = os.path.join(HERE, "toca2_cycle.lua").replace("\\", "/")


def alive():
    out = subprocess.run(["tasklist"], capture_output=True, text=True).stdout.lower()
    return "pcsx-redux" in out


def lua_run(tag, delay, opts):
    fields = ["'%s'" % tag, str(delay)]
    for k, v in opts.items():
        fields.append("%s = %s" % (k, json.dumps(v) if not isinstance(v, bool) else str(v).lower()))
    return "{" + ", ".join(fields) + "}"


def run_group(name, runs, extra):
    subprocess.run([sys.executable, "dd_run.py", "--stop", "t"], cwd=DD)
    subprocess.run(["sh", "nav_toca2_race.sh"], cwd=DD, capture_output=True)
    if not alive():
        return False
    for f in os.listdir(D):
        if f.startswith("cyc_"):
            os.remove(os.path.join(D, f))
    start = len(open(LOG).read())
    lua = "RUNS = {%s}; %s; dofile([[%s]]) -- %s\n" % (
        ", ".join(lua_run(*r[:2], r[2] if len(r) > 2 else {}) for r in runs), extra, CYCLE, time.time())
    with open(os.path.join(D, "ctl.lua"), "w") as f:
        f.write(lua)
    t0 = time.time()
    while time.time() - t0 < 200 * len(runs) + 60:
        time.sleep(3)
        if "cycle all done" in open(LOG).read()[start:]:
            dest = os.path.join(D, "groups", name)
            os.makedirs(dest, exist_ok=True)
            for f in os.listdir(D):
                if f.startswith("cyc_"):
                    shutil.move(os.path.join(D, f), os.path.join(dest, f))
            return True
        if not alive():
            print(name, "Redux closed", flush=True)
            return False
    print(name, "timed out", flush=True)
    return False


def main():
    groups = json.load(open(sys.argv[1]))
    extra = sys.argv[sys.argv.index("--lua") + 1] if "--lua" in sys.argv else ""
    for name, runs in groups.items():
        for attempt in range(3):
            if run_group(name, runs, extra):
                print(name, "done", flush=True)
                break
        else:
            print(name, "FAILED 3 times", flush=True)
    subprocess.run([sys.executable, "dd_run.py", "--stop", "t"], cwd=DD)


if __name__ == "__main__":
    main()
