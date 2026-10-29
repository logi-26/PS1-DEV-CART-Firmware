@echo off
rem PS1 DEV CART GUI: builds dist\ps1devcart_gui\ (ps1devcart_gui.exe and manager_files\), no Python needed (needs: pip install pyinstaller)
rem Copy the folder's contents next to ps1transfer.exe (it looks for ps1transfer next to itself, or in ..\..\..\ps1transfer).
cd /d "%~dp0"
python -m PyInstaller --noconfirm ps1devcart_gui.spec
