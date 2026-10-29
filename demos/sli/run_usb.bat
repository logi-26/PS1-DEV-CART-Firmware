@echo off
rem PS1 SLI without a link cable: the two consoles joined through their cartridges and this computer (the USB link
rem relay, which stays open: close the window to stop). About 25 fps this way, 50 with the link cable.
rem   run_usb.bat                  the first two cartridges (ps1transfer -list)
rem   run_usb.bat LEFT RIGHT       by their serial numbers
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
	echo Two PS1 DEV CART cartridges are needed, connected to this computer: run_usb.bat LEFT RIGHT
	exit /b 1
)
%T% -n %LEFT% -link usb -f sli.ps-exe || exit /b 1
%T% -n %RIGHT% -link usb -f sli.ps-exe || exit /b 1
rem The sides: 0x534C4900 (left) and 0x534C4901 (right) at 0x801BF000
%T% -n %LEFT% -poke 0x801BF000 1397508352 -s 4 > nul
%T% -n %RIGHT% -poke 0x801BF000 1397508353 -s 4 > nul
echo %LEFT% draws the left half, %RIGHT% the right half, joined through this computer (close to stop).
%T% -n %LEFT% -relay %RIGHT%
