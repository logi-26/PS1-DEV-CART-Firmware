-- Finds the menu strings in RAM and logs which code reads them (and the SIO1 registers, as before).
local ffi = require('ffi')
local logf = io.open('menu_watch.log', 'w')
local function say(...) logf:write(string.format(...) .. '\n'); logf:flush() end
local seen = {}
local function reader(label)
  return function()
    local pc = PCSX.getRegisters().pc
    local key = label .. string.format(' pc=%08x', pc)
    if not seen[key] then seen[key] = 0; say('READ %s', key) end
    seen[key] = seen[key] + 1
    return true
  end
end
-- SIO1 registers
for _, off in ipairs({0x1050, 0x1054, 0x1058, 0x105a, 0x105c, 0x105e}) do
  for _, kind in ipairs({'Read', 'Write'}) do
    PCSX.addBreakpoint(0x1f800000 + off, kind, 2, 'sio',
      reader('SIO1 ' .. kind .. ' ' .. string.format('%04x', off)))
  end
end
-- timer: the interrupt status register is read every frame
local hits, found = 0, false
local wanted = { 'NEW GAME', 'SKIRMISH', 'LINK GAME', 'OPTIONS' }
PCSX.addBreakpoint(0x1f801070, 'Read', 2, 'timer', function()
  hits = hits + 1
  if found or hits % 120 ~= 0 then return true end
  local ok, err = pcall(function()
    local s = ffi.string(PCSX.getMemPtr(), 0x200000)
    local any = false
    for _, w in ipairs(wanted) do
      local p = s:find(w .. '\0', 1, true)
      if p then
        any = true
        local addr = 0x80000000 + p - 1
        say('FOUND "%s" at %08x', w, addr)
        PCSX.addBreakpoint(addr, 'Read', 1, w, reader('TEXT ' .. w))
      end
    end
    if any then found = true end
  end)
  if not ok then say('scan error: %s', tostring(err)); found = true end
  return true
end)
say('watching')
