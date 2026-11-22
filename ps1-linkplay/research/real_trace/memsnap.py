"""Read RAM regions from a console's dev cart while the game runs (ps1transfer -memlive), in pieces."""
import subprocess, time
EXE = r"..\..\..\ps1transfer\ps1transfer.exe"


class Live:
    def __init__(self, name):
        self.p = subprocess.Popen([EXE, "-n", name, "-memlive"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1)

    def read(self, addr, n, piece=1024):
        out = bytearray()
        for a in range(addr, addr + n, piece):
            k = min(piece, addr + n - a)
            self.p.stdin.write("read %08X %d\n" % (a, k)); self.p.stdin.flush()
            line = self.p.stdout.readline()
            while line and not line.startswith("MEMREAD"):
                line = self.p.stdout.readline()
            if not line:
                raise IOError("ps1transfer ended")
            out += bytes.fromhex(line.split()[2])
        return bytes(out)

    def close(self):
        try:
            self.p.stdin.write("quit\n"); self.p.stdin.flush()
        except OSError:
            pass
        self.p.terminate()
