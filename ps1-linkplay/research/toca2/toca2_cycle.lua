-- TOCA 2 Touring Cars (Europe, SLES_015.47, GAME.EXE): determinism test by restarting the race (no save states:
-- Redux closes loading them in this game). Run from dd_auto.lua's ctl.lua while a 2-player race is running
-- (../dd/nav_toca2_race.sh):
--   RUNS = {{'A', 30}, {'B', 67}}; FIXED_STEP = true; dofile([[...toca2_cycle.lua]])
-- A run's optional fields: patch = false (this race without the step patch, the original code put back),
-- pads = false (the pads not scripted: nobody drives), frames = N (stop after N frames, default 900).
-- transplant = {lo, hi}: at the race's frame 0, RAM lo..hi copied from research/toca2/ref_f0.ram (a good run's RAM
-- at its frame 0), to find what a race keeps from before its start.
-- SEED_SETUP = true: the car setup's seeding (0x8009E2D0 from 0x8007ECD0, normally rand()) given 0x4321 too.
-- Each run: pause (pad 1 START) -> ESCI DA CORSA -> SI -> VUOI CORRERE ANCORA: SI -> car setup: pad 1 X (FATTO) ->
-- wait DELAY vertical blanks -> pad 2 X (INIZIA CORSA) -> the race: its seeding (0x8009E2D0 from 0x80074728) given
-- 0x1234; from the next game frame both pads scripted per frame (raw buffers at 0x800A6024), the time-mixed random
-- 0x8008F6B8 given 0 for VSync(-1); page hashes 0x800D5000-0x800FFFFF each frame to 320, RAM / screenshots at
-- 300, 600, 900. Output DD_DIR/cyc_TAG.txt (+ _N.ram/.raw/.txt). "cycle done" in dd.log at the end.
-- FIXED_STEP: the frame's time step (0x800A14D0: root counter 2 time measured by the pacer) a constant 0x28000.
local ffi = require('ffi')
REF_F0 = REF_F0 or (debug.getinfo(1, 'S').source:match('^@(.*[/\\])') or '') .. 'ref_f0.ram'
local BUF1, BUF2 = 0x800E0374, 0x800E0396
local mem = ffi.cast('uint8_t*', PCSX.getMemPtr())
local function rd32(a) return ffi.cast('uint32_t*', PCSX.getMemPtr())[bit.rshift(bit.band(a, 0x1FFFFF), 2)] end
local function wr32(a, v) ffi.cast('uint32_t*', PCSX.getMemPtr())[bit.rshift(bit.band(a, 0x1FFFFF), 2)] = v end
local function wr8(a, v) mem[bit.band(a, 0x1FFFFF)] = v end

local function set_patch(on)
  if on then
    wr32(0x800A14D0, 0x3C020002); wr32(0x800A14D4, 0x03E00008); wr32(0x800A14D8, 0x34428000)
  else   -- the original: lw v0, 0x12e4(gp) / jr ra / nop
    wr32(0x800A14D0, 0x8F8212E4); wr32(0x800A14D4, 0x03E00008); wr32(0x800A14D8, 0x00000000)
  end
end

-- SOUND_RAND: the sound module's rand calls (GAME.EXE 0x8008B000-0x80092000: sound effects picked when an SPU voice
-- has ended, speech; their timing follows the audio, not the game frames) sent to a copy of rand with its own state
-- (at 0x8000D000, state 0x8000D040), so the game's own sequence (0x800E3650) only moves with the game.
if SOUND_RAND then
  local stub = {0x3C088001, 0x8D02D040, 0x3C0941C6, 0x35294E6D, 0x00490018, 0x00001012, 0x24423039, 0xAD02D040,
                0x00021402, 0x03E00008, 0x30427FFF}
  for i, v in ipairs(stub) do wr32(0x8000D000 + (i - 1) * 4, v) end
  wr32(0x8000D040, 0x5EED)
  local jal_rand, jal_stub = 0x0C000000 + bit.rshift(0x000C56E8, 2), 0x0C000000 + bit.rshift(0x0000D000, 2)
  local n = 0
  for a = 0x8008B000, 0x80091FFC, 4 do
    if rd32(a) == jal_rand then wr32(a, jal_stub); n = n + 1 end
  end
  say('sound rand: %d calls moved', n)
end

local X, SQ, LEFT, RIGHT = 0x4000, 0x8000, 0x80, 0x20
local function script(p, f)
  local m = X
  local k = (f + p * 37) % 160
  if k < 25 then m = bit.bor(m, LEFT) elseif k >= 70 and k < 90 then m = bit.bor(m, RIGHT) end
  if (f + p * 11) % 400 >= 380 then m = SQ end
  return m
end
local function give(buf, m)
  local v = bit.band(bit.bnot(m), 0xFFFF)
  wr8(buf, 0x00); wr8(buf + 1, 0x41); wr8(buf + 2, bit.band(v, 0xFF)); wr8(buf + 3, bit.rshift(v, 8))
end

local run_i, out, tag, cur = 0, nil, nil, nil
local frame, seeded, armed = -1, false, false
local function log(fmt, ...) out:write(string.format(fmt, ...) .. '\n'); out:flush() end
local function save(n)
  local s = PCSX.GPU.takeScreenShot()
  local f = io.open(DD_DIR .. '/cyc_' .. tag .. '_' .. n .. '.raw', 'wb'); f:write(tostring(s.data)); f:close()
  local m = io.open(DD_DIR .. '/cyc_' .. tag .. '_' .. n .. '.txt', 'w')
  m:write(string.format('%d %d %s\n', s.width, s.height, tostring(s.bpp))); m:close()
  local r = io.open(DD_DIR .. '/cyc_' .. tag .. '_' .. n .. '.ram', 'wb')
  r:write(ffi.string(PCSX.getMemPtr(), 0x200000)); r:close()
