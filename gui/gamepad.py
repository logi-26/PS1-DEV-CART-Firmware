"""
PS1 DEV CART GUI: game controllers plugged into this computer (e.g. a PS1 controller on a USB adapter), read for the
Remote Controller page. No extra package: the Windows joystick API (winmm) on Windows, /dev/input/js* on Linux.

A controller's state: the buttons held (numbers), the axes (-1.0 to 1.0) and the hat (D-pad) direction. Inputs are
named by strings, so a mapping can be saved in the settings: "b3" (button 3), "a1+" / "a1-" (axis 1 pushed to its
positive or negative side; for a stick: the axis, "-" when it goes the other way), "h:up" (the hat).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import glob
import os
import struct
import sys

PRESS = 0.5         # An axis used as a button: pressed past half way
DETECT = 0.6        # Learning a mapping: an axis must move this far from where it rests

# PS1 button -> input, for adapters numbering their buttons like most PS1/PS2 USB adapters (the D-pad on a hat)
DEFAULT_BUTTONS = {"Triangle": "b0", "Circle": "b1", "Cross": "b2", "Square": "b3", "L2": "b4", "R2": "b5",
                   "L1": "b6", "R1": "b7", "Select": "b8", "Start": "b9", "L3": "b10", "R3": "b11",
                   "Up": "h:up", "Right": "h:right", "Down": "h:down", "Left": "h:left"}
DEFAULT_STICKS = {"LX": "a0", "LY": "a1", "RX": "a2", "RY": "a3"}
# Known adapters (USB vendor:product): their own layout
KNOWN = {
    "6666:0667": {"Select": "b8", "Start": "b11", "L3": "b9", "R3": "b10",     # PSX to PC converter (D-pad: buttons)
                  "Up": "b12", "Right": "b13", "Down": "b14", "Left": "b15"},
}


def default_mapping(key):
    buttons = dict(DEFAULT_BUTTONS)
    buttons.update(KNOWN.get(key, {}))
    return {"buttons": buttons, "sticks": dict(DEFAULT_STICKS)}


class State:
    def __init__(self, buttons=(), axes=(), hat=None):
        self.buttons = set(buttons)     # Numbers of the buttons held
        self.axes = list(axes)          # -1.0 to 1.0
        self.hat = hat                  # (x, y), each -1, 0 or 1 (y: -1 up), or None without a hat

    def active(self, spec):
        """The input is pressed (a button, an axis past half way, the hat in that direction)."""
        if not spec:
            return False
        if spec[0] == "b":
            return int(spec[1:]) in self.buttons
        if spec[0] == "a":
            i = int(spec[1:-1])
            v = self.axes[i] if i < len(self.axes) else 0.0
            return v > PRESS if spec[-1] == "+" else v < -PRESS
        if spec.startswith("h:") and self.hat:
            x, y = self.hat
            return {"up": y < 0, "down": y > 0, "left": x < 0, "right": x > 0}.get(spec[2:], False)
        return False

    def axis(self, spec):
        """A stick axis (0 to 255, 128 centred): "a2", or "a2-" turned the other way."""
        if not spec or spec[0] != "a":
            return 128
        i = int(spec[1:].rstrip("+-"))
        v = self.axes[i] if i < len(self.axes) else 0.0
        if spec.endswith("-"):
            v = -v
        return max(0, min(255, int(round((v + 1) * 127.5))))

    def changes(self, rest):
        """Inputs pressed now that weren't at rest (learning a mapping): buttons, axes, hat directions."""
        found = ["b%d" % b for b in sorted(self.buttons - rest.buttons)]
        for i, v in enumerate(self.axes):
            r = rest.axes[i] if i < len(rest.axes) else 0.0
            if v - r > DETECT:
                found.append("a%d+" % i)
            elif r - v > DETECT:
                found.append("a%d-" % i)
        if self.hat and self.hat != (rest.hat or (0, 0)):
            x, y = self.hat
            found += ["h:" + d for d, on in (("up", y < 0), ("down", y > 0), ("left", x < 0), ("right", x > 0)) if on]
        return found

    def at_rest(self, rest):
        return not self.changes(rest) and not (rest.buttons - self.buttons)


def describe(spec):
    """Text of an input: "button 3", "axis 1 +", "D-pad up"."""
    if not spec:
        return "-"
    if spec[0] == "b":
        return "button %d" % (int(spec[1:]) + 1)
    if spec[0] == "a":
        return "axis %s" % spec[1:]
    return "hat " + spec[2:]


# ---------------------------------------------------------------- Windows (winmm)

