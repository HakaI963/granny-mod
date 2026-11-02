#!/usr/bin/env python3
"""Rename the package inside a *binary* AndroidManifest.xml (AXML).

apktool cannot rebuild this APK: AppLovin ships drawable file names such as
"$applovin_ic_mute_to_unmute__0.xml" which aapt rejects, so both `apktool b` and
`apktool b --no-res` fail. We therefore edit the binary manifest directly and
re-assemble the APK by replacing entries in the original zip.

String pool layout, confirmed byte-by-byte against this file (251 strings,
flags=0 so no UTF-8 side data, no styles):

    chunk header : uint16 type=0x0001, uint16 headerSize=28, uint32 size
    pool header  : uint32 stringCount, uint32 styleCount, uint32 flags,
                   uint32 stringsStart, uint32 stylesStart
    offsets      : uint32[stringCount], relative to stringsStart
    string       : uint16 utf16Length, utf16Length code units, uint16 NUL

so "theme" occupies 2 + 10 + 2 = 14 bytes, which matches the observed
offset difference between entry 0 and entry 1.
"""
import argparse
import struct
import sys

RES_STRING_POOL_TYPE = 0x0001
RES_XML_TYPE = 0x0003
RES_XML_START_ELEMENT_TYPE = 0x0102
RES_XML_RESOURCE_MAP_TYPE = 0x0180
SKIP_CHUNKS = {RES_STRING_POOL_TYPE, RES_XML_RESOURCE_MAP_TYPE, 0x0100, 0x0101, 0x0103, 0x0104}


def read_strings(d: bytes):
    """Return (pool_off, phdr, strcount, strings) parsed from an AXML blob."""
    ftype, fhdr, _fsize = struct.unpack_from("<HHI", d, 0)
    if ftype != RES_XML_TYPE:
        raise SystemExit(f"not AXML (type 0x{ftype:04x})")
    ptype, phdr, _psize = struct.unpack_from("<HHI", d, fhdr)
    if ptype != RES_STRING_POOL_TYPE:
        raise SystemExit(f"first chunk is not a string pool (0x{ptype:04x})")
    strcount, stylecount, flags, strstart, _styles = struct.unpack_from("<IIIII", d, fhdr + 8)
    if flags & (1 << 8):
        raise SystemExit("UTF-8 pool not supported")
    if stylecount:
        raise SystemExit("styled pool not supported")
    base = fhdr + strstart
    offs = struct.unpack_from(f"<{strcount}I", d, fhdr + phdr)
    out = []
    for o in offs:
        p = base + o
        (n,) = struct.unpack_from("<H", d, p)
        out.append(d[p + 2:p + 2 + n * 2].decode("utf-16-le"))
    return fhdr, phdr, strcount, strstart, out


def _string_at(d: bytes, pool_off: int, phdr: int, base: int, idx: int) -> str:
    o = struct.unpack_from("<I", d, pool_off + phdr + 4 * idx)[0]
    n = struct.unpack_from("<H", d, base + o)[0]
    return d[base + o + 2:base + o + 2 + n * 2].decode("utf-16-le")


def strip_attrs(data: bytes, drop: list) -> tuple:
    """Remove named attributes from the first element that carries any of them.

    Returns (bytes, how_many_were_removed). Attribute records are fixed size, so
    dropping one means memmove-ing the remainder and shrinking the chunk plus the
    file header.
    """
    ftype, fhdr, _fs = struct.unpack_from("<HHI", data, 0)
    ptype, phdr, psize = struct.unpack_from("<HHI", data, fhdr)
    strcount = struct.unpack_from("<I", data, fhdr + 8)[0]
    strstart = struct.unpack_from("<I", data, fhdr + 20)[0]
    base = fhdr + strstart

    o = fhdr + psize
    buf = bytearray(data)
    removed = 0
    while o < len(buf) - 8:
        t, h, sz = struct.unpack_from("<HHI", buf, o)
        if t in SKIP_CHUNKS:
            o += sz if sz else 4
            continue
        if t == RES_XML_START_ELEMENT_TYPE:
            ext = o + h
            _ns, _name, attrStart, attrSize, attrCount = struct.unpack_from("<IIHHH", buf, ext)
            ab = ext + attrStart
            names = []
            for i in range(attrCount):
                ni = struct.unpack_from("<I", buf, ab + i * attrSize + 4)[0]
                names.append(_string_at(buf, fhdr, phdr, base, ni))
            keep = [i for i, n in enumerate(names) if n not in drop]
            if len(keep) != attrCount:
                removed = attrCount - len(keep)
                newattr = b"".join(bytes(buf[ab + i * attrSize: ab + (i + 1) * attrSize]) for i in keep)
                tail = bytes(buf[ab + attrCount * attrSize:])
                struct.pack_into("<H", buf, ext + 12, len(keep))
                struct.pack_into("<I", buf, o + 4, sz - removed * attrSize)
                struct.pack_into("<I", buf, 4, len(buf) - removed * attrSize)
                buf[ab:ab + attrCount * attrSize] = newattr
                del buf[len(buf) - len(tail):]
                buf.extend(tail)
                return bytes(buf), removed
        o += sz if sz else 4
    return bytes(buf), 0


def patch(data: bytes, old: str, new: str) -> bytes:
    fhdr, phdr, strcount, strstart, strings = read_strings(data)
    base = fhdr + strstart

    changed = 0
    for i, s in enumerate(strings):
        if s == new or s.startswith(new + ".") or s.startswith(new + "-"):
            continue  # already migrated: keeps re-running the patch safe
        if s == old or s.startswith(old + ".") or s.startswith(old + "-"):
            strings[i] = new + s[len(old):]
            changed += 1
    if not changed:
        raise SystemExit("no matching strings - nothing renamed")

    blob = bytearray()
    offs = []
    for s in strings:
        offs.append(len(blob))
        enc = s.encode("utf-16-le") + b"\x00\x00"
        blob += struct.pack("<H", len(s)) + enc
    while len(blob) % 4:
        blob += b"\x00"

    pool_size = phdr + 4 * strcount + len(blob)
    pool = bytearray()
    pool += struct.pack("<HHI", RES_STRING_POOL_TYPE, phdr, pool_size)
    pool += struct.pack("<IIIII", strcount, 0, 0, phdr + 4 * strcount, 0)
    pool += struct.pack(f"<{strcount}I", *offs)
    pool += blob

    tail = data[fhdr + (struct.unpack_from("<I", data, fhdr + 4)[0]):]
    out = bytearray(data[:fhdr]) + pool + bytearray(tail)
    struct.pack_into("<I", out, 4, len(out))  # file header size

    # The shipped manifest declares requiredSplitTypes="base__abi"; we merge the
    # split's native libs in, so that declaration must go or the installer
    # refuses the APK with INSTALL_FAILED_MISSING_SPLIT.
    out, n = strip_attrs(bytes(out), ["requiredSplitTypes", "splitTypes"])
    print(f"renamed {changed} identifier(s); stripped {n} split attribute(s)", file=sys.stderr)
    return bytes(out)


def patch_file(path: str, old: str, new: str) -> None:
    """In-place helper for the injector."""
    data = open(path, "rb").read()
    open(path, "wb").write(patch(data, old, new))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("manifest")
    ap.add_argument("new_pkg")
    ap.add_argument("--old", default="com.dvloper.granny")
    a = ap.parse_args()
    data = open(a.manifest, "rb").read()
    out = patch(data, a.old, a.new_pkg)
    open(a.manifest, "wb").write(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
