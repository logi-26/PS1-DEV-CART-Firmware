#!/bin/sh
# Installs the Net Yaroze build environment in WSL (run by the PS1 DEV CART extension, with sudo):
# - the toolchain: GCC 2.8.1 and binutils 2.16.1 for mipsel-unknown-ecoff (yaroze-toolchain.tar.gz, GNU tools only,
#   built by Net-Yaroze-WSL-Build-Environment's toolchain/build.sh, see NOTICE.txt),
# - the SDK from the Net Yaroze Software Development Disc (PSX/INCLUDE, PSX/LIB, copied from the disc image by the
#   extension), set up as PSX3 (the Net Yaroze compiler of the Windows XP setups) has it: the same libps, headers,
#   linker script and driver specs, so that programs build the same,
# - yarexe (gwald's, built from its unmodified source).
#
# usage: sudo sh install.sh TOOLCHAIN.tar.gz SDK_FOLDER [ROOT]
#   SDK_FOLDER: the folder with PSX/INCLUDE and PSX/LIB; ROOT: installs into ROOT/opt/yaroze (tests), default /
set -e
TARBALL=${1:?toolchain tarball}
SDK=${2:?SDK folder (PSX/INCLUDE, PSX/LIB)}/PSX
ROOT=${3:-}
PREFIX=$ROOT/opt/yaroze
TARGET=mipsel-unknown-ecoff
TOOLDIR=$PREFIX/$TARGET
GCCLIB=$PREFIX/lib/gcc-lib/$TARGET/2.8.1

[ -f "$SDK/LIB/LIBPS.A" ] || { echo "no LIBPS.A in $SDK/LIB" >&2; exit 1; }

# yarexe is built here: a C compiler with the static C library, and git
need=""
command -v gcc >/dev/null 2>&1 || need="$need gcc"
[ -f /usr/lib/x86_64-linux-gnu/libc.a ] || [ -f /usr/lib/libc.a ] || need="$need libc6-dev"
command -v git >/dev/null 2>&1 || need="$need git"
if [ -n "$need" ]; then
	echo "Installing$need..."
	apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq $need
fi

echo "Toolchain: GCC 2.8.1, binutils 2.16.1 -> $PREFIX"
# (unpacked over an earlier install: other tools there, e.g. mkpsxiso or rsdlink, stay)
mkdir -p "${ROOT:-/}"
tar xzf "$TARBALL" -C "${ROOT:-/}"

echo "SDK: headers, libps, linker script, specs"
# Headers: made on DOS (upper case names, CRLF lines, which break backslash-continued macros on Linux), with PSX3's
# changes: C++ guards and no delete() in libps.h (a C++ keyword), no strlen() in strings.h (GCC's builtin)
mkdir -p "$TOOLDIR/include"
(cd "$SDK/INCLUDE" && find . -type f) | while read -r f; do
	dst="$TOOLDIR/include/$(echo "$f" | tr 'A-Z' 'a-z')"
	mkdir -p "$(dirname "$dst")"
	tr -d '\r' < "$SDK/INCLUDE/$f" > "$dst"
done
awk '
	/^\/\* Graphics Structures \*\/$/ && !open { print "#ifdef __cplusplus"; print "extern \"C\"{"; print "#endif"; print ""; open = 1 }
	/^#endif \/\* _LIBPS_H_ \*\/$/ { print "#ifdef __cplusplus"; print "}\t  //extern \"C\""; print "#endif"; print "" }
	/^extern int delete\(char \*\);$/ { next }
	{ print }' "$TOOLDIR/include/libps.h" > "$TOOLDIR/include/libps.h.new"
mv "$TOOLDIR/include/libps.h.new" "$TOOLDIR/include/libps.h"
sed -i 's|^extern int   strlen (|//extern int   strlen (|' "$TOOLDIR/include/strings.h"
# C++: <new> and <new.h> next to the SDK's headers, GCC's own include folder set aside (stddef.h, stdarg.h... are the
# SDK's)
cp "$GCCLIB/include/new" "$GCCLIB/include/new.h" "$TOOLDIR/include/"
rm -rf "$GCCLIB/include.gcc"
mv "$GCCLIB/include" "$GCCLIB/include.gcc"

# libps, with ioctl() (a BIOS B0 call PSX3's libps has, used for the tty of link play)
mkdir -p "$TOOLDIR/lib"
cp "$SDK/LIB/LIBPS.A" "$TOOLDIR/lib/libps.a"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/ioctl.s" <<'EOF'
	.text
	.globl	ioctl
	.set	noreorder
ioctl:
	li	$10, 0xb0
	jr	$10
	li	$9, 0x37
	nop
EOF
"$PREFIX/bin/$TARGET-as" -o "$tmp/ioctl.o" "$tmp/ioctl.s"
(cd "$tmp" && "$PREFIX/bin/$TARGET-ar" rs "$TOOLDIR/lib/libps.a" ioctl.o)
# libsngcc: the compiler's support library (64-bit integers, float conversions...): GCC 2.8.1's libgcc
cp "$GCCLIB/libgcc.a" "$TOOLDIR/lib/libsngcc.a"

# The SDK's linker script (entry _start, from libps)
tr -d '\r' < "$SDK/LIB/LDSCRIPT/MIPSPSX.X" |
	sed "s|SEARCH_DIR(.*)|SEARCH_DIR($TOOLDIR/lib);|" > "$TOOLDIR/lib/yaroze.x"

# Driver specs: PSX3's (specs next to this script, made by its GCC 2.8.1: -lps instead of libc, -lsngcc instead of
# libgcc, no crt0, little endian), with the SDK's linker script
sed "s|@YAROZE_X@|$TOOLDIR/lib/yaroze.x|" "$(dirname "$0")/specs" > "$GCCLIB/specs"

echo "yarexe"
REPO=https://github.com/gwald/Yarexe
COMMIT=cc79e98
git clone -q "$REPO" "$tmp/Yarexe"
git -C "$tmp/Yarexe" checkout -q "$COMMIT"
# (yarexe.c uses errno without <errno.h>, and calls the static casepath() before defining it; its licence forbids
# distributing modified source, so it is built unmodified with this forced-include header)
cat > "$tmp/prelude.h" <<'EOF'
#include <errno.h>
static int casepath(char const *path, char *r);
EOF
gcc -static -O3 -w -include "$tmp/prelude.h" "$tmp/Yarexe/yarexe.c" -o "$tmp/yarexe"
mkdir -p "$PREFIX/libexec"
install -m 755 "$tmp/yarexe" "$PREFIX/libexec/yarexe"
# The Linux build doesn't translate the script's '\' paths: the wrapper hands it a copy with '/' separators and no CRs
# (paths stay relative to the current folder, as with siocons)
cat > "$PREFIX/bin/yarexe" <<EOF
#!/bin/sh
# yarexe wrapper: yarexe <siocons script> [-v]
[ -f "\$1" ] || exec "$PREFIX/libexec/yarexe" "\$@"
script=\$(mktemp ./.yarexe-XXXXXX)
trap 'rm -f "\$script"' EXIT
tr -d '\\015' < "\$1" | tr '\\134' '/' > "\$script"
shift
"$PREFIX/libexec/yarexe" "\$script" "\$@"
EOF
chmod 755 "$PREFIX/bin/yarexe"

echo "Net Yaroze build environment installed in $PREFIX"
