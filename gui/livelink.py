"""
PS1 DEV CART GUI: the live link with the running program, shared by the pages (see App.live_start in
ps1devcart_gui.py): ps1transfer -memlive (watch list, remote controller), or the frame capture's session (-frame),
which also takes the remote pad commands.

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
LIVE_LINK = "Live link"
FRAME_CAPTURE = "Frame capture"
LIVE_PROTOCOL = ("MEMLIVE", "MEMREAD", "MEMWROTE", "MEMDUMP", "MEMERR", "MEMPAD", "FRM")   # Its answers (not in the log)
