@echo off
rem PicoStation Manager: builds dist\picostation_manager\ (picostation_manager.exe and manager_files\), no Python needed
rem to run it (needs: python -m pip install pyinstaller pillow ttkbootstrap). Then checks the built program's files.
cd /d "%~dp0"
python -m PyInstaller --noconfirm picostation_manager.spec || exit /b 1
dist\picostation_manager\picostation_manager.exe --selftest dist\selftest.txt
set RESULT=%ERRORLEVEL%
type dist\selftest.txt
exit /b %RESULT%
