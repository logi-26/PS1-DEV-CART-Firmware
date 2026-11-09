"""
Live test of the Movie Tools page's "Test on the PS1" with a console (the movie tester on it, its movie from this
computer's file server): the page driven as a user would, what the tester reports checked. Every scenario runs to its
end (a failed check is noted, the next ones still run), then a summary.

    python tests/movie/live_movie_tools.py [--cartridge NAME]

Scenarios: a RAM movie (start, pace, pause / play, step, restart, colours, picture size, loop, Stop: the console's
movie stopped), a streamed movie (2x, read while it plays: its pace kept, the frames not decoded in time dropped as
designed), a 1x movie, no USB error.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import argparse
import os
import subprocess
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
GUI = os.path.join(ROOT, "gui")
sys.path.insert(0, HERE)
sys.path.insert(0, GUI)
os.chdir(GUI)

import importlib.util                               # noqa: E402

from moviehost import TMP, make_movie               # noqa: E402

spec = importlib.util.spec_from_file_location("gui_main", os.path.join(GUI, "ps1devcart_gui.py"))
gui_main = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gui_main)


def tester_symbols():
    """Addresses of the tester's variables (its symbols, videotest.elf next to ps1transfer)."""
    nm = os.path.join(ROOT, "buildtools", "psn00bsdk", "bin", "mipsel-none-elf-nm")
    out = subprocess.run([nm, os.path.join(ROOT, "ps1transfer", "videotest.elf")], capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        f = line.split()
        if len(f) == 3 and f[2] in ("playing", "paused", "frames_shown", "ended"):
            syms[f[2]] = int(f[0], 16) & 0xFFFFFFFF
    return syms


def peek(cart, adrs):
    exe = os.path.join(ROOT, "ps1transfer", "ps1transfer.exe")
    r = subprocess.run([exe, "-n", cart, "-peek", "0x%08X" % adrs, "-s", "4"], capture_output=True, text=True)
    for line in r.stdout.splitlines():
        f = line.split()
        if f and f[0].upper() == "%08X" % adrs:
            return int("".join(reversed(f[1:5])), 16)
    return None


class Live:
    def __init__(self, app, page):
        self.app, self.p = app, page
        self.results, self.scenario = [], ""
        self.reset()
        orig = page._tester_line

        def spy(line):
            self.seen(line)
            orig(line)
        page._tester_line = spy

    def reset(self):
        self.pos, self.info, self.opened, self.errors, self.ends, self.history = None, None, None, [], 0, []

    def seen(self, line):
        if "VIDTEST" in line and not line.startswith("VIDTEST"):
            line = line[line.index("VIDTEST"):]
        f = line.split()
        try:
            if line.startswith("VIDTEST pos"):
                k = ("frame", "frames", "decode_us", "bytes", "scale", "stalls", "late", "dropped", "buf")
                self.pos = dict(zip(k, (int(x) for x in f[2:11])))
                self.pos["t"] = time.time()
                self.history.append(self.pos)
            elif line.startswith("VIDTEST info"):
                self.info = (int(f[2]), int(f[3]), int(f[4]), int(f[5]))
            elif line.startswith("VIDTEST open"):
                self.opened = f[2:]
            elif line.startswith("VIDTEST end"):
                self.ends += 1
            elif line.startswith("VIDTEST error"):
                self.errors.append(line)
        except (ValueError, IndexError):
            pass

    def check(self, name, ok, detail=""):
        self.results.append((self.scenario, name, bool(ok), detail))
        print("%-5s %s: %s %s" % ("ok" if ok else "FAIL", self.scenario, name, detail), flush=True)


def wait(cond, timeout):
    return ("until", cond, timeout)


def sleep(s):
    return ("sleep", s)


def start(live, path, mode="Auto", loop=False, speed="From the movie"):
    p = live.p
    live.reset()
    p.test_file.set(path) if hasattr(p.test_file, "set") else p.test_file.var.set(path)
    p.test_mode.set(mode)
    p.test_speed.set(speed)
    p.test_loop.set(loop)
    p.test_depth.set("15 bits (with the information)")
    p.test_scale.set("Fit the screen")
    p.test_play()
    yield wait(lambda: live.pos is not None and live.pos["frame"] > 0, 60)
    live.check("tester playing", live.pos is not None and live.pos["frame"] > 0, "opened %s" % live.opened)


def frame_rate(live, seconds):
    a = live.pos
    yield sleep(seconds)
    b = live.pos
    live._fps = (b["frame"] - a["frame"]) / max(0.1, b["t"] - a["t"]) if a and b else 0


def stop(live):
    live.p.test_stop()
    yield wait(lambda: not live.p._tester_running(), 15)
    yield sleep(3)


def scenario_ram(live, path, fps, syms, cart):
    yield from start(live, path, "RAM", loop=False)
    live.check("RAM mode", live.opened and live.opened[-1] == "ram", str(live.opened))
    yield from frame_rate(live, 1.0)
    live.check("pace", abs(live._fps - fps) < fps * 0.2, "%.1f frames a second (the movie: %g)" % (live._fps, fps))
    live.check("information", live.info and live.info[0] == 320 and live.info[2] == 37800, str(live.info))
    live.p.send("toggle")
    yield sleep(0.6)
    a = live.pos["frame"]
    yield sleep(1.0)
    live.check("paused", live.pos["frame"] == a, "frame %d -> %d" % (a, live.pos["frame"]))
    live.p.send("step")
    yield sleep(1.0)
    live.check("next frame (paused)", live.pos["frame"] == a + 1, "frame %d -> %d" % (a, live.pos["frame"]))
    live.p.send("toggle")
    yield from frame_rate(live, 0.8)
    live.check("played again", live._fps > fps * 0.6, "%.1f frames a second" % live._fps)
    live.p.send("restart")
    live.p.test_depth.set("24 bits (as games: no drawing)")
    live.p.test_depth.event_generate("<<ComboboxSelected>>")
    yield sleep(0.6)
    yield from frame_rate(live, 1.0)
    live.check("24 bits", live._fps > fps * 0.7 and not live.errors, "%.1f frames a second" % live._fps)
    live.p.test_depth.set("15 bits (with the information)")
    live.p.test_depth.event_generate("<<ComboboxSelected>>")
    for scale in ("1x", "Full screen", "Fit the screen"):
        live.p.test_scale.set(scale)
        live.p.test_scale.event_generate("<<ComboboxSelected>>")
        yield sleep(0.7)
    live.p.send("restart")
    yield sleep(1.2)
    live.check("restart", live.pos["frame"] < fps * 1.6, "frame %d" % live.pos["frame"])
    yield wait(lambda: live.ends > 0, 15)
    live.check("the end", live.ends > 0)
    live.check("no late or dropped frames (RAM)", live.pos["late"] == 0 and live.pos["dropped"] == 0, str(live.pos))
    # Loop on: played again after its end
    live.p.test_loop.set(True)
    live.p.send("loop 1")
    live.p.send("restart")
    yield wait(lambda: live.ends > 1, 15)
    yield sleep(1.5)
    live.check("loop: played again", live.ends > 1 and 0 < live.pos["frame"] < fps * 2, "frame %d" % live.pos["frame"])
    live.check("no tester errors", not live.errors, "; ".join(live.errors))
    yield from stop(live)
    a = peek(cart, syms["frames_shown"])
    yield sleep(1.0)
    b = peek(cart, syms["frames_shown"])
    playing = peek(cart, syms["playing"])
    live.check("Stop: the console's movie stopped", playing == 0 and a == b,
               "playing %s, frames shown %s -> %s" % (playing, a, b))


def scenario_stream(live, path, fps, speed):
    yield from start(live, path, "Stream", speed="%dx" % speed)
    live.check("stream mode", live.opened and live.opened[-1] == "stream", str(live.opened))
    yield from frame_rate(live, 4)
    shown_rate = live._fps
    live.check("pace kept (frames shown or dropped)", abs(shown_rate - fps) < fps * 0.15,
               "%.1f frames a second" % shown_rate)
    yield wait(lambda: live.ends > 0, 30)
    live.check("the end", live.ends > 0)
    dropped = live.pos["dropped"] if live.pos else -1
    live.check("dropped frames reported", dropped >= 0, "late %s dropped %s stalls %s" % (
        live.pos and live.pos["late"], dropped, live.pos and live.pos["stalls"]))
    live.check("no tester errors", not live.errors, "; ".join(live.errors))
    yield from stop(live)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cartridge", default="BILL")
    args = ap.parse_args()
    folder = os.path.join(TMP, "live")
    os.makedirs(folder, exist_ok=True)
    print("Making the test movies...", flush=True)
    ram, _ = make_movie(os.path.join("live", "live_ram.str"), seconds=3, fps=15)
    stream2, _ = make_movie(os.path.join("live", "live_stream2.str"), seconds=8, fps=15)
    stream1, _ = make_movie(os.path.join("live", "live_stream1.str"), seconds=8, fps=15, speed=1, rate=18900,
                            stereo=False)
    syms = tester_symbols()
    app = gui_main.App()
    app.geometry("1400x950+0+0")
    app.show_page("Movie Tools")
    page = app.pages["Movie Tools"]
    page.tabs.select(len(page.tabs.tabs()) - 1)
    live = Live(app, page)
    errors = []
    app.report_callback_exception = lambda *a: (errors.append("".join(traceback.format_exception(*a))),
                                                print(errors[-1], flush=True))
    scenarios = [("RAM movie", scenario_ram, (ram, 15, syms, args.cartridge)),
                 ("streamed 2x", scenario_stream, (stream2, 15, 2)),
                 ("streamed 1x", scenario_stream, (stream1, 15, 1))]

    def runner():
        for name, fn, a in scenarios:
            live.scenario = name
            try:
                yield from fn(live, *a)
            except Exception:
                live.check("ran", False, traceback.format_exc())
                yield from stop(live)

    gen = runner()

    def step():
        try:
            item = next(gen)
        except StopIteration:
            return finish()
        if item[0] == "sleep":
            app.after(int(item[1] * 1000), step)
        else:
            _, cond, timeout = item
            deadline = time.time() + timeout

            def poll():
                if cond() or time.time() > deadline:
                    step()
                else:
                    app.after(100, poll)
            poll()

    def finish():
        log = app.log.get("1.0", "end")
        live.scenario = "USB link"
        live.check("no USB error in the log", "USB error" not in log)
        live.check("no Python errors", not errors, "\n".join(errors)[:1500])
        bad = [r for r in live.results if not r[2]]
        print("\n%d checks, %d failed" % (len(live.results), len(bad)))
        for r in bad:
            print("  FAIL %s: %s %s" % (r[0], r[1], r[3]))
        app.after(500, app.destroy)

    app.after(8000, step)
    app.mainloop()


if __name__ == "__main__":
    main()
