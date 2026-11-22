"""Builds the PS1 DEV CART virtual link driver (vlink_hw.json, used by usb_link.py): vlink.c in the three gaps of
vlink_hw.ld, its rings at 0x8000D810 (the kernel hook's cheat table space, after its magic word: the code engine
stays off). The json holds each gap's bytes and the SHA-1 of the game's original bytes there (checked before
installing), and the three functions the patches jump to.

  python build_hw.py SLES_013.43        (the game's executable, extracted from the disc: for the original bytes)
"""
import hashlib, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.environ.get("MIPS_TOOLS") or os.path.join(os.environ.get("APPDATA", ""), "mips", "mips", "bin")
CC, OBJCOPY, NM = (os.path.join(TOOLS, "mipsel-none-elf-" + t) for t in ("gcc", "objcopy", "nm"))
BUF, RX, TX = 0x8000D810, 128, 128
FLAGS = ["-Os", "-G0", "-march=r3000", "-msoft-float", "-mno-abicalls", "-fno-pic", "-ffreestanding", "-fno-builtin",
         "-nostdlib", "-mno-gpopt", "-fno-toplevel-reorder"]
GAPS = {".a": (0x80153DFC, 0x2DC), ".b": (0x801540E0, 0x274), ".c": (0x801548E4, 0x398)}


def run(*args):
    r = subprocess.run(args, capture_output=True, text=True, cwd=HERE)
    if r.returncode:
        sys.exit(r.stdout + r.stderr)
    return r.stdout


