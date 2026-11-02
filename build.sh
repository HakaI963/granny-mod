#!/usr/bin/env bash
# Build the modded Granny APK.
#
# Inputs (all in the repo or produced by earlier CI steps):
#   base/granny-base.apk        the shipped base APK
#   base/granny-arm64.apk       the shipped arm64 config split
#   loader/build/libgrannymod.so  NDK-built loader
#
# Steps: baksmali classes3 -> patch Application -> smali -> inject into a copy
# of the base APK (renamed package, split merged, libs added) -> align -> sign.
set -euo pipefail

PKG="${PKG:-com.dvloper.granny.mod}"
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT

[ -f base/granny-base.apk ] || { echo "base/granny-base.apk required" >&2; exit 1; }
[ -f base/granny-arm64.apk ] || { echo "base/granny-arm64.apk required" >&2; exit 1; }

# --- 1. patch the dex that holds the Application class --------------------
python3 - "$W" <<'PY'
import sys, zipfile, os
w = sys.argv[1]
z = zipfile.ZipFile('base/granny-base.apk')
open(os.path.join(w, 'classes3.dex'), 'wb').write(z.read('classes3.dex'))
PY

baksmali d "$W/classes3.dex" -o "$W/smali"
cp patch/SafeDKApplication.smali "$W/smali/com/safedk/android/SafeDKApplication.smali"
smali a "$W/smali" -o "$W/classes3-patched.dex"

# --- 2. inject -------------------------------------------------------------
python3 patch/inject.py \
  --base base/granny-base.apk \
  --split base/granny-arm64.apk \
  --dex "$W/classes3-patched.dex" \
  --module "${MODULE:-loader/build/libgrannymod.so}" \
  --pkg "$PKG" \
  --out "$W/unsigned.apk"

# --- 3. align + sign -------------------------------------------------------
zipalign -p -f 4 "$W/unsigned.apk" "$W/aligned.apk"

if [ -n "${KEYSTORE:-}" ]; then
  apksigner sign --ks "$KEYSTORE" --ks-pass "pass:${KEYPASS}" \
    --out granny-modded.apk "$W/aligned.apk"
else
  keytool -genkeypair -keystore "$W/mod.keystore" -storepass android -keypass android \
    -dname "CN=GrannyMod" -alias mod -keyalg RSA -keysize 2048 -validity 10000 >/dev/null 2>&1
  apksigner sign --ks "$W/mod.keystore" --ks-pass pass:android --key-pass pass:android \
    --out granny-modded.apk "$W/aligned.apk"
fi

apksigner verify --print-certs granny-modded.apk | head -3
echo "package: $(aapt dump badging granny-modded.apk 2>/dev/null | sed -n "s/^package: name='\([^']*\)'.*/\1/p")"
ls -la granny-modded.apk
