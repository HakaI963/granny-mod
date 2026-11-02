#!/usr/bin/env bash
# variant.sh <name> <rename:0|1> <dexpatch:0|1> <module:0|1>
set -euo pipefail
NAME=$1; RENAME=$2; DEXP=$3; MOD=$4
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
PKGARG="com.dvloper.granny.mod"; [ "$RENAME" = 0 ] && PKGARG=""
python3 - "$W" <<'PY'
import sys, zipfile, os
w=sys.argv[1]
z=zipfile.ZipFile('base/granny-base.apk')
open(os.path.join(w,'classes3.dex'),'wb').write(z.read('classes3.dex'))
PY
baksmali d "$W/classes3.dex" -o "$W/smali" >/dev/null
if [ "$DEXP" = 1 ]; then
  cp patch/SafeDKApplication.smali "$W/smali/com/safedk/android/SafeDKApplication.smali"
fi
smali a "$W/smali" -o "$W/c3.dex" >/dev/null
MODARG=""; [ "$MOD" = 1 ] && MODARG="loader/build/libgrannymod.so"
python3 patch/inject.py --base base/granny-base.apk --split base/granny-arm64.apk \
  --dex "$W/c3.dex" --module "${MODARG:-/nonexistent}" --pkg "$PKGARG" --out "$W/u.apk"
zipalign -p -f 4 "$W/u.apk" "$W/a.apk"
apksigner sign --ks mod.keystore --ks-pass pass:android --key-pass pass:android \
  --out "/tmp/v-$NAME.apk" "$W/a.apk"
cp "/tmp/v-$NAME.apk" "/sdcard/Download/Altaris/v-$NAME.apk"
echo "built $NAME rename=$RENAME dex=$DEXP mod=$MOD $(stat -c%s /tmp/v-$NAME.apk)"
