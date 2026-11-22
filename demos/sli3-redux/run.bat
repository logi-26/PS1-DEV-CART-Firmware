@echo off
rem PS1 SLI 3 SCREENS on three PCSX-Redux windows, no console (see sli_redux3.py).
rem   run.bat                      three windows side by side on the main monitor (640 x 480 each)
rem   run.bat fullscreen           one screen per monitor, full screen (left to right as the monitors are arranged)
rem   run.bat --layout monitors    one screen per monitor, in a window
rem   run.bat --width 600          smaller windows in the row
cd /d "%~dp0"
if /i "%~1"=="fullscreen" (
    shift
    python sli_redux3.py --layout fullscreen %2 %3 %4 %5 %6 %7 %8 %9
) else (
    python sli_redux3.py %*
)