if sys.platform == "win32":
    import ctypes
    import ctypes.wintypes as wt

    class _JOYCAPSW(ctypes.Structure):
        _fields_ = [("wMid", wt.WORD), ("wPid", wt.WORD), ("szPname", wt.WCHAR * 32), ("wXmin", wt.UINT),
                    ("wXmax", wt.UINT), ("wYmin", wt.UINT), ("wYmax", wt.UINT), ("wZmin", wt.UINT), ("wZmax", wt.UINT),
                    ("wNumButtons", wt.UINT), ("wPeriodMin", wt.UINT), ("wPeriodMax", wt.UINT), ("wRmin", wt.UINT),
                    ("wRmax", wt.UINT), ("wUmin", wt.UINT), ("wUmax", wt.UINT), ("wVmin", wt.UINT), ("wVmax", wt.UINT),
                    ("wCaps", wt.UINT), ("wMaxAxes", wt.UINT), ("wNumAxes", wt.UINT), ("wMaxButtons", wt.UINT),
                    ("szRegKey", wt.WCHAR * 32), ("szOEMVxD", wt.WCHAR * 260)]

    class _JOYINFOEX(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("dwFlags", wt.DWORD), ("dwXpos", wt.DWORD), ("dwYpos", wt.DWORD),
                    ("dwZpos", wt.DWORD), ("dwRpos", wt.DWORD), ("dwUpos", wt.DWORD), ("dwVpos", wt.DWORD),
                    ("dwButtons", wt.DWORD), ("dwButtonNumber", wt.DWORD), ("dwPOV", wt.DWORD),
                    ("dwReserved1", wt.DWORD), ("dwReserved2", wt.DWORD)]

    JOY_RETURNALL = 0xFF
    JOYCAPS_HASZ, JOYCAPS_HASR, JOYCAPS_HASU, JOYCAPS_HASV, JOYCAPS_HASPOV = 0x01, 0x02, 0x04, 0x08, 0x10

    try:
        _winmm = ctypes.WinDLL("winmm")
    except OSError:
        _winmm = None

    def _oem_name(vid, pid):
        """The name Windows shows for the controller (Game Controllers), from the registry."""
        import winreg
        key = (r"System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM"
               r"\VID_%04X&PID_%04X" % (vid, pid))
        for root in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
            try:
                with winreg.OpenKey(root, key) as k:
                    return winreg.QueryValueEx(k, "OEMName")[0]
            except OSError:
                pass
        return ""

    class Gamepad:
        def __init__(self, index, caps):
            self.index = index
            self.key = "%04x:%04x" % (caps.wMid, caps.wPid)
            self.name = "%s (%s)" % (_oem_name(caps.wMid, caps.wPid) or caps.szPname, self.key)
            # Axes in winmm's order (X, Y, then Z, R, U, V when present): a0, a1, ...
            ranges = [("dwXpos", caps.wXmin, caps.wXmax), ("dwYpos", caps.wYmin, caps.wYmax)]
            for flag, field, lo, hi in ((JOYCAPS_HASZ, "dwZpos", caps.wZmin, caps.wZmax),
                                        (JOYCAPS_HASR, "dwRpos", caps.wRmin, caps.wRmax),
                                        (JOYCAPS_HASU, "dwUpos", caps.wUmin, caps.wUmax),
                                        (JOYCAPS_HASV, "dwVpos", caps.wVmin, caps.wVmax)):
                if caps.wCaps & flag:
                    ranges.append((field, lo, hi))
            self.ranges = ranges
            self.has_hat = bool(caps.wCaps & JOYCAPS_HASPOV)
            self.info = _JOYINFOEX()
            self.info.dwSize = ctypes.sizeof(self.info)
            self.info.dwFlags = JOY_RETURNALL

        def read(self):
            """The state now, or None if the controller is unplugged."""
            if _winmm.joyGetPosEx(self.index, ctypes.byref(self.info)):
                return None
            info = self.info
            axes = [((getattr(info, f) - lo) / max(1, hi - lo)) * 2 - 1 for f, lo, hi in self.ranges]
            buttons = [i for i in range(32) if info.dwButtons & (1 << i)]
            hat = None
            if self.has_hat:
                pov = info.dwPOV
                hat = (0, 0) if pov > 36000 else (
                    (1 if 2250 <= pov <= 15750 else -1 if 20250 <= pov <= 33750 else 0),
                    (-1 if pov <= 6750 or pov >= 29250 else 1 if 11250 <= pov <= 24750 else 0))
            return State(buttons, axes, hat)

        def close(self):
            pass

    def find():
        """The controllers plugged in."""
        pads = []
        if not _winmm:
            return pads
        for i in range(_winmm.joyGetNumDevs()):
            caps = _JOYCAPSW()
            if _winmm.joyGetDevCapsW(i, ctypes.byref(caps), ctypes.sizeof(caps)):
                continue
            pad = Gamepad(i, caps)
            if pad.read() is not None:      # (an empty slot of the driver answers the caps but not the state)
                pads.append(pad)
        return pads

# ---------------------------------------------------------------- Linux (joystick API)

else:
    import fcntl

    JSIOCGNAME = 0x80806A13        # _IOC(_IOC_READ, 'j', 0x13, 128)
    JS_EVENT = struct.Struct("IhBB")

    class Gamepad:
        def __init__(self, path):
            self.path = path
            self.fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            name = bytearray(128)
            try:
                fcntl.ioctl(self.fd, JSIOCGNAME, name)
                name = name.split(b"\0", 1)[0].decode(errors="replace")
            except OSError:
                name = ""
            self.key = name or os.path.basename(path)
            self.name = "%s (%s)" % (name or "Controller", os.path.basename(path))
            self.has_hat = False        # (the hat is two axes on Linux)
            self.buttons = set()
            self.axes = []

        def read(self):
            try:
                while True:
                    data = os.read(self.fd, JS_EVENT.size * 64)
                    if not data:
                        return None
                    for off in range(0, len(data) - JS_EVENT.size + 1, JS_EVENT.size):
                        _, value, kind, number = JS_EVENT.unpack_from(data, off)
                        kind &= 0x7F
                        if kind == 1:
                            if value:
                                self.buttons.add(number)
                            else:
                                self.buttons.discard(number)
                        elif kind == 2:
                            while len(self.axes) <= number:
                                self.axes.append(0.0)
                            self.axes[number] = value / 32767.0
            except BlockingIOError:
                pass
            except OSError:
                return None
            return State(self.buttons, self.axes)

        def close(self):
            try:
                os.close(self.fd)
            except OSError:
                pass

    def find():
        pads = []
        for path in sorted(glob.glob("/dev/input/js*")):
            try:
                pads.append(Gamepad(path))
            except OSError:
                pass
        return pads
