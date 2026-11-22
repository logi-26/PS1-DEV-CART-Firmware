-- Red Alert Retaliation (SLES_013.43): what its link connect routine (0x80114F20) sees and returns.
--   0x80114FB0: the SIO1 status at the start (bit 0x80: DSR -> gives up, result 1)
--   0x80114FD8: is CTS already high (non-zero: the other console got there first -> responder)
--   0x80115004: each poll for CTS while waiting
--   0x801150B4: the result (0: linked, 1: DSR high at the start, 2: CTS never came)
local logf = io.open('link_watch.log', 'w')
local function frame()
  local m = PCSX.getMemPtr()
  return m[0x1B4EB8] + m[0x1B4EB9] * 256 + m[0x1B4EBA] * 65536
end
local function say(fmt, ...)
  logf:write(string.format('frame %d  ', frame()) .. string.format(fmt, ...) .. '\n')
  logf:flush()
end
local polls = 0
local function at(addr, what)
  PCSX.addBreakpoint(addr, 'Exec', 4, what, function()
    local r = PCSX.getRegisters().GPR.n
    if addr == 0x80115004 then
      polls = polls + 1
      if polls <= 5 or polls % 50 == 0 then say('%s: v0=%08x (poll %d)', what, r.v0, polls) end
    else
      say('%s: v0=%08x', what, r.v0)
      if addr == 0x801150B4 then polls = 0 end
    end
    return true
  end)
end
at(0x80114F20, 'connect called')
at(0x80114FB0, 'status at the start (DSR = 0x80)')
at(0x80114FD8, 'CTS already high?')
at(0x80115004, 'waiting for CTS')
at(0x801150B4, 'connect returns (0 ok, 1 DSR, 2 no CTS)')
-- Heartbeat: the frame counter's store (0x800C7CA8), logged every 250 frames: the breakpoints work
local beats = 0
PCSX.addBreakpoint(0x800C7CA8, 'Exec', 4, 'frame', function()
  beats = beats + 1
  if beats % 250 == 1 then say('heartbeat %d', beats) end
  return true
end)
-- The SIO1 registers: the first access of each kind from each place
local seen = {}
local names = { [0x1050]='DATA', [0x1054]='STAT', [0x1058]='MODE', [0x105A]='CTRL', [0x105E]='BAUD' }
for off, name in pairs(names) do
  for _, kind in ipairs({'Read', 'Write'}) do
    PCSX.addBreakpoint(0x1F800000 + off, kind, 2, 'sio', function()
      local key = string.format('%s %s pc=%08x', kind, name, PCSX.getRegisters().pc)
      if not seen[key] then seen[key] = true; say('SIO1 %s', key) end
      return true
    end)
  end
end
say('watching')