def main():
    exe = open(sys.argv[1], "rb").read()
    orig = lambda a, n: exe[a - 0x80010000 + 0x800:a - 0x80010000 + 0x800 + n]
    elf = os.path.join(HERE, "vlink_hw.elf")
    run(CC, *FLAGS, "-DVLINK_HW", "-DVLINK_BUF=0x%X" % BUF, "-DRX_SIZE=%d" % RX, "-DTX_SIZE=%d" % TX,
        "-T", "vlink_hw.ld", "-Wl,-e,0", "-o", elf, "vlink_head.S", "vlink.c")
    syms = {m.group(3): int(m.group(1), 16) for m in re.finditer(r"^([0-9a-f]+) (\w) (\w+)$", run(NM, elf), re.M)}
    out = {"buf": BUF, "rx_size": RX, "tx_size": TX, "gaps": [], "entries": {}}
    for sec, (adrs, size) in GAPS.items():
        binf = os.path.join(HERE, "vlink_hw%s.bin" % sec.replace(".", "_"))
        run(OBJCOPY, "-O", "binary", "-j", sec, elf, binf)
        data = open(binf, "rb").read()
        os.remove(binf)
        if len(data) > size:
            sys.exit("%s: %d bytes, the gap has %d" % (sec, len(data), size))
        out["gaps"].append({"adrs": adrs, "hex": data.hex(), "size": size,
                            "orig_sha1": hashlib.sha1(orig(adrs, size)).hexdigest()})
        print("%s at %08X: %d of %d bytes" % (sec, adrs, len(data), size))
    stray = run(OBJCOPY, "-O", "binary", "-j", ".stray", elf, os.path.join(HERE, "stray.bin")) or ""
    if os.path.getsize(os.path.join(HERE, "stray.bin")):
        sys.exit("code outside the gaps (a function without IN())")
    os.remove(os.path.join(HERE, "stray.bin"))
    # (_sio_control enters through vlink_auto_entry: Net Play's automatic LINK GAME, see vlink_head.S)
    for name, patch, entry in (("vlink_control", 0x80154354, "vlink_auto_entry"), ("vlink_read", 0x80153DF4, None),
                               ("vlink_write", 0x801540D8, None)):
        out["entries"][name] = {"patch": patch, "target": syms[entry or name], "orig": orig(patch, 16).hex()}
        print("%08X -> %s %08X" % (patch, name, out["entries"][name]["target"]))
    # Single instructions of the game patched (checked against the original): the in-game exchange made symmetric
    # (vlink.c vlink_free_write): its role test (responder: read first) a nop, its write the driver's free write
    jal = lambda t: 0x0C000000 | ((t >> 2) & 0x03FFFFFF)
    out["words"] = []
    for adrs, want, new in ((0x8011528C, 0x1440000B, 0), (0x8011529C, jal(0x8014D740), jal(syms["vlink_free_write"]))):
        was = int.from_bytes(orig(adrs, 4), "little")
        if was != want:
            sys.exit("%08X is %08X, not %08X: another version of the game?" % (adrs, was, want))
        out["words"].append({"adrs": adrs, "orig": was, "new": new})
        print("%08X: %08X -> %08X" % (adrs, was, new))
    # Installed with the driver: the link page's wait for the other player, connect(1500, ...) at 0x80114F20 called
    # from 0x800BF528 with "li a0,1500" in its delay slot: 18000 passes (~10 minutes) for Net Play's lobby; Triangle
    # still cancels it (the game's own handling)
    out["install_words"] = []
    for adrs, want, new in ((0x800BF52C, 0x240405DC, 0x24044650),):
        was = int.from_bytes(orig(adrs, 4), "little")
        if was != want:
            sys.exit("%08X is %08X, not %08X: another version of the game?" % (adrs, was, want))
        out["install_words"].append({"adrs": adrs, "orig": was, "new": new})
        print("%08X: %08X -> %08X (installed)" % (adrs, was, new))
    out["service"] = syms["vlink_service"]
    # Net Play's automatic link play (ps1transfer -vlinkauto, usblink.c VlAuto; found 2026-11-14 in the front end
    # overlay, the menus loaded from DATA.MIX at 0x80010000, see ps1-linkplay/NETPLAY_PLAN.md "Automatic link play"):
    #   1. main menu showing: its menu (0x801A9A94) with its items (0x801A9AE4), shown (+18h = 1000h; 0 in its pages:
    #      it only goes to one once faded out);
    #   2. the link page's values left from a previous visit cleared (its "shown" and START): the game keeps them after
    #      the page (they held at the main menu: a second run wrote START into a page not there, 2026-11-14 on BILL);
    #      the page sets its "shown" to 0 itself when entered, then fades in;
    #   3. armed: the driver's auto word (stats[6]) = the main menu loop's return address from its _sio_control status
    #      call (0x80013024): vlink_auto_entry sets the loop's choice s4 = 4 there, LINK GAME;
    #   4. taken: the word back to 0 (the driver did it; a failure here = the driver's entry not reached);
    #   5. the link page showing: its menu (0x801A9DD8) with its items (0x801A9E28), the START SETUP item's callback
    #      (0x80019804), shown;
    #   6. country (item 0's value, 0x801A9E48) and color (item 1's, 0x801A9E7C) as chosen in the Manager ($0, $1);
    #   7. START SETUP: item 2's value 4 (what its callback returns on X; kept, as the callback returns the value it is
    #      given): the page fades out, copies country and color, raises RTS and waits for the other console.
    auto_word = BUF + 0x168         # (vlink_head.S VLINK_AUTO)
    out["auto_steps"] = ",".join([
        "w801A9A98=801A9AE4/801A9AAC=1000",
        "s801A9DF0=0/801A9EB0=0",
        "a%08X=80013024" % auto_word,
        "w%08X=0" % auto_word,
        "w801A9DDC=801A9E28/801A9EC0=80019804/801A9DF0=1000",
        "s801A9E48=$0/801A9E7C=$1",
        "s801A9EB0=4"])
    # The game's own choices for the Manager's page (any game: up to 4, their values $0-$3 in the steps, in order)
    out["options"] = [{"label": "Country", "values": ["Russia", "England", "Ukraine", "Germany", "France"]},
                      {"label": "Color", "values": ["Beige", "Blue", "Red", "Green"]}]
    # The host's game settings, chosen in the Manager before hosting (shown in the lobby; ps1transfer -vlinkhostauto,
    # tune "hostauto": run by the host once paired): the setup screen after the link (front end overlay, link page
    # 0x800189C8 from 0x80018D58: the same menu 0x801A9DD8 with 11 items at 0x801A9EC4, 34h each, value at +20h;
    # found 2026-11-14 in two linked Redux, research/redux_trace/setup_auto.lua). Item 10 is the host's start: its
    # callback (0x80019670) gives 4 on X, and that value (0x801AA0EC) is what the main menu checks (4: the link game
    # started). Emulator: values written, then 0x801AA0EC = 4: both games started with them (map 12, $5000, no bases).
    #   1. the setup screen showing: 11 items, its items, the start item's callback, shown (1000h);
    #   2. 1.5 s (the other console on its setup screen too);
    #   3. the settings ($2-$11: after the two options above) into the items' values;
    #   4. 1 s (the other console shown them), then START.
    host_items = 0x801A9EC4
    value = lambda k: host_items + k * 0x34 + 0x20
    out["host_steps"] = ",".join([
        "w801A9DD8=B/801A9DDC=%08X/%08X=80019670/801A9DF0=1000" % (host_items, host_items + 10 * 0x34 + 0x30),
        "d5DC",
        "s" + "/".join("%08X=$%d" % (value(k), k + 2) for k in range(10)),
        "d3E8",
        "s%08X=4" % value(10)])
    on_off = {"values": ["Off", "On"], "codes": [0, 1]}
    out["host_options"] = [
        {"label": "Map", "values": [str(m) for m in range(1, 106)], "codes": list(range(1, 106)), "default": "1"},
        {"label": "Units", "values": [str(u) for u in range(13)], "default": "6"},
        {"label": "Tech level", "values": [str(t) for t in range(1, 11)], "codes": list(range(1, 11)), "default": "10"},
        {"label": "Credits", "values": [str(c * 1000) for c in range(11)], "codes": [c * 1000 for c in range(11)],
         "default": "10000"},
        {"label": "AI teams", "values": ["0", "1", "2"], "default": "0"},
        dict(on_off, label="Bases", default="On"),
        dict(on_off, label="Ore regrows", default="On"),
        dict(on_off, label="Shroud regrows", default="Off"),
        dict(on_off, label="Crates", default="On"),
        dict(on_off, label="Capture flag", default="Off")]
    # Kept while the link runs (ps1transfer -vlinkkeep; also once by the Manager's installer): the main menu's idle
    # timer, which leaves for the intro movie after 1500 passes without a button (overlay 0x80012F10: s6 counted,
    # "beq s6,v0" at 0x8001338C with v0 = 5DCh, return 7 = the movie), made a nop: the game can wait at its main menu
    # for the Manager. The menus come from the disc each time they are shown, so it is checked each second: the
    # instruction and its neighbours as found (front end overlay, 2026-11-14), then the nop written.
    # With it (2026-11-16, research/redux_trace/menu_lock.py, checked on the game's code by menu_lock_sim.py): the
    # main menu's LINK GAME always shown, named "DEV CART NET PLAY" (English, French, German texts) and skipped by the
    # cursor (id -3: drawn, never chosen; the menus' cursor skips negative ids, the main menu takes a negative choice
    # as none), so only the Manager starts a link game (the driver's automatic start sets the choice itself). The
    # checks: first the idle timer's instruction as found, alone (written last: a pass broken off is done again),
    # so a pass is one read once done or away from the menus (each read stops the console's game for a moment: with
    # 9 read each second the link broke at setup and in game, 2026-11-17); then words of the overlay these never
    # change, read only when the menus were just loaded again.
    out["keep_steps"] = ",".join([
        "w8001338C=12C20006",
        "w80013388=0000B021/80013390=26D60001/800133A8=24020007/80013010=AE720000/8001A934=27BDFFD8/"
        "80024AD4=4D004B4F/80025300=4F4E0067/80025BD4=4E004B4F",
        "s80013024=2402FFFD/80013028=AE620000/80013030=00000000/80013054=06810002/8001AAAC=044100CB/"
        "8001AB10=0440FFE8/8001AB8C=04410093/8001ABEC=0440FFE9/8002499C=017D0130/80024AC0=20564544/"
        "80024AC4=54524143/80024AC8=54454E20",
        "s80024ACC=414C5020/80024AD0=20730059/8002533C=56454400/80025340=52414320/80025344=454E2054/"
        "80025348=4C502054/8002534C=44005941/80025A9C=01850130/80025BC0=20564544/80025BC4=54524143/"
        "80025BC8=54454E20/80025BCC=414C5020",
        "s80025BD0=20730059",
        "s8001338C=00000000"])
    # Words of the game the relay logs when they change (ps1transfer -vlinkwatch: where the game is, to follow a failed
    # start): the main menu shown (1000h), the link page / setup screen menu's item count (3 / 11), the setup screen's
    # start value (4: the link game started), the game type (0x80168CC4 byte 0: 1 link), the run loop's "game chosen"
    out["watch"] = ",".join(["801A9AAC=main menu", "801A9DD8=page items", "801AA0EC=setup start",
                             "80168CC4=game type", "801B5278=game chosen"])
    # What the player does on the console without the automatic start (the page's instructions)
    out["link_play"] = "DEV CART NET PLAY on the game's main menu, then START SETUP"
    with open(os.path.join(HERE, "vlink_hw.json"), "w") as f:
        json.dump(out, f, indent=1)
    # The Manager's Net Play patch package (gui/netplay.py): the same, with the game's name and discs, and where its
    # executable is checked to be running
    out["title"] = "Command & Conquer: Red Alert - Retaliation"
    out["discs"] = ["SLES_013.43", "SLES_113.43"]       # (Allies, Soviet: the same code)
    out["entry"] = [0x80154E4C, orig(0x80154E4C, 16).hex()]
    pkg = os.path.join(HERE, "..", "..", "gui", "data", "netplay", "red_alert_retaliation.json")
    os.makedirs(os.path.dirname(pkg), exist_ok=True)
    with open(pkg, "w") as f:
        json.dump(out, f, indent=1)
    print("Manager patch package written: %s" % os.path.normpath(pkg))
    print("vlink_hw.json written; rings at %08X (%d bytes)" % (BUF, 112 + RX + TX))     # (magic, counters 36 + 40, stats 32)


if __name__ == "__main__":
    main()
