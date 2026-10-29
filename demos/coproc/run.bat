@echo off
rem PS1 CO-PROCESSOR: the co-processor server in its own window, then coproc.ps-exe sent to the cartridge and the
rem bridge started (run this on the computer the cartridge is connected to).
rem   run.bat                  first cartridge
rem   run.bat -n SERIAL        on the cartridge with that serial number (ps1transfer -list)
rem Close both windows (or Ctrl+C) to stop.
cd /d "%~dp0"
start "PS1 CO-PROCESSOR server" python coproc_server.py
timeout /t 2 /nobreak > nul
..\..\ps1transfer\ps1transfer.exe %* -r -e -f coproc.ps-exe -net 127.0.0.1:7030
