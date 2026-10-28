// PS1 DEV CART extension: the Net Yaroze SDK files read from the Net Yaroze Software Development Disc (the redump
// image: a .bin of raw 2352 byte sectors, or an .iso of 2048 byte sectors), without mounting it: a small ISO 9660
// reader. The files a build needs (PSX/INCLUDE, PSX/LIB) are copied to a folder, then installed in WSL by
// yaroze/install.sh.

const fs = require('fs');
const path = require('path');

// The folders of the disc copied (the headers, libps, its linker script and specs)
const SDK_DIRS = ['PSX/INCLUDE', 'PSX/LIB'];

// Reads the 2048 data bytes of the sectors of a disc image (raw sectors: mode 1 data after 16 bytes, mode 2 form 1
// after 24)
class DiscImage {
    constructor(file) {
        this.fd = fs.openSync(file, 'r');
        const size = fs.fstatSync(this.fd).size;
        this.raw = size % 2352 === 0 && size % 2048 !== 0 ? true : this.hasSync();
        this.sectorSize = this.raw ? 2352 : 2048;
        if (this.raw) {
            const header = Buffer.alloc(16);
            fs.readSync(this.fd, header, 0, 16, 16 * 2352);
            this.dataOffset = header[15] === 2 ? 24 : 16;
        } else {
            this.dataOffset = 0;
        }
    }

    // Raw sectors start with the sync pattern 00 FF*10 00
    hasSync() {
        const b = Buffer.alloc(12);
        fs.readSync(this.fd, b, 0, 12, 0);
        return b[0] === 0 && b[11] === 0 && b.subarray(1, 11).every(x => x === 0xFF);
    }

    read(lba, length) {
        const out = Buffer.alloc(length);
        for (let done = 0, s = lba; done < length; s++) {
            const n = Math.min(2048, length - done);
            fs.readSync(this.fd, out, done, n, s * this.sectorSize + this.dataOffset);
            done += n;
        }
        return out;
    }

    close() {
        fs.closeSync(this.fd);
    }
}

// The entries of a directory: [{ name, lba, size, dir }]
function readDirectory(disc, lba, size) {
    const data = disc.read(lba, size);
    const entries = [];
    for (let sector = 0; sector < size; sector += 2048) {
        for (let i = sector; i < Math.min(sector + 2048, size);) {
            const len = data[i];
            if (!len) {
                break;      // (the rest of the sector is unused)
            }
            const nameLen = data[i + 32];
            const raw = data.toString('latin1', i + 33, i + 33 + nameLen);
            if (!(nameLen === 1 && (raw === '\0' || raw === '\x01'))) {
                entries.push({ name: raw.replace(/;\d+$/, '').replace(/\.$/, ''), lba: data.readUInt32LE(i + 2),
                               size: data.readUInt32LE(i + 10), dir: !!(data[i + 25] & 2) });
            }
            i += len;
        }
    }
    return entries;
}

// Copies a folder of the disc (path like 'PSX/LIB', any case) and its subfolders into dest; returns the files copied
function copyFolder(disc, root, discPath, dest) {
    let dir = root;
    for (const part of discPath.split('/')) {
        dir = readDirectory(disc, dir.lba, dir.size).find(e => e.dir && e.name.toUpperCase() === part.toUpperCase());
        if (!dir) {
            throw new Error(`no ${discPath} on the disc: is it the Net Yaroze Software Development Disc?`);
        }
    }
    const files = [];
    const walk = (d, out) => {
        fs.mkdirSync(out, { recursive: true });
        for (const e of readDirectory(disc, d.lba, d.size)) {
            const target = path.join(out, e.name);
            if (e.dir) {
                walk(e, target);
            } else {
                fs.writeFileSync(target, disc.read(e.lba, e.size));
                files.push(target);
            }
        }
    };
    walk(dir, dest);
    return files;
}

// Copies the SDK folders of the disc image into dest (dest/PSX/INCLUDE, dest/PSX/LIB); returns the number of files
function extractSdk(image, dest) {
    const disc = new DiscImage(image);
    try {
        const pvd = disc.read(16, 2048);
        if (pvd[0] !== 1 || pvd.toString('latin1', 1, 6) !== 'CD001') {
            throw new Error(`${path.basename(image)} is not a CD image (ISO 9660)`);
        }
        const root = { lba: pvd.readUInt32LE(156 + 2), size: pvd.readUInt32LE(156 + 10) };
        fs.rmSync(dest, { recursive: true, force: true });
        let count = 0;
        for (const d of SDK_DIRS) {
            count += copyFolder(disc, root, d, path.join(dest, ...d.split('/'))).length;
        }
        return count;
    } finally {
        disc.close();
    }
}

module.exports = { extractSdk };
