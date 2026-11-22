-- CTR determinism run (dofile'd by ctl.lua with TAG, DELAY (vsyncs added to every menu step) and PATCH (true: the
-- fixed frame step, 0x80034ED4 = nop) set). From boot: Battle, 2P, both characters, the first level, START BATTLE
-- through the emulator's pads; then from the battle's first game frame both pads come from a script, by game frame:
-- written into each player's raw pad buffer as the game's per-frame pad code reads it (0x80025738, a1 = the buffer),
-- as a digital pad (type 41h: no analog sticks). RAM dumps and screenshots at frames 300, 600, 900.
local ffi = require('ffi')
local mem = ffi.cast('uint8_t*', PCSX.getMemPtr())
local function wr8(a, v) mem[bit.band(a, 0x1FFFFF)] = v end
local function rd32(a) return ffi.cast('uint32_t*', PCSX.getMemPtr())[bit.rshift(bit.band(a, 0x1FFFFF), 2)] end
local function wr32(a, v) ffi.cast('uint32_t*', PCSX.getMemPtr())[bit.rshift(bit.band(a, 0x1FFFFF), 2)] = v end

local PAD2 = PCSX.SIO0.slots[2].pads[1]
local CROSS = PCSX.CONSTS.PAD.BUTTON.CROSS
local d = DELAY or 0
local menu = {                 -- {vsync, what}
  {3400, 'DOWN'}, {3420, 'DOWN'}, {3440, 'DOWN'}, {3460, 'DOWN'}, {3500, 'CROSS'},   -- Battle
  {3650, 'CROSS'},                                                                   -- 2P
  {3800, 'CROSS'}, {3825, 'P2CROSS'},                                                -- characters
  {3950, 'CROSS'},                                                                   -- level
  {4100, 'CROSS'},                                                                   -- START BATTLE
}
local shots_at = {3390, 3640, 3790, 3940, 4090, 4300}

-- The scripted pads, by game frame from the battle's start: a function frame -> p1 mask, p2 mask
local B = {SELECT = 0, START = 3, UP = 4, RIGHT = 5, DOWN = 6, LEFT = 7, L2 = 8, R2 = 9, L1 = 10, R1 = 11,
           TR = 12, O = 13, X = 14, SQ = 15}
local function m(...) local v = 0; for _, n in ipairs({...}) do v = bit.bor(v, bit.lshift(1, B[n])) end; return v end
local function script(f)
  if f < 90 then return 0, 0 end                     -- (the countdown)
  local p1, p2 = m('X'), m('X')
  local k = math.floor(f / 45)
  if k % 4 == 1 then p1 = bit.bor(p1, m('LEFT')) elseif k % 4 == 3 then p1 = bit.bor(p1, m('RIGHT')) end
  if k % 5 == 2 then p2 = bit.bor(p2, m('RIGHT')) elseif k % 5 == 4 then p2 = bit.bor(p2, m('LEFT')) end
  if f % 60 == 30 then p1 = bit.bor(p1, m('O')) end  -- (fire whatever weapon is held)
  if f % 70 == 50 then p2 = bit.bor(p2, m('O')) end
  if f % 150 >= 140 then p1 = bit.bor(p1, m('R1')) end   -- (hops)
  return p1, p2
end

frame, started = -1, false
local bufs = {}
local rands = 0
DET_BPS = {}
-- Game frames: the frame step's computation (once per frame)
local dumped = {}
DET_BPS[#DET_BPS + 1] = PCSX.addBreakpoint(0x80034EA8, 'Exec', 4, 'frame', function()
  if started then
    frame = frame + 1
    for _, f in ipairs({300, 600, 900}) do       -- (at the same point of the game's frame in every run)
      if frame == f and not dumped[f] then dumped[f] = true; dump(TAG .. '_f' .. f) end
    end
  end
  return true
end)
-- The battle's start (candidate: the race start's step = 32)
DET_BPS[#DET_BPS + 1] = PCSX.addBreakpoint(0x8003CB7C, 'Exec', 4, 'race start', function()
  if not started and vs > 4100 + d then
    started = true; frame = 0
    if PATCH then wr32(0x80034ED4, 0); PCSX.invalidateCache() end
    say('%s battle start at vsync %d (patch %s, word %08x), rand calls so far %d', TAG, vs, tostring(PATCH),
        rd32(0x80034ED4), rands)
  end
  return true
end)
-- The pads, per player, as the game's per-frame pad code reads them
DET_BPS[#DET_BPS + 1] = PCSX.addBreakpoint(0x80025738, 'Exec', 4, 'pad', function()
  if not started or frame < 0 then return true end
  local a1 = PCSX.getRegisters().GPR.n.a1
  if not bufs[a1] then bufs[a1] = true; say('%s pad buffer %08x', TAG, a1) end
  local p1, p2 = script(frame)
  local mask = (a1 == 0x80096AD4) and p1 or p2
  local v = bit.band(bit.bnot(mask), 0xFFFF)
  -- (as the pad sends them: +2 SELECT..LEFT (bits 0-7), +3 L2..SQUARE (bits 8-15), active low)
  wr8(a1, 0); wr8(a1 + 1, 0x41); wr8(a1 + 2, bit.band(v, 0xFF)); wr8(a1 + 3, bit.rshift(v, 8))
  return true
end)
DET_BPS[#DET_BPS + 1] = PCSX.addBreakpoint(0x80078BE8, 'Exec', 4, 'rand', function()
  rands = rands + 1
  if started then say('%s rand at frame %d ra=%08x', TAG, frame, PCSX.getRegisters().GPR.n.ra) end
  return true
end)

local done = {}
VSYNC_HOOK = function()
  for _, e in ipairs(menu) do
    if vs == e[1] + d then
      if e[2] == 'P2CROSS' then PAD2.setOverride(CROSS) else hold(e[2], 6) end
      say('%s menu %s', TAG, e[2])
    end
    if e[2] == 'P2CROSS' and vs == e[1] + d + 6 then PAD2.clearOverride(CROSS) end
  end
  for _, t in ipairs(shots_at) do if vs == t + d then shot(TAG .. '_m' .. t) end end
  if started then
    for _, f in ipairs({300, 600, 900}) do
      if not done[f] and frame >= f then
        done[f] = true; shot(TAG .. '_f' .. f)
        say('%s frame %d at vsync %d', TAG, frame, vs)
        if f == 900 then say('%s done', TAG) end
      end
    end
  end
end
return TAG
