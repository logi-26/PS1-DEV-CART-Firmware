-- Logs every access to the SIO1 registers (0x1F801050-0x1F80105F) with the PC that made it.
local logf = io.open('sio_access.log', 'w')
local counts, order, total = {}, {}, 0
local names = { [0x1050]='DATA', [0x1054]='STAT', [0x1058]='MODE', [0x105A]='CTRL', [0x105C]='MISC', [0x105E]='BAUD', [0x1070]='ISTAT' }
local function dump()
  local f = io.open('sio_summary.log', 'w')
  for _, k in ipairs(order) do f:write(string.format('%s x%d\n', k, counts[k])) end
  f:close()
end
local function hook(kind, addr, width)
  return function()
    local pc = PCSX.getRegisters().pc
    local key = string.format('%s %s pc=%08x', kind, names[addr & 0xffff] or '?', pc)
    if not counts[key] then counts[key] = 0; table.insert(order, key); logf:write(key .. '\n'); logf:flush() end
    counts[key] = counts[key] + 1
    total = total + 1
    if total % 2000 == 0 then dump() end
    return true
  end
end
for addr, _ in pairs(names) do
  local a = 0x1f800000 + addr
  PCSX.addBreakpoint(a, 'Read', 2, 'sio watch r', hook('R', a, 2))
  PCSX.addBreakpoint(a, 'Write', 2, 'sio watch w', hook('W', a, 2))
end
logf:write('watching\n'); logf:flush()
