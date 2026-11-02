# Granny Mod — mod support for the shipped APK

Adds a **Mods** button and a runtime mod API to `com.dvloper.granny` 1.8.12 by
injecting a native loader into the original game. Nothing here reimplements
Granny: it patches the real binary.

## Why injection and not scene editing

The APK ships **IL2CPP only** — there is no `Assembly-CSharp.dll`, just
`libil2cpp.so` + `global-metadata.dat`. A Unity `onClick` needs a managed
method, and none can be added, so menu wiring is done by detouring the game's
own `UnityEngine.UI.Button.OnPointerClick`.

The build also cannot use `apktool b`: AppLovin ships drawables named
`$applovin_ic_...xml`, which aapt rejects. So `patch/inject.py` edits the
binary AXML directly and rebuilds the archive by replacing zip entries.

## Layout

```
loader/            native module (NDK, arm64-v8a), embeds Lua 5.4
  src/main.c         bind il2cpp, pump, install hooks
  src/hooks.c        detour managed method pointers
  src/reflect.c      class/field/method lookup by name
  src/menu.c         Mods button cloned from the real PlayButton
  src/mods.c         mod.json discovery, enable/disable, persistence
  src/monetise.c     ads + IAP neutralisation
  src/scripts.c      Lua sandbox
patch/
  SafeDKApplication.smali  Application <clinit> that loadLibrary's the module
  axml_rename.py           binary-manifest package rename + attr strip
  inject.py                zip surgery assembler
  test_rename.py           rename/idempotency tests
base/                (not committed) granny-base.apk + granny-arm64.apk
build.sh             end-to-end: smali -> inject -> align -> sign
```

## Reversing notes

`libil2cpp.so` exports **241 `il2cpp_*` symbols**, including
`class_from_name`, `runtime_invoke`, `field_set_value`,
`field_static_set_value` and `class_get_fields`, so mods address the game by
managed name rather than by offset.

`il2cpp_delegate_create` is **not** exported, which is why UnityEvent cannot be
used and clicks are caught by detouring `Button.OnPointerClick`.

Managed classes were read from `global-metadata.dat` v31 (10,037 types,
65,995 methods, 40,942 fields) and cross-checked against an independent parser.

Menu geometry from `level1` RectTransforms:

| | |
|---|---|
| label size | 160×30, localScale (0.69, 1.57) |
| column x | 178.5 |
| Play / MoreGames / Quit y | 76.8 / −86.4 / −153.6 |
| button pitch | 67.2 |
| **Mods** (new) | x 178.5, y 9.6 — one pitch below Play |

`GrannyDecisions` (`0x145b020`) returns early when any of six busy flags at
`+0x2c1…+0x2c7` is set; `followPlayer` (`0x145bbac`) adds two more at
`+0x214`/`+0x215`. All eight are reproduced in `AiBusy`.

## Building

CI (`.github/workflows/build-apk.yml`) builds the loader with the NDK and emits
`granny-modded.apk`. It needs the two shipped APKs in `base/`, which are not
committed; supply them via a private release asset or a local copy.

Local:

```bash
mkdir -p base && cp /path/to/com.dvloper.granny.apk base/granny-base.apk \
              && cp /path/to/config.arm64_v8a.apk  base/granny-arm64.apk
cmake -S loader -B loader/build -DANDROID_ABI=arm64-v8a \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake
cmake --build loader/build
bash build.sh
```

## Mod format

```json
{
  "name": "Fast Granny",
  "enabled": true,
  "targets": { "EnemyAIGranny.walkSpeed": 5.5, "FPSControllerNEW.forwardSpeed": 7.25 }
}
```

Mods live in `/sdcard/Android/media/com.dvloper.granny.mod/mods/<name>/`,
rescanned every 3 s; the in-game Mods screen toggles and deletes them.

## Legal

Granny is © DVloper. This repo contains no game binaries; the base and split
APKs must be supplied by the user and are not redistributed here.
