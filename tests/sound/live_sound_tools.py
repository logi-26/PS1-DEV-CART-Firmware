"""
Live test of the Sound Tools page with a console (the sound tester on it, through ps1transfer -sndlive): the page
driven as a user would, what the console reports checked. Every scenario is run to its end (a failed check is noted,
the next ones still run), then a summary.

    python tests/sound/live_sound_tools.py [--corpus DIR]    (DIR: the folder holding Alundra/, Raw/, test-audio/...)

Scenarios: VH + VB + SEQ (start, time going at the right speed, seek, pause / resume, tempo, transpose, mutes, reverb,
the VH read back, stop), the music's end (played again or stopped), a SEP (its SEQs chosen), a big bank (reverb with
no room), a VAG alone, a VAB file alone (programs played), the USB link (no error all along).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import argparse
import os
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
GUI = os.path.normpath(os.path.join(HERE, "..", "..", "gui"))
sys.path.insert(0, GUI)
os.chdir(GUI)

import importlib.util                               # noqa: E402

spec = importlib.util.spec_from_file_location("gui_main", os.path.join(GUI, "ps1devcart_gui.py"))
gui_main = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gui_main)
import vab as vabmod                                 # noqa: E402
from pages.sound import REVERBS, SPU_RAM             # noqa: E402


class Live:
    """The page and what the console reported (SND lines), with the checks' results."""

    def __init__(self, app, page):
        self.app, self.p = app, page
        self.results, self.t0 = [], time.time()
        self.reset()
        orig = page.line

        def spy(line):
            self.seen(line)
            orig(line)
        page.line = spy

    def reset(self):
        self.pos, self.state, self.chan, self.info, self.vh_hex = None, None, None, None, None
        self.errors, self.fx = [], None

    def seen(self, line):
        if "SND" in line and not line.startswith("SND"):
            line = line[line.index("SND"):]
        f = line.split()
        try:
            if line.startswith("SNDPOS"):
                self.pos = (int(f[1]), int(f[2]), int(f[3]), time.time())
            elif line.startswith("SNDSTATE"):
                self.state = dict(seq=int(f[6]), voices=int(f[9], 16), bar=int(f[7]))
            elif line.startswith("SNDCHAN"):
                self.chan = dict(rev=int(f[1]), used=int(f[2], 16), active=int(f[3], 16),
                                 sep=int(f[20]) if len(f) > 21 else 0, seps=int(f[21]) if len(f) > 21 else 0)
            elif line.startswith("SNDINFO"):
                self.info = dict(kind=int(f[1]), programs=int(f[2]))
            elif line.startswith("SNDFX"):
                self.fx = (int(f[1]), int(f[2], 16))
            elif line.startswith("SNDVH"):
                self.vh_hex = f[1] if len(f) > 1 else ""
            elif line.startswith("SNDERR"):
                self.errors.append(line)
        except (ValueError, IndexError):
            pass

    def check(self, name, ok, detail=""):
        self.results.append((self.scenario, name, bool(ok), detail))
        print("%-5s %s: %s %s" % ("ok" if ok else "FAIL", self.scenario, name, detail), flush=True)

    def send(self, line):
        self.p.send(line)


def wait(cond, timeout):
    """('until', condition, timeout s): a step of a scenario waiting for the console."""
    return ("until", cond, timeout)


def sleep(s):
    return ("sleep", s)


def start(live, files):
    """The files set on the page and the tester started; waits for it to be live."""
    p = live.p
    live.reset()
    p.files.listbox.delete(0, "end")
    for f in files:
        p.files.listbox.insert("end", f)
    for attempt in range(2):            # (once more if the Manager was busy with the console)
        p.play()
        yield wait(lambda: p.live and live.fx is not None and live.state is not None, 40)
        if p.live:
            break
        live.app.runner.stop()
        yield sleep(5)
    live.check("tester live", p.live and live.fx is not None, "(%d files)" % len(files))
    yield sleep(0.5)


def stop(live):
    live.app.runner.stop()
    yield wait(lambda: not live.p.live, 15)
    yield sleep(2.5)


def rate(live, seconds):
    """How fast the music's time goes (1.0: real time), measured over some seconds."""
    a = live.pos
    yield sleep(seconds)
    b = live.pos
    live._rate = (b[0] - a[0]) / 1000.0 / max(0.1, b[3] - a[3]) if a and b else 0.0


