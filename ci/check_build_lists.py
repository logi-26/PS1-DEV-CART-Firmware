"""
PS1 DEV CART: every .c file of ps1transfer/ is in its Makefiles and Code::Blocks projects (a file added to one build
and forgotten in the others breaks that build only, where nobody may notice). serialport.c is Windows only.

  python ci/check_build_lists.py

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import glob
import os
import re
import sys

DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "ps1transfer")
WINDOWS_ONLY = {"serialport"}


def main():
    sources = sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(DIR, "*.c")))
    missing = []
    for name, linux in (("Makefile_win", False), ("Makefile_linux", True), ("ps1transfer_win.cbp", False),
                        ("ps1transfer_linux.cbp", True)):
        with open(os.path.join(DIR, name), encoding="utf-8") as f:
            text = f.read()
        for s in sources:
            if linux and s in WINDOWS_ONLY:
                continue
            pattern = r'<Unit filename="%s\.c"' % s if name.endswith(".cbp") else r"[\\/]%s\.o\b" % s
            if not re.search(pattern, text):
                missing.append((name, s + ".c"))
    for name, src in missing:
        print("::error::ps1transfer/%s doesn't build %s" % (name, src))
    print("%d sources, %s" % (len(sources), "all listed" if not missing else "%d missing" % len(missing)))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
