#!/usr/bin/env bash
# Patches the recovery sepolicy with rules.te using magiskpolicy on a rooted
# watch (the magiskpolicy shipped in the Magisk APK only runs on Android).
#
# usage: sepolicy/patch-sepolicy.sh <adb serial> <stock sepolicy> <output>
# The stock sepolicy comes out of build.sh --extract-sepolicy.
set -euo pipefail
serial=$1 in=$2 out=$3
here=$(cd "$(dirname "$0")" && pwd)

args=()
while IFS= read -r line; do
  [[ -z "$line" || "$line" == \#* ]] && continue
  args+=("\"$line\"")
done < "$here/rules.te"

adb -s "$serial" push "$in" /data/local/tmp/sepolicy.in >/dev/null
adb -s "$serial" shell "su -c '/data/adb/magisk/magiskpolicy --load /data/local/tmp/sepolicy.in --save /data/local/tmp/sepolicy.out ${args[*]} && chmod 644 /data/local/tmp/sepolicy.out'"
adb -s "$serial" pull /data/local/tmp/sepolicy.out "$out" >/dev/null
adb -s "$serial" shell "su -c 'rm -f /data/local/tmp/sepolicy.in /data/local/tmp/sepolicy.out'"
echo "patched sepolicy: $out"
