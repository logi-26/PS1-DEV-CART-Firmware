"""Checks menu_lock.py's patch on the game's own code (research, 2026-11-16: Redux crashed loading the savestates):
the menus' input routine 0x8001A934 run by a small MIPS interpreter on the main menu's RAM (.menu/menu.ram),
patched or not, with down / up pressed: the cursor's items in turn. Calls out of the routine (sound, mouse) return 0.
Also the main menu's LINK GAME id after its _sio_control call and the "negative choice" fix.

  python menu_lock_sim.py
"""
import os
import struct
import subprocess
import sys
import tempfile

from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN
from capstone.mips import MIPS_OP_REG, MIPS_OP_IMM, MIPS_OP_MEM

HERE = os.path.dirname(os.path.abspath(__file__))
MENU = 0x801A9A94
PAD_PRESSED = 0x80024912
MOUSE = 0x8002754C


class Cpu:
    def __init__(self, ram):
        self.m = bytearray(ram)
        self.r = [0] * 32
        self.hi = self.lo = 0
        self.md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
        self.md.detail = True
        self.cache = {}

    def rd(self, a, n):
        return int.from_bytes(self.m[a & 0x1FFFFF:(a & 0x1FFFFF) + n], "little")

    def wr(self, a, n, v):
        self.m[a & 0x1FFFFF:(a & 0x1FFFFF) + n] = (v & ((1 << (8 * n)) - 1)).to_bytes(n, "little")

    def reg(self, op):
        return self.md.reg_name(op.reg)

    def get(self, name):
        return self.r[REGS.index(name)]

    def set(self, name, v):
        if name != "zero":
            self.r[REGS.index(name)] = v & 0xFFFFFFFF

    def ins(self, pc):
        if pc not in self.cache:
            self.cache[pc] = next(self.md.disasm(bytes(self.m[pc & 0x1FFFFF:(pc & 0x1FFFFF) + 4]), pc))
        return self.cache[pc]

    def call(self, pc, a0, stub=lambda target: 0, limit=200000):
        """Runs from pc until it returns (ra = a sentinel); jal elsewhere: stub(target) in v0."""
        self.set("a0", a0)
        self.set("sp", 0x801FF000)
        self.set("ra", 0xDEAD0000)
        delay = None
        for _ in range(limit):
            if pc == 0xDEAD0000:
                return s32(self.get("v0"))
            i = self.ins(pc)
            nxt = delay if delay is not None else pc + 4
            delay = None
            target = self.step(i, pc, stub)
            if target is not None:          # (a branch taken: its delay slot first)
                self.step(self.ins(pc + 4), pc + 4, stub, in_delay=True)
                pc = target
                continue
            pc = nxt
        raise RuntimeError("no return")

    def step(self, i, pc, stub, in_delay=False):
        m, ops = i.mnemonic, i.operands
        R = lambda k: self.get(self.reg(ops[k])) if ops[k].type == MIPS_OP_REG else ops[k].imm
        if m == "nop":
            return None
        if m in ("addiu", "addi"):
            self.set(self.reg(ops[0]), R(1) + ops[2].imm)
        elif m in ("addu", "add"):
            self.set(self.reg(ops[0]), R(1) + R(2))
        elif m in ("subu", "sub"):
            self.set(self.reg(ops[0]), R(1) - R(2))
        elif m == "move":
            self.set(self.reg(ops[0]), R(1))
        elif m == "lui":
            self.set(self.reg(ops[0]), ops[1].imm << 16)
        elif m == "andi":
            self.set(self.reg(ops[0]), R(1) & (ops[2].imm & 0xFFFF))
        elif m == "ori":
            self.set(self.reg(ops[0]), R(1) | (ops[2].imm & 0xFFFF))
        elif m == "and":
            self.set(self.reg(ops[0]), R(1) & R(2))
        elif m == "or":
            self.set(self.reg(ops[0]), R(1) | R(2))
        elif m == "sll":
            self.set(self.reg(ops[0]), R(1) << ops[2].imm)
        elif m == "srl":
            self.set(self.reg(ops[0]), R(1) >> ops[2].imm)
        elif m == "sra":
            self.set(self.reg(ops[0]), s32(R(1)) >> ops[2].imm)
        elif m == "slt":
            self.set(self.reg(ops[0]), int(s32(R(1)) < s32(R(2))))
        elif m == "slti":
            self.set(self.reg(ops[0]), int(s32(R(1)) < ops[2].imm))
        elif m == "sltiu":
            self.set(self.reg(ops[0]), int(R(1) < (ops[2].imm & 0xFFFFFFFF)))
        elif m == "sltu":
            self.set(self.reg(ops[0]), int(R(1) < R(2)))
        elif m == "mult":
            p = s32(R(0)) * s32(R(1))
            self.lo, self.hi = p & 0xFFFFFFFF, (p >> 32) & 0xFFFFFFFF
        elif m == "mflo":
            self.set(self.reg(ops[0]), self.lo)
        elif m == "mfhi":
            self.set(self.reg(ops[0]), self.hi)
        elif m in ("lw", "lh", "lhu", "lb", "lbu", "sw", "sh", "sb"):
            mem = ops[1].mem
            a = (self.get(self.md.reg_name(mem.base)) + mem.disp) & 0xFFFFFFFF
            n = {"w": 4, "h": 2, "b": 1}[m[1]]
            if m[0] == "l":
                v = self.rd(a, n)
                if m in ("lh", "lb") and v >> (8 * n - 1):
                    v -= 1 << (8 * n)
                self.set(self.reg(ops[0]), v)
            else:
                self.wr(a, n, self.get(self.reg(ops[0])))
        elif m in ("beq", "bne", "beqz", "bnez", "bgez", "bltz", "blez", "bgtz", "b"):
            target = ops[-1].imm
            if m == "beq":
                take = R(0) == R(1)
            elif m == "bne":
                take = R(0) != R(1)
            elif m == "beqz":
                take = R(0) == 0
            elif m == "bnez":
                take = R(0) != 0
            elif m == "bgez":
                take = s32(R(0)) >= 0
            elif m == "bltz":
                take = s32(R(0)) < 0
            elif m == "blez":
                take = s32(R(0)) <= 0
            elif m == "bgtz":
                take = s32(R(0)) > 0
            else:
                take = True
            return target if take else None
        elif m == "j":
            return ops[0].imm
        elif m == "jal":
            self.step(self.ins(pc + 4), pc + 4, stub, in_delay=True)
            self.set("v0", stub(ops[0].imm))
            return pc + 8
        elif m == "jalr":
            self.step(self.ins(pc + 4), pc + 4, stub, in_delay=True)
            self.set("v0", stub(R(0)) if True else 0)
            return pc + 8
        elif m == "jr":
            return R(0)
        else:
            raise NotImplementedError("%x: %s %s" % (pc, m, i.op_str))
        return None


