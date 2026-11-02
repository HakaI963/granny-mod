#!/usr/bin/env bash
# One-shot launch test. Run this once the Altaris shell service is reconnected
# (open the Altaris app and re-enable its shell/engine toggle).
#
# It installs and launches each bisect variant in order and prints which one
# first fails to stay running, so the cause is isolated to a single change.
set -uo pipefail
for v in A B C D; do
  [ -f "/tmp/v-$v.apk" ] || { echo "missing /tmp/v-$v.apk (run ./variant.sh)"; continue; }
  cp "/tmp/v-$v.apk" "/sdcard/Download/Altaris/v-$v.apk"
done
echo "APKs staged in /sdcard/Download/Altaris/. Now bisect with the altaris tools:"
echo "  altaris_install_apk path=/sdcard/Download/Altaris/v-A.apk   -> launch -> altaris_clones (running?)"
echo "  repeat for B (rename), C (dex hook), D (loader module)"
