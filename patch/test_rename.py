#!/usr/bin/env python3
"""Tests for patch/rename.py.

Regression coverage for two bugs this script actually had:
  1. a substring ".mod.mod" guard that false-positived on the legitimate
     authority "com.dvloper.granny.mod.mobileadsinitprovider", and
  2. per-attribute rewriting that double-applied the suffix because a provider
     element carries both android:name and android:authorities.
"""
import re
import subprocess
import sys
import tempfile
import os

RENAME = os.path.join(os.path.dirname(__file__), "rename.py")
OLD = "com.dvloper.granny"
NEW = "com.dvloper.granny.mod"

MANIFEST = """<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{pkg}">
  <uses-permission android:name="{pkg}.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION"/>
  <permission android:name="{pkg}.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION"
              android:protectionLevel="signature"/>
  <application android:name="{pkg}.MyApp">
    <provider android:authorities="{pkg}.applovininitprovider" android:name="x"/>
    <provider android:authorities="{pkg}.mobileadsinitprovider" android:name="y"/>
    <provider android:name="{pkg}.playgamesinitprovider" android:authorities="{pkg}.playgamesinitprovider"/>
    <activity android:name="{pkg}.MainActivity"/>
  </application>
</manifest>
"""


def run(manifest_text):
    d = tempfile.mkdtemp()
    p = os.path.join(d, "AndroidManifest.xml")
    with open(p, "w", encoding="utf-8") as fh:
        fh.write(manifest_text)
    r = subprocess.run([sys.executable, RENAME, p, NEW], capture_output=True, text=True)
    assert r.returncode == 0, f"rename failed: {r.stderr}"
    with open(p, encoding="utf-8") as fh:
        return fh.read()


def check(name, cond):
    print(("ok   " if cond else "FAIL ") + name)
    return 0 if cond else 1


def main():
    out = run(MANIFEST.format(pkg=OLD))
    rc = 0
    rc |= check("package attribute renamed", f'package="{NEW}"' in out)
    rc |= check("every authority moved",
                all(f"{NEW}.{a}" in out for a in
                    ("applovininitprovider", "mobileadsinitprovider", "playgamesinitprovider")))
    rc |= check("no double suffix", ".mod.mod" not in out)
    rc |= check("provider with both name and authorities is single-suffixed",
                re.search(r'android:authorities="com\.dvloper\.granny\.mod\.playgamesinitprovider"', out)
                and ".mod.mod" not in out)
    # Assert on the exact set of rewritten identifiers. A naive substring
    # check is useless here: the *correct* output for the package attribute is
    # "com.dvloper.granny.mod", which starts with the old id.
    ids = set(re.findall(r'"(com\.dvloper\.granny[^"]*)"', out))
    rc |= check("every identifier is migrated",
                all(i == NEW or i.startswith(NEW + ".") for i in ids))
    rc |= check("identifier set is the expected one",
                ids == {NEW, NEW + ".DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION",
                        NEW + ".MyApp", NEW + ".applovininitprovider",
                        NEW + ".mobileadsinitprovider", NEW + ".playgamesinitprovider",
                        NEW + ".MainActivity"})

    # idempotency: feeding the result back must be a no-op
    again = run(out)
    rc |= check("idempotent", again == out)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