REGS = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "s0", "s1",
        "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v >> 31 else v


def cursor_walk(ram, item4_id, button, presses=7, dsr=None):
    """The cursor's item after each press of button (pressed word), LINK GAME's id set as the main menu sets it."""
    cpu = Cpu(ram)
    cpu.wr(0x801A9AE4 + 4 * 0x34, 4, item4_id)
    cpu.wr(MOUSE, 4, 0)
    out = []
    for _ in range(presses):
        cpu.wr(PAD_PRESSED, 2, button)
        cpu.wr(MENU + 0x18, 4, 0x1000)          # (the menu shown: input taken)
        res = cpu.call(0x8001A934, MENU, stub=lambda t: 0 if t != 0x8001F6A4 else 0)
        out.append(s32(cpu.rd(MENU + 0x24, 4)))
    cpu.wr(PAD_PRESSED, 2, 0x40)                # X: the choice
    chosen = cpu.call(0x8001A934, MENU)
    return out, chosen


def main():
    ram = open(os.path.join(HERE, ".menu", "menu.ram"), "rb").read()
    patched_path = os.path.join(tempfile.gettempdir(), "menu_lock_patched.ram")
    subprocess.run([sys.executable, os.path.join(HERE, "menu_lock.py"), "--ram", patched_path], check=True,
                   capture_output=True)
    patched = open(patched_path, "rb").read()
    os.remove(patched_path)
    print("Unpatched, LINK GAME shown (id 4):")
    print("  down:", cursor_walk(ram, 4, 0x4000), " up:", cursor_walk(ram, 4, 0x1000))
    print("Patched, LINK GAME shown and locked (id -3):")
    down, up = cursor_walk(patched, -3, 0x4000), cursor_walk(patched, -3, 0x1000)
    print("  down:", down, " up:", up)
    assert 4 not in down[0] and 4 not in up[0], "the cursor reached item 4"
    # Every cursor position: X never gives 4 or a negative
    for start in range(4):
        cpu = Cpu(patched)
        cpu.wr(0x801A9AE4 + 4 * 0x34, 4, -3 & 0xFFFFFFFF)
        cpu.wr(MENU + 0x24, 4, start)
        cpu.wr(MENU + 0x20, 4, start)
        cpu.wr(PAD_PRESSED, 2, 0x40)
        cpu.wr(MENU + 0x18, 4, 0x1000)
        print("  X on item %d -> %d" % (start, cpu.call(0x8001A934, MENU)))
    # The main menu's own code: the item's id after the _sio_control call, both DSR values (the call's v0)
    for dsr in (0, 0x80):
        cpu = Cpu(patched)
        cpu.set("s3", 0x801A9BB4)
        cpu.set("s1", 4)
        cpu.set("v0", dsr)
        pc = 0x80013024
        while pc < 0x80013034:
            t = cpu.step(cpu.ins(pc), pc, lambda t: 0)
            pc = t if t is not None else pc + 4
        print("  main menu, _sio_control bit 80h %s: LINK GAME id %d" % ("set" if dsr else "clear",
                                                                       s32(cpu.rd(0x801A9BB4, 4))))
    # A negative choice (mouse click on the item: -3) made "none" (-1) by the main menu
    for choice in (-3, -2, 4, 2):
        cpu = Cpu(patched)
        cpu.set("s4", choice)
        cpu.set("v0", -2)
        pc = 0x80013054
        t = cpu.step(cpu.ins(pc), pc, lambda t: 0)
        if t is None:
            cpu.step(cpu.ins(pc + 4), pc + 4, lambda t: 0)
            cpu.step(cpu.ins(pc + 8), pc + 8, lambda t: 0)       # (the "s4 = -1")
        else:
            cpu.step(cpu.ins(pc + 4), pc + 4, lambda t: 0)
        print("  choice %d -> s4 %d" % (choice, s32(cpu.get("s4"))))


if __name__ == "__main__":
    main()
