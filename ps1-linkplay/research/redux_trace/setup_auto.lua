-- Research (run as ctl.lua of run_pair.py --hw, one copy per emulator, SETUP_ROLE set first): the automatic link
-- play as the relay does it (main menu -> LINK GAME -> country/color -> START), then at the setup screen: RAM dumps,
-- screenshots and the menu's items logged, to find the game settings and the host's start.
-- A ("host") starts at once; B ("join") once A's RTS reaches it (the driver's peer_lines, 0x8000D8CC bit 1).
local a = ...
local ffi = require('ffi')
local dir = SETUP_DIR
local function r32(adr) return a.r32(adr) end
local function w32(adr, v) a.w32(adr, v) end
local function say(...) a.say(...) end
local function dump(name)
  local f = io.open(dir .. '/' .. name .. '.ram', 'wb')
  f:write(ffi.string(PCSX.getMemPtr(), 0x200000)); f:close()
  say('SETUP dumped %s', name)
end
local function shot(name)
  local s = PCSX.GPU.takeScreenShot()
  local f = io.open(dir .. '/' .. name .. '.raw', 'wb'); f:write(tostring(s.data)); f:close()
  local m = io.open(dir .. '/' .. name .. '.txt', 'w')
  m:write(string.format('%d %d %s\n', s.width, s.height, tostring(s.bpp))); m:close()
  say('SETUP shot %s', name)
end
local function menu_log(tag)
  local m = 0x801A9DD8
  local items = r32(m + 4)
  local parts = {}
  for i = 0, 15 do parts[#parts + 1] = string.format('%x', r32(m + i * 4)) end
  say('SETUP %s menu: %s', tag, table.concat(parts, ' '))
  local n = r32(m)
  if n > 0 and n < 20 and items >= 0x80000000 then
    for k = 0, n - 1 do
      local it = {}
      for i = 0, 12 do it[#it + 1] = string.format('%x', r32(items + k * 0x34 + i * 4)) end
      say('SETUP %s item %d @%08x: %s', tag, k, items + k * 0x34, table.concat(it, ' '))
    end
  end
end

local step, t, shots = 0, 0, 0
local country, color = (SETUP_ROLE == 'host') and 0 or 1, (SETUP_ROLE == 'host') and 2 or 3
SETUP_LISTENER = PCSX.Events.createEventListener('GPU::Vsync', function()
  t = t + 1
  local ok, err = pcall(function()
    if step == 0 then                                   -- main menu showing (and B: the host's RTS seen)
      if r32(0x801A9A98) == 0x801A9AE4 and r32(0x801A9AAC) == 0x1000 and
         (SETUP_ROLE == 'host' or bit.band(r32(0x8000D8CC), 2) ~= 0) then
        w32(0x801A9DF0, 0); w32(0x801A9EB0, 0); w32(0x8000D978, 0x80013024)
        step = 1; say('SETUP %s: armed at the main menu', SETUP_ROLE)
      end
    elseif step == 1 then                               -- the link page showing
      if r32(0x8000D978) == 0 and r32(0x801A9DDC) == 0x801A9E28 and r32(0x801A9EC0) == 0x80019804 and
         r32(0x801A9DF0) == 0x1000 then
        w32(0x801A9E48, country); w32(0x801A9E7C, color); w32(0x801A9EB0, 4)
        step = 2; t = 0; say('SETUP %s: START written', SETUP_ROLE)
      end
    elseif step == 2 then                               -- the setup screen: its menu has 11 items (0x80018D6C)
      if r32(0x801A9DD8) == 11 then
        step = 3; t = 0; say('SETUP %s: setup screen', SETUP_ROLE)
      end
    elseif step == 3 then
      if t == 100 then menu_log('s0'); dump(SETUP_ROLE .. '_setup0'); shot(SETUP_ROLE .. '_setup0') end
      if t > 100 and t % 250 == 0 and shots < 6 then
        shots = shots + 1; menu_log('s' .. shots); shot(SETUP_ROLE .. '_setup' .. shots)
      end
    end
  end)
  if not ok then say('SETUP error %s', tostring(err)); SETUP_LISTENER = nil end
end)
return 'setup_auto ' .. SETUP_ROLE
