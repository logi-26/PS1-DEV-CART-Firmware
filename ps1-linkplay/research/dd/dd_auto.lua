-- Destruction Derby (SCUS_943.02) research: pads given to the game from a script, as pad play would (research/dd).
-- The game reads its pads in 0x80040E18 from the BIOS buffers (InitPAD: port 1 0x801E72EC, port 2 0x801E730E):
-- a breakpoint there writes the scripted buttons into both buffers just before, and counts the game's pad reads
-- ("reads": the game's own frames, whatever the vertical blanks).
-- Commands: a file ctl.lua next to the log, run each time it changes. Helpers for it:
--   pads(p1, p2)            buttons held from now on (names: 'X', 'O', 'SQ', 'TR', 'START', 'SELECT', 'UP',
--                           'DOWN', 'LEFT', 'RIGHT', 'L1', 'R1', 'L2', 'R2'; a table of names, or nil for none)
--   tap(p, name, reads)     one button on pad p (1 or 2) for some reads (default 3), then released
--   script(list)            {read, p1, p2} entries: from that read on (counted from script()), those buttons
--   shot(name)              screenshot to <dir>/name.raw/.txt (shot2png.py)
--   dump(name)              RAM to <dir>/name.ram
--   hashes(every, n, name)  from now: a hash of RAM regions every `every` reads, n times, to <dir>/name.txt
-- DD_DIR (set by the launcher) is the folder.
local ffi = require('ffi')
local logf = io.open(DD_DIR .. '/dd.log', 'w')
vs, reads = 0, 0
function say(fmt, ...)
  logf:write(string.format('%7d %6d  ', vs, reads) .. string.format(fmt, ...) .. '\n'); logf:flush()
end

BTN = {SELECT = 0, L3 = 1, R3 = 2, START = 3, UP = 4, RIGHT = 5, DOWN = 6, LEFT = 7, L2 = 8, R2 = 9, L1 = 10,
       R1 = 11, TR = 12, O = 13, X = 14, SQ = 15}
local function mask(names)
  local m = 0
  if type(names) == 'string' then names = {names} end
  for _, n in ipairs(names or {}) do m = bit.bor(m, bit.lshift(1, BTN[n])) end
  return m
end

held = {0, 0}                  -- buttons held on each pad (bits set = pressed)
local taps = {}                -- {pad, mask, until_read}
local sched, sched_base = nil, 0
function pads(p1, p2) held = {mask(p1), mask(p2)} end
function tap(p, name, n) taps[#taps + 1] = {p, mask(name), reads + (n or 3)} end
function script(list) sched, sched_base = list, reads; say('script: %d entries', #list) end

local mem = ffi.cast('uint8_t*', PCSX.getMemPtr())
local function wr8(a, v) mem[bit.band(a, 0x1FFFFF)] = v end
local function rd32(a) return ffi.cast('uint32_t*', PCSX.getMemPtr())[bit.rshift(bit.band(a, 0x1FFFFF), 2)] end
function rd(a) return rd32(a) end

local function give(buf, m)    -- a digital pad's answer: status 0, type 41h, buttons active low
  local v = bit.band(bit.bnot(m), 0xFFFF)
  wr8(buf, 0x00); wr8(buf + 1, 0x41); wr8(buf + 2, bit.band(v, 0xFF)); wr8(buf + 3, bit.rshift(v, 8))
end

hash_every, hash_left, hash_file = 0, 0, nil
local regions = {}             -- {from, to} (set by ctl.lua: the game's data; default all of RAM but the stack)
function hash_regions(list) regions = list end
local function ram_hash()
  local h = 2166136261
  local m32 = ffi.cast('uint32_t*', PCSX.getMemPtr())
  local list = (#regions > 0) and regions or {{0x80010000, 0x801F0000}}
  for _, r in ipairs(list) do
    for a = bit.rshift(bit.band(r[1], 0x1FFFFF), 2), bit.rshift(bit.band(r[2], 0x1FFFFF), 2) - 1 do
      h = bit.bxor(h, m32[a]); h = bit.tobit(h * 16777619)
    end
  end
  return bit.band(h, 0xFFFFFFFF)
end
function hashes(every, n, name)
  hash_every, hash_left, hash_base = every, n, reads
  hash_file = io.open(DD_DIR .. '/' .. name .. '.txt', 'w')
  say('hashes every %d reads, %d times -> %s', every, n, name)
end

-- (the game's pad routine and its two buffers: Destruction Derby's unless the launcher set others; PAD_READ = 0:
-- no hook, the pads left to the emulator)
PAD_READ = PAD_READ or 0x80040E18
PAD_BUFS = PAD_BUFS or {0x801E72EC, 0x801E730E}
if PAD_READ ~= 0 then PAD_BP = PCSX.addBreakpoint(PAD_READ, 'Exec', 4, 'pad read', function()
  reads = reads + 1
  local m = {held[1], held[2]}
  if sched then
    local rel = reads - sched_base
    for _, e in ipairs(sched) do
      if rel >= e[1] then m = {mask(e[2]), mask(e[3])} end
    end
  end
  for i = #taps, 1, -1 do
    local t = taps[i]
    if reads <= t[3] then m[t[1]] = bit.bor(m[t[1]], t[2]) else table.remove(taps, i) end
  end
  give(PAD_BUFS[1], m[1]); give(PAD_BUFS[2], m[2])
  if hash_file and hash_left > 0 and (reads - hash_base) % hash_every == 0 then
    hash_file:write(string.format('%d %08x %d\n', reads - hash_base, ram_hash(), vs)); hash_file:flush()
    hash_left = hash_left - 1
    if hash_left == 0 then hash_file:close(); hash_file = nil; say('hashes done') end
  end
  return true
end) end

-- The emulator's own pad 1 (games whose pads aren't hooked): hold(name, vsyncs), PCSX.CONSTS.PAD.BUTTON names
EMU_PAD = PCSX.SIO0.slots[1].pads[1]
local emu_release = {}
function hold(name, n)
  local b = PCSX.CONSTS.PAD.BUTTON[name]
  EMU_PAD.setOverride(b); emu_release[b] = vs + (n or 3)
end

function dump(name)
  local f = io.open(DD_DIR .. '/' .. name .. '.ram', 'wb')
  f:write(ffi.string(PCSX.getMemPtr(), 0x200000)); f:close()
  say('dumped %s', name)
end
function shot(name)
  local s = PCSX.GPU.takeScreenShot()
  local f = io.open(DD_DIR .. '/' .. name .. '.raw', 'wb'); f:write(tostring(s.data)); f:close()
  local m = io.open(DD_DIR .. '/' .. name .. '.txt', 'w')
  m:write(string.format('%d %d %s\n', s.width, s.height, tostring(s.bpp))); m:close()
  say('shot %s', name)
end

local ctl_path = DD_DIR .. '/ctl.lua'
local ctl_seen, ctl_check = nil, 0
local function check_ctl()
  local now = os.clock()
  if now - ctl_check < 0.3 then return end
  ctl_check = now
  local f = io.open(ctl_path, 'rb')
  if not f then return end
  local code = f:read('a'); f:close()
  if code == ctl_seen then return end
  ctl_seen = code
  local fn, err = loadstring(code)
  if not fn then say('ctl: %s', tostring(err)) return end
  local ok, res = pcall(fn)
  say('ctl ran: %s %s', tostring(ok), tostring(res))
end

VSYNC_HOOK = nil
LISTENER = PCSX.Events.createEventListener('GPU::Vsync', function()
  vs = vs + 1
  for b, t in pairs(emu_release) do if vs >= t then EMU_PAD.clearOverride(b); emu_release[b] = nil end end
  if VSYNC_HOOK then
    local ok, e = pcall(VSYNC_HOOK)
    if not ok then say('hook: %s', tostring(e)); VSYNC_HOOK = nil end
  end
  pcall(check_ctl)
end)
say('dd_auto ready')
