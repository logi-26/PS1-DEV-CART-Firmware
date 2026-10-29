@echo off
rem PS1 SLI: the demo on both consoles, joined by the link cable (run this on the computer both cartridges are connected
rem to). The first cartridge draws the left half, the second the right one (Select on either controller swaps them).
rem   run.bat                      the first two cartridges (ps1transfer -list)
rem   run.bat LEFT RIGHT           by their serial numbers, e.g. run.bat A10PVL53 A10PUCVT
setlocal
cd /d "%~dp0"
set T=..\..\ps1transfer\ps1transfer.exe
set LEFT=%1
set RIGHT=%2
if "%RIGHT%"=="" for /f "tokens=2,4" %%a in ('%T% -list ^| findstr /b /c:"-z "') do (
	if "%%a"=="0" set LEFT=%%b
	if "%%a"=="1" set RIGHT=%%b
)
if "%RIGHT%"=="" (
	echo Two PS1 DEV CART cartridges are needed, connected to this computer: run.bat LEFT RIGHT
	exit /b 1
)
%T% -n %LEFT% -link -f sli.ps-exe || exit /b 1
%T% -n %RIGHT% -link -f sli.ps-exe || exit /b 1
rem The sides: 0x534C4900 (left) and 0x534C4901 (right) at 0x801BF000
%T% -n %LEFT% -poke 0x801BF000 1397508352 -s 4 > nul
%T% -n %RIGHT% -poke 0x801BF000 1397508353 -s 4 > nul
echo %LEFT% draws the left half, %RIGHT% the right half. The consoles now talk over the link cable alone.
