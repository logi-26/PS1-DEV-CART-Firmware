-- Determinism run (dofile'd by ctl.lua with T_START, T_ONE, T_GO, TAG set): from boot, START at T_START, one
-- console (X) at T_ONE, GO! (X) at T_GO; then the same driving script for both runs, by the game's pad reads
-- counted from GO!; hashes every 50 reads, RAM dumps at reads 1000 and 1800 after GO!.
local drive = {{0, 'X'}, {4, nil}}
for k = 0, 17 do
  local base = 300 + k * 100
  drive[#drive + 1] = {base, {'X'}}
  drive[#drive + 1] = {base + 30, {'X', (k % 2 == 0) and 'LEFT' or 'RIGHT'}}
  drive[#drive + 1] = {base + 60, {'X'}}
end
drive[#drive + 1] = {2100, nil}
local go_base, dumped = nil, {}
VSYNC_HOOK = function()
  if vs == T_START then tap(1, 'START'); say('%s START', TAG) end
  if vs == T_ONE then tap(1, 'X'); say('%s one console', TAG) end
  if vs == T_GO then
    script(drive); go_base = reads; hashes(50, 44, TAG .. '_hashes'); say('%s GO at read %d', TAG, reads)
  end
  if go_base then
    for _, r in ipairs({1000, 1800}) do
      if not dumped[r] and reads - go_base >= r then dumped[r] = true; dump(TAG .. '_' .. r); shot(TAG .. '_' .. r) end
    end
  end
end
return TAG
