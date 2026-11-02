#!/usr/bin/env python3
"""Rename the package inside a binary resources.arsc (RES_TABLE).

Why this is required
--------------------
Unity's player does this, verbatim, in com.unity3d.player.N and in
UnityPlayer.GetGlViewContentDescription:

    getIdentifier("game_view_content_description", "string", getPackageName())
    getString(<that result>)              # there is no zero guard

getPackageName() is answered from the *manifest*, but the name an id resolves
against is the package name stored in *resources.arsc*. Rename only the
manifest and the lookup returns 0, getString(0) throws

    Resources$NotFoundException: String resource ID #0x0

and UnityPlayerActivity dies in onCreate before rendering a single frame. The
two files must be rewritten together. This is why a naive repackage of a Unity
game with a new applicationId crashes at launch.

Format (as shipped by Granny 1.8.12, verified byte by byte)
-----------------------------------------------------------
    table   @0     uint16 type=0x0002, uint16 headerSize=12,  uint32 size
    pool    @12    uint16 type=0x0001, uint16 headerSize=28,  uint32 size
                   uint32 stringCount=4006, uint32 styleCount=0, uint32 flags,
                   uint32 stringsStart=16052, uint32 stylesStart
                   (flags bit 8 set -> UTF-8 entries)
    package @233108  uint16 type=0x0200, uint16 headerSize=288, uint32 size=144868
                     uint32 id = 0x7f (127)
                     uint16 name[128]   inline UTF-16, NUL terminated, at +12

The name is a *fixed-size inline field*, not a string-pool reference, and the
package header size is fixed at 288. Renaming is therefore a size-preserving
byte patch: no offsets, pool sizes or chunk sizes change at all. That holds for
any name up to 127 characters, which covers every realistic applicationId.
"""
import struct
import sys

RES_TABLE_TYPE = 0x0002
RES_STRING_POOL_TYPE = 0x0001
RES_TABLE_PACKAGE_TYPE = 0x0200
NAME_FIELD_OFFSET = 12      # right after header(8) + id(4)
NAME_FIELD_BYTES = 256      # uint16[128]
NAME_MAX_CHARS = 127        # leave room for the NUL terminator


def find_package(data: bytes):
    """Return the offset of the first RES_TABLE_PACKAGE chunk, or None."""
    ttype, thdr, tsize = struct.unpack_from("<HHI", data, 0)
    if ttype != RES_TABLE_TYPE:
        raise SystemExit(f"not a RES_TABLE (type 0x{ttype:04x})")
    o = thdr
    while o + 8 <= len(data):
        ctype, chdr, csize = struct.unpack_from("<HHI", data, o)
        if csize < 8 or o + csize > len(data):
            break
        if ctype == RES_TABLE_PACKAGE_TYPE:
            return o, thdr
        o += csize
    raise SystemExit("no RES_TABLE_PACKAGE chunk found")


def _decode_name(raw: bytes) -> str:
    """Decode a fixed UTF-16 name field.

    Scanning the *bytes* for b"\\x00\\x00" is wrong: every ASCII character is
    stored as <c> 0x00, so an ASCII char followed by a NUL terminator reads as
    a double zero mid-string. Decode whole code units first, then split.
    """
    units = len(raw) // 2
    chars = struct.unpack_from(f"<{units}H", raw, 0)
    return "".join(chr(c) for c in chars).split("\x00", 1)[0]


def get_name(data: bytes) -> str:
    pkg_off, _ = find_package(data)
    raw = data[pkg_off + NAME_FIELD_OFFSET: pkg_off + NAME_FIELD_OFFSET + NAME_FIELD_BYTES]
    return _decode_name(bytes(raw))


def patch(data: bytes, old: str, new: str, verbose: bool = True) -> bytes:
    if len(new) > NAME_MAX_CHARS:
        raise SystemExit(f"new package name {new!r} exceeds {NAME_MAX_CHARS} chars")
    pkg_off, thdr = find_package(data)
    pid = struct.unpack_from("<I", data, pkg_off + 8)[0]

    out = bytearray(data)
    field = out[pkg_off + NAME_FIELD_OFFSET: pkg_off + NAME_FIELD_OFFSET + NAME_FIELD_BYTES]
    current = _decode_name(bytes(field))
    if current == new:
        if verbose:
            print(f"arsc: package already {new}", file=sys.stderr)
        return data
    if current != old:
        raise SystemExit(f"arsc package is {current!r}, expected {old!r} - refusing")

    enc = new.encode("utf-16-le") + b"\x00\x00"
    if len(enc) > NAME_FIELD_BYTES:
        raise SystemExit("encoded name too long for the 256-byte field")
    # Rewrite the whole field so no stale characters survive past the new NUL.
    out[pkg_off + NAME_FIELD_OFFSET: pkg_off + NAME_FIELD_OFFSET + NAME_FIELD_BYTES] = \
        enc + b"\x00" * (NAME_FIELD_BYTES - len(enc))

    if len(out) != len(data):
        raise SystemExit("internal error: patch was not size-preserving")
    struct.pack_into("<I", out, 4, len(out))  # table size, unchanged; asserts consistency
    if verbose:
        print(f"arsc: package id 0x{pid:02x} renamed {current} -> {new} "
              f"(size preserved: {len(out)} bytes)", file=sys.stderr)
    return bytes(out)


def patch_file(path: str, old: str, new: str) -> None:
    data = open(path, "rb").read()
    open(path, "wb").write(patch(data, old, new))


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("arsc")
    ap.add_argument("new_pkg")
    ap.add_argument("--old", default="com.dvloper.granny")
    ap.add_argument("--show", action="store_true", help="print the current package and exit")
    a = ap.parse_args()
    d = open(a.arsc, "rb").read()
    if a.show:
        print(get_name(d))
        sys.exit(0)
    patch_file(a.arsc, a.old, a.new_pkg)
