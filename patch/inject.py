#!/usr/bin/env python3
"""Assemble the modded APK by replacing entries in the shipped zip.

apktool cannot rebuild Granny (AppLovin ships drawable names aapt rejects), so
we modify the original archive directly:

  AndroidManifest.xml  package renamed in the binary AXML
  classes3.dex         SafeDKApplication.<clinit> loads libgrannymod.so
  lib/arm64-v8a/*      merged from the config split, plus our module
  META-INF/*           dropped; the result is re-signed by zipalign/apksigner

Everything else is copied byte-for-byte, including the 26 MB resource streams
and the IL2CPP metadata, so the game itself is untouched.
"""
import argparse
import os
import shutil
import subprocess
import sys
import zipfile

import axml_rename

OLD = "com.dvloper.granny"
DEX_MEMBER = "classes3.dex"
# Signature block file names in a v1-signed jar. Everything else under
# META-INF/ must survive: kotlinx.coroutines ships ServiceLoader entries there
# (CoroutineExceptionHandler / MainDispatcherFactory) and dropping them breaks
# startup for any code that touches Dispatchers.Main.
_SIG_EXT = (".SF", ".RSA", ".DSA", ".EC", ".MF")


def _is_signature(name: str) -> bool:
    up = name.upper()
    if not up.startswith("META-INF/"):
        return False
    if "/SERVICES/" in up or up.endswith(".VERSION") or ".version" in name:
        return False
    return up.endswith(_SIG_EXT) or "SIGNATURES" in up


def rebuild(src: str, split: str, dex: str, module: str, newpkg: str, out: str) -> None:
    a_wants_dex = bool(dex)
    if os.path.exists(out):
        os.remove(out)
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zout:
        # carry the doNotCompress list over from the source entry flags
        for it in zin.infolist():
            name = it.filename
            # META-INF is dropped (we re-sign); the manifest and dex are
            # re-added below from their patched copies, so skip the originals.
            if _is_signature(name):
                continue
            if name == "AndroidManifest.xml" or (a_wants_dex and name == DEX_MEMBER):
                continue
            data = zin.read(name)
            info = zipfile.ZipInfo(name, date_time=it.date_time)
            # stored entries (dex/arsc/resource streams) must stay uncompressed
            info.compress_type = zipfile.ZIP_STORED if it.compress_type == zipfile.ZIP_STORED else zipfile.ZIP_DEFLATED
            info.external_attr = it.external_attr
            zout.writestr(info, data)

        # renamed binary manifest
        manifest = zin.read("AndroidManifest.xml")
        tmp = out + ".axml"
        with open(tmp, "wb") as fh:
            fh.write(manifest)
        if newpkg and newpkg != OLD:
            axml_rename.patch_file(tmp, OLD, newpkg)
        zout.write(tmp, "AndroidManifest.xml", zipfile.ZIP_STORED)
        os.remove(tmp)

        # patched dex
        zout.write(dex, DEX_MEMBER, zipfile.ZIP_STORED)

        # native libraries: the split's, then ours
        if split:
            with zipfile.ZipFile(split) as zs:
                for n in zs.namelist():
                    if n.startswith("lib/arm64-v8a/"):
                        zout.writestr(zipfile.ZipInfo(n, (1980, 1, 1, 0, 0, 0)),
                                      zs.read(n))
        if module:
            if os.path.exists(module):
                zout.write(module, "lib/arm64-v8a/libgrannymod.so", zipfile.ZIP_STORED)
            else:
                print(f"WARNING: no {module}; building without the loader "
                      f"(the game stays unmodified, mods will not load)", file=sys.stderr)

    print("wrote", out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--split")
    ap.add_argument("--dex", required=True, help="patched classes3.dex")
    ap.add_argument("--module", help="libgrannymod.so")
    ap.add_argument("--pkg", default=OLD + ".mod")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    rebuild(a.base, a.split, a.dex, a.module, a.pkg, a.out)
    return 0


if __name__ == "__main__":
    main()
