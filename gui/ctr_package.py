"""Crash Team Racing (USA) for pad play: the host's choices (what the Manager's popups offer) and the game's setup as
memory writes, applied by the relay at the main menu (as research/ctr/ctr_setup.lua does in the emulator).

  setup_writes(settings, chars) -> [(address, size, value)], addresses as numbers or ("G", offset) for gGT + offset
  settings: {"mode": "vs" | "battle" | "arcade", "cup": bool, "level": id, "cup_id": 0-3, "laps": 3|5|7,
             "difficulty": 1-3, "battle": {"type": "points"|"time"|"life", "points": 5|10|15, "minutes": 3|6|9 or 0
             (life: forever), "lives": 3|6|9, "teams": [t per player], "weapons": bits}}
  chars: the players' characters (player 1 first), as many as players.
"""

GGT_PTR = 0x8008D2AC
CHAR_IDS = 0x80086E84
DESIRED_MENU, MENU_LOAD_TRACK = 0x8008D924, 0x80085A94
# gameMode1
BATTLE, MAIN_MENU, POINT_LIMIT, LIFE_LIMIT, TIME_LIMIT = 0x20, 0x2000, 0x4000, 0x8000, 0x10000
TIME_TRIAL, ADVENTURE, ARENA, ARCADE, ADV_CUP, LOADING = 0x20000, 0x80000, 0x100000, 0x400000, 0x10000000, 0x40000000
CUP_ANY = 0x10                       # gameMode2
DIFFICULTY = (0x50, 0xA0, 0xF0)
MINUTE = 0xE100
ALL_WEAPONS = 0x34DE

CHARACTERS = ["Crash Bandicoot", "Dr. Neo Cortex", "Tiny Tiger", "Coco Bandicoot", "N. Gin", "Dingodile",
              "Polar", "Pura"]
TRACKS = [(3, "Crash Cove"), (6, "Roo's Tubes"), (4, "Tiger Temple"), (14, "Coco Park"), (9, "Mystery Caves"),
          (2, "Blizzard Bluff"), (8, "Sewer Speedway"), (0, "Dingo Canyon"), (5, "Papu's Pyramid"),
          (1, "Dragon Mines"), (12, "Polar Pass"), (10, "Cortex Castle"), (15, "Tiny Arena"),
          (7, "Hot Air Skyway"), (11, "N. Gin Labs"), (13, "Oxide Station")]
ARENAS = [(18, "Nitro Court"), (19, "Rampage Ruins"), (21, "Skull Rock"), (23, "Rocky Road")]
# (id, name, first track: data.ArcadeCups 0x80084148 in the executable, 18 bytes each, +2)
CUPS = [(0, "Wumpa Cup", 3), (1, "Crystal Cup", 6), (2, "Nitro Cup", 9), (3, "Crash Cup", 12)]
WEAPONS = [(0x1, "Bowling bomb"), (0x2, "Missile"), (0x4, "TNT / Nitro"), (0x8, "Beaker"), (0x10, "Turbo"),
           (0x40, "Mask"), (0x80, "Clock"), (0x400, "Shield"), (0x800, "Warp orb"), (0x1000, "Invisibility"),
           (0x2000, "Super engine")]
MODES = {"vs": (2, 4), "battle": (2, 4), "arcade": (2, 2)}          # players: min, max


# The Manager's windows (the package's "options" / "host_options", the same form as the link games'): the player's
# own choice, and the host's game settings; settings_from_choices() turns the host's into the settings above.
PLAYER_OPTIONS = [{"label": "Character", "values": CHARACTERS, "default": CHARACTERS[0]}]
MODE_NAMES = ["VS race", "VS cup", "Battle", "Arcade race (2 players)", "Arcade cup (2 players)"]
TEAM_NAMES = ["Every player alone", "Players 1+2 against 3+4", "Players 1+3 against 2+4", "Player 1 against the rest"]
HOST_OPTIONS = [
    {"label": "Mode", "values": MODE_NAMES, "default": MODE_NAMES[0]},
    {"label": "Players", "values": ["2", "3", "4"], "default": "4"},
    {"label": "Track", "values": [n for _, n in TRACKS], "default": TRACKS[0][1]},
    {"label": "Laps", "values": ["3", "5", "7"], "default": "3"},
    {"label": "Cup", "values": [c[1] for c in CUPS], "default": CUPS[0][1]},
    {"label": "Difficulty", "values": ["Easy", "Medium", "Hard"], "default": "Medium"},
    {"label": "Arena", "values": [n for _, n in ARENAS], "default": ARENAS[0][1]},
    {"label": "Battle type", "values": ["Points", "Time", "Lives"], "default": "Points"},
    {"label": "Points", "values": ["5", "10", "15"], "default": "10"},
    {"label": "Minutes", "values": ["3", "6", "9", "No limit"], "default": "6"},
    {"label": "Lives", "values": ["3", "6", "9"], "default": "6"},
    {"label": "Teams", "values": TEAM_NAMES, "default": TEAM_NAMES[0]},
    {"label": "Weapons", "values": ["All"] + [n for _, n in WEAPONS], "default": "All"},
]


