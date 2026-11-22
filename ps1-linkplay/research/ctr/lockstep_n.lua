-- CTR pad play prototype for 2-4 players (research, 2026-11-17): N emulators in lockstep through hub.py.
-- dofile'd by ctl.lua (after dd_auto.lua, with ctr_mt.lua and ctr_setup.lua) with:
--   TAG, ME (this emulator's player, 1..N), N (players), SETUP (a setup{} table, the same on all), SEED,
--   DELAY (vertical blanks before the setup, to give the emulators different timing), INPUT_DELAY (frames, default 2),
--   SOURCE ('script' or 'pad': the emulator's own pad 1), FRAMES (frames logged per race, default 900).
-- The game is started straight from the main menu (ctr_setup.lua) with a fake multitap of N pads (ctr_mt.lua).
-- Each race (from 0x8003CB7C, the race start; "epoch" e = 1, 2, ...) until the next loading: each game frame f
-- (0x80034EA8) this player's input for f + INPUT_DELAY goes out ("e p f buttons"), and every other player's for f is
-- waited for; all N written into the multitap slots. The fixed frame step (0x80034ED4 = nop). rand: the game's BIOS
-- rand (stub 0x80078BE8) replaced by the BIOS's generator on a seed of our own (SEED at the setup, SEED + e at each
-- race start), the same on every console whatever its BIOS / history.
-- Logs (DD_DIR): ls_frames.txt "e f p1 .. pN", dd.log (rand calls, waits), screenshots / RAM at frames 300/600/900.
local ffi = require('ffi')
pcall(ffi.cdef, 'void Sleep(uint32_t ms);')
local function sleep_ms(n) if not pcall(function() ffi.C.Sleep(n) end) then end end
local m32 = ffi.cast('uint32_t*', PCSX.getMemPtr())
local function wr32(x, v) m32[bit.rshift(bit.band(x, 0x1FFFFF), 2)] = v end

local D = INPUT_DELAY or 2
local LAST = FRAMES or 900
local d = DELAY or 0
local BTN_ = {SELECT = 0, START = 3, UP = 4, RIGHT = 5, DOWN = 6, LEFT = 7, L2 = 8, R2 = 9, L1 = 10, R1 = 11,
              TR = 12, O = 13, X = 14, SQ = 15}
local function m(...) local v = 0; for _, n in ipairs({...}) do v = bit.bor(v, bit.lshift(1, BTN_[n])) end; return v end
-- scripted players: accelerate, steer in a pattern of their own, fire now and then
local function scripted(who, f)
  if f < 90 then return 0 end
  local v = m('X')
  local k = math.floor(f / (35 + 10 * who))
  local c = k % (3 + who)
  if c == 1 then v = bit.bor(v, m('LEFT')) elseif c == 3 then v = bit.bor(v, m('RIGHT')) end
  if f % (50 + 10 * who) == 20 * who then v = bit.bor(v, m('O')) end
  if who == 1 and f % 150 >= 140 then v = bit.bor(v, m('R1')) end
  return v
end
local function local_pad()
  local mem = ffi.cast('uint8_t*', PCSX.getMemPtr())
  local v = bit.bor(mem[0x9F60E], bit.lshift(mem[0x9F60F], 8))
  return bit.band(bit.bnot(v), 0xFFFF)
end

-- the generator (the BIOS's: seed = seed * 0x41C64E6D + 0x3039, result (seed >> 16) & 0x7FFF)
local seed = 0
local function srand(s) seed = bit.tobit(s) end
local function rand()
  -- 32-bit multiply without losing bits: split into 16-bit halves
  local lo, hi = bit.band(seed, 0xFFFF), bit.band(bit.rshift(seed, 16), 0xFFFF)
  local mlo, mhi = 0x4E6D, 0x41C6
  local p = lo * mlo
  local mid = (hi * mlo + lo * mhi) % 0x10000
  local r = (p + mid * 0x10000 + 0x3039) % 0x100000000
  seed = bit.tobit(r)
  return bit.band(bit.rshift(seed, 16), 0x7FFF)
end

local out_f = io.open(DD_DIR .. '/ls_out.txt', 'w')
local in_path = DD_DIR .. '/ls_in.txt'
local in_f = nil
local remote = {}               -- remote["e:p:f"] = buttons
local frames_f = io.open(DD_DIR .. '/ls_frames.txt', 'w')
local function read_remote()
  if not in_f then in_f = io.open(in_path, 'r'); if not in_f then return end end
  in_f:seek('cur', 0)
  while true do
    local line = in_f:read('*l')
    if not line then break end
    local e, p, f, v = line:match('^(%d+) (%d+) (%d+) (%d+)$')
    if e then remote[e .. ':' .. p .. ':' .. f] = tonumber(v) end
  end
end
local function send(e, f, v) out_f:write(string.format('%d %d %d %d\n', e, ME, f, v)); out_f:flush() end

epoch, frame, racing, set_up = 0, -1, false, false
local waited_ms, waits, rands = 0, 0, 0
local mine = {}
LS_BPS = LS_BPS or {}
for _, b in ipairs(LS_BPS) do b:remove() end
LS_BPS = {}
local function gmode1() return r32(r32(CTR.GGT_PTR)) end

-- Frame counters kept since power-on, read by the game's logic (BOTS_ThTick_Drive, VehPhysGeneral_JumpAndFriction:
-- gGT+0x1CEC; VehPhysCrash_AnyTwoCars: gGT+0x1CF8): the same on every console.
local function reset_counters()
  local g = r32(CTR.GGT_PTR)
  wr32(g + 0x1CEC, 0); wr32(g + 0x1CF8, 0)
end
-- At a load's start (the level's bots get their paths while it loads): the generators seeded, the same on every
-- console: the BIOS rand (replaced), RngDeadCoed's two states (0x8008D668: bots' paths / starts / driving, items,
-- potions; gGT+0x252C: particles, sparks) and MixRNG ($gp+0x4B8 = 0x8008D424, 16 bits: held items, bots, particles)
local loading_seen = false
local function load_starts(where)
  if loading_seen then return end
  loading_seen = true
  local s = SEED + epoch + 1
  srand(s)
  local g = r32(CTR.GGT_PTR)
  wr32(0x8008D668, bit.bxor(0x30215400, s * 2654435761 % 0x100000000))
  wr32(0x8008D66C, bit.bxor(0x493583FE, s))
  wr32(g + 0x252C, bit.bxor(0x12345678, s * 40503 % 0x100000000))
  wr32(g + 0x2530, bit.bxor(0x9ABCDEF0, s))
  local mix = ffi.cast('uint16_t*', PCSX.getMemPtr())
  mix[bit.rshift(0x8D424, 1)] = bit.band(s * 7919, 0xFFFF)
  reset_counters()
  say('%s load starts (%s) at vsync %d: seeded %d', TAG, where, vs, s)
