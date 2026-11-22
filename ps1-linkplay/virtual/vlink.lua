-- Virtual link cable in PCSX-Redux (see vlink.c): installs vlink.bin when Red Alert Retaliation starts, patches the
-- three serial library functions to it, and carries its rings to the other emulator through two files.
-- Set before dofile: VLINK_NAME ('A'/'B'), VLINK_BIN (vlink.bin), VLINK_OUT (file this side writes), VLINK_IN (the
-- other side's), VLINK_LOG.
local ffi = require('ffi')
ffi.cdef[[
int QueryPerformanceCounter(int64_t *count);
int QueryPerformanceFrequency(int64_t *freq);
]]
local qpc = ffi.new('int64_t[1]')
ffi.C.QueryPerformanceFrequency(qpc)
local QPF = tonumber(qpc[0])
local function wall()       -- seconds, real time (os.clock is CPU time)
  ffi.C.QueryPerformanceCounter(qpc)
  return tonumber(qpc[0]) / QPF
end
local WAIT_MAX = 0.020      -- the longest one wait holds the emulator, in s
local HOLD_PER_FRAME = 0.015    -- the most all waits hold it between two vertical blanks (so its window still
                                -- answers, and a wait that never ends still reaches the game's own timeout)
local hold_left = HOLD_PER_FRAME

local CODE, BUF = 0x8000C000, 0x8000D000
local RX_SIZE, TX_SIZE, LN_SIZE = 4096, 4096, 16
local SERVICE = CODE + 0x18
if VLINK_HW then            -- the PS1 DEV CART build (build_hw.py): in the serial library's gaps, rings at 0x8000D810
  BUF, RX_SIZE, TX_SIZE, SERVICE = VLINK_HW.buf, VLINK_HW.rx_size, VLINK_HW.tx_size, VLINK_HW.service
end
-- (vlink.c VLinkBuf: magic, the receive area, the carrier's counters, the game's counters, the send area, stats.
-- Not rings: received bytes are given only into an empty area, ending where it ends; sent bytes start at tx_base)
local P0 = 4 + RX_SIZE       -- (the carrier's counters)
local C0 = P0 + 36           -- (the game's part)
local O = { magic = 0, rx = 4, rx_head = P0, tx_tail = P0 + 4, ln_head = P0 + 8, credit_in = P0 + 12,
            abort = P0 + 16, ln_ring = P0 + 20,
            rx_tail = C0, tx_head = C0 + 4, ln_tail = C0 + 8, my_lines = C0 + 12, saved = C0 + 16,
            peer_lines = C0 + 20, peer_seen = C0 + 24, credit_used = C0 + 28, waiting = C0 + 32, tx_base = C0 + 36,
            tx = C0 + 40, stats = C0 + 40 + TX_SIZE }
local SIZE = 112 + RX_SIZE + TX_SIZE
local ENTRY = 0x80154E4C
local PATCHES = {           -- address, original first word, blob offset
  { 0x80154354, 0x27BDFFE8, 0x00 },    -- _sio_control
  { 0x80153DF4, 0x27BDFFD8, 0x08 },    -- read
  { 0x801540D8, 0x27BDFFD0, 0x10 },    -- write
}

local mem = PCSX.getMemPtr()
local m32 = ffi.cast('uint32_t *', mem)
local function off(a) return bit.band(a, 0x1FFFFF) end
local function r32(a) return m32[off(a) / 4] end
local function w32(a, v) m32[off(a) / 4] = v end
local function B(field) return BUF + O[field] end

local logf = io.open(VLINK_LOG, 'w')
local t0 = os.clock()
local function say(fmt, ...)
  logf:write(string.format('%9.3f  ', os.clock() - t0) .. string.format(fmt, ...) .. '\n')
  logf:flush()
end

local out = io.open(VLINK_OUT, 'wb')
local inf = nil
local in_pos = 0
local inbuf = ''            -- received, not parsed yet
local pend_data = {}        -- received data bytes waiting for room in rx (array of strings)
local pend_data_len = 0
local pend_lines = {}       -- received line changes waiting for room in ln
local installed = false
local counts = { sent = 0, recv = 0, lines_out = 0, lines_in = 0, services = 0, credit = 0, taken_back = 0 }
local last_in_check = 0

local function install()
  for _, p in ipairs(PATCHES) do
    if r32(p[1]) ~= p[2] then
      say('not patched: %08x is %08x, not %08x (another version of the game?)', p[1], tonumber(r32(p[1])), p[2])
      return
    end
  end
  for i = 0, SIZE - 1 do mem[off(BUF) + i] = 0 end
  local targets = {}
  if VLINK_HW then
    for _, g in ipairs(VLINK_HW.gaps) do
      for i = 0, #g.hex / 2 - 1 do mem[off(g.adrs) + i] = tonumber(g.hex:sub(i * 2 + 1, i * 2 + 2), 16) end
    end
    targets = { VLINK_HW.control, VLINK_HW.read, VLINK_HW.write }
  else
    local f = io.open(VLINK_BIN, 'rb')
    local blob = f:read('*a')
    f:close()
    for i = 1, #blob do mem[off(CODE) + i - 1] = blob:byte(i) end
    for i, p in ipairs(PATCHES) do targets[i] = CODE + p[3] end
  end
  w32(B('magic'), 0x4B4E4C56)
  w32(B('peer_seen'), 1)
  for i, p in ipairs(PATCHES) do
    w32(p[1], 0x08000000 + math.floor(targets[i] / 4) % 0x4000000)
    w32(p[1] + 4, 0)
  end
  if VLINK_HW and VLINK_HW.words then       -- (the in-game exchange made symmetric: vlink.c vlink_free_write)
    for _, w in ipairs(VLINK_HW.words) do
      if r32(w.adrs) == w.orig then w32(w.adrs, w.new) else say('word %08x not patched: %08x', w.adrs, tonumber(r32(w.adrs))) end
    end
  end
  if PCSX.invalidateCache then PCSX.invalidateCache() end
  installed = true
  say('installed (%s), rings at %08x; patched _sio_control, read, write', VLINK_HW and 'PS1 DEV CART layout' or
      'blob at ' .. string.format('%08x', CODE), BUF)
end

-- Second headers (see usblink.c VL_IS_HEADER): our next 6-byte read after the other's header gets its copy at once
-- (only when asked for: our request still goes to the other side, and its second header is dropped)
local function is_header(s) return #s == 6 and s:byte(1) == 0xBD and s:byte(2) == 0xDA and s:byte(5) == 0xAF and
                                   s:byte(6) == 0xDE end
local copy_hdr, copy_t, copy_used, out_part = nil, 0, false, ''
local dup_ok, dup_bad = 0, 0

-- Received records -> pending data / line changes
local function parse()
  local p = 1
  while true do
    local t = inbuf:byte(p)
    if t == nil then break end
    if t == 0 then
      if #inbuf - p + 1 < 3 then break end
      local n = inbuf:byte(p + 1) + inbuf:byte(p + 2) * 256
      if #inbuf - p + 1 < 3 + n then break end
      local msg = inbuf:sub(p + 3, p + 2 + n)
      if is_header(msg) and copy_used then
        if msg == copy_used then dup_ok = dup_ok + 1 else dup_bad = dup_bad + 1; say('second header differs') end
        copy_used = false
      else
        copy_used = false
        if is_header(msg) then copy_hdr, copy_t = msg, wall() else copy_hdr = nil end
        if n > 0 then
          table.insert(pend_data, msg)
          pend_data_len = pend_data_len + n
        end
      end
      counts.recv = counts.recv + n
      p = p + 3 + n
    elseif t == 1 then
      if #inbuf - p + 1 < 2 then break end
      table.insert(pend_lines, inbuf:byte(p + 1))
      counts.lines_in = counts.lines_in + 1
      say('in: lines %d (DTR %d RTS %d)', inbuf:byte(p + 1), inbuf:byte(p + 1) % 2, math.floor(inbuf:byte(p + 1) / 2))
      p = p + 2
    elseif t == 2 or t == 3 then
      if #inbuf - p + 1 < 3 then break end
      local n = inbuf:byte(p + 1) + inbuf:byte(p + 2) * 256
      local c = r32(B('credit_in'))
      if t == 2 then
        c = c + n
        counts.credit = counts.credit + n
      else
        c = math.max(c - n, r32(B('credit_used')))
        counts.taken_back = counts.taken_back + n
        say('in: credit of %d bytes taken back', n)
      end
      w32(B('credit_in'), c)
      p = p + 3
    else
      say('bad record type %d: stream dropped', t)
      p = #inbuf + 1
    end
  end
  inbuf = inbuf:sub(p)
end

local force_check = false
local moved = nil          -- (wall time of the last thing carried for or from this side: abort after 1.5 s of waiting,
local gone = false         -- then 0.1 s while nothing comes from the other side: the game retries many times)
local wait_seen = nil
local waits, wait_time = 0, 0
local function service()
  if not installed then return end
  counts.services = counts.services + 1
  local before = r32(B('tx_tail')) + r32(B('rx_head')) + r32(B('ln_head')) + r32(B('credit_in'))
  -- tx: the game's records -> the other side
  local head, tail = r32(B('tx_head')), r32(B('tx_tail'))
  if head ~= tail then
    local parts = {}
    local base = off(B('tx')) - r32(B('tx_base'))
    while tail ~= head do
      table.insert(parts, string.char(mem[base + tail]))
      tail = tail + 1
    end
    local s = table.concat(parts)
    -- log line changes going out
    local q = 1
    while q <= #s do        -- (records may be split between calls only by a full ring: rare, logging only)
      local t = s:byte(q)
      if t == 1 and q + 1 <= #s then
        say('out: lines %d (DTR %d RTS %d)', s:byte(q + 1), s:byte(q + 1) % 2, math.floor(s:byte(q + 1) / 2))
        counts.lines_out = counts.lines_out + 1
        q = q + 2
      elseif t == 0 and q + 2 <= #s then
        local n = s:byte(q + 1) + s:byte(q + 2) * 256
        counts.sent = counts.sent + n
        q = q + 3 + n
      elseif (t == 2 or t == 3) and q + 2 <= #s then
        q = q + 3
      else
        break
      end
    end
    out:write(s)
    out:flush()
    w32(B('tx_tail'), tail)
    -- our own records: the first header of a pair gives us the credit for the second
    out_part = out_part .. s
    local q = 1
    while true do
      local t = out_part:byte(q)
      if t == nil then break end
      local len
      if t == 0 then
        if #out_part - q + 1 < 3 then break end
        local n = out_part:byte(q + 1) + out_part:byte(q + 2) * 256
        if #out_part - q + 1 < 3 + n then break end
        len = 3 + n
      elseif t == 2 then
        if #out_part - q + 1 < 3 then break end
        local want = out_part:byte(q + 1) + out_part:byte(q + 2) * 256
        if copy_hdr and want == 6 and wall() - copy_t < 0.2 then     -- the copy, asked for: given now
          table.insert(pend_data, 1, copy_hdr)
          pend_data_len = pend_data_len + 6
          copy_used = copy_hdr
        end
        copy_hdr = nil
        len = 3
      elseif t == 1 then len = 2 else len = 3 end
      if #out_part - q + 1 < len then break end
      q = q + len
    end
    out_part = out_part:sub(q)
  end
  -- rx: the other side's records (file checked at most every 0.5 ms of CPU time)
  local now = os.clock()
  if force_check or now - last_in_check >= 0.0005 then
    last_in_check = now
    if not inf then inf = io.open(VLINK_IN, 'rb') end
    if inf then
      local size = inf:seek('end')
      if size > in_pos then
        inf:seek('set', in_pos)
        local s = inf:read(size - in_pos)
        in_pos = in_pos + #s
        inbuf = inbuf .. s
        gone = false
        parse()
      end
    end
  end
  if pend_data_len > 0 and r32(B('rx_head')) == r32(B('rx_tail')) then    -- (only into an empty area)
    local parts, n = {}, 0
    while n < RX_SIZE and #pend_data > 0 do
      local s = pend_data[1]
      local k = math.min(RX_SIZE - n, #s)
      table.insert(parts, s:sub(1, k))
      n = n + k
      if k == #s then table.remove(pend_data, 1) else pend_data[1] = s:sub(k + 1) end
    end
    local s = table.concat(parts)
    local base = off(B('rx')) + RX_SIZE - n
    for i = 1, n do mem[base + i - 1] = s:byte(i) end
    pend_data_len = pend_data_len - n
    w32(B('rx_head'), r32(B('rx_head')) + n)
  end
  if #pend_lines > 0 then
    local lh, lt = r32(B('ln_head')), r32(B('ln_tail'))
    while #pend_lines > 0 and lh - lt < LN_SIZE do
      mem[off(B('ln_ring')) + lh % LN_SIZE] = table.remove(pend_lines, 1)
      lh = lh + 1
    end
    w32(B('ln_head'), lh)
  end
  local after = r32(B('tx_tail')) + r32(B('rx_head')) + r32(B('ln_head')) + r32(B('credit_in'))
  local now = wall()
  if r32(B('waiting')) ~= 0 then     -- (waiting: seen in the last 0.2 s; its time counted from when it began)
    if not wait_seen or now - wait_seen > 0.2 then moved = now end
    wait_seen = now
  end
  if after ~= before or not moved then
    moved = now
  elseif wait_seen and now - wait_seen <= 0.2 and now - moved > (gone and 0.1 or 1.5) then
    w32(B('abort'), r32(B('abort')) + 1)
    if not gone then say('abort: waited 1.5 s with nothing from the other side') end
    gone = true
    moved = now
  end
end

-- The game waits for the other side: hold the emulator (real time, up to WAIT_MAX) until what it waits for has come
-- (rx bytes for a read, credit for a write), instead of running on to its next frame first (Redux runs a frame in a
-- burst then sleeps: each wait would cost a frame)
local function ready(what)
  if what == 1 then return r32(B('rx_head')) ~= r32(B('rx_tail')) end
  return r32(B('credit_in')) ~= r32(B('credit_used'))
end
local function wait(what)
  service()
  if ready(what) then return end
  if hold_left <= 0 then return end
  local t = wall()
  local stop = t + math.min(WAIT_MAX, hold_left)
  force_check = true
  repeat
    service()
  until ready(what) or wall() >= stop
  force_check = false
  waits = waits + 1
  wait_time = wait_time + (wall() - t)
  hold_left = hold_left - (wall() - t)
end

local last_report = 0
local last_frame, last_wall = nil, nil
local function report()
  local now = os.clock()
  if installed and now - last_report >= 5 then
    last_report = now
    local s = B('stats')
    say('stats: reads %d writes %d, bytes in %d out %d, timeouts read %d write %d, resets %d, ctl %d; '
        .. 'lines mine %d peer %d; credit %d used %d; carried out %d in %d; magic %08x', tonumber(r32(s)),
        tonumber(r32(s + 4)), tonumber(r32(s + 8)), tonumber(r32(s + 12)), tonumber(r32(s + 16)),
        tonumber(r32(s + 28)), tonumber(r32(s + 20)), tonumber(r32(s + 24)), tonumber(r32(B('my_lines'))),
        tonumber(r32(B('peer_lines'))), tonumber(r32(B('credit_in'))), tonumber(r32(B('credit_used'))), counts.sent,
        counts.recv, tonumber(r32(B('magic'))))
    local f, w = tonumber(r32(0x801B4EB8)), wall()
    if last_frame then
      say('waits %d, %.0f ms held in all; game frames %.1f a second', waits, wait_time * 1000,
          (f - last_frame) / (w - last_wall))
    end
    last_frame, last_wall = f, w
  end
end

-- (the handles are kept: Redux removes a breakpoint when Lua garbage-collects its handle)
VLINK_BREAKPOINTS = VLINK_BREAKPOINTS or {}
VLINK_BREAKPOINTS.entry = PCSX.addBreakpoint(ENTRY, 'Exec', 4, 'vlink install', function()
  if not installed then install() end
  return true
end)
-- The serial library's entries that the driver doesn't replace: logged when the game calls them (the async routines
-- and the interrupt handler are where the PS1 DEV CART build keeps code: reaching them would crash)
local WATCH = { [0x80153C08] = 'library init', [0x80154C8C] = 'library deinit (DelDrv)',
                [0x80153D94] = 'async read start', [0x80154074] = 'async write start (gap A in the cart build)',
                [0x801548E4] = 'SIO interrupt handler (gap C in the cart build)', [0x80154D38] = 'driver read/write',
                [0x80154C84] = 'driver close' }
local watch_hits = {}
for adrs, what in pairs(WATCH) do
  VLINK_BREAKPOINTS = VLINK_BREAKPOINTS or {}
  VLINK_BREAKPOINTS['w' .. adrs] = PCSX.addBreakpoint(adrs, 'Exec', 4, what, function()
    watch_hits[adrs] = (watch_hits[adrs] or 0) + 1
    if watch_hits[adrs] <= 5 then
      local r = PCSX.getRegisters().GPR.n
      say('CALLED %s (%08x) from ra %08x, a0 %x a1 %x (hit %d)', what, adrs, tonumber(r.ra), tonumber(r.a0),
          tonumber(r.a1), watch_hits[adrs])
    end
    return true
  end)
end
-- The SIO interrupt check (0x80154868) returning non-zero = the handler would run
VLINK_BREAKPOINTS.verifier_ret = PCSX.addBreakpoint(0x801548DC, 'Exec', 4, 'irq verifier return', function()
  local v0 = tonumber(PCSX.getRegisters().GPR.n.v0)
  if v0 ~= 0 then
    watch_hits.ver = (watch_hits.ver or 0) + 1
    if watch_hits.ver <= 5 then say('SIO INTERRUPT: the library check returns %d (the handler runs)', v0) end
  end
  return true
end)

local errors = 0
VLINK_BREAKPOINTS.service = PCSX.addBreakpoint(SERVICE, 'Exec', 4, 'vlink service', function()
  local ok, err = pcall(function()
    local what = installed and r32(B('waiting')) or 0
    if what ~= 0 then wait(what) else service() end
    report()
  end)
  if not ok then
    errors = errors + 1
    if errors <= 5 then say('error: %s', tostring(err)) end
  end
  return true
end)
-- At each vertical blank: install once the game's library has been in RAM for 50 frames (the entry breakpoint
-- doesn't always fire), and carry the rings, so the stats keep coming while nothing is linked
-- Where the game is at each vertical blank (pc and ra, in 256-byte blocks), the 10 most common every 5 s
local where = {}
local function sample()
  local r = PCSX.getRegisters()
  local pc, ra = tonumber(r.pc), tonumber(r.GPR.n.ra)
  local k = string.format('%08x (ra %08x)', pc - pc % 256, ra - ra % 16)
  where[k] = (where[k] or 0) + 1
end
local last_where = 0
local function report_where()
  local now = os.clock()
  if now - last_where < 5 then return end
  last_where = now
  local list = {}
  for k, v in pairs(where) do table.insert(list, { k, v }) end
  table.sort(list, function(x, y) return x[2] > y[2] end)
  local parts = {}
  for i = 1, math.min(10, #list) do table.insert(parts, string.format('%s x%d', list[i][1], list[i][2])) end
  say('where: %s', table.concat(parts, ', '))
  where = {}
end
-- Commands without a restart: a file ctl.lua next to the log, run once each time it changes
local ctl_path = VLINK_LOG:gsub('vlink%.log$', 'ctl.lua')
local ctl_seen, ctl_check = nil, 0
local function check_ctl()
  local now = os.clock()
  if now - ctl_check < 1 then return end
  ctl_check = now
  local f = io.open(ctl_path, 'rb')
  if not f then return end
  local code = f:read('*a')
  f:close()
  if code == ctl_seen then return end
  ctl_seen = code
  local fn, err = loadstring(code)
  if not fn then say('ctl: %s', tostring(err)) return end
  local ok, res = pcall(fn, { say = say, r32 = r32, w32 = w32, mem = mem, B = B })
  say('ctl ran: %s %s', tostring(ok), tostring(res))
end

local vsyncs, present = 0, 0
local ok, listener = pcall(function()
  return PCSX.Events.createEventListener('GPU::Vsync', function()
    vsyncs = vsyncs + 1
    hold_left = HOLD_PER_FRAME
    if vsyncs % 500 == 1 then say('vsync %d', vsyncs) end
    if not installed then
      if r32(PATCHES[1][1]) == PATCHES[1][2] then present = present + 1 else present = 0 end
      if present >= 50 then install() end
    end
    service()
    report()
    pcall(sample)
    pcall(report_where)
    pcall(check_ctl)
  end)
end)
VLINK_LISTENER = ok and listener or nil
say('%s: waiting for the game (entry %08x); out %s, in %s; vsync listener %s', VLINK_NAME, ENTRY, VLINK_OUT, VLINK_IN,
    tostring(ok))
