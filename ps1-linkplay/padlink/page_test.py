"""The Manager's Net Play page with pad play, without consoles: two pages (two players' Managers, in a small stub
app) and two emulators (padlink_emu.lua), a lobby server on a spare port. Player A hosts a 4-player battle, B joins
from A's room in its list, A presses Start now with 2; the pages' screenshots are saved here (page_*.png).
  python page_test.py [--seconds 150]"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GUI = os.path.join(HERE, "..", "..", "gui")
DD = os.path.join(HERE, "..", "research", "dd")
sys.path.insert(0, GUI)
PORT = 7097

import ttkbootstrap as tb  # noqa: E402
from PIL import ImageGrab  # noqa: E402

import netplay  # noqa: E402
import pages.netplay as page_mod  # noqa: E402


class Settings:
    def __init__(self):
        self.d = {}

    def field(self, k, default=""):
        return self.d.get(k, default)

    def set_field(self, k, v):
        self.d[k] = v


class Runner:
    task = None
    busy = False


class FakeApp:
    def __init__(self, root, serial):
        self.root, self.s = root, serial
        self.settings = Settings()
        self.runner = Runner()

    def serial(self):
        return self.s

    def after(self, ms, fn, *a):
        return self.root.after(ms, fn, *a)

    def error(self, text):
        print("APP ERROR (%s): %s" % (self.s, text), flush=True)

    def confirm(self, text):
        return True

    def stop_task(self):
        pass

    def exe(self):
        return "ps1transfer"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=150)
    a = ap.parse_args()
    run = [sys.executable, os.path.join(DD, "dd_run.py")]
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, "..", "lobby", "netplay_server.py"), "--port",
                            str(PORT)], stdout=open(os.path.join(HERE, ".server.log"), "w"), stderr=subprocess.STDOUT)
    dirs = {}
    for name in "ab":
        d = dirs[name] = os.path.join(DD, ".redux_" + name)
        subprocess.run(run + ["--stop", name], cwd=DD)
        for f in ("pl_cmd.txt", "pl_ans.txt"):
            if os.path.exists(os.path.join(d, f)):
                os.remove(os.path.join(d, f))
        subprocess.run(run + [name, "--game", "ctr"], cwd=DD, check=True)
        with open(os.path.join(d, "ctl.lua"), "w") as f:
            f.write('TAG = "%s"\nPL_LUA = [[%s]]\ndofile([[%s]])\nreturn "%s %f"\n'
                    % (name.upper(), os.path.join(HERE, "padlink_ctr.lua").replace("\\", "/"),
                       os.path.join(HERE, "padlink_emu.lua").replace("\\", "/"), name, time.time()))
    page_mod.LOBBY_SERVER = "127.0.0.1:%d" % PORT
    root = tb.Window(themename="darkly")
    root.withdraw()
    pkg = netplay.patch_for("SCUS_944.26")
    pages = {}
    for i, name in enumerate("ab"):
        win = tb.Toplevel(title="Manager %s" % name.upper())
        win.geometry("980x900+%d+20" % (20 + 990 * i))
        app = FakeApp(root, "PLAYER_" + name.upper())
        p = page_mod.NetPlayPage(win, app)
        p.pack(fill="both", expand=True)
        p.pad_emu_dir = dirs[name]
        pages[name] = p
        p.on_show()
        p._set_game(pkg, "Crash Team Racing (SCUS_944.26): supported.", "success")
    A, B = pages["a"], pages["b"]
    for label, value in (("Mode", "Battle"), ("Players", "4"), ("Arena", "Skull Rock"), ("Battle type", "Points"),
                         ("Points", "10"), ("Teams", "Every player alone")):
        A.app.settings.set_field("netplay.host.Crash Team Racing.%s" % label, value)
    A.app.settings.set_field("netplay.option.Crash Team Racing.Character", "Coco Bandicoot")
    B.app.settings.set_field("netplay.option.Crash Team Racing.Character", "Coco Bandicoot")  # (taken: offered not)
    t0 = time.time()
    steps = {"host": 3, "join": 8, "shot_room": 12, "start": 14, "shot_play": 100}
    done = set()

    def shot(tag):
        root.update()
        for name, p in pages.items():
            w = p.winfo_toplevel()
            x, y = w.winfo_rootx(), w.winfo_rooty()
            ImageGrab.grab((x, y, x + w.winfo_width(), y + w.winfo_height())).save(
                os.path.join(HERE, "page_%s_%s.png" % (tag, name)))
        print("shot", tag, flush=True)

    def tick():
        t = time.time() - t0
        if t > steps["host"] and "host" not in done:
            done.add("host")
            A._pad_go(None)
        if t > steps["join"] and "join" not in done:
            room = next((g for g in B.games_shown if g.get("players")), None)
            if room:
                done.add("join")
                B.games.selection_set(str(room["id"]))
                B._show_details()
                free = [c for i, c in enumerate(page_mod.ctr_package.CHARACTERS) if i not in room["chars"]]
                B.app.settings.set_field("netplay.option.Crash Team Racing.Character", free[1])
                B._pad_go(room)
        if t > steps["shot_room"] and "shot_room" not in done and "join" in done:
            done.add("shot_room")
            shot("room")
        if t > steps["start"] and "start" not in done and len(A.pad_room) >= 2:
            done.add("start")
            A.start_now()
        if t > steps["shot_play"] and "shot_play" not in done:
            done.add("shot_play")
            shot("play")
        if t > a.seconds:
            for p in pages.values():
                p.stop()
            root.after(3000, root.quit)
            return
        root.after(200, tick)

    root.after(500, tick)
    root.mainloop()
    srv.terminate()
    for name in "ab":
        subprocess.run(run + ["--stop", name], cwd=DD)
    for name, p in pages.items():
        print(name, "state:", p.state.cget("text"), "|", p.stats.cget("text"))


if __name__ == "__main__":
    main()
