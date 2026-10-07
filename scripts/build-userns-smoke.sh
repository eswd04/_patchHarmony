#!/bin/sh
# Static ARM64 Bionic needs the API 29+ CRT's ELF TLS alignment.
set -eu

SRCDIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NDK_DIR=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [ -z "$NDK_DIR" ]; then
	echo 'Set ANDROID_NDK_HOME to your Android NDK directory.' >&2
	exit 2
fi
TOOLCHAIN="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin"
OUTPUT=${1:-"$SRCDIR/out/op15/userns-smoke"}
mkdir -p "$(dirname -- "$OUTPUT")"
BUILD_TMP=$(mktemp "${OUTPUT}.XXXXXX")
trap 'rm -f "$BUILD_TMP"' 0

"$TOOLCHAIN/aarch64-linux-android29-clang" \
	-static -pthread -Wall -Wextra -Werror -O2 \
	"$SRCDIR/tests/userns_smoke.c" -o "$BUILD_TMP"

# Check the finished ELF, so a toolchain/configuration change cannot silently
# reintroduce a binary that aborts before main() on ARM64 Android.
python3 - "$BUILD_TMP" <<'PY'
import struct
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 18)[0] != 183:
    sys.exit("Expected a little-endian ARM64 ELF64 executable")
offset = struct.unpack_from("<Q", data, 32)[0]
entry_size, count = struct.unpack_from("<HH", data, 54)
tls = []
for index in range(count):
    header = struct.unpack_from("<IIQQQQQQ", data, offset + index * entry_size)
    if header[0] == 3:  # PT_INTERP
        sys.exit("Expected a statically linked executable")
    if header[0] == 7:  # PT_TLS
        tls.append(header)
if len(tls) != 1:
    sys.exit("Expected one Bionic TLS segment")
_, _, file_offset, address, _, _, _, alignment = tls[0]
if alignment < 64 or alignment & (alignment - 1) or address % alignment or file_offset % alignment:
    sys.exit(f"Invalid ARM64 Bionic TLS alignment: {alignment}")
print(f"Verified static ARM64 ELF: PT_TLS alignment={alignment}, skew=0")
PY

chmod 0755 "$BUILD_TMP"
mv -f -- "$BUILD_TMP" "$OUTPUT"
echo "-> $OUTPUT"
