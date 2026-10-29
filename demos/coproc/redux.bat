@echo off
rem PS1 CO-PROCESSOR in the PCSX-Redux emulator (no PS1 DEV CART needed): see redux.ps1.
rem   redux.bat                                    PCSX-Redux found by itself
rem   redux.bat -Redux C:\path\to\pcsx-redux.exe   that PCSX-Redux
rem Close PCSX-Redux to leave.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0redux.ps1" %*