def scenario_seq(live, vh, vb, seq):
    yield from start(live, [vh, vb, seq])
    live.check("VAB loaded", live.info and live.info["kind"] == 1, str(live.info))
    live.send("seq 1")
    yield wait(lambda: live.state and live.state["seq"] == 2, 5)
    live.check("SEQ playing", live.state and live.state["seq"] == 2)
    yield sleep(1.5)
    live.check("notes sounding", live.state and live.state["voices"] != 0, "voices %s" % (live.state or {}).get("voices"))
    yield from rate(live, 5)
    live.check("time at real speed", 0.93 < live._rate < 1.07, "rate %.3f" % live._rate)
    length = live.pos[1] if live.pos else 0
    target = min(60000, length // 2)
    live.send("seek %d" % target)
    yield sleep(1.0)
    live.check("seek", live.pos and target <= live.pos[0] <= target + 1600, "asked %d, at %s" % (
        target, live.pos and live.pos[0]))
    live.send("pause 1")
    yield sleep(1.0)
    a = live.pos[0]
    yield sleep(2.0)
    live.check("paused: time stopped", live.pos[2] == 1 and live.pos[0] == a, "pos %d -> %d" % (a, live.pos[0]))
    live.send("pause 0")
    yield sleep(0.5)
    yield from rate(live, 3)
    live.check("resumed", live.pos[2] == 0 and 0.9 < live._rate < 1.1, "rate %.3f" % live._rate)
    # Effects: reverb, depth, voices, transpose, tempo, mono, mute
    live.send("fx 0 64 0 0 200 0 0")
    yield sleep(0.5)
    yield from rate(live, 3)
    live.check("tempo 200%", 1.85 < live._rate < 2.15, "rate %.3f" % live._rate)
    live.send("fx 0 64 0 12 100 1 0")
    yield sleep(1.5)
    live.check("transpose + mono: still playing", live.state["seq"] == 2 and not live.errors)
    live.send("fx 0 64 0 0 100 0 65535")
    yield sleep(1.5)
    live.check("all channels muted: none sounding", live.chan and live.chan["active"] == 0,
               "active %04X" % (live.chan or {}).get("active", -1))
    live.send("fx 0 64 0 0 100 0 0")
    yield sleep(1.5)
    live.check("unmuted: channels sounding", live.chan and live.chan["active"] != 0)
    samples_end = live.fx[1]
    for rev in (5, 8, 0):
        live.send("fx %d 64 0 0 100 0 0" % rev)
        yield sleep(1.5)
        fits = (SPU_RAM - REVERBS[rev][1]) & ~1023 >= samples_end
        want = rev if (fits or rev == 0) else -1
        live.check("reverb %s" % REVERBS[rev][0], live.chan and live.chan["rev"] == want,
                   "reported %s, expected %d (samples to %05X)" % ((live.chan or {}).get("rev"), want, samples_end))
    live.send("vabread")
    yield wait(lambda: live.vh_hex is not None, 5)
    size = vabmod.Vab.load(vh).vh_size()
    live.check("VH read back", live.vh_hex is not None and len(live.vh_hex) == 2 * size,
               "%s bytes, VH %d" % (len(live.vh_hex or "") // 2, size))
    live.send("seq 0")
    yield sleep(1.0)
    live.send("stop")
    yield sleep(1.5)
    live.check("stopped: silent", live.state["seq"] == 1 and live.state["voices"] == 0,
               "seq %d voices %X" % (live.state["seq"], live.state["voices"]))
    live.check("no SNDERR", not live.errors, "; ".join(live.errors))
    yield from stop(live)


def scenario_end(live, *files):
    """A music without an endless loop: at its end played again (repeat) or stopped."""
    yield from start(live, list(files))
    length = live.pos[1] if live.pos else 0
    live.check("length known", length > 3000, "%d ms" % length)
    live.send("repeat 0")
    live.send("seq 1")
    yield sleep(1.0)
    live.send("seek %d" % (length - 1500))
    yield wait(lambda: live.state and live.state["seq"] == 1, 6)
    live.check("played once: stopped at the end", live.state["seq"] == 1, "seq %d" % live.state["seq"])
    live.send("repeat 1")
    live.send("seq 1")
    yield sleep(1.0)
    live.send("seek %d" % (length - 1500))
    yield sleep(3.5)
    live.check("repeat: played again from the start", live.state["seq"] == 2 and live.pos[0] < 4000,
               "seq %d at %d ms" % (live.state["seq"], live.pos[0]))
    live.check("no SNDERR", not live.errors, "; ".join(live.errors))
    yield from stop(live)


def scenario_endless(live, vh, vb, seq):
    """A music with an endless loop (a game's): at its loop end back to the loop start, also with repeat off."""
    yield from start(live, [vh, vb, seq])
    length = live.pos[1] if live.pos else 0
    live.send("repeat 0")
    live.send("seq 1")
    yield sleep(1.0)
    live.send("seek %d" % (length - 1500))
    yield sleep(4.0)
    live.check("endless loop: still playing after its end", live.state["seq"] == 2 and live.pos[0] < length - 1500,
               "seq %d at %d ms (length %d)" % (live.state["seq"], live.pos[0], length))
    live.check("no SNDERR", not live.errors, "; ".join(live.errors))
    yield from stop(live)


def scenario_sep(live, vab_file, sep):
    yield from start(live, [vab_file, sep])
    yield sleep(1.0)
    count = live.chan["seps"] if live.chan else 0
    live.check("SEP: its SEQs", count >= 2, "%d SEQs" % count)
    live.send("seq 1")
    lengths = []
    for i in range(min(count, 4)):
        live.send("sep %d" % i)
        yield sleep(2.0)
        lengths.append(live.pos[1] if live.pos else 0)
        live.check("SEP SEQ %d chosen and playing" % i, live.chan["sep"] == i and live.state["seq"] == 2,
                   "sep %d seq %d len %d" % (live.chan["sep"], live.state["seq"], lengths[-1]))
    live.check("no SNDERR", not live.errors, "; ".join(live.errors))
    yield from stop(live)


def scenario_vag(live, vag):
    yield from start(live, [vag])
    live.check("VAG loaded", live.info and live.info["kind"] == 2, str(live.info))
    live.send("play 60")
    yield sleep(0.6)
    live.check("VAG played", live.state["voices"] & (1 << 23), "voices %X" % live.state["voices"])
    live.send("release")
    live.send("play 72")
    yield sleep(0.4)
    live.check("VAG another note", live.state["voices"] & (1 << 23))
    live.send("release")
    yield sleep(2.0)
    live.check("no SNDERR", not live.errors, "; ".join(live.errors))
    yield from stop(live)


def scenario_vab(live, vab_file):
    yield from start(live, [vab_file])
    n = live.info["programs"] if live.info else 0
    live.check("VAB file loaded", live.info and live.info["kind"] == 1 and n > 0, str(live.info))
    for i in range(min(n, 3)):
        live.send("program %d" % i)
        live.send("play 60")
        sounded = []
        # (as soon as a voice sounds: program 2 of test_bank is a pluck, silent soon after)
        yield wait(lambda: (sounded.append(1) if live.state["voices"] else None) or sounded, 1.5)
        live.check("program %d played" % i, sounded, "voices %X" % live.state["voices"])
        live.send("release")
        yield sleep(0.3)
    live.check("no SNDERR", not live.errors, "; ".join(live.errors))
    yield from stop(live)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default=os.path.normpath(os.path.join(HERE, "..", "..", "..")))
    args = ap.parse_args()
    c = args.corpus
    j = os.path.join
    ta = j(c, "test-audio")
    scenarios = [
        ("Alundra VH+VB+SEQ", scenario_seq, (j(c, "Alundra", "Alundra.vh"), j(c, "Alundra", "Alundra.vb"),
                                             j(c, "Alundra", "Alundra.seq"))),
        ("PoPoLoCrois endless loop", scenario_endless, (j(c, "PoPoLoCrois Monogatari", "Bgm01.vh"),
                                                        j(c, "PoPoLoCrois Monogatari", "Bgm01.vb"),
                                                        j(c, "PoPoLoCrois Monogatari", "Bgm01-05.seq"))),
        ("SDK music end", scenario_end, (j(ta, "psyq-sdk-samples", "BASIC", "SAMPLE.VAB"),
                                         j(ta, "psyq-sdk-samples", "BASIC", "BRAHMS.SEQ"))),
        ("SDK SEP", scenario_sep, (j(ta, "psyq-sdk-samples", "BASIC", "SAMPLE.VAB"),
                                   j(ta, "psyq-sdk-samples", "BASIC", "SAMPLE.SEP"))),
        ("Net Yaroze big bank", scenario_seq, (j(c, "Net Yaroze Software Development Kit", "PSX", "DATA", "SOUND",
                                                 "STD0.VH"),
                                               j(c, "Net Yaroze Software Development Kit", "PSX", "DATA", "SOUND",
                                                 "STD0.VB"),
                                               j(c, "Net Yaroze Software Development Kit", "PSX", "DATA", "SOUND",
                                                 "GOGO.SEQ"))),
        ("VAG alone", scenario_vag, (j(ta, "tone.vag"),)),
        ("VAB file alone", scenario_vab, (j(ta, "test_bank.vab"),)),
    ]
    app = gui_main.App()
    app.geometry("1400x950+0+0")
    app.show_page("Sound Tools") if "Sound Tools" in app.pages else None
    page = next(pg for pg in app.pages.values() if pg.__class__.__name__ == "SoundPage")
    live = Live(app, page)
    errors = []
    app.report_callback_exception = lambda *a: (errors.append("".join(traceback.format_exception(*a))), print(errors[-1], flush=True))

    def runner():
        for name, fn, files in scenarios:
            missing = [f for f in files if not os.path.isfile(f)]
            live.scenario = name
            if missing:
                live.check("files", False, "missing: %s" % missing)
                continue
            try:
                yield from fn(live, *files)
            except Exception:
                live.check("ran", False, traceback.format_exc())
                yield from stop(live)

    gen = runner()

    def step(item=None):
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
        live.check("no USB error in the log", "USB error" not in log,
                   " | ".join(l for l in log.splitlines() if "USB error" in l)[:300])
        live.check("no Python errors", not errors, "\n".join(errors)[:1500])
        bad = [r for r in live.results if not r[2]]
        print("\n%d checks, %d failed" % (len(live.results), len(bad)))
        for r in bad:
            print("  FAIL %s: %s %s" % (r[0], r[1], r[3]))
        app.after(500, app.destroy)

    app.after(8000, step)            # (after the Manager's first look at the console)
    app.mainloop()


if __name__ == "__main__":
    main()
