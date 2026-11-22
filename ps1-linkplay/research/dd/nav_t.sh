# nav_t.sh BUTTON WAIT SHOTNAME: press BUTTON on emulator pad 1 ("2:BUTTON" = pad 2, "none" = no press), wait,
# screenshot to .redux_t/SHOTNAME.png
D=.redux_t
case "$1" in
none) ;;
2:*) b=${1#2:}; echo "P2=PCSX.SIO0.slots[2].pads[1]; local b=PCSX.CONSTS.PAD.BUTTON['$b']; P2.setOverride(b); local t=vs+6; VSYNC_HOOK=function() if vs>=t then P2.clearOverride(b); VSYNC_HOOK=nil end end -- $RANDOM" > $D/ctl.lua; sleep 1;;
*) echo "hold('$1', 6) -- $RANDOM" > $D/ctl.lua; sleep 1;;
esac
sleep ${2:-2}
echo "shot('$3') -- $RANDOM" > $D/ctl.lua; sleep 1.2; python png.py $D $3
