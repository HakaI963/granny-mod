#!/usr/bin/env python3
"""Rename a decoded APK's package identity in its binary-authored manifest.

Granny's manifest carries the package in `package=` plus seven derived
identifiers (initialisation providers, the AudienceNetwork/Ironsource/
LevelPlay content providers, the Play Games init provider and the
dynamic-receiver permission). All of them have to move together, otherwise the
modded APK collides with the store build at install time or fails to resolve
its own providers.

Usage: rename.py AndroidManifest.xml com.dvloper.granny.mod
"""
import re
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    path, new_pkg = sys.argv[1], sys.argv[2]
    old_pkg = "com.dvloper.granny"
    if not new_pkg.startswith(old_pkg):
        # still allowed, but this script only knows how to rewrite the old id
        print(f"warning: {new_pkg} does not extend {old_pkg}", file=sys.stderr)

    with open(path, encoding="utf-8") as fh:
        text = fh.read()

    found = re.findall(r'package="([^"]+)"', text)
    if not found:
        print("no package= attribute found", file=sys.stderr)
        return 1
    if found[0] == new_pkg:
        print(f"already renamed to {new_pkg}")
        return 0

    # rewrite in one pass so an already-renamed manifest is idempotent
    def sub_id(value: str) -> str:
        # Already-migrated identifiers must be left alone, otherwise re-running
        # the patch yields "com.dvloper.granny.mod.mod".
        if value == new_pkg or value.startswith(new_pkg + "."):
            return value
        if value == old_pkg or value.startswith(old_pkg + "."):
            return new_pkg + value[len(old_pkg):]
        return value

    def _quoted(m):
        # any "..." literal whose contents start with the old package id
        return '"' + sub_id(m.group(1)) + '"'

    def _pkg_attr(m):
        return m.group(1) + sub_id(m.group(2)) + m.group(3)

    # One pass over every quoted identifier. Doing this per-attribute in
    # sequence double-applied the suffix (an element carries both android:name
    # and android:authorities for the same provider), producing
    # "…granny.mod.mod.applovininitprovider".
    text = re.sub(r'"(' + re.escape(old_pkg) + r'(?:\.[A-Za-z0-9_]+)*)"', _quoted, text)
    # the package= attribute is separate: it may be exactly the bare old id
    text = re.sub(r'(package=")([^"]+)(")', _pkg_attr, text)

    # Guard against the double-suffix bug by comparing the package attribute
    # itself; a substring test would false-positive on authorities such as
    # "com.dvloper.granny.mod.mobileadsinitprovider".
    written = re.search(r'package="([^"]+)"', text)
    if not written or written.group(1) != new_pkg:
        print(f"package did not rename cleanly: {written and written.group(1)}", file=sys.stderr)
        return 1

    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)

    pkg = re.search(r'package="([^"]+)"', text).group(1)
    print(f"package={pkg}")
    for m in re.finditer(r'android:authorities="([^"]+)"', text):
        print("  authority", m.group(1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
