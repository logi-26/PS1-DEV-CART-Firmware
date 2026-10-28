"""
PS1 DEV CART GUI: symbol files read for the Memory page (the variables and functions of a program, their address and
size): ELF executable (sizes and kinds from its symbol table), Net Yaroze ECOFF executable, GNU ld map file and nm
output ("nm -S" gives the sizes). As ps1transfer's -sym (LoadSymbols in ps1transfer/main.c), with the sizes and kinds
too. A size the file doesn't give is estimated from the next symbol (shown with a ~).

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file,
You can obtain one at https://mozilla.org/MPL/2.0/.
"""
import re
import struct

ECOFF_MAGIC = 0x0162
MAX_ESTIMATED = 4096        # An estimated size is not more (the gap to the next symbol may be padding or code)


class Symbol:
    __slots__ = ("name", "adrs", "size", "kind", "estimated")

    def __init__(self, name, adrs, size=0, kind="?"):
        self.name = name
        self.adrs = adrs
        self.size = size            # Bytes, 0: unknown
        self.kind = kind            # "data", "func" or "?"
        self.estimated = False      # Size estimated from the next symbol

    def size_text(self):
        return ("~%d" if self.estimated else "%d") % self.size if self.size else ""


def load(path):
    """The symbols of a file (sorted by address), raises ValueError if it isn't a symbol file."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] == b"\x7fELF":
        syms = _elf(data)
    elif len(data) > 2 and struct.unpack_from("<H", data)[0] == ECOFF_MAGIC:
        syms = _ecoff(data)
    else:
        syms = _text(data.decode("latin-1"))
    if not syms:
        raise ValueError("no symbols found (an ELF, a Net Yaroze executable, a .map or nm output is needed)")
    # One per name and address (not the compiler's internal labels: $L15...), sorted; the sizes not given estimated
    # from the next symbol
    unique = {}
    for s in syms:
        if s.name.startswith("$"):
            continue
        old = unique.get((s.name, s.adrs))
        if old is None or (not old.size and s.size):
            unique[(s.name, s.adrs)] = s
    syms = sorted(unique.values(), key=lambda s: (s.adrs, s.name))
    for i, s in enumerate(syms):
        if not s.size:
            following = [t.adrs for t in syms[i + 1:i + 8] if t.adrs > s.adrs]
            if following and following[0] - s.adrs <= MAX_ESTIMATED:
                s.size = following[0] - s.adrs
                s.estimated = True
    return syms


def _elf(d):
    if d[4] != 1 or d[5] != 1:
        raise ValueError("not a 32-bit little endian ELF file")
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
    sections = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    syms = []
    for sh in sections:
        if sh[1] != 2 or sh[6] >= shnum:            # SHT_SYMTAB, its string table
            continue
        strtab = sections[sh[6]][4]
        for off in range(sh[4], sh[4] + sh[5] - 15, 16):
            name, value, size, info, _, shndx = struct.unpack_from("<IIIBBH", d, off)
            typ = info & 15
            if not shndx or typ > 2 or not name:     # Undefined, or a section/file symbol
                continue
            end = d.index(b"\0", strtab + name)
            text = d[strtab + name:end].decode("latin-1")
            if not text or text.startswith((".", "$")):
                continue
            if typ == 2:
                kind = "func"
            elif typ == 1:
                kind = "data"
            elif shndx < shnum and sections[shndx][2] & 4:     # Label in code (e.g. in assembly)
                kind = "func"
            elif shndx < shnum and not text.startswith("_"):    # Label in data (e.g. a converted PsyQ library's)
                kind = "data"
            else:
                kind = "?"          # Other labels (e.g. the linker's __end, _gp): not shown as variables
            syms.append(Symbol(text, value, size, kind))
    return syms


def _ecoff(d):
    symptr, = struct.unpack_from("<I", d, 8)
    if not symptr or symptr + 96 > len(d) or struct.unpack_from("<H", d, symptr)[0] != 0x7009:
        raise ValueError("no symbol table in this Net Yaroze executable (stripped?)")
    h = struct.unpack_from("<24I", d, symptr)
    isym, sym_off, ss_off, ss_ext_off = h[8], h[9], h[15], h[17]
    ifd, fd_off, iext, ext_off = h[18], h[19], h[22], h[23]

    def name_at(off):
        end = d.find(b"\0", off)
        return d[off:end].decode("latin-1") if 0 <= off < len(d) and end > off else ""

    def kind_of(bits, external):
        st, sc = bits & 0x3F, (bits >> 6) & 0x1F
        if sc not in (1, 2, 3, 5, 13, 14, 15):      # text, data, bss, absolute, sdata, sbss, rdata
            return None
        if (external and st not in (1, 6)) or (not external and st not in (2, 14)):
            return None
        return "func" if sc == 1 or st in (6, 14) else ("?" if sc == 5 else "data")

    syms = []
    for i in range(iext):           # EXTR: flags, file, SYMR: name, value, st/sc/index
        _, _, name, value, bits = struct.unpack_from("<HHIII", d, ext_off + i * 16)
        kind = kind_of(bits, True)
        if kind:
            syms.append(Symbol(name_at(ss_ext_off + name), value, 0, kind))
    for i in range(ifd):            # FDR: names (issBase at 8), symbols (isymBase at 16, csym at 20)
        iss_base, = struct.unpack_from("<I", d, fd_off + i * 72 + 8)
        isym_base, csym = struct.unpack_from("<II", d, fd_off + i * 72 + 16)
        for j in range(csym):
            if isym_base + j >= isym:
                break
            name, value, bits = struct.unpack_from("<III", d, sym_off + (isym_base + j) * 12)
            kind = kind_of(bits, False)
            if kind:
                syms.append(Symbol(name_at(ss_off + iss_base + name), value, 0, kind))
    return [s for s in syms if s.name]


MAP_SECTION = re.compile(r"^\s*\.(\w+)")                                         # " .text   0x80010000 ..."
MAP_SYMBOL = re.compile(r"^\s+0x([0-9a-fA-F]{8,16})\s+([A-Za-z_][\w.$:<>~]*)\s*$")  # "   0x80012345   player"
NM_LINE = re.compile(r"^([0-9a-fA-F]{8})\s+(?:([0-9a-fA-F]{8})\s+)?([A-Za-z])\s+(.+)$")


def _text(text):
    syms = []
    section = ""
    for line in text.splitlines():
        m = NM_LINE.match(line)
        if m:               # nm ("80012345 T main") or nm -S ("80012345 00000004 B lives")
            letter = m.group(3)
            if letter in "Uuw":
                continue
            kind = "func" if letter in "Tt" else ("data" if letter in "BbDdGgSsRrCc" else "?")
            syms.append(Symbol(m.group(4).strip(), int(m.group(1), 16), int(m.group(2), 16) if m.group(2) else 0,
                               kind))
            continue
        m = MAP_SYMBOL.match(line)
        if m:               # GNU ld map: the symbol's section is the last one named before it
            name = m.group(2)
            if "=" in line or name.startswith(("_", ".")) and name.endswith(("_start", "_end", "_size")):
                continue
            syms.append(Symbol(name, int(m.group(1), 16) & 0xFFFFFFFF, 0,
                               "func" if section.startswith("text") else ("data" if section else "?")))
            continue
        m = MAP_SECTION.match(line)
        if m:
            section = m.group(1)
    return syms
