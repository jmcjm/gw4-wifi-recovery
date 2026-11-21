#!/usr/bin/env bash
# Builds the Wi-Fi ADB recovery image from a stock SM-R870 recovery image.
#
# usage:
#   ./build.sh --extract-sepolicy <stock recovery.img> <output sepolicy>
#   ./build.sh <stock recovery.img> <patched sepolicy> <output image>
#
# Environment:
#   MAGISKBOOT   x86_64 magiskboot (default: tools/magiskboot, see --tools)
#   NDK          Android NDK root, for building gw4ui
#   AP_PASSWORD_FILE  file with the WPA2 password of the fallback access point
#                     (default: ap-password.txt; created with a random
#                     16-character password when missing)
#
#   ./build.sh --tools <Magisk APK>  extracts the x86_64 magiskboot into tools/
#
# Only the ramdisk changes. The kernel, dtb and recovery_dtbo are copied
# byte for byte from the stock image (magiskboot unpack/repack -n).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
MAGISKBOOT=${MAGISKBOOT:-$here/tools/magiskboot}
AP_PASSWORD_FILE=${AP_PASSWORD_FILE:-$here/ap-password.txt}

die() { echo "build.sh: $*" >&2; exit 1; }

if [[ ${1:-} == --tools ]]; then
  apk=${2:?Magisk APK path}
  mkdir -p "$here/tools"
  unzip -p "$apk" lib/x86_64/libmagiskboot.so > "$here/tools/magiskboot"
  chmod 755 "$here/tools/magiskboot"
  echo "tools/magiskboot extracted"
  exit 0
fi

[[ -x $MAGISKBOOT ]] || die "magiskboot not found at $MAGISKBOOT (run ./build.sh --tools <Magisk APK>)"
command -v lz4 >/dev/null || die "lz4 is required"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

unpack() {
  cp "$1" "$work/stock.img"
  (cd "$work" && "$MAGISKBOOT" unpack -n stock.img >/dev/null 2>&1) || die "unpack failed"
  grep -q . "$work/ramdisk.cpio" || die "no ramdisk in $1"
  lz4 -dqf "$work/ramdisk.cpio" "$work/ramdisk.raw"
}

if [[ ${1:-} == --extract-sepolicy ]]; then
  [[ $# -eq 3 ]] || die "usage: --extract-sepolicy <stock recovery.img> <output>"
  unpack "$2"
  (cd "$work" && "$MAGISKBOOT" cpio ramdisk.raw "extract sepolicy sepolicy" >/dev/null 2>&1)
  cp "$work/sepolicy" "$3"
  echo "stock sepolicy: $3"
  exit 0
fi

[[ $# -eq 3 ]] || die "usage: ./build.sh <stock recovery.img> <patched sepolicy> <output image>"
[[ -f $1 && -f $2 ]] || die "missing input file"
stock=$(realpath "$1") policy=$(realpath "$2") out=$(realpath -m "$3")

# gw4ui: static ARMv7 binary, so it does not depend on the ramdisk libraries.
: "${NDK:?set NDK to the Android NDK root}"
cc=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi30-clang
strip=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
"$cc" -O2 -Wall -Wextra -static -o "$work/gw4ui.full" "$here/ui/gw4ui.c"
"$strip" -o "$work/gw4ui" "$work/gw4ui.full"

# Access point config with the password filled in.
if [[ ! -f $AP_PASSWORD_FILE ]]; then
  (umask 077; tr -dc 'A-Za-z0-9' </dev/urandom | head -c 16 > "$AP_PASSWORD_FILE")
  echo "new access point password in $AP_PASSWORD_FILE"
fi
pass=$(cat "$AP_PASSWORD_FILE")
[[ ${#pass} -ge 8 ]] || die "access point password must have at least 8 characters"
sed "s/@AP_PASSWORD@/$pass/" "$here/overlay/vendor/etc/wifi/wifiadb-ap.conf.in" > "$work/wifiadb-ap.conf"

unpack "$stock"
cd "$work"
"$MAGISKBOOT" cpio ramdisk.raw "extract prop.default prop.default" >/dev/null 2>&1
# adbd keeps root and needs no key: access is guarded by the network.
sed -i -e 's/^ro.secure=1/ro.secure=0/' \
       -e 's/^ro.debuggable=0/ro.debuggable=1/' \
       -e 's/^ro.adb.secure=1/ro.adb.secure=0/' prop.default

"$MAGISKBOOT" cpio ramdisk.raw \
  "add 0644 system/etc/init/wifiadb.rc $here/overlay/system/etc/init/wifiadb.rc" \
  "add 0755 system/bin/wifiadb.sh $here/overlay/system/bin/wifiadb.sh" \
  "add 0755 system/bin/gw4ui gw4ui" \
  "add 0600 vendor/etc/wifi/wifiadb-ap.conf wifiadb-ap.conf" \
  "add 0644 prop.default prop.default" \
  "add 0644 sepolicy $policy" >/dev/null 2>&1

lz4 -l -12 -f -q ramdisk.raw ramdisk.cpio
"$MAGISKBOOT" repack -n stock.img new.img >/dev/null 2>&1 || die "repack failed"

# The kernel and device trees must come out unchanged.
mkdir check && cd check
"$MAGISKBOOT" unpack -n ../new.img >/dev/null 2>&1
for f in kernel dtb recovery_dtbo; do
  cmp -s "$f" "../$f" || die "$f differs from the stock image"
done
cd ..

cp new.img "$out"
sha256sum "$out"
