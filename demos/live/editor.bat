@echo off
rem PS1 LIVE editor (live_editor.py): start the game from it (PS1 DEV CART or PCSX-Redux), then edit the level while
rem it runs. Needs Python 3 (with Tkinter, part of the usual Windows installer).
cd /d "%~dp0"
start "" pythonw live_editor.py
