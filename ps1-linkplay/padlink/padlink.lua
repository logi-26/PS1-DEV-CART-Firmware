-- The CTR pad play driver (padlink.c, padlink_ctr.json) in PCSX-Redux: this script does what the computer does for a
-- console (ps1transfer + the Manager): installs the driver at the main menu, fills its exchange area, sets the game
-- up (ctr_setup.lua's setup{}), and carries the button frames: at each pl_service call (the console's USB service)
-- the frames this player published go out to hub.py ("race player frame buttons", players 1-based), the others'
-- come in and are written into the exchange area's windows.
-- dofile'd after dd_auto.lua (research/dd) and ctr_setup.lua, with: TAG, ME (1..N), N, SETUP, SEED, DELAY (blanks
-- before installing), INPUT_DELAY, SOURCE ('script': scripted buttons through pc.test_buttons; 'pad': the emulator's
-- pad 1, through the game's libpad as on a console), PL_JSON (padlink_ctr.json), HASH_FRAMES.
-- Logs in DD_DIR: dd.log, ls_hash.txt ("race frame hash" from the driver), ls_frames.txt.
local ffi = require('ffi')
pcall(ffi.cdef, 'void Sleep(uint32_t ms);')
local function sleep_ms(n) pcall(function() ffi.C.Sleep(n) end) end
local mem = ffi.cast('uint8_t*', PCSX.getMemPtr())
local m16 = ffi.cast('uint16_t*', PCSX.getMemPtr())
local m32 = ffi.cast('uint32_t*', PCSX.getMemPtr())
local function a(x) return bit.band(x, 0x1FFFFF) end
local function rd8(x) return mem[a(x)] end
local function rd16(x) return m16[bit.rshift(a(x), 1)] end
local function rd32(x) return m32[bit.rshift(a(x), 2)] end
local function wr8(x, v) mem[a(x)] = v end
local function wr16(x, v) m16[bit.rshift(a(x), 1)] = v end
local function wr32(x, v) m32[bit.rshift(a(x), 2)] = v end

local J = dofile(PL_LUA)        -- padlink_ctr.lua (build.py)
local BUF = J.buf
local O = { magic = 0, players = 4, me = 5, delay = 6, flags = 7, seed = 8, epoch = 0xC, test = 0xE, have = 0x10,
            buttons = 0x18, abort = 0x98, epoch_now = 0xA0, frame = 0xA2, out_upto = 0xA4, state = 0xA6,
            out = 0xA8, hash = 0xC8, waits = 0xE8, wait_loops = 0xEC, loads = 0xF0 }
local function B(k) return BUF + O[k] end
local D = INPUT_DELAY or 2
local d = DELAY or 0

local BTN_ = {SELECT = 0, START = 3, UP = 4, RIGHT = 5, DOWN = 6, LEFT = 7, L2 = 8, R2 = 9, L1 = 10, R1 = 11,
              TR = 12, O = 13, X = 14, SQ = 15}
local function m(...) local v = 0; for _, n in ipairs({...}) do v = bit.bor(v, bit.lshift(1, BTN_[n])) end; return v end
local function scripted(who, fr)
  if fr < 90 then return 0 end
  local v = m('X')
  local k = math.floor(fr / (35 + 10 * who))
  local c = k % (3 + who)
  if c == 1 then v = bit.bor(v, m('LEFT')) elseif c == 3 then v = bit.bor(v, m('RIGHT')) end
  if fr % (50 + 10 * who) == 20 * who then v = bit.bor(v, m('O')) end
  if who == 1 and fr % 150 >= 140 then v = bit.bor(v, m('R1')) end
  return v
end

local out_f = io.open(DD_DIR .. '/ls_out.txt', 'w')
local in_f = nil
local remote = {}            -- remote[e][p][f] = buttons
local hash_f = io.open(DD_DIR .. '/ls_hash.txt', 'w')
local frames_f = io.open(DD_DIR .. '/ls_frames.txt', 'w')
local sent_upto, sent_epoch = -1, 0
local logged_hash = -1
local installed = false

local function install()
  local g = J.gap
  -- the gap must still be the game's own code (its first word)
  if rd32(g.adrs) ~= g.first then
    say('%s NOT INSTALLED: gap %08x holds %08x, not %08x', TAG, g.adrs, rd32(g.adrs), g.first); return false
  end
  for _, w in ipairs(J.words) do
    if rd32(w.adrs) ~= w.orig and rd32(w.adrs) ~= w.new then
      say('%s NOT INSTALLED: %08x is %08x, not %08x', TAG, w.adrs, rd32(w.adrs), w.orig); return false
    end
  end
  for i = 0, #g.hex / 2 - 1 do wr8(g.adrs + i, tonumber(g.hex:sub(2 * i + 1, 2 * i + 2), 16)) end
  for i = 0, J.buf_size - 1 do wr8(BUF + i, 0) end
  wr8(B('players'), N); wr8(B('me'), ME - 1); wr8(B('delay'), D)
  wr32(B('seed'), SEED)
  for p = 0, 3 do wr16(B('have') + 2 * p, 0xFFFF) end
  wr8(B('flags'), (SOURCE == 'script') and 3 or 1)
  wr32(B('magic'), 0x4B4E4C50)
  for _, w in ipairs(J.words) do wr32(w.adrs, w.new) end
  PCSX.invalidateCache()
  say('%s padlink installed (%d bytes at %08x), players %d, me %d, delay %d', TAG, #g.hex / 2, g.adrs, N, ME, D)
  return true
end

local function read_in()
  if not in_f then in_f = io.open(DD_DIR .. '/ls_in.txt', 'r'); if not in_f then return false end end
  in_f:seek('cur', 0)
  local any = false
  while true do
    local line = in_f:read('*l')
    if not line then break end
    local e, p, fr, v = line:match('^(%d+) (%d+) (%d+) (%d+)$')
    if e then
      e, p, fr = tonumber(e), tonumber(p), tonumber(fr)
      remote[e] = remote[e] or {}
      remote[e][p] = remote[e][p] or {}
      remote[e][p][fr] = tonumber(v)
      any = true
    end
  end
  return any
end

-- The computer's side of one exchange
local function carry()
  local e = rd16(B('epoch_now'))
  if e == 0 then return end
  -- this player's frames out
  local upto = rd16(B('out_upto'))
  if e ~= sent_epoch then sent_epoch, sent_upto = e, -1 end
  while sent_upto < upto do
    sent_upto = sent_upto + 1
    out_f:write(string.format('%d %d %d %d\n', e, ME, sent_upto, rd16(B('out') + 2 * bit.band(sent_upto, 15))))
  end
  out_f:flush()
  -- the others' in: for each, the frames received in a row from 0, the last 16 of them in its window
  local any = read_in()
  local r = remote[e] or {}
  local now = rd16(B('frame'))
  if now == 0xFFFF then now = -1 end
  for p = 1, N do
    if p ~= ME then
      local t = r[p] or {}
      local have = rd16(B('have') + 2 * (p - 1))
      if rd16(B('epoch')) ~= e then have = -1 end
      if have == 0xFFFF then have = -1 end
      while t[have + 1] ~= nil and have + 1 <= now + 15 do
        have = have + 1
        wr16(B('buttons') + 2 * (16 * (p - 1) + bit.band(have, 15)), t[have])
      end
      wr16(B('have') + 2 * (p - 1), (have >= 0) and have or 0xFFFF)
    end
  end
  wr16(B('epoch'), e)
  -- the driver's state hashes (for frames now-7 .. now-1)
  local key = e * 65536
  for fr = math.max(logged_hash + 1 - key, now - 7), now - 1 do
    if fr >= 0 and fr <= (HASH_FRAMES or 900) then
      hash_f:write(string.format('%d %d %08x\n', e, fr, rd32(B('hash') + 4 * bit.band(fr, 7))))
      logged_hash = key + fr
    end
  end
  hash_f:flush()
  return any
end

LS_BPS = LS_BPS or {}
for _, b in ipairs(LS_BPS) do b:remove() end
LS_BPS = {}
local last_frame, last_state, last_log = -1, -1, 0
LS_BPS[#LS_BPS + 1] = PCSX.addBreakpoint(J.service, 'Exec', 4, 'pl_service', function()
  local got = carry()
  if rd16(B('state')) == 3 and not got then sleep_ms(1) end     -- waiting: hold the emulator as a console waits
  local fr = rd16(B('frame'))
  if fr ~= last_frame and fr % 300 == 0 and fr > 0 then
    say('%s race %d frame %d at vsync %d: waited in %d frames, %d loops', TAG, rd16(B('epoch_now')), fr, vs,
        rd32(B('waits')), rd32(B('wait_loops')))
  end
  if fr ~= last_frame and (fr == 300 or fr == 600 or fr == 900) then
    shot(string.format('%s_pl_r%d_%d', TAG, rd16(B('epoch_now')), fr))
  end
  last_frame = fr
  return true
end)
-- The scripted player's buttons for the frame pl_frame is about to publish (frame + delay)
LS_BPS[#LS_BPS + 1] = PCSX.addBreakpoint(J.frame, 'Exec', 4, 'pl_frame', function()
  if SOURCE == 'script' then
    local racing = rd16(BUF + 0x11C) ~= 0
    local nxt = racing and (ffi.cast('int32_t*', PCSX.getMemPtr())[bit.rshift(a(BUF + 0x120), 2)] + 1) or 0
    local v = scripted(ME, nxt + D)
    for _, x in ipairs(EXTRA or {}) do
      local e = rd16(B('epoch_now')) + (racing and 0 or 1)
      if x[1] == e and x[3] == ME and nxt + D >= x[2] and nxt + D < x[2] + 4 then v = bit.bor(v, m(x[4])) end
    end
    wr16(B('test'), v)
  end
  local st = rd16(B('state'))
  if st ~= last_state then
    if st == 1 then say('%s loading (loads %d) at vsync %d', TAG, rd32(B('loads')), vs)
    elseif st == 2 and last_state == 1 then say('%s race %d starts at vsync %d', TAG, rd16(B('epoch_now')), vs) end
    last_state = st
  end
  return true
end)

VSYNC_HOOK = function()
  if not installed and vs >= 3400 + d and at_main_menu() then
    if install() then
      installed = true
      say('%s %s', TAG, setup(SETUP))
    end
  end
end
return TAG .. ' padlink'
