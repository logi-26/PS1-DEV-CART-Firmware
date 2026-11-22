#!/bin/sh
# nav.sh NAME "lua taps" : runs the taps, waits, screenshots NAME (in .redux_a), converts it
D=.redux_a
cat > $D/ctl.lua <<LUA
local t0 = vs
$2
VSYNC_HOOK = function() if vs == t0 + ${3:-90} then shot('$1'); VSYNC_HOOK = nil end end
return '$1'
LUA
for i in $(seq 1 40); do sleep 0.5; [ -f $D/$1.txt ] && [ $(find $D/$1.txt -newer $D/ctl.lua | wc -l) = 1 ] && break; done
sleep 0.5; python png.py $D $1
