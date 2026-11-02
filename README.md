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

## Making a repackaged Granny actually boot

Three separate things had to be fixed, each found on device rather than assumed.

**1. Missing splits.** The manifest declares `android:requiredSplitTypes="base\_\_abi"`
while we merge the arm64 split's libraries in, so the installer answers
`INSTALL_FAILED_MISSING_SPLIT`. `patch/axml_rename.py` strips both attributes in
the binary AXML (the attribute record is a fixed 20 bytes, so removal is a
memmove plus shrinking the element chunk and the file header).

**2. ServiceLoader files are not signature files.** Dropping all of `META-INF/`
also deleted `kotlinx.coroutines.CoroutineExceptionHandler` and
`MainDispatcherFactory`, which is what makes `Dispatchers.Main` resolve. The
coroutine-based ad/IAP stack then fails during init. Only the v1 signature block
is removed now; 53 entries survive, and only `stamp-cert-sha256` is dropped.

**3. The package name must be changed in two places, not one.** Unity does this,
with no zero guard, in `com.unity3d.player.N`:

```java
getString(getIdentifier("game_view_content_description", "string", getPackageName()))
```

`getPackageName()` is answered from the manifest, but the name an id resolves
against lives inside **`resources.arsc`**. Renaming only the manifest makes the
lookup return 0 and `getString(0)` throw

```
Resources$NotFoundException: String resource ID #0x0
```

during `UnityPlayerActivity.onCreate`, before a single frame is drawn.
`patch/arsc_rename.py` rewrites the arsc package string: it is a fixed inline
`uint16[128]` field at offset 12 of the package chunk (id `0x7f`), verified byte
by byte, so the patch preserves the file size and needs no offset recompute. The
field must be decoded as whole code units before splitting on NUL - every ASCII
character carries a `0x00` high byte, so scanning bytes for `0x00 0x00` stops
halfway through the string.

This was proven by bisect, then confirmed by exporting both clones and comparing:

| variant | manifest | resources.arsc | result |
|---|---|---|---|
| A: merge + strip attrs only | `com.altaris.apps.c79d449a4` | `com.altaris.apps.c79d449a4` | boots to Unity |
| B: + manifest rename only | `com.altaris.apps.c88b10f2d` | `com.dvloper.granny` | dies in onCreate |

The clone engine rewrites the arsc name itself, but only when it still equals the
manifest - which is why a half-renamed APK ends up mismatched.

## Loader notes

`libil2cpp.so` is dlopen'd: neither `libunity.so` nor `libmain.so` has any
undefined `il2cpp_*` symbol or a `NEEDED` on it, so there is no PLT hook
available on il2cpp itself. Both *do* import `dlsym`
(`R_AARCH64_JUMP_SLOT` at `0x1455598` and `0x9120`), so `main.c` patches that GOT
slot and wraps the `il2cpp_init` pointer they resolve. That yields one
deterministic fact - the runtime is initialised - which matters because calling
`il2cpp_domain_get()` earlier faults inside il2cpp:

```
Fatal signal 11 (SIGSEGV), fault addr 0x135
#02 libil2cpp.so+0x1382700   (body that the il2cpp_domain_get thunk 0x133de04 branches to)
#03 libgrannymod.so+0x2838   gm_try_bind
```

`gm_try_bind` therefore now runs on the Unity main thread immediately after
`il2cpp_init` returns, and the bind thread refuses to probe at all if
interception did not happen, rather than crashing the game.

Managed cloning uses `UnityEngine.Object.Internal_CloneSingle` (one argument,
returns `Object`) rather than `Object.Instantiate<T>`, because invoking a generic
method *definition* through `il2cpp_runtime_invoke` is undefined. Both names were
confirmed present in the shipped `global-metadata.dat` first.

## Legal

Granny is © DVloper. This repo contains no game binaries; the base and split
APKs must be supplied by the user and are not redistributed here.
