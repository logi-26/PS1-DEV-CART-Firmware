-- Red Alert Retaliation's link game page (code loaded from DATA.MIX at 0x80012590): its handshake.
--   0x80018B54: CTS at the first check (non-zero: the other console's RTS already up)
--   0x80018B6C: CTS while waiting (logged when it changes)
--   0x80018D20: "linked" (s1: 0 = CTS was up at once, 1 = it rose while waiting); RTS dropped, message 1E5h / 1E6h
--   0x80018C84: left on a button
-- and every SIO1 register access (the first one from each place), so the rest of the page can be followed.
local logf = io.open('page_watch.log', 'w')
local t0 = os.clock()
local function say(fmt, ...)
  logf:write(string.format('%8.3f  ', os.clock() - t0) .. string.format(fmt, ...) .. '\n')
  logf:flush()
end
local last = nil
local function at(addr, what, every)
  PCSX.addBreakpoint(addr, 'Exec', 4, what, function()
    local r = PCSX.getRegisters().GPR.n
    if every then
      if r.v0 ~= last then last = r.v0; say('%s: v0=%x', what, r.v0) end
    else
      say('%s: v0=%x s1=%x s5=%x a0=%x', what, r.v0, r.s1, r.s5, r.a0)
    end
    return true
  end)
end
at(0x80012590, 'page code called')
at(0x80018B54, 'first CTS check')
at(0x80018B6C, 'CTS while waiting', true)
at(0x80018D20, 'linked')
at(0x80018C84, 'left on a button')
-- (no breakpoints on the SIO1 registers: the game reads them all the time, the emulator stopped responding)
say('watching')
