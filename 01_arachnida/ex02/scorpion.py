#!/usr/bin/env python3
"""
scorpion - basic attributes + EXIF/metadata of image files, with -d to delete.

Uses Pillow to decode the image and read EXIF. With -d, writes a
metadata-free copy (universal rebuild from pixels) -- but only for images that
actually carry metadata, so a clean file is never copied again.

Arguments may be image files or folders (or a mix); relative paths are
resolved to absolute and de-duplicated. A folder is scanned (non-recursive)
for images by supported extension.

Covers the same extensions as spider (.jpg/.jpeg/.png/.gif/.bmp) and prints:
  - basic attributes: format, mode, dimensions, size, timestamps;
  - EXIF metadata including the Exif sub-IFD and GPS IFD;
  - decimal coordinates + nearest place (reverse_geocoder + pycountry) when a
    valid GPS fix is present.

Usage:
    ./scorpion.py FILE_OR_DIR [FILE_OR_DIR ...]        # show metadata
    ./scorpion.py -d FILE_OR_DIR [FILE_OR_DIR ...]     # show + strip each image
"""

import os
import sys
import math
import argparse
import contextlib
import datetime
from PIL import Image, ExifTags, UnidentifiedImageError
import reverse_geocoder as _rg
import pycountry as _pycountry

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
    """
    Format a byte count as a human-readable size.
    Arguments:
        size: number of bytes (int)
    Returns:
        a string like "1.12 MB" (str)
    """
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if size < 1024:
            return f"{size:.2f} {unit}"
        size /= 1024
    return f"{size:.2f} PB"


def press_any_key(prompt="  Press Enter to continue..."):
    """
    Pause until the user presses Enter.
    Arguments:
        prompt: message shown while waiting (str)
    Returns:
        None (returns immediately if stdin is closed/piped)
    """
    try:
        input(prompt)
    except EOFError:                        # piped/closed stdin -> don't block
        pass


def fmt_time(ts):
    """
    Format a POSIX timestamp as a local date-time string.
    Arguments:
        ts: seconds since the epoch (float)
    Returns:
        "YYYY-MM-DD HH:MM:SS" in local time (str)
    """
    return datetime.datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S")


def print_basic(path, img):
    """
    Print the basic file/image attributes block.
    Arguments:
        path: path to the image file, used for stat() (str)
        img: an open Pillow image, for format/mode/size (PIL.Image.Image)
    Returns:
        None (prints to stdout)
    """
    st = os.stat(path)
    print("  Basic attributes:")
    print(f"    Format        : {img.format}")
    print(f"    Mode          : {img.mode}")
    print(f"    Dimensions    : {img.width} x {img.height} px")
    print(f"    File size     : {human_size(st.st_size)} ({st.st_size} bytes)")
    print(f"    Modified      : {fmt_time(st.st_mtime)}")
    print(f"    Inode changed : {fmt_time(st.st_ctime)}")
    btime = getattr(st, "st_birthtime", None)
    if btime:
        print(f"    Created       : {fmt_time(btime)}")


def clean_value(value):
    """
    Make a tag value readable: shorten binary blobs, strip NUL padding.
    Arguments:
        value: a raw EXIF tag value (bytes, str, or other)
    Returns:
        "<N bytes>" for bytes, a NUL-stripped str for text, otherwise the
        value unchanged
    """
    if isinstance(value, bytes):
        return f"<{len(value)} bytes>"
    if isinstance(value, str):
        return value.replace("\x00", "").strip()
    return value


def print_ifd(title, items, names):
    """
    Print one IFD section as a table of tag -> value.
    Arguments:
        title: section heading, e.g. "GPS IFD" (str)
        items: tag id -> value mapping for this IFD (dict)
        names: tag id -> human-readable name lookup table (dict)
    Returns:
        None (prints to stdout)
    """
    print(f"    [{title}]")
    if not items:
        print("      (empty)")
        return
    for tag_id, value in items.items():
        name = names.get(tag_id, f"0x{tag_id:04X}")
        print(f"      {str(name):<22}: {clean_value(value)!r}")


def clean_ref(ref):
    """
    Normalise a GPS reference to an upper-case letter.
    Arguments:
        ref: the raw GPS*Ref value (bytes or str)
    Returns:
        "N"/"S"/"E"/"W", or "" if absent or NUL-padded (str)
    """
    if isinstance(ref, bytes):
        ref = ref.decode(errors="ignore")
    return (ref or "").replace("\x00", "").strip().upper()


def dms_to_decimal(dms, ref):
    """
    Convert a GPS coordinate from degrees/minutes/seconds to decimal degrees.
    Arguments:
        dms: the (degrees, minutes, seconds) triple (sequence of 3 numbers)
        ref: hemisphere reference, "N"/"S"/"E"/"W" (str or bytes)
    Returns:
        the coordinate in decimal degrees, negative for S/W (float)
    """
    deg, minutes, sec = (float(x) for x in dms)
    dec = deg + minutes / 60 + sec / 3600
    if clean_ref(ref) in ("S", "W"):
        dec = -dec
    return dec


def gps_to_latlon(gps_ifd):
    """
    Extract (lat, lon) in decimal degrees from a GPS IFD.

    Phones often write a GPS IFD with no actual fix: empty refs and 0/0
    rationals that decode to NaN. Those are rejected, not returned.
    Arguments:
        gps_ifd: the GPS IFD, a tag id -> value mapping (dict)
    Returns:
        (lat, lon) as floats, or None if GPS is absent/invalid (tuple | None)
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
    """
    Resolve the nearest place and country for a coordinate.
    Arguments:
        lat: latitude in decimal degrees (float)
        lon: longitude in decimal degrees (float)
    Returns:
        "City, Region, Country" (str), or None on any lookup failure
    """
    try:
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
    """
    Print decimal coordinates and, if resolvable, the location.
    Arguments:
        gps_ifd: the GPS IFD, a tag id -> value mapping (dict)
    Returns:
        None (prints nothing when there is no valid GPS fix)
    """
    latlon = gps_to_latlon(gps_ifd)
    if not latlon:
        return
    lat, lon = latlon
    print(f"      {'Coordinates':<22}: {lat:.6f}, {lon:.6f}")
    where = reverse_geocode(lat, lon)
    if where:
        print(f"      {'Location':<22}: {where}")


def print_exif(img):
    """
    Print the EXIF report: IFD0, the Exif sub-IFD and the GPS IFD.
    Arguments:
        img: an open Pillow image (PIL.Image.Image)
    Returns:
        None (prints to stdout; prints "EXIF: none present" if there is none)
    """
    exif = img.getexif()           # top-level / IFD0 tags
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
        press_any_key("\n  Press Enter to continue...")
        return

    print("\n  EXIF metadata:")
    ifd0 = {k: v for k, v in exif.items()
            if k not in (EXIF_IFD_TAG, GPS_IFD_TAG)}
    print_ifd("IFD0 (main image)", ifd0, TAG_NAMES)
    if exif_ifd:
        print_ifd("Exif sub-IFD", exif_ifd, TAG_NAMES)
    if gps_ifd:
        print_ifd("GPS IFD", gps_ifd, GPS_NAMES)
        print_gps_location(gps_ifd)
        
    press_any_key("\n  Press Enter to continue...")


def has_metadata(img):
    """
    Report whether an image carries descriptive metadata worth stripping.

    Checks the fields our strip removes (EXIF incl. Exif/GPS sub-IFDs, XMP,
    comment, ICC profile, PNG text chunks). Deliberately ignores the jfif/dpi
    markers Pillow re-adds on save, so a freshly-stripped file reads as clean
    (this is what prevents <name>_clean_clean accumulation).
    Arguments:
        img: an open Pillow image (PIL.Image.Image)
    Returns:
        True if descriptive metadata is present, else False (bool)
    """
    if len(img.getexif()) > 0:
        return True
    info = img.info
    if info.get("xmp") or info.get("comment") or info.get("icc_profile"):
        return True
    if getattr(img, "text", None):             # PNG tEXt/iTXt/zTXt
        return True
    return False


def report(path):
    """
    Show the full metadata report for one file.
    Arguments:
        path: path to the image file (str)
    Returns:
        True if the file was a readable image, False otherwise (bool)
    """
    print("=" * 60)
    print(path)
    print("=" * 60)
    try:
        with Image.open(path) as img:
            print_basic(path, img)
            print_exif(img)
    except (UnidentifiedImageError, OSError) as e:
        print(f"\033[91m  cannot read image: {e}\033[0m")
        press_any_key("\n  Press Enter to continue...")
        print()
        return False
    print()
    return True


# --------------------------------------------------------------------------- #
#  -d : metadata deletion
# --------------------------------------------------------------------------- #

def cleaned_path(path):
    """
    Build the output path for a cleaned copy (photo.jpg -> photo_clean.jpg).
    Arguments:
        path: the original file path (str)
    Returns:
        the "<stem>_clean<ext>" path, next to the original (str)
    """
    stem, ext = os.path.splitext(path)
    return f"{stem}_clean{ext}"


def strip_metadata(src, dst):
    """
    Universal strip: rebuild the image from its raw pixel bytes only, so NO
    metadata (EXIF, GPS, XMP, text chunks, ...) can survive. For JPEG this
    re-encodes the pixels (a small quality loss) -- the trade-off for a
    format-independent, guaranteed-clean result. tobytes()/frombytes() is the
    stable, non-deprecated pixel API; getdata()/putdata() is deprecated in
    Pillow 12.
    Arguments:
        src: path to the source image (str)
        dst: path to write the metadata-free copy (str)
    Returns:
        None (writes the cleaned image to dst)
    """
    with Image.open(src) as img:
        fmt = img.format                                   # true format, by content
        clean = Image.frombytes(img.mode, img.size, img.tobytes())
        if img.mode == "P":
            palette = img.getpalette()
            if palette:
                clean.putpalette(palette)
        clean.save(dst, format=fmt)  

def process(path, delete):
    """
    Show metadata; with -d, strip to a clean copy only if metadata is present.
    Arguments:
        path: path to the image file (str)
        delete: if True, write <name>_clean.ext when the image has metadata,
                show it, then pause (bool)
    Returns:
        None
    """
    ok = report(path)
    if not delete or not ok:
        return

    # Only clean images that actually carry metadata. This also means a
    # already-clean file is never copied again (no _clean_clean accumulation).
    try:
        with Image.open(path) as img:
            present = has_metadata(img)
    except (UnidentifiedImageError, OSError):
        return
    if not present:
        print("  no metadata to remove; nothing to clean.\n")
        return

    dst = cleaned_path(path)
    try:
        strip_metadata(path, dst)
    except (UnidentifiedImageError, OSError) as e:
        print(f"  cannot clean image: {e}\n")
        return
    print(f"  --> metadata stripped; clean copy written to:\n      {dst}\n")
    report(dst)
    press_any_key()


def gather_paths(args):
    """
    Flatten file/folder arguments into a de-duplicated list of image paths.

    Files are taken as given (Image.open validates them); folders are scanned
    non-recursively for images by supported extension. Paths are made absolute,
    which resolves relative paths and lets duplicates (e.g. a folder and a file
    inside it) collapse to one entry. Unreadable/missing arguments are skipped
    with a message.
    Arguments:
        args: the raw path arguments from the command line (list[str])
    Returns:
        a de-duplicated list of absolute image paths, in argument/scan order
        (list[str])
    """
    paths, seen = [], set()

    def add(p):
        ap = os.path.abspath(p)
        if ap not in seen:
            seen.add(ap)
            paths.append(ap)

    for arg in args:
        if os.path.isdir(arg):
            try:
                names = sorted(os.listdir(arg))
            except OSError as e:
                print(f"  skipping folder (cannot read): {arg} ({e})")
                continue
            found = False
            for name in names:
                full = os.path.join(arg, name)
                if os.path.isfile(full):
                    try:
                        with Image.open(full) as img:
                            found = True
                            add(full)
                    except (UnidentifiedImageError, OSError):
                        pass
            if not found:
                print(f"  (no supported images in folder: {arg})")
        elif os.path.isfile(arg):
            add(arg)
        else:
            print(f"  skipping (not found): {arg}")
    return paths


def main():
    """
    Parse the command line and process each gathered image path.
    Arguments:
        none (reads sys.argv)
    Returns:
        None (exits non-zero on a usage error or on Ctrl-C)
    """
    parser = argparse.ArgumentParser(
        description="Display image metadata; optionally strip it with -d.")
    parser.add_argument("files", nargs="+", metavar="FILE_OR_DIR",
                        help="image file(s) or folder(s) to inspect")
    parser.add_argument("-d", "--delete", action="store_true",
                        help="write a metadata-free copy (<name>_clean.ext) "
                             "for each image that has metadata")
    args = parser.parse_args()

    try:
        for path in gather_paths(args.files):
            process(path, args.delete)
    except KeyboardInterrupt:
        print("\n[scorpion] aborted.")
        sys.exit(130)
    
    print("\n[scorpion] done. Don't forget to delete .venv if you don't need it anymore.")


if __name__ == "__main__":
    main()