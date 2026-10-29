@echo off
rem PS1 SLI 3 SCREENS: two, three or more screens through the hub (run this on the computer the cartridges are
rem connected to), see run.ps1.
rem   run.bat                            every cartridge connected
rem   run.bat A10PVL53 A10PUCVT redux    these cartridges and PCSX-Redux, from left to right
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run.ps1" %*
