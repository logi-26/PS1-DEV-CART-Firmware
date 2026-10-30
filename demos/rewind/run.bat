@echo off
rem PS1 REWIND: the rewind server in its own window, then pegdrop.ps-exe sent to the cartridge and the bridge started
rem (run this on the computer the cartridge is connected to).
rem   run.bat                  first cartridge
rem   run.bat -n SERIAL        on the cartridge with that serial number (ps1transfer -list)
rem Close both windows (or Ctrl+C) to stop.
cd /d "%~dp0"
start "PS1 REWIND server" python rewind_server.py
timeout /t 2 /nobreak > nul
..\..\ps1transfer\ps1transfer.exe %* -r -e -f pegdrop.ps-exe -net 127.0.0.1:7040
