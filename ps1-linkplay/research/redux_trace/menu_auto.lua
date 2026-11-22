-- Research helper for driving Red Alert's menus from outside (auto LINK GAME).
-- Commands: a file ctl.lua next to the log, run once each time it changes. Helpers for it:
--   press('CROSS', frames)   hold a pad button (PCSX.CONSTS.PAD.BUTTON names) for some vsyncs
--   dump('name')             RAM (2 MB) to <dir>/name.ram
--   save(name) / loadst(name)   savestate to/from <dir>/name.sst
--   watch(addr, 'Write'|'Read', len)  log every PC that touches addr (first 20 per PC)
--   unwatch()                remove the watches
-- MENU_DIR (set by the launcher) is the folder for the log and files.
local ffi = require('ffi')
local logf = io.open(MENU_DIR .. '/menu.log', 'w')
local vs = 0
function say(fmt, ...)
  logf:write(string.format('%7d  ', vs) .. string.format(fmt, ...) .. '\n'); logf:flush()
end

say('menu_auto loading')
local keys = {}
for k, v in pairs(PCSX) do keys[#keys + 1] = k .. ':' .. type(v) end
say('PCSX: %s', table.concat(keys, ' '))
B, pad = nil, nil
local okp, errp = pcall(function()
  B = PCSX.CONSTS.PAD.BUTTON
  pad = PCSX.SIO0.slots[1].pads[1]
end)
say('pad api: %s %s', tostring(okp), tostring(errp))
local held = {}                -- button -> vsync it is released
function press(name, frames)
  local b = B[name]
  if not b then say('no button %s', name) return end
  if not pad then say('no pad api') return end
  pad.setOverride(b)
  held[b] = vs + (frames or 3)
end

-- a queue of {button, frames, gap}: one after another
local queue, qnext = {}, 0
function seq(list)
  for _, e in ipairs(list) do queue[#queue + 1] = e end
end

function dump(name)
  local f = io.open(MENU_DIR .. '/' .. name .. '.ram', 'wb')
  f:write(ffi.string(PCSX.getMemPtr(), 0x200000)); f:close()
  say('dumped %s', name)
end

function save(name)
  local s = PCSX.createSaveState()
  local f = Support.File.open(MENU_DIR .. '/' .. name .. '.sst', 'TRUNCATE')
  f:writeMoveSlice(s); f:close()
  say('saved %s', name)
end

function loadst(name)
  local f = Support.File.open(MENU_DIR .. '/' .. name .. '.sst')
  PCSX.loadSaveState(f); f:close()
  say('loaded %s', name)
end

local watches = {}
function watch(addr, kind, len)
  local seen = {}
  watches[#watches + 1] = PCSX.addBreakpoint(addr, kind or 'Write', len or 4, 'w', function()
    local r = PCSX.getRegisters()
    local pc = r.pc
    seen[pc] = (seen[pc] or 0) + 1
    if seen[pc] <= 20 then
      local g = r.GPR.n
      say('%s %08x by pc=%08x ra=%08x v0=%x v1=%x a0=%x a1=%x s0=%x', kind or 'Write', addr, pc, g.ra, g.v0, g.v1,
          g.a0, g.a1, g.s0)
    end
    return true
  end)
  say('watching %08x', addr)
end
function unwatch() watches = {}; collectgarbage(); say('watches removed') end

function rd32(a)
  local m = ffi.cast('uint32_t*', PCSX.getMemPtr())
  return m[bit.rshift(bit.band(a, 0x1FFFFF), 2)]
end
function rd8(a)
  local m = ffi.cast('uint8_t*', PCSX.getMemPtr())
  return m[bit.band(a, 0x1FFFFF)]
end

local ctl_path = MENU_DIR .. '/ctl.lua'
local ctl_seen, ctl_check = nil, 0
local function check_ctl()
  local now = os.clock()
  if now - ctl_check < 0.5 then return end
  ctl_check = now
  local f = io.open(ctl_path, 'rb')
  if not f then return end
  local code = f:read('a'); f:close()
  if code == ctl_seen then return end
  ctl_seen = code
  local fn, err = load_code(code)
  if not fn then say('ctl: %s', tostring(err)) return end
  local ok, res = pcall(fn)
  say('ctl ran: %s %s', tostring(ok), tostring(res))
end
load_code = loadstring

VSYNC_HOOK = nil               -- ctl.lua may set a function run every vsync
listener = PCSX.Events.createEventListener('GPU::Vsync', function()
  vs = vs + 1
  for b, t in pairs(held) do
    if vs >= t then pad.clearOverride(b); held[b] = nil end
  end
  if #queue > 0 and vs >= qnext and next(held) == nil then
    local e = table.remove(queue, 1)
    if e[1] ~= 'WAIT' then press(e[1], e[2] or 3) end
    qnext = vs + (e[2] or 3) + (e[3] or 8)
  end
  if VSYNC_HOOK then pcall(VSYNC_HOOK) end
  pcall(check_ctl)
end)
say('menu_auto ready')
