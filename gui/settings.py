"""
PS1 DEV CART GUI settings: saved as JSON in the user's configuration folder
(%APPDATA%\\ps1devcart-gui on Windows, ~/.config/ps1devcart-gui on Linux).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import json
import os
import sys

APP_NAME = "ps1devcart-gui"
MAX_RECENT = 10

DEFAULTS = {
    "ps1transfer": "",          # Path of ps1transfer (empty: next to this GUI, see devices.default_ps1transfer)
    "theme": "bootstrap-dark",
    "device": "",               # USB serial number of the last cartridge used
    "geometry": "",
    "fields": {},               # Last value of each field, by key
    "recent": {},               # Recent files of each file field, by key
}


def config_dir():
    if sys.platform == "win32":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, APP_NAME)


OLD_APP_NAME = "ps1usb-gui"     # Settings folder of the earlier versions, copied once


class Settings:
    def __init__(self):
        self.path = os.path.join(config_dir(), "settings.json")
        old = os.path.join(os.path.dirname(config_dir()), OLD_APP_NAME, "settings.json")
        if not os.path.exists(self.path) and os.path.exists(old):
            try:
                os.makedirs(config_dir(), exist_ok=True)
                with open(old, encoding="utf-8") as src, open(self.path, "w", encoding="utf-8") as dst:
                    dst.write(src.read())
            except OSError:
                pass
        self.data = json.loads(json.dumps(DEFAULTS))
        try:
            with open(self.path, encoding="utf-8") as f:
                self.data.update(json.load(f))
        except (OSError, ValueError):
            pass

    def get(self, key, default=None):
        return self.data.get(key, default)

    def set(self, key, value):
        self.data[key] = value

    def field(self, key, default=""):
        return self.data["fields"].get(key, default)

    def set_field(self, key, value):
        self.data["fields"][key] = value

    def recent(self, key):
        return list(self.data["recent"].get(key, []))

    def add_recent(self, key, value):
        if not value:
            return
        items = [v for v in self.data["recent"].get(key, []) if v != value]
        self.data["recent"][key] = ([value] + items)[:MAX_RECENT]

    def save(self):
        try:
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            with open(self.path, "w", encoding="utf-8") as f:
                json.dump(self.data, f, indent=2)
        except OSError:
            pass
