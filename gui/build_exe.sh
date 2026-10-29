#!/bin/sh
# PS1 DEV CART GUI: builds dist/ps1devcart_gui/ (ps1devcart_gui and manager_files/), no Python needed (needs: pip install pyinstaller)
# Copy the folder's contents next to ps1transfer (it looks for ps1transfer next to itself, or in ../../../ps1transfer).
cd "$(dirname "$0")"
python3 -m PyInstaller --noconfirm ps1devcart_gui.spec