def settings_from_choices(ch):
    """The host's choices (label -> value, as in HOST_OPTIONS) -> (settings, most players). Choices that don't
    apply to the mode are ignored (e.g. Laps in a battle). Weapons: "All", or one weapon only."""
    get = lambda k: ch.get(k) or next(o["default"] for o in HOST_OPTIONS if o["label"] == k)
    mode = MODE_NAMES.index(get("Mode")) if get("Mode") in MODE_NAMES else 0
    most = 2 if mode >= 3 else int(get("Players"))
    s = {"mode": ("vs", "vs", "battle", "arcade", "arcade")[mode], "cup": mode in (1, 4)}
    if s["mode"] == "battle":
        s["level"] = dict((n, i) for i, n in ARENAS)[get("Arena")]
        typ = ("points", "time", "life")[["Points", "Time", "Lives"].index(get("Battle type"))]
        mins = 0 if get("Minutes") == "No limit" else int(get("Minutes"))
        if typ == "time" and not mins:
            mins = 6
        t = TEAM_NAMES.index(get("Teams")) if get("Teams") in TEAM_NAMES else 0
        teams = ([0, 1, 2, 3], [0, 0, 1, 1], [0, 1, 0, 1], [0, 1, 1, 1])[t]
        w = get("Weapons")
        s["battle"] = {"type": typ, "points": int(get("Points")), "minutes": mins, "lives": int(get("Lives")),
                       "teams": teams, "weapons": ALL_WEAPONS if w == "All" else dict((n, b) for b, n in WEAPONS)[w]}
    elif s["cup"]:
        s["cup_id"] = [c[1] for c in CUPS].index(get("Cup"))
    else:
        s["level"] = dict((n, i) for i, n in TRACKS)[get("Track")]
        s["laps"] = int(get("Laps"))
    if s["mode"] == "arcade":
        s["difficulty"] = ["Easy", "Medium", "Hard"].index(get("Difficulty")) + 1
    return s, most


def check_start(s, chars):
    """Why the game can't start with these players' characters (None if it can): e.g. a battle whose players are
    all in one team (a host who started early with only players 1 and 2 of "1+2 against 3+4")."""
    try:
        setup_writes(s, chars)
    except ValueError as e:
        return str(e)
    return None


