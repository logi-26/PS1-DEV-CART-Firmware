@echo off
rem PS1 DEV CART Radio: the radio server in its own window, then radio.ps-exe sent to the cartridge with the bridge.
rem   run.bat                      the UK stations of radio_server.py, first cartridge
rem   run.bat -n SERIAL            on the cartridge with that serial number (ps1transfer -list)
rem Close both windows (or Ctrl+C) to stop.
cd /d "%~dp0"
start "PS1 DEV CART Radio server" python radio_server.py
timeout /t 2 /nobreak > nul
..\..\ps1transfer\ps1transfer.exe %* -r -e -f radio.ps-exe -net 127.0.0.1:7777
