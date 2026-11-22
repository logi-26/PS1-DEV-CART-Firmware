-- CTR pad play prototype: lockstep between two emulators (research, 2026-11-17).
-- dofile'd by ctl.lua with: TAG ('A' or 'B'), ME (1: this emulator's player is player 1, 2: player 2), DELAY (vertical
-- blanks added to every menu step, to give the two emulators different timing), INPUT_DELAY (frames, default 2),
-- SOURCE ('script': scripted local player, 'pad': the emulator's pad 1, i.e. someone playing), FRAMES (how long the
-- script / the logs run, default 900).
-- Menus: the same scripted choices on both (Battle, 2P, both characters, the first level, START BATTLE), through the
-- emulator's pads. From the battle's start (0x8003CB7C) the game's frames are counted (0x80034EA8) and, each frame f:
--   this emulator's input for frame f + INPUT_DELAY is written to ls_out.txt ("f buttons", one line each);
--   the other emulator's input for frame f is read from ls_in.txt (the relay, relay.py, copies the other one's
--   ls_out.txt there with a network delay), waiting for it if it hasn't arrived (emulated time stands still, as a
--   console would wait);
--   both players' buttons are written into the game's raw pad buffers as its per-frame pad code reads them
--   (0x80025738: a1 = 0x80096AD4 player 1, 0x80096AF6 player 2).
-- Also the fixed frame step (0x80034ED4 = nop) from the battle's start. Logs: ls_frames.txt (f p1 p2 per frame),
-- rand calls and RAM / screenshots at frames 300, 600, 900 in dd.log / the folder.
local ffi = require('ffi')
pcall(ffi.cdef, 'void Sleep(uint32_t ms);')
local function sleep_ms(n)
  if not pcall(function() ffi.C.Sleep(n) end) then
    local t = os.clock() + n / 1000; while os.clock() < t do end
  end
end
local mem = ffi.cast('uint8_t*', PCSX.getMemPtr())
local function wr8(a, v) mem[bit.band(a, 0x1FFFFF)] = v end
local function rd8(a) return mem[bit.band(a, 0x1FFFFF)] end
local function wr32(a, v) ffi.cast('uint32_t*', PCSX.getMemPtr())[bit.rshift(bit.band(a, 0x1FFFFF), 2)] = v end

local D = INPUT_DELAY or 2
local LAST = FRAMES or 900
local d = DELAY or 0
local PAD2 = PCSX.SIO0.slots[2].pads[1]
local CROSS = PCSX.CONSTS.PAD.BUTTON.CROSS
local menu = {
  {3400, 'DOWN'}, {3420, 'DOWN'}, {3440, 'DOWN'}, {3460, 'DOWN'}, {3500, 'CROSS'},
  {3650, 'CROSS'}, {3800, 'CROSS'}, {3825, 'P2CROSS'}, {3950, 'CROSS'}, {4100, 'CROSS'},
}

local B = {SELECT = 0, START = 3, UP = 4, RIGHT = 5, DOWN = 6, LEFT = 7, L2 = 8, R2 = 9, L1 = 10, R1 = 11,
           TR = 12, O = 13, X = 14, SQ = 15}