def setup_writes(s, chars):
    """The writes that set the game up (the last one starts the load), for len(chars) players."""
    n = len(chars)
    mode = s.get("mode", "vs")
    lo, hi = MODES[mode]
    if not lo <= n <= hi:
        raise ValueError("%s takes %d-%d players, not %d" % (mode, lo, hi, n))
    if len(set(chars)) != n:
        raise ValueError("two players with the same character")
    w = []
    m1_clear = BATTLE | POINT_LIMIT | LIFE_LIMIT | TIME_LIMIT | TIME_TRIAL | ADVENTURE | ARENA | ARCADE | ADV_CUP
    m1_set = {"arcade": ARCADE, "battle": BATTLE}.get(mode, 0)
    m2_set = 0
    level = s.get("level", 3)
    if s.get("cup") and mode != "battle":
        m2_set |= CUP_ANY
        c = s.get("cup_id", 0)
        w += [(("G", 0x1E58), 4, c), (("G", 0x1E5C), 4, 0)] + [(("G", 0x1E60 + 4 * i), 4, 0) for i in range(8)]
        level = CUPS[c][2]
    w.append((("G", 0x1CA9), 1, n))
    w.append((("G", 0x1D33), 1, 3 if mode == "battle" else s.get("laps", 3)))
    if mode == "arcade":
        w.append((("G", 0x1EBC), 4, DIFFICULTY[s.get("difficulty", 2) - 1]))
    ids = list(chars) + [c for c in range(8) if c not in chars]
    w += [(CHAR_IDS + 2 * i, 2, ids[i]) for i in range(8)]
    if mode == "battle":
        b = s.get("battle", {})
        typ = b.get("type", "points")
        mins = b.get("minutes", 3 if typ == "time" else 0)
        if typ == "points":
            m1_set |= POINT_LIMIT
        elif typ == "time":
            m1_set |= TIME_LIMIT | POINT_LIMIT
        else:
            m1_set |= LIFE_LIMIT | (TIME_LIMIT if mins > 0 else 0)
        w.append((("G", 0x1D8C), 4, b.get("points", 5)))
        w.append((("G", 0x1D84), 4, mins * MINUTE if mins > 0 else 0xFFFFFFFF))
        w.append((("G", 0x1D88), 4, b.get("lives", 3)))
        weapons = b.get("weapons", ALL_WEAPONS)
        w.append((("G", 0x1DA0), 4, weapons))
        bits = [i for i in range(14) if weapons & (1 << i)]
        w += [(("G", 0x1DF4 + 4 * k), 4, i) for k, i in enumerate(bits)]
        w.append((("G", 0x1DF0), 4, len(bits)))
        teams = list(b.get("teams", range(n)))[:n]
        teams += [i for i in range(len(teams), 4)]
        if len(set(teams[:n])) < 2:
            raise ValueError("a battle needs at least 2 teams")
        w += [(("G", 0x1DA4 + i), 1, teams[i]) for i in range(4)]
        flags = 0
        for t in teams[:n]:
            flags |= 1 << t
        w += [(("G", 0x1DD8), 4, flags), (("G", 0x1DDC), 4, len(set(teams[:n])))]
    # the mode words: read-modify-write (the relay: ("G", 0) and ("G", 8) "&~clear |set")
    w.append((("G", 0), "mode", (m1_clear, m1_set)))
    w.append((("G", 8), "mode", (0x0FFFFFFF & ~0x3000 | CUP_ANY, m2_set)))   # cheats and cup cleared
    w.append((("G", 0x1EB0), 4, level))
    w.append((DESIRED_MENU, 4, MENU_LOAD_TRACK))
    return w


def describe(s, n):
    """The settings as one line of text for the lobby list."""
    mode = s.get("mode", "vs")
    names = dict(TRACKS + ARENAS)
    parts = [{"vs": "VS", "battle": "Battle", "arcade": "Arcade"}[mode] + (" Cup" if s.get("cup") else "")]
    if s.get("cup") and mode != "battle":
        parts.append(CUPS[s.get("cup_id", 0)][1])
    else:
        parts.append(names.get(s.get("level", 3), "?"))
    if mode == "battle":
        b = s.get("battle", {})
        typ = b.get("type", "points")
        if typ == "points":
            parts.append("%d points" % b.get("points", 5))
        elif typ == "time":
            parts.append("%d minutes" % b.get("minutes", 3))
        else:
            parts.append("%d lives" % b.get("lives", 3) + (", %d min" % b["minutes"] if b.get("minutes") else ""))
    elif not s.get("cup"):
        parts.append("%d laps" % s.get("laps", 3))
    if mode == "arcade":
        parts.append(("easy", "medium", "hard")[s.get("difficulty", 2) - 1])
    parts.append("up to %d players" % n)
    return ", ".join(parts)


if __name__ == "__main__":
    for st, ch in (({"mode": "vs", "level": 3, "laps": 5}, [5, 6, 7, 0]),
                   ({"mode": "battle", "level": 19, "battle": {"type": "life", "minutes": 6, "lives": 6,
                                                               "teams": [0, 1, 1]}}, [2, 4, 6]),
                   ({"mode": "arcade", "cup": True, "cup_id": 0, "difficulty": 1}, [0, 1])):
        print(describe(st, len(ch)))
        for x in setup_writes(st, ch):
            print("   ", x)
