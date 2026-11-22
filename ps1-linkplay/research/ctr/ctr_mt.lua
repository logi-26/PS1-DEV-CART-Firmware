-- CTR research helper (2026-11-17): a fake multitap on port 1 with MT_N pads, all players' buttons from the script.
-- dofile'd by ctl.lua after dd_auto.lua (dd_run.py --game ctr). The game's pad system (base 0x80096804) reads port 1's
-- raw buffer 0x80096AD4 (0x22 bytes): byte 0 = 0 and byte 1 = 0x80 -> a multitap, 4 slots of 8 bytes from +2
-- (status 0 = connected, type 41h, buttons x2 active low); else ports 1 and 2 single pads (0x80096AF6).
-- Written each vertical blank at the game's pad code (0x80025410) and again at each frame's read (0x80025738).
--   mt(n)               n pads connected (0: the multitap off, the emulator's own pads)
--   press(p, name, n)   player p (1-4) holds a button for n vertical blanks (default 4)
--   seq(list, gap)      {p, name} entries one after another, `gap` blanks apart (default 30); seq_done() when over
local ffi = require('ffi')
local mem = ffi.cast('uint8_t*', PCSX.getMemPtr())
local function wr8(a, v) mem[bit.band(a, 0x1FFFFF)] = v end
local BUF = 0x80096AD4
MT_N = MT_N or 4
MT_HELD = {0, 0, 0, 0}
local rel = {}                 -- {p, mask, until vs}
function mt(n) MT_N = n end
function press(p, name, n)
  local m = bit.lshift(1, BTN[name]); rel[#rel + 1] = {p, m, vs + (n or 4)}
  MT_HELD[p] = bit.bor(MT_HELD[p], m)
end
local function fill()
  if MT_N == 0 then return end
  wr8(BUF, 0); wr8(BUF + 1, 0x80)
  for s = 0, 3 do
    local a = BUF + 2 + 8 * s
    if s < MT_N then
      local v = bit.band(bit.bnot(MT_HELD[s + 1]), 0xFFFF)
      wr8(a, 0); wr8(a + 1, 0x41); wr8(a + 2, bit.band(v, 0xFF)); wr8(a + 3, bit.rshift(v, 8))
      for i = 4, 7 do wr8(a + i, 0xFF) end
    else
      wr8(a, 0xFF); for i = 1, 7 do wr8(a + i, 0) end
    end
  end
end
MT_BPS = MT_BPS or {}
for _, b in ipairs(MT_BPS) do b:remove() end
MT_BPS = {}
MT_BPS[1] = PCSX.addBreakpoint(0x80025410, 'Exec', 4, 'mt vblank', function() fill(); return true end)
MT_BPS[2] = PCSX.addBreakpoint(0x80025738, 'Exec', 4, 'mt frame', function() fill(); return true end)

local queue, gap, next_at = {}, 30, 0
SEQ_DONE = true
function seq(list, g) queue, gap, next_at, SEQ_DONE, SEQ_ACTIVE = list, g or 30, vs + 5, false, true end
MT_HOOK = function()
  for i = #rel, 1, -1 do
    local r = rel[i]
    if vs >= r[3] then MT_HELD[r[1]] = bit.band(MT_HELD[r[1]], bit.bnot(r[2])); table.remove(rel, i) end
  end
  if #queue > 0 and vs >= next_at then
    local e = table.remove(queue, 1)
    if e[1] == 0 then next_at = vs + e[2]     -- {0, n}: wait n blanks
    else press(e[1], e[2], e[3]); next_at = vs + gap end
  elseif SEQ_ACTIVE and #queue == 0 and vs >= next_at then
    SEQ_DONE = true; SEQ_ACTIVE = false
  end
end
return 'ctr_mt ready'
