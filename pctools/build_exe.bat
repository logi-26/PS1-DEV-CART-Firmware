@echo off
rem PicoStation Manager: builds dist\picostation_manager\ (picostation_manager.exe and manager_files\), no Python needed
rem to run it (needs: python -m pip install pyinstaller pillow ttkbootstrap). Then checks the built program's files.
rem The program to run (or copy, with its manager_files\ folder): dist\picostation_manager\picostation_manager.exe
rem (build\ is PyInstaller's work folder, deleted after the build: the .exe in it doesn't run).
cd /d "%~dp0"
python -m PyInstaller --noconfirm picostation_manager.spec || exit /b 1
rmdir /s /q build
dist\picostation_manager\picostation_manager.exe --selftest dist\selftest.txt
set RESULT=%ERRORLEVEL%
type dist\selftest.txt
echo The program: %~dp0dist\picostation_manager\picostation_manager.exe
exit /b %RESULT%
