#!/usr/bin/env python3
"""
scorpion basic attributes + EXIF/metadata of image files.

This implementation uses Pillow to decode the image and read EXIF.
The low-level byte decoding is delegated to Pillow.

Covers the same extensions as spider (.jpg/.jpeg/.png/.gif/.bmp) and prints:
  - basic attributes: format, mode, dimensions, size, timestamps (incl. creation
    date where the platform exposes it);
  - EXIF metadata for formats that carry it, including the Exif sub-IFD
    (exposure, DateTimeOriginal, lens, ...) and the GPS IFD (coordinates),
    which a naive getexif()-only approach misses;
  - when GPS coordinates are present, the decimal lat/lon and (if the optional
    reverse_geocoder + pycountry packages are installed) the nearest place and
    country.

Usage:    ./scorpion_lib.py FILE1 [FILE2 ...]
Requires: Pillow                       (mandatory)
Optional: reverse_geocoder, pycountry  (for the Location line; degrees still
                                        print without them)
"""

import os
import sys
import math
import signal
import contextlib
import datetime
from PIL import Image, ExifTags, UnidentifiedImageError
import reverse_geocoder as _rg
import pycountry as _pycountry

# Reverse geocoding is OPTIONAL: 
# Use the secure version in case of fail.

# Same set spider handles.
SUPPORTED = (".jpg", ".jpeg", ".png", ".gif", ".bmp")

# Reverse lookup tables shipped with Pillow: numeric tag id -> human name.
TAG_NAMES = ExifTags.TAGS      # IFD0 + Exif tags
GPS_NAMES = ExifTags.GPSTAGS   # GPS IFD tags

# Pointer tags: their "value" is an offset to a sub-IFD, not data to print.
EXIF_IFD_TAG = 0x8769
GPS_IFD_TAG = 0x8825

# Numeric tag ids inside the GPS IFD.
GPS_LAT_REF = 1
GPS_LAT = 2
GPS_LON_REF = 3
GPS_LON = 4


def human_size(size):
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if size < 1024:
            return f"{size:.2f} {unit}"
        size /= 1024
    return f"{size:.2f} PB"


def fmt_time(ts):
    return datetime.datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S")


def print_basic(path, img):
    st = os.stat(path)
    print("  Basic attributes:")
    print(f"    Format        : {img.format}")
    print(f"    Mode          : {img.mode}")
    print(f"    Dimensions    : {img.width} x {img.height} px")
    print(f"    File size     : {human_size(st.st_size)} ({st.st_size} bytes)")
    print(f"    Modified      : {fmt_time(st.st_mtime)}")
    print(f"    Inode changed : {fmt_time(st.st_ctime)}")
    # True creation (birth) time only exists on some platforms/filesystems.
    btime = getattr(st, "st_birthtime", None)
    if btime:
        print(f"    Created       : {fmt_time(btime)}")


def clean_value(value):
    """Make a tag value readable: shorten binary blobs, strip NUL padding."""
    if isinstance(value, bytes):
        return f"<{len(value)} bytes>"
    if isinstance(value, str):
        # EXIF ASCII fields are often NUL-padded to a fixed width.
        return value.replace("\x00", "").strip()
    return value


def print_ifd(title, items, names):
    print(f"    [{title}]")
    if not items:
        print("      (empty)")
        return
    for tag_id, value in items.items():
        name = names.get(tag_id, f"0x{tag_id:04X}")
        print(f"      {str(name):<22}: {clean_value(value)!r}")


def clean_ref(ref):
    """Normalise a GPS ref to an upper-case letter, or '' if absent/padded."""
    if isinstance(ref, bytes):
        ref = ref.decode(errors="ignore")
    return (ref or "").replace("\x00", "").strip().upper()


def dms_to_decimal(dms, ref):
    """(degrees, minutes, seconds) + 'N'/'S'/'E'/'W' -> signed decimal degrees."""
    deg, minutes, sec = (float(x) for x in dms)
    dec = deg + minutes / 60 + sec / 3600
    if clean_ref(ref) in ("S", "W"):
        dec = -dec
    return dec


def gps_to_latlon(gps_ifd):
    """Return (lat, lon) in decimal degrees, or None if GPS is absent/invalid.

    Phones often write a GPS IFD with no actual fix: empty refs and 0/0
    rationals that decode to NaN. Those must be rejected, not printed.
    """
    rlat = clean_ref(gps_ifd.get(GPS_LAT_REF))
    rlon = clean_ref(gps_ifd.get(GPS_LON_REF))
    if rlat not in ("N", "S") or rlon not in ("E", "W"):
        return None
    try:
        lat = dms_to_decimal(gps_ifd[GPS_LAT], rlat)
        lon = dms_to_decimal(gps_ifd[GPS_LON], rlon)
    except (KeyError, TypeError, ValueError, ZeroDivisionError):
        return None
    if not (math.isfinite(lat) and math.isfinite(lon)):
        return None
    return lat, lon


def reverse_geocode(lat, lon):
    """Nearest place + country for a coordinate, or None if geocoding is off."""

    try:
        # reverse_geocoder prints a "Loading..." banner to stdout on first use;
        # redirect stdout to /dev/null just for that call so our report stays clean.
        with open(os.devnull, "w") as devnull, \
                contextlib.redirect_stdout(devnull):
            hit = _rg.search((lat, lon), mode=1)[0]   # mode=1 = single-threaded
        country = _pycountry.countries.get(alpha_2=hit["cc"])
        place = ", ".join(p for p in (hit["name"], hit["admin1"]) if p)
        name = country.name if country else hit["cc"]
        return f"{place}, {name}" if place else name
    except Exception:
        return None


def print_gps_location(gps_ifd):
    """Print decimal coordinates and (if available) the resolved location."""
    latlon = gps_to_latlon(gps_ifd)
    if not latlon:
        return
    lat, lon = latlon
    print(f"      {'Coordinates':<22}: {lat:.6f}, {lon:.6f}")
    where = reverse_geocode(lat, lon)
    if where:
        print(f"      {'Location':<22}: {where}")


def print_exif(img):
    exif = img.getexif()           # top-level / IFD0 tags
    # Pull the sub-IFDs explicitly; getexif() alone does NOT descend into them.
    try:
        exif_ifd = exif.get_ifd(EXIF_IFD_TAG)
    except Exception:
        exif_ifd = {}
    try:
        gps_ifd = exif.get_ifd(GPS_IFD_TAG)
    except Exception:
        gps_ifd = {}

    if not exif and not exif_ifd and not gps_ifd:
        print("\n  EXIF: none present")
        return

    print("\n  EXIF metadata:")
    # IFD0 without the two pointer tags (shown as their own sections instead).
    ifd0 = {k: v for k, v in exif.items()
            if k not in (EXIF_IFD_TAG, GPS_IFD_TAG)}
    print_ifd("IFD0 (main image)", ifd0, TAG_NAMES)
    if exif_ifd:
        print_ifd("Exif sub-IFD", exif_ifd, TAG_NAMES)
    if gps_ifd:
        print_ifd("GPS IFD", gps_ifd, GPS_NAMES)
        print_gps_location(gps_ifd)


def report(path):
    print("=" * 60)
    print(path)
    print("=" * 60)

    """
    ext = os.path.splitext(path)[1].lower()
    if ext not in SUPPORTED:
        print(f"  unsupported extension '{ext}' "
              f"(expected {', '.join(SUPPORTED)})\n")
        return
    if not os.path.isfile(path):
        print("  not a file / cannot read\n")
        return
    """
    try:
        with Image.open(path) as img:
            print_basic(path, img)
            print_exif(img)
    except (UnidentifiedImageError, OSError) as e:
        print(f"  cannot read image: {e}")
    print()


def main():
    # Use the secure version in case of fail.
    
    if len(sys.argv) < 2:
        print("Usage: scorpion_lib.py FILE1 [FILE2 ...]", file=sys.stderr)
        sys.exit(1)
    for path in sys.argv[1:]:
        report(path)
    print("[scorpion] done.\nDon't forget to delete the .venv folder if you don't need it anymore.")


if __name__ == "__main__":
    main()