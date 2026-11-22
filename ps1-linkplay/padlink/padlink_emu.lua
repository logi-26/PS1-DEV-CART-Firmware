-- The kernel hook's exchange in PCSX-Redux, for padrelay.py --emu DIR (the relay installs and runs the driver itself,
-- exactly as on a console): requests "seq X adrs wdata(hex or -) rsize" appended to DD_DIR/pl_cmd.txt, answered in
-- pl_ans.txt ("seq hex", "seq ok"): served at each vertical blank and each time the driver calls pl_service (as the
-- kernel hook serves USB at each interrupt and in pl_service). While the driver waits for the others (its state 3)
-- with nothing to serve, the emulator is held a millisecond each time (a console waits in real time).
-- dofile'd after dd_auto.lua with TAG, PL_LUA (padlink_ctr.lua). Screenshots at frames 300 / 600 / 900 of each race.
local ffi = require('ffi')
pcall(ffi.cdef, 'void Sleep(uint32_t ms);')
local function sleep_ms(n) pcall(function() ffi.C.Sleep(n) end) end
local mem = ffi.cast('uint8_t*', PCSX.getMemPtr())
local function a(x) return bit.band(x, 0x1FFFFF) end
local J = dofile(PL_LUA)
local BUF = J.buf

local cmd_f, partial = nil, ''
local ans_f = nil
local served = 0
local function serve()
  if not cmd_f then
    cmd_f = io.open(DD_DIR .. '/pl_cmd.txt', 'r'); if not cmd_f then return false end
  end
  ans_f = ans_f or io.open(DD_DIR .. '/pl_ans.txt', 'a')
  local pos = cmd_f:seek('cur', 0)
  local size = cmd_f:seek('end', 0)
  if size < pos then cmd_f:seek('set', 0); partial = ''   -- (a new relay emptied it)
  else cmd_f:seek('set', pos) end
  local data = cmd_f:read('*a')
  if not data or data == '' then return false end
  data = partial .. data
  local any = false
  for line in data:gmatch('([^\n]*)\n') do
    local seq, adrs, wd, rs = line:match('^(%d+) X (%x+) (%S+) (%d+)$')
    if seq then
      adrs = tonumber(adrs, 16)
      if wd ~= '-' then
        for i = 0, #wd / 2 - 1 do mem[a(adrs + i)] = tonumber(wd:sub(2 * i + 1, 2 * i + 2), 16) end
        if adrs >= 0x80010000 and adrs < 0x8008C000 then PCSX.invalidateCache() end   -- (the game's code)
      end
      rs = tonumber(rs)
      if wd ~= '-' then adrs = adrs + #wd / 2 end      -- (as the kernel hook: the read follows what was written)
      if rs > 0 then
        local t = {}
        for i = 0, rs - 1 do t[#t + 1] = string.format('%02x', mem[a(adrs + i)]) end
        ans_f:write(seq .. ' ' .. table.concat(t) .. '\n')
      else
        ans_f:write(seq .. ' ok\n')
      end
      served = served + 1
      any = true
    end
  end
  partial = data:match('[^\n]*$') or ''
  ans_f:flush()
  return any
end

local last_frame = -1
PL_BPS = PL_BPS or {}
for _, b in ipairs(PL_BPS) do b:remove() end
PL_BPS = {}
PL_BPS[1] = PCSX.addBreakpoint(J.service, 'Exec', 4, 'pl_service', function()
  local got = serve()
  local state = mem[a(BUF + 0xA6)]
  if state == 3 and not got then sleep_ms(1) end
  local fr = mem[a(BUF + 0xA2)] + 256 * mem[a(BUF + 0xA3)]
  if fr ~= last_frame and (fr == 300 or fr == 600 or fr == 900) then
    shot(string.format('%s_rl_r%d_%d', TAG, mem[a(BUF + 0xA0)], fr))
    say('%s race %d frame %d at vsync %d', TAG, mem[a(BUF + 0xA0)], fr, vs)
  end
  last_frame = fr
  return true
end)
VSYNC_HOOK = function() serve() end
say('%s exchange ready', TAG)
return TAG .. ' padlink_emu'
