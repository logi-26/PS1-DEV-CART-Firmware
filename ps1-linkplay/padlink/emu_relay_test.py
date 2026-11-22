"""The whole pad play chain without consoles: the lobby server, N emulators (padlink_emu.lua: the kernel hook's
exchange) and one padrelay.py per emulator, as the Manager would run them: emulator a's relay hosts a room for
--max players, the others join it; the host starts with those who joined (--start-at: "Start now" when that many
are in, else when the room is full). The relays install the driver, set the game up and carry the frames; each one
compares the state hashes of every player (DESYNC lines).
  python emu_relay_test.py [--players 3] [--max 4] [--settings JSON] [--seconds 150] [--leave P:SECONDS]
  python emu_relay_test.py --stop
Logs: research/dd/.redux_X/relay.log (each relay's lines), the server's in .server.log here."""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DD = os.path.join(HERE, "..", "research", "dd")
NAMES = "abcd"
PORT = 7098


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--players", type=int, default=2)
    ap.add_argument("--max", type=int, default=0)
    ap.add_argument("--settings", default='{"mode": "vs", "level": 3, "laps": 3}')
    ap.add_argument("--seconds", type=float, default=150)
    ap.add_argument("--leave", default="", help="P:S - player P's relay quits S seconds into the game")
    ap.add_argument("--keep", action="store_true", help="leave the emulators running at the end")
    ap.add_argument("--stop", action="store_true")
    a = ap.parse_args()
    run = [sys.executable, os.path.join(DD, "dd_run.py")]
    if a.stop:
        for n in NAMES:
            subprocess.run(run + ["--stop", n], cwd=DD)
        return
    n, mx = a.players, a.max or a.players
    names = NAMES[:n]
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, "..", "lobby", "netplay_server.py"), "--port", str(PORT)],
                           stdout=open(os.path.join(HERE, ".server.log"), "w"), stderr=subprocess.STDOUT)
    dirs = [os.path.join(DD, ".redux_" + x) for x in names]
    for name, d in zip(names, dirs):
        subprocess.run(run + ["--stop", name], cwd=DD)
        for f in ("pl_cmd.txt", "pl_ans.txt"):
            if os.path.exists(os.path.join(d, f)):
                os.remove(os.path.join(d, f))
        subprocess.run(run + [name, "--game", "ctr"], cwd=DD, check=True)
    for name, d in zip(names, dirs):
        with open(os.path.join(d, "ctl.lua"), "w") as f:
            f.write('TAG = "%s"\nPL_LUA = [[%s]]\ndofile([[%s]])\nreturn "%s %f"\n'
                    % (name.upper(), os.path.join(HERE, "padlink_ctr.lua").replace("\\", "/"),
                       os.path.join(HERE, "padlink_emu.lua").replace("\\", "/"), name, time.time()))
    time.sleep(2)
    relays, logs, controls = [], [], []
    chars = [0, 3, 5, 7]
    for i, (name, d) in enumerate(zip(names, dirs)):
        ctl = os.path.join(d, "relay_control.txt")
        open(ctl, "w").close()
        args = [sys.executable, os.path.join(HERE, "..", "..", "gui", "padrelay.py"), "--lobby", "127.0.0.1:%d" % PORT,
                "--name", "EMU_" + name.upper(), "--emu", d, "--char", str(chars[i]), "--script", str(i + 1),
                "--control", ctl]
        args += ["--host", str(mx), "--settings", a.settings] if i == 0 else ["--join", "1"]
        lf = open(os.path.join(d, "relay.log"), "w")
        relays.append(subprocess.Popen(args, stdout=lf, stderr=subprocess.STDOUT))
        logs.append(os.path.join(d, "relay.log"))
        controls.append(ctl)
        time.sleep(1.0)
    if n < mx:                                    # "Start now" with those who joined
        time.sleep(2)
        with open(controls[0], "a") as f:
            f.write("start\n")
    print("%d emulators, room for %d, %s" % (n, mx, a.settings), flush=True)
    t0 = time.time()
    left = False
    while time.time() - t0 < a.seconds:
        time.sleep(2)
        if a.leave and not left:
            p, s = a.leave.split(":")
            started = "SET UP" in open(logs[0]).read()
            if started and time.time() - t0 > float(s):
                with open(controls[int(p) - 1], "a") as f:
                    f.write("quit\n")
                left = True
                print("player %s quits" % p, flush=True)
        if any(r.poll() is not None for r in relays if relays.index(r) != (int(a.leave.split(":")[0]) - 1 if a.leave else -1)):
            break
    for r in relays:
        r.terminate()
    srv.terminate()
    if not a.keep:
        for name in names:
            subprocess.run(run + ["--stop", name], cwd=DD)
    for name, lf in zip(names, logs):
        print("--- relay %s" % name)
        print(open(lf).read()[-2500:])


if __name__ == "__main__":
    main()
