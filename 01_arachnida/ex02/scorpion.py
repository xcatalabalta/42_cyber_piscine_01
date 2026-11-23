#!/usr/bin/env python3
"""
scorpion - display basic attributes and EXIF metadata of image files.

Usage:
    ./scorpion.py FILE1 [FILE2 ...]

Handles .jpg/.jpeg, .png, .gif, .bmp. EXIF lives in JPEG; the other
formats show basic attributes only. The EXIF/TIFF parsing is hand-written
with struct (no Pillow/exifread) so the logic is our own.
"""

import os
import sys
import struct
import datetime

# ---- EXIF tag id -> human name (common subset; unknown shown as hex) --------
TAGS = {
    0x0100: "ImageWidth", 0x0101: "ImageLength", 0x010E: "ImageDescription",
    0x010F: "Make", 0x0110: "Model", 0x0112: "Orientation",
    0x011A: "XResolution", 0x011B: "YResolution", 0x0128: "ResolutionUnit",
    0x0131: "Software", 0x0132: "DateTime", 0x013B: "Artist",
    0x8298: "Copyright", 0x8769: "ExifOffset", 0x8825: "GPSInfo",
    0x829A: "ExposureTime", 0x829D: "FNumber", 0x8822: "ExposureProgram",
    0x8827: "ISOSpeedRatings", 0x9000: "ExifVersion",
    0x9003: "DateTimeOriginal", 0x9004: "DateTimeDigitized",
    0x920A: "FocalLength", 0xA002: "ExifImageWidth", 0xA003: "ExifImageHeight",
    0xA430: "CameraOwnerName", 0xA431: "BodySerialNumber",
}
GPS_TAGS = {
    0x0000: "GPSVersionID", 0x0001: "GPSLatitudeRef", 0x0002: "GPSLatitude",
    0x0003: "GPSLongitudeRef", 0x0004: "GPSLongitude", 0x0005: "GPSAltitudeRef",
    0x0006: "GPSAltitude",
}

# TIFF field type -> (size in bytes, struct code)
TYPE_SIZE = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8,
             11: 4, 12: 8}


def detect_format(data):
    if data[:3] == b"\xff\xd8\xff":
        return "JPEG"
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return "PNG"
    if data[:6] in (b"GIF87a", b"GIF89a"):
        return "GIF"
    if data[:2] == b"BM":
        return "BMP"
    return "UNKNOWN"


def dimensions(fmt, data):
    try:
        if fmt == "PNG":
            w, h = struct.unpack(">II", data[16:24])
            return w, h
        if fmt == "GIF":
            w, h = struct.unpack("<HH", data[6:10])
            return w, h
        if fmt == "BMP":
            w, h = struct.unpack("<ii", data[18:26])
            return w, abs(h)
        if fmt == "JPEG":
            i = 2
            while i + 9 < len(data):
                if data[i] != 0xFF:
                    i += 1
                    continue
                marker = data[i + 1]
                # SOF markers carry the frame dimensions
                if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
                    h, w = struct.unpack(">HH", data[i + 5:i + 9])
                    return w, h
                if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
                    i += 2
                    continue
                seg_len = struct.unpack(">H", data[i + 2:i + 4])[0]
                i += 2 + seg_len
    except (struct.error, IndexError):
        pass
    return None


# ---- EXIF ------------------------------------------------------------------
def find_exif(data):
    """Return the TIFF block (bytes starting at the byte-order mark) or None."""
    i = 2
    while i + 4 < len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        if marker == 0xD9 or marker == 0xDA:   # EOI / start of scan -> stop
            break
        if 0xD0 <= marker <= 0xD7 or marker == 0x01:
            i += 2
            continue
        seg_len = struct.unpack(">H", data[i + 2:i + 4])[0]
        if marker == 0xE1:                     # APP1
            payload = data[i + 4:i + 2 + seg_len]
            if payload[:6] == b"Exif\x00\x00":
                return payload[6:]
        i += 2 + seg_len
    return None


