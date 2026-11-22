-- CTR research (2026-11-17): start a game straight from the main menu, no menus: the settings written into the game,
-- then the menu system's "load the track" step (sdata->ptrDesiredMenu = &data.menuQueueLoadTrack), as every menu
-- path ends (CTR-ModSDK decompile, 230 overlay; addresses USA SCUS_944.26 = the ModSDK's 926 build).
--   setup{mode = 'vs' | 'battle' | 'arcade', cup = false, players = 2..4 (arcade 1..2), chars = {p1, p2, ...},
--         level = levelID (races 0-17, arenas 18-24), laps = 3 | 5 | 7, cup_id = 0..3, difficulty = 1..3 (arcade),
--         battle = {type = 'points' | 'time' | 'life', points = 5|10|15, minutes = 3|6|9 (time) or 3|6|0 (life:
--                   0 = forever), lives = 3|6|9, teams = {t1, t2, ...} (0-3), weapons = bit mask (11 bits, 0x34DE all)}}
-- Returns the writes as text (to check); the game's own load then runs.
local ffi = require('ffi')
local m8 = ffi.cast('uint8_t*', PCSX.getMemPtr())
local m32 = ffi.cast('uint32_t*', PCSX.getMemPtr())
local function a(x) return bit.band(x, 0x1FFFFF) end
function r32(x) return m32[bit.rshift(a(x), 2)] end
function r8(x) return m8[a(x)] end
local function w32(x, v) m32[bit.rshift(a(x), 2)] = v end
local function w8(x, v) m8[a(x)] = v end
local function w16(x, v) w8(x, bit.band(v, 0xFF)); w8(x + 1, bit.band(bit.rshift(v, 8), 0xFF)) end

CTR = {
  GGT_PTR = 0x8008D2AC, CHAR_IDS = 0x80086E84, DESIRED_MENU = 0x8008D924, MENU_LOAD_TRACK = 0x80085A94,
  ARCADE_CUPS = 0x80084148,  -- 4 x {short name; {short track, icon} x 4} (18 bytes)
  -- gameMode1
  BATTLE = 0x20, MAIN_MENU = 0x2000, POINT_LIMIT = 0x4000, LIFE_LIMIT = 0x8000, TIME_LIMIT = 0x10000,
  TIME_TRIAL = 0x20000, ADVENTURE = 0x80000, ARENA = 0x100000, ARCADE = 0x400000, ADV_CUP = 0x10000000,
  LOADING = 0x40000000,
  CUP_ANY = 0x10,            -- gameMode2
  DIFFICULTY = {0x50, 0xA0, 0xF0},
  MINUTE = 0xE100,
}

-- The main menu is showing (not loading, not the attract demo)
function at_main_menu()
  local g = r32(CTR.GGT_PTR)
  if g == 0 then return false end
  local m1 = r32(g)
  return bit.band(m1, CTR.MAIN_MENU) ~= 0 and bit.band(m1, CTR.LOADING) == 0 and r8(g + 0x1D32) == 0
end

function setup(t)
  local g = r32(CTR.GGT_PTR)
  local m1 = r32(g)
  m1 = bit.band(m1, bit.bnot(bit.bor(CTR.BATTLE, CTR.POINT_LIMIT, CTR.LIFE_LIMIT, CTR.TIME_LIMIT, CTR.TIME_TRIAL,
                                     CTR.ADVENTURE, CTR.ARENA, CTR.ARCADE, CTR.ADV_CUP)))
  local m2 = r32(g + 8)
  m2 = bit.band(m2, bit.bnot(0x0FFFFFFF - 0x1000 - 0x2000))   -- no cheats, no cup (keep CUP_NEW_* bits)
  m2 = bit.band(m2, bit.bnot(CTR.CUP_ANY))
  local n = t.players or 2
  local level = t.level or 3
  if t.mode == 'arcade' then m1 = bit.bor(m1, CTR.ARCADE) end
  if t.mode == 'battle' then m1 = bit.bor(m1, CTR.BATTLE) end
  if t.cup and t.mode ~= 'battle' then
    m2 = bit.bor(m2, CTR.CUP_ANY)
    local c = t.cup_id or 0
    w32(g + 0x1E58, c); w32(g + 0x1E5C, 0)
    for i = 0, 7 do w32(g + 0x1E60 + 4 * i, 0) end
    local base = CTR.ARCADE_CUPS + 18 * c + 2
    level = bit.bor(r8(base), bit.lshift(r8(base + 1), 8))
  end
  w8(g + 0x1CA9, n)
  w8(g + 0x1D33, (t.mode == 'battle') and 3 or (t.laps or 3))
  if t.mode == 'arcade' then w32(g + 0x1EBC, CTR.DIFFICULTY[t.difficulty or 2]) end
  -- characters: the players' then the rest (the bots in arcade) in order, none twice
  local used, ids = {}, {}
  for i = 1, n do ids[i] = t.chars[i]; used[t.chars[i]] = true end
  local c = 0
  for i = n + 1, 8 do
    while used[c] do c = c + 1 end
    ids[i] = c; used[c] = true
  end
  for i = 1, 8 do w16(CTR.CHAR_IDS + 2 * (i - 1), ids[i]) end
  if t.mode == 'battle' then
    local b = t.battle or {}
    local typ = b.type or 'points'
    if typ == 'points' then m1 = bit.bor(m1, CTR.POINT_LIMIT)
    elseif typ == 'time' then m1 = bit.bor(m1, CTR.TIME_LIMIT, CTR.POINT_LIMIT)
    else
      m1 = bit.bor(m1, CTR.LIFE_LIMIT)
      if (b.minutes or 0) > 0 then m1 = bit.bor(m1, CTR.TIME_LIMIT) end
    end
    w32(g + 0x1D8C, b.points or 5)                        -- killLimit
    local mins = b.minutes or ((typ == 'time') and 3 or 0)
    w32(g + 0x1D84, (mins > 0) and mins * CTR.MINUTE or 0xFFFFFFFF)   -- originalEventTime
    w32(g + 0x1D88, b.lives or 3)                         -- lifeLimit
    local weapons = b.weapons or 0x34DE
    w32(g + 0x1DA0, weapons)
    local nw = 0
    for i = 0, 13 do
      if bit.band(weapons, bit.lshift(1, i)) ~= 0 then w32(g + 0x1DF4 + 4 * nw, i); nw = nw + 1 end
    end
    w32(g + 0x1DF0, nw)
    local teams = b.teams or {0, 1, 2, 3}
    local flags, nt = 0, 0
    for i = 1, 4 do
      w8(g + 0x1DA4 + i - 1, teams[i] or (i - 1))
      if i <= n then
        local f = bit.lshift(1, teams[i])
        if bit.band(flags, f) == 0 then flags = bit.bor(flags, f); nt = nt + 1 end
      end
    end
    w32(g + 0x1DD8, flags); w32(g + 0x1DDC, nt)
  end
  w32(g, m1); w32(g + 8, m2)
  w32(g + 0x1EB0, level)                                  -- currLEV: the menu's track to load
  w32(CTR.DESIRED_MENU, CTR.MENU_LOAD_TRACK)
  return string.format('setup %s cup %s players %d level %d chars %s mode1 %08x mode2 %08x', t.mode,
                       tostring(t.cup), n, level, table.concat(ids, ','), m1, m2)
end
return 'ctr_setup ready'