local function m(...) local v = 0; for _, n in ipairs({...}) do v = bit.bor(v, bit.lshift(1, B[n])) end; return v end
-- The scripted players (the determinism test's, one per player)
local function scripted(who, f)
  if f < 90 then return 0 end
  local v = m('X')
  local k = math.floor(f / 45)
  if who == 1 then
    if k % 4 == 1 then v = bit.bor(v, m('LEFT')) elseif k % 4 == 3 then v = bit.bor(v, m('RIGHT')) end
    if f % 60 == 30 then v = bit.bor(v, m('O')) end
    if f % 150 >= 140 then v = bit.bor(v, m('R1')) end
  else
    if k % 5 == 2 then v = bit.bor(v, m('RIGHT')) elseif k % 5 == 4 then v = bit.bor(v, m('LEFT')) end
    if f % 70 == 50 then v = bit.bor(v, m('O')) end
  end
  return v
end
-- The emulator's pad 1 as buttons (someone playing on this emulator), read from its raw buffer before it's replaced
-- (the pad library writes it each vertical blank: 0x8009F60E/F, active low, +0 SELECT..LEFT, +1 L2..SQUARE)
local function local_pad()
  local v = bit.bor(rd8(0x8009F60E), bit.lshift(rd8(0x8009F60F), 8))
  return bit.band(bit.bnot(v), 0xFFFF)
end

local out_f = io.open(DD_DIR .. '/ls_out.txt', 'w')
local in_path = DD_DIR .. '/ls_in.txt'
local in_f = nil
local remote = {}              -- frame -> buttons (the other player's)
local mine = {}                -- frame -> buttons (this player's)
local frames_f = io.open(DD_DIR .. '/ls_frames.txt', 'w')
local function read_remote()
  if not in_f then in_f = io.open(in_path, 'r'); if not in_f then return end end
  in_f:seek('cur', 0)          -- (past an end of file reached before)
  while true do
    local line = in_f:read('*l')
    if not line then break end
    local f, v = line:match('^(%d+) (%d+)$')
    if f then remote[tonumber(f)] = tonumber(v) end
  end
end

frame, started = -1, false
local cur = {0, 0}             -- the buttons of this frame: player 1, player 2
local waited_ms, waits = 0, 0
local rands = 0
LS_BPS = {}
LS_BPS[#LS_BPS + 1] = PCSX.addBreakpoint(0x8003CB7C, 'Exec', 4, 'race start', function()
  if not started and vs > 4100 + d then
    started = true; frame = -1
    wr32(0x80034ED4, 0); PCSX.invalidateCache()
    for f = 0, D - 1 do mine[f] = 0; out_f:write(string.format('%d 0\n', f)) end
    out_f:flush()
    say('%s battle start at vsync %d, input delay %d frames, rand calls so far %d', TAG, vs, D, rands)
  end
  return true
end)
LS_BPS[#LS_BPS + 1] = PCSX.addBreakpoint(0x80034EA8, 'Exec', 4, 'frame', function()
  if not started then return true end
  frame = frame + 1
  local f = frame
  -- this player's input for frame f + D
  local v = (SOURCE == 'pad') and local_pad() or scripted(ME, f + D)
  mine[f + D] = v
  out_f:write(string.format('%d %d\n', f + D, v)); out_f:flush()
  -- the other player's for frame f (waiting for it)
  local t0 = os.clock()
  read_remote()
  if remote[f] == nil then
    waits = waits + 1
    while remote[f] == nil do
      sleep_ms(1); read_remote()
      if os.clock() - t0 > 20 then say('%s frame %d: nothing from the other side for 20 s', TAG, f); remote[f] = 0 end
    end
    waited_ms = waited_ms + (os.clock() - t0) * 1000
  end
  local other = remote[f]; remote[f] = nil
  if ME == 1 then cur = {mine[f], other} else cur = {other, mine[f]} end
  mine[f] = nil
  if f <= LAST then frames_f:write(string.format('%d %d %d\n', f, cur[1], cur[2])); frames_f:flush() end
  if f == 300 or f == 600 or f == 900 then dump(TAG .. '_ls' .. f) end
  if f > 0 and f % 300 == 0 then
    say('%s frame %d at vsync %d: waited for the other side in %d frames, %.0f ms in all', TAG, f, vs, waits,
        waited_ms)
  end
  return true
end)
LS_BPS[#LS_BPS + 1] = PCSX.addBreakpoint(0x80025738, 'Exec', 4, 'pad', function()
  if not started or frame < 0 then return true end
  local a1 = PCSX.getRegisters().GPR.n.a1
  local mask = (a1 == 0x80096AD4) and cur[1] or cur[2]
  local v = bit.band(bit.bnot(mask), 0xFFFF)
  wr8(a1, 0); wr8(a1 + 1, 0x41); wr8(a1 + 2, bit.band(v, 0xFF)); wr8(a1 + 3, bit.rshift(v, 8))
  return true
end)
LS_BPS[#LS_BPS + 1] = PCSX.addBreakpoint(0x80078BE8, 'Exec', 4, 'rand', function()
  rands = rands + 1
  if started and frame <= LAST then say('%s rand at frame %d ra=%08x', TAG, frame, PCSX.getRegisters().GPR.n.ra) end
  return true
end)

local shot_done = {}
VSYNC_HOOK = function()
  for _, e in ipairs(menu) do
    if vs == e[1] + d then
      if e[2] == 'P2CROSS' then PAD2.setOverride(CROSS) else hold(e[2], 6) end
    end
    if e[2] == 'P2CROSS' and vs == e[1] + d + 6 then PAD2.clearOverride(CROSS) end
  end
  if started then
    for _, f in ipairs({300, 600, 900}) do
      if not shot_done[f] and frame >= f then shot_done[f] = true; shot(TAG .. '_ls' .. f) end
    end
    if frame >= LAST and not shot_done.done then shot_done.done = true; say('%s done', TAG) end
  end
end
return TAG .. ' lockstep'