def read_value(tiff, endian, type_, count, value_field):
    size = TYPE_SIZE.get(type_, 1) * count
    if size <= 4:
        raw = value_field[:size]
    else:
        off = struct.unpack(endian + "I", value_field)[0]
        raw = tiff[off:off + size]

    if type_ == 2:  # ASCII
        return raw.split(b"\x00", 1)[0].decode("ascii", "replace")
    codes = {1: "B", 3: "H", 4: "I", 6: "b", 8: "h", 9: "i"}
    if type_ in codes:
        vals = struct.unpack(endian + codes[type_] * count, raw)
        return vals[0] if count == 1 else list(vals)
    if type_ in (5, 10):  # (S)RATIONAL
        code = "II" if type_ == 5 else "ii"
        out = []
        for k in range(count):
            num, den = struct.unpack(endian + code, raw[k * 8:k * 8 + 8])
            out.append(f"{num}/{den}")
        return out[0] if count == 1 else out
    if type_ == 7:  # UNDEFINED
        return raw
    return raw


def parse_ifd(tiff, endian, offset, names):
    """Return (list of (name, value), dict of sub-IFD pointers)."""
    entries = []
    subifds = {}
    if offset + 2 > len(tiff):
        return entries, subifds
    (n,) = struct.unpack(endian + "H", tiff[offset:offset + 2])
    p = offset + 2
    for _ in range(n):
        if p + 12 > len(tiff):
            break
        tag, type_, count = struct.unpack(endian + "HHI", tiff[p:p + 8])
        value_field = tiff[p + 8:p + 12]
        p += 12
        if tag in (0x8769, 0x8825):            # pointers to sub-IFDs
            subifds[tag] = struct.unpack(endian + "I", value_field)[0]
            continue
        try:
            val = read_value(tiff, endian, type_, count, value_field)
        except (struct.error, IndexError):
            val = "<unreadable>"
        name = names.get(tag, f"0x{tag:04X}")
        entries.append((name, val))
    return entries, subifds


def parse_exif(tiff):
    if tiff[:2] == b"II":
        endian = "<"
    elif tiff[:2] == b"MM":
        endian = ">"
    else:
        return None
    (ifd0_off,) = struct.unpack(endian + "I", tiff[4:8])
    result = []
    ifd0, subs = parse_ifd(tiff, endian, ifd0_off, TAGS)
    result.append(("IFD0 (main image)", ifd0))
    if 0x8769 in subs:
        sub, _ = parse_ifd(tiff, endian, subs[0x8769], TAGS)
        result.append(("Exif sub-IFD", sub))
    if 0x8825 in subs:
        gps, _ = parse_ifd(tiff, endian, subs[0x8825], GPS_TAGS)
        result.append(("GPS IFD", gps))
    return result


# ---- reporting -------------------------------------------------------------
def human_size(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n} bytes" if unit == "B" else f"{n/1024:.1f} {unit} ({n} bytes)"
        n /= 1024


def report(path):
    print("=" * 60)
    print(path)
    print("=" * 60)
    try:
        st = os.stat(path)
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        print(f"  cannot read: {e}\n")
        return

    fmt = detect_format(data)
    print("  Basic attributes:")
    print(f"    Format        : {fmt}")
    print(f"    File size     : {human_size(st.st_size)}")
    dim = dimensions(fmt, data)
    if dim:
        print(f"    Dimensions    : {dim[0]} x {dim[1]} px")
    mtime = datetime.datetime.fromtimestamp(st.st_mtime)
    ctime = datetime.datetime.fromtimestamp(st.st_ctime)
    print(f"    Modified      : {mtime:%Y-%m-%d %H:%M:%S}")
    print(f"    Inode changed : {ctime:%Y-%m-%d %H:%M:%S}")

    if fmt == "JPEG":
        tiff = find_exif(data)
        if tiff is None:
            print("\n  EXIF: none present")
        else:
            parsed = parse_exif(tiff)
            if not parsed:
                print("\n  EXIF: present but unreadable")
            else:
                print("\n  EXIF metadata:")
                for section, entries in parsed:
                    print(f"    [{section}]")
                    if not entries:
                        print("      (empty)")
                    for name, val in entries:
                        if isinstance(val, list):
                            val = " ".join(str(x) for x in val)
                        print(f"      {name:<20}: {val}")
    else:
        print("\n  EXIF: not applicable for this format")
    print()


def main():
    if len(sys.argv) < 2:
        print("Usage: scorpion.py FILE1 [FILE2 ...]", file=sys.stderr)
        sys.exit(1)
    for path in sys.argv[1:]:
        report(path)


if __name__ == "__main__":
    main()