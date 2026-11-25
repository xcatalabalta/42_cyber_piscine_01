# scorpion — behavior & usage

`scorpion` displays the **basic attributes** and **EXIF/metadata** of image
files and can optionally **strip that metadata** into a clean copy. It is the
library-based ("permissive") implementation: Pillow decodes the image and reads
EXIF; the reporting and deletion logic is the program's own.

---

## Requirements

- **Python 3**
- **Pillow** — mandatory (decoding + EXIF). Installed as `Pillow`, imported as `PIL`.
- **reverse_geocoder** and **pycountry** — used for the GPS *Location* line
  (imported unconditionally, so they must be installed for the program to start).
- Typically launched through `scorpion.sh`, which builds a local `.venv` from
  `requirements.txt` on first run and then executes the program inside it.

```
Pillow>=10,<13
reverse_geocoder
pycountry
```

---

## Usage

```
./scorpion.py FILE_OR_DIR [FILE_OR_DIR ...]        # show metadata
./scorpion.py -d FILE_OR_DIR [FILE_OR_DIR ...]     # show + strip each image
```

| Option | Meaning |
|--------|---------|
| `FILE_OR_DIR` | one or more image files and/or folders (at least one required) |
| `-d`, `--delete` | write a metadata-free copy `<name>_clean.ext` for each image that has metadata |
| `-h`, `--help` | argparse-generated help and usage |

Arguments may freely mix files and folders. The program pauses for **Enter**
after each file's report (piped/redirected input skips the pause automatically).
On completion it prints a reminder about the `.venv`.

---

## What it accepts — detection is by content, not extension

There is **no extension filter**. Any file Pillow can decode is treated as an
image, and the extension is irrelevant to that decision:

- **A named file** is opened directly and validated by its bytes, so a
  **correct**, **wrong** (`.png` on a JPEG), **false** (`.pdf` on a JPEG),
  **nonsense** (`.kaka`), or **missing** extension are all handled the same way.
  A file Pillow cannot decode is reported as unreadable.
- **A folder** is scanned **non-recursively**: every regular file is probed with
  Pillow and the ones that decode as images are included (again regardless of
  extension). Subfolders are not descended into.

This means formats beyond the five "spider" types (e.g. **TIFF**, WebP) are also
accepted whenever Pillow supports them — see *Limitations* for a TIFF caveat.

All paths are resolved to **absolute** paths and **de-duplicated**, so:

- relative paths (`.`, `img/a.jpg`) work from any working directory, and
- naming both a folder and a file inside it processes that file only once.

Missing arguments are skipped with `skipping (not found): …`; a folder with no
decodable images prints `(no supported images in folder: …)`; an unreadable
image is shown (in red) as `cannot read image: …`.

---

## The report (view mode)

For each image the program prints:

**Basic attributes**
- `Format` — real format detected by Pillow (JPEG/PNG/GIF/BMP/TIFF/…)
- `Mode` — pixel mode (RGB, RGBA, P, …)
- `Dimensions` — width × height in pixels
- `File size` — human-readable plus exact byte count
- `Modified` / `Inode changed` — filesystem timestamps (`st_mtime`, `st_ctime`)
- `Created` — only when the platform exposes a birth time

**EXIF metadata** (when present) — organized by directory:
- `[IFD0 (main image)]` — top-level tags (Make, Model, Orientation, Software, DateTime, …)
- `[Exif sub-IFD]` — capture settings (ExposureTime, FNumber, ISO, DateTimeOriginal, FocalLength, …)
- `[GPS IFD]` — raw GPS tags, followed by:
  - `Coordinates` — latitude, longitude in **decimal degrees**, and
  - `Location` — nearest place and country (via `reverse_geocoder` + `pycountry`)

If there is no EXIF, it prints `EXIF: none present`.

### GPS handling details
- Coordinates are converted from the EXIF degrees/minutes/seconds triples using
  the `N/S/E/W` reference (negative for S and W).
- A GPS block with **no real fix** (empty refs, `0/0` rationals that decode to
  NaN — common on phones with location off) is detected and the
  `Coordinates`/`Location` lines are **suppressed**; the raw GPS tags still show.
- `Location` is the nearest city in `reverse_geocoder`'s offline dataset, so it
  names a place near the coordinate (e.g. a suburb), not a precise landmark.

Tag values are cleaned for readability: binary blobs show as `<N bytes>`, and
NUL-padded strings are trimmed. Unknown tags are shown by hex id (e.g. `0x0220`).

---

## Deletion mode (`-d`)

With `-d`, after showing a file's metadata the program strips it **only if the
image actually carries metadata**:

1. **Show** the original's metadata.
2. **Check** for descriptive metadata — EXIF (incl. Exif/GPS sub-IFDs), XMP,
   comment, ICC profile, or PNG text chunks. (The `jfif`/`dpi` markers Pillow
   re-adds on save are deliberately ignored.)
   - If **none**: prints `no metadata to remove; nothing to clean.` and skips.
3. **Strip** to a clean copy `<name>_clean.ext` next to the original.
4. **Show** the cleaned copy's metadata (now empty).
5. **Pause** for Enter.

### How stripping works
The clean image is rebuilt from **raw pixel bytes only**
(`Image.frombytes(mode, size, img.tobytes())`) and saved with an **explicit
format** equal to the source's true format
(`clean.save(dst, format=img.format)`). So:

- **No** descriptive metadata — EXIF, GPS, XMP, text chunks — can survive.
- The cleaned copy is always encoded in the image's **true format**, and keeps
  the **original filename** even if its extension is wrong, false, or absent
  (e.g. a JPEG named `photo.pdf` yields `photo_clean.pdf` that is still a valid
  JPEG; a `photo` with no extension yields `photo_clean`).
- The indexed/GIF palette is preserved for `P`-mode images.

### Why there is no `_clean_clean` buildup
Because a freshly-stripped file reports *no* descriptive metadata, re-running
`-d` sees nothing to strip in the existing `_clean` copies and leaves them
alone. There is no name-based skipping — the metadata gate alone prevents
accumulation. (Exception: TIFF — see *Limitations*.)

---

## Output example

```
============================================================
/home/user/img/eiffel-tower.jpg
============================================================
  Basic attributes:
    Format        : JPEG
    Mode          : RGB
    Dimensions    : 3000 x 2000 px
    File size     : 1.12 MB (1176112 bytes)
    Modified      : 2026-11-24 13:32:28
    Inode changed : 2026-11-24 13:32:32

  EXIF metadata:
    [IFD0 (main image)]
      Make                  : 'OPPO'
      Model                 : 'OPPO A94 5G'
      DateTime              : '2026:11:24 11:06:11'
    [Exif sub-IFD]
      DateTimeOriginal      : '2026:11:24 11:06:11'
      FNumber               : 1.7
    [GPS IFD]
      GPSLatitudeRef        : 'N'
      GPSLatitude           : (48.0, 51.0, 29.74)
      Coordinates           : 48.858261, 2.294500
      Location              : Vanves, Ile-de-France, France
```

---

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | normal completion |
| 2 | argparse usage error (no files, unknown flag) |
| 130 | interrupted with Ctrl-C |

---

## Limitations & notes

- **JPEG re-encodes on strip.** Rebuilding from pixels re-saves the image, so a
  stripped JPEG loses a small amount of quality. For *lossless* EXIF removal
  from JPEG specifically, `piexif.remove()` is the alternative (JPEG-only); the
  pixel rebuild is the format-independent, guaranteed-clean approach.
- **Animated GIFs** collapse to their first frame (pixel rebuild reads one frame).
- **TIFF exception to the no-accumulation rule.** In TIFF the metadata directory
  *is* the native container, so a valid TIFF always carries structural tags
  (ImageWidth, Compression, StripOffsets, …) that `getexif()` reports. A stripped
  TIFF therefore still shows those tags and is seen as "has metadata," so
  repeated `-d` runs *will* produce `*_clean_clean.tiff`. Descriptive metadata
  (Make, GPS, …) is still removed — only the mandatory structural tags remain.
  The five required formats (jpg/png/gif/bmp) are unaffected.
- **`Location` is approximate** — nearest city in an offline dataset, not a
  precise address.
- **Reverse geocoding needs its two packages**; without them the import fails at
  startup (this build imports them unconditionally).