end

-- the button sequence: {vertical blanks after the previous step, pad, button name}
local seq, seq_i, seq_at = nil, 0, 0
local held = nil
local start_next

local B = {}
for _, b in ipairs(CYC_BPS or {}) do b:remove() end
CYC_BPS = B
-- a Lua error in a breakpoint pauses Redux: every callback runs protected, its error logged
local addbp = PCSX.addBreakpoint
local function bp(adr, kind, w, name, fn)
  return addbp(adr, kind, w, name, function()
    local ok, e = pcall(fn)
    if not ok then say('%s: %s', name, tostring(e)) end
    return true
  end)
end
B[#B + 1] = bp(0x8009E2D0, 'Exec', 4, 'cyc seed', function()
  local r = PCSX.getRegisters()
  if SEED_SETUP and r.GPR.n.ra == 0x8007ECD8 then
    if out then log('setup seed %04x -> 4321 at vs %d', r.GPR.n.a0, vs) end
    r.GPR.n.a0 = 0x4321
  end
  if armed and r.GPR.n.ra == 0x80074730 then
    log('race seed %04x -> 1234 at vs %d', r.GPR.n.a0, vs)
    r.GPR.n.a0 = 0x1234
    seeded, armed = true, false
  end
  return true
end)
B[#B + 1] = bp(0x8004B3D0, 'Exec', 4, 'cyc frame', function()
  if frame < 0 then
    if not seeded then return true end
    frame = 0
    log('race frame 0 at vs %d', vs)
    if cur.transplant then
      local lo, hi = cur.transplant[1], cur.transplant[2]
      local f = io.open(REF_F0, 'rb'); f:seek('set', bit.band(lo, 0x1FFFFF))
      local data = f:read(hi - lo); f:close()
      ffi.copy(mem + bit.band(lo, 0x1FFFFF), data, #data)
      log('transplant %08x..%08x (%d bytes)', lo, hi, #data)
    end
  else
    frame = frame + 1
  end
  if frame <= 320 then
    local m32 = ffi.cast('uint32_t*', PCSX.getMemPtr())
    local hs = {}
    for page = 0xD5, 0xFF do
      local h = 2166136261
      for a = page * 1024, page * 1024 + 1023 do h = bit.bxor(h, m32[a]); h = bit.tobit(h * 16777619) end
      hs[#hs + 1] = string.format('%08x', bit.band(h, 0xFFFFFFFF))
    end
    log('h %d %s', frame, table.concat(hs, ' '))
    if DUMP_AT and frame == DUMP_AT then save('f' .. frame) end
  end
  if frame == 300 or frame == 600 or frame == 900 then
    log('frame %d vs %d rand %08x', frame, vs, rd32(0x800E3650))
    save(frame)
  end
  if frame == (cur.frames or 900) then
    log('done'); out:close()
    say('cycle %s done', tag)
    frame, seeded = -1, false
    start_next()
  end
  return true
end)
B[#B + 1] = bp(0x800A6024, 'Exec', 4, 'cyc pads', function()
  if frame >= 0 and cur.pads ~= false then give(BUF1, script(0, frame)); give(BUF2, script(1, frame)) end
  return true
end)
B[#B + 1] = bp(0x800C56E8, 'Exec', 4, 'cyc rand', function()
  if frame >= 0 and out then
    local r = PCSX.getRegisters()
    local ra = r.GPR.n.ra
    if ra ~= 0x8009E304 and ra ~= 0x8009E344 then log('r %d %08x %08x', frame, ra, rd32(0x800E3650)) end
  end
end)
B[#B + 1] = bp(0x8008F6CC, 'Exec', 4, 'cyc vsync random', function()
  if frame >= 0 then PCSX.getRegisters().GPR.n.v0 = 0 end
  return true
end)

local function pad(n) return PCSX.SIO0.slots[n].pads[1] end
start_next = function()
  run_i = run_i + 1
  local r = RUNS[run_i]
  if not r then say('cycle all done'); VSYNC_HOOK = nil; return end
  tag, cur = r[1], r
  set_patch(FIXED_STEP and r.patch ~= false)
  out = io.open(DD_DIR .. '/cyc_' .. tag .. '.txt', 'w')
  log('run %s delay %d patch %s pads %s frames %d', tag, r[2], tostring(FIXED_STEP and r.patch ~= false),
      tostring(r.pads ~= false), r.frames or 900)
  seq = {{60, 1, 'START'}, {50, 1, 'UP'}, {40, 1, 'CROSS'}, {50, 1, 'UP'}, {40, 1, 'CROSS'}, {120, 1, 'CROSS'},
         {600, 1, 'CROSS'}, {100 + r[2], 2, 'CROSS'}}
  seq_i, seq_at = 1, vs
  say('cycle %s: delay %d', tag, r[2])
end
VSYNC_HOOK = function()
  if held and vs >= held[3] then pad(held[1]).clearOverride(held[2]); held = nil end
  if seq and seq[seq_i] and vs - seq_at >= seq[seq_i][1] then
    local s = seq[seq_i]
    local b = PCSX.CONSTS.PAD.BUTTON[s[3]]
    pad(s[2]).setOverride(b); held = {s[2], b, vs + 6}
    if seq_i == #seq then armed = true end
    seq_i, seq_at = seq_i + 1, vs
  end
end
start_next()
