-- TOCA 2 Touring Cars (Europe, SLES_015.47, GAME.EXE) determinism test for pad play (research/toca2/NOTES.md).
-- Run from dd_auto.lua's ctl.lua:  TAG, DELAY = 'A', 0; dofile([[...toca2_det.lua]])
-- Loads DD_DIR/prerace.sst (car setup, player 2 on START RACE), waits DELAY vertical blanks, presses pad 2 X.
-- The race's start: its random table seeding (0x8009E2D0 called from 0x80074728, seed [0x800F0970]) given a fixed
-- seed (what the driver would do); from the next game frame (frame function 0x8004B3D0): both pads scripted by game frame (raw buffers written at the all-pads read
-- 0x800A6024), the time-mixed random 0x8008F6B8 given 0 instead of VSync(-1); rand calls logged (frame, caller);
-- screenshots and RAM at frames 300 / 600 / 900. Output: DD_DIR/det_TAG.txt, det_TAG_N.raw/.txt/.ram
local ffi = require('ffi')
local GP = 0x800D5A60
local STATE = GP + 0x408
local RAND_STATE = 0x800E3650
local BUF1, BUF2 = 0x800E0374, 0x800E0396
local mem = ffi.cast('uint8_t*', PCSX.getMemPtr())
local function rd32(a) return ffi.cast('uint32_t*', PCSX.getMemPtr())[bit.rshift(bit.band(a, 0x1FFFFF), 2)] end
local function wr32(a, v) ffi.cast('uint32_t*', PCSX.getMemPtr())[bit.rshift(bit.band(a, 0x1FFFFF), 2)] = v end
local function wr8(a, v) mem[bit.band(a, 0x1FFFFF)] = v end

local out = io.open(DD_DIR .. '/det_' .. TAG .. '.txt', 'w')
local function log(fmt, ...) out:write(string.format(fmt, ...) .. '\n'); out:flush() end

-- buttons (bits set = pressed): SELECT 0, START 3, UP 4, RIGHT 5, DOWN 6, LEFT 7, L2 8, R2 9, L1 10, R1 11, TR 12,
-- O 13, X 14, SQ 15
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

local frame = -1           -- game frames since the race's start (-1: not yet)
local seeded = false
local started_vs = nil
local BPS = {}
local function shot_ram(n)
  local s = PCSX.GPU.takeScreenShot()
  local f = io.open(DD_DIR .. '/det_' .. TAG .. '_' .. n .. '.raw', 'wb'); f:write(tostring(s.data)); f:close()
  local m = io.open(DD_DIR .. '/det_' .. TAG .. '_' .. n .. '.txt', 'w')
  m:write(string.format('%d %d %s\n', s.width, s.height, tostring(s.bpp))); m:close()
  local r = io.open(DD_DIR .. '/det_' .. TAG .. '_' .. n .. '.ram', 'wb')
  r:write(ffi.string(PCSX.getMemPtr(), 0x200000)); r:close()
end

BPS[#BPS + 1] = PCSX.addBreakpoint(0x8004B3D0, 'Exec', 4, 'det frame', function()
  if frame < 0 then
    if not seeded then return true end
    frame = 0
    log('race frame 0 at vs %d (delay %d)', vs, DELAY)
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
    if DUMP_AT and frame == DUMP_AT then
      local r = io.open(DD_DIR .. '/det_' .. TAG .. '_f' .. frame .. '.ram', 'wb')
      r:write(ffi.string(PCSX.getMemPtr(), 0x200000)); r:close()
    end
  end
  if frame == 300 or frame == 600 or frame == 900 then
    log('frame %d vs %d rand %08x', frame, vs, rd32(RAND_STATE))
    shot_ram(frame)
  end
  if frame == 900 then
    log('done')
    for _, b in ipairs(BPS) do b:remove() end
    out:close()
    say('det %s done', TAG)
  end
  return true
end)
BPS[#BPS + 1] = PCSX.addBreakpoint(0x8009E2D0, 'Exec', 4, 'det seed', function()
  local r = PCSX.getRegisters()
  if r.GPR.n.ra == 0x80074730 and started_vs then
    log('race seed %04x -> 1234 at vs %d', r.GPR.n.a0, vs)
    r.GPR.n.a0 = 0x1234
    seeded = true
  end
  return true
end)
BPS[#BPS + 1] = PCSX.addBreakpoint(0x800A6024, 'Exec', 4, 'det pads', function()
  if frame >= 0 then
    give(BUF1, script(0, frame)); give(BUF2, script(1, frame))
  end
  return true
end)
BPS[#BPS + 1] = PCSX.addBreakpoint(0x8008F6CC, 'Exec', 4, 'det vsync random', function()
  if frame >= 0 then PCSX.getRegisters().GPR.n.v0 = 0 end
  return true
end)
local rand_n = 0
BPS[#BPS + 1] = PCSX.addBreakpoint(0x800C56E8, 'Exec', 4, 'det rand', function()
  if frame >= 0 then
    rand_n = rand_n + 1
    if rand_n <= 4000 then log('r %d %08x', frame, PCSX.getRegisters().GPR.n.ra) end
  end
  return true
end)

local f = Support.File.open(DD_DIR .. '/prerace.sst')
PCSX.loadSaveState(f); f:close()
-- FIXED_STEP: the frame's time step (0x800A14D0, the root counter 2 time measured by the pacer) made a constant
-- 0x28000 (40 ms: two PAL vertical blanks): lui v0, 2 / jr ra / ori v0, v0, 0x8000
if FIXED_STEP then
  wr32(0x800A14D0, 0x3C020002); wr32(0x800A14D4, 0x03E00008); wr32(0x800A14D8, 0x34428000)
end
local t0 = vs
VSYNC_HOOK = function()
  if not started_vs and vs - t0 >= DELAY + 5 then
    started_vs = vs
    local P2 = PCSX.SIO0.slots[2].pads[1]
    P2.setOverride(PCSX.CONSTS.PAD.BUTTON.CROSS)
  elseif started_vs and vs - started_vs == 6 then
    PCSX.SIO0.slots[2].pads[1].clearOverride(PCSX.CONSTS.PAD.BUTTON.CROSS)
  end
end
say('det %s: delay %d', TAG, DELAY)
