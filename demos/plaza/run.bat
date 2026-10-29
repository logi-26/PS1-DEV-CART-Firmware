@echo off
rem PS1 PLAZA: plays on the PS1 PLAZA server through the cartridge.
rem   run.bat                  first cartridge
rem   run.bat -n SERIAL        on the cartridge with that serial number (ps1transfer -list)
rem Close the window (or Ctrl+C) to leave.
cd /d "%~dp0"
..\..\ps1transfer\ps1transfer.exe %* -r -e -f plaza.ps-exe -net play.logi26.co.uk:7020