end

LS_BPS[#LS_BPS + 1] = PCSX.addBreakpoint(0x8003CB7C, 'Exec', 4, 'load', function()
  if set_up then
    if racing then
      racing = false
      say('%s race %d over at frame %d (a load): waited in %d frames, %.0f ms', TAG, epoch, frame, waits, waited_ms)
    end
    load_starts('0x8003CB7C')
  end
  return true
end)
-- The lockstep's frame: once per pass of the main loop, where the game reads its pads (GAMEPAD_ProcessAnyoneVars),
-- in races, the pause menu and the menus alike
LS_BPS[#LS_BPS + 1] = PCSX.addBreakpoint(0x800262D0, 'Exec', 4, 'frame', function()
  if not set_up then return true end
  if bit.band(gmode1(), CTR.LOADING) ~= 0 then
    if racing then
      racing = false
      say('%s race %d over at frame %d (loading): waited in %d frames, %.0f ms', TAG, epoch, frame, waits, waited_ms)
    end
    load_starts('loading flag')
    for i = 1, 4 do MT_HELD[i] = 0 end
    return true
  end
  if not racing then
    if not loading_seen then return true end
    -- a load just ended: the next race (epoch) starts here, together on every console
    loading_seen = false
    racing = true; epoch = epoch + 1; frame = -1; mine = {}
    reset_counters()
    for f = 0, D - 1 do mine[f] = 0; send(epoch, f, 0) end
    say('%s race %d start at vsync %d, input delay %d, rand calls so far %d', TAG, epoch, vs, D, rands)
  end
  frame = frame + 1
  local f = frame
  local v = (SOURCE == 'pad') and local_pad() or scripted(ME, f + D)
  -- extra scripted presses: EXTRA = {{race, frame, player, 'BUTTON'}, ...} (held 4 frames)
  for _, e in ipairs(EXTRA or {}) do
    if e[1] == epoch and e[3] == ME and f + D >= e[2] and f + D < e[2] + 4 then v = bit.bor(v, m(e[4])) end
  end
  mine[f + D] = v
  send(epoch, f + D, v)
  local cur = {}
  local t0 = os.clock()
  read_remote()
  for p = 1, N do
    if p == ME then cur[p] = mine[f]
    else
      local k = epoch .. ':' .. p .. ':' .. f
      if remote[k] == nil then
        waits = waits + 1
        while remote[k] == nil do
          sleep_ms(1); read_remote()
          if os.clock() - t0 > 20 then say('%s race %d frame %d: nothing from player %d for 20 s', TAG, epoch, f, p); remote[k] = 0 end
        end
      end
      cur[p] = remote[k]; remote[k] = nil
    end
  end
  waited_ms = waited_ms + (os.clock() - t0) * 1000
  mine[f] = nil
  for p = 1, 4 do MT_HELD[p] = cur[p] or 0 end
  if f <= (HASH_FRAMES or 0) then     -- each driver's struct hashed, to find the first frame that differs
    local g = r32(CTR.GGT_PTR)
    local hs = {}
    for i = 0, 7 do
      local p = r32(g + 0x24EC + 4 * i)
      local h = 0
      if p ~= 0 then
        local base = bit.rshift(bit.band(p, 0x1FFFFF), 2)
        for k = 0, 0x638 / 4 - 1 do h = bit.bxor(bit.rol(h, 5), m32[base + k]) end
      end
      hs[#hs + 1] = bit.tohex(h)
    end
    hash_f = hash_f or io.open(DD_DIR .. '/ls_hash.txt', 'w')
    hash_f:write(string.format('%d %d %d %s\n', f, r32(g + 0x1CEC), r32(g + 0x1CF8), table.concat(hs, ' ')))
    hash_f:flush()
  end
  if f <= LAST then frames_f:write(epoch .. ' ' .. f .. ' ' .. table.concat(cur, ' ') .. '\n'); frames_f:flush() end
  if f == 300 or f == 600 or f == 900 or (DUMP_AT and DUMP_AT[f]) then dump(TAG .. '_r' .. epoch .. '_' .. f) end
  if f > 0 and f % 300 == 0 then
    say('%s race %d frame %d at vsync %d: waited in %d frames, %.0f ms in all', TAG, epoch, f, vs, waits, waited_ms)
  end
  return true
end)
-- the game's rand (its stub calling the BIOS) replaced: v0 = our generator, back to the caller
LS_BPS[#LS_BPS + 1] = PCSX.addBreakpoint(0x80078BE8, 'Exec', 4, 'rand', function()
  if not set_up then return true end
  rands = rands + 1
  local regs = PCSX.getRegisters()
  local ra = regs.GPR.n.ra
  regs.GPR.n.v0 = rand()     -- (the stub is now jr ra / nop: it returns this)
  if racing and frame <= LAST then say('%s rand at race %d frame %d ra=%08x v=%d', TAG, epoch, frame, ra, regs.GPR.n.v0) end
  return true
end)

local shot_done = {}
mt(N)
MT_HELD = {0, 0, 0, 0}
VSYNC_HOOK = function()
  MT_HOOK()
  if not set_up and vs >= 3400 + d and at_main_menu() then
    srand(SEED)
    wr32(0x80078BE8, 0x03E00008); wr32(0x80078BEC, 0)   -- rand: jr ra / nop
    wr32(0x80034ED4, 0); PCSX.invalidateCache()           -- the fixed frame step
    say('%s %s', TAG, setup(SETUP)); set_up = true
  end
  if racing then
    for _, f in ipairs({300, 600, 900}) do
      local k = epoch .. ':' .. f
      if not shot_done[k] and frame >= f then shot_done[k] = true; shot(TAG .. '_r' .. epoch .. '_' .. f) end
    end
  end
end
return TAG .. ' lockstep_n'
