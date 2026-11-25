# JPEG metadata byte structure (+ PIL)

JPEG is the metadata-rich format and the one the raw scorpion spends most of its
effort on. It has **two nested structures**: the JPEG *marker container* on the
outside, and a complete *TIFF/EXIF tree* inside one of its segments.

See `metadata_bytes_00_overview.md` for the shared primitives (endianness,
signatures, the walk-the-records idea).

---

## Basic attributes: format + dimensions

Before any metadata, scorpion reports the **format** and **dimensions**, read
straight from the bytes. (File size and timestamps come separately from
`os.stat` — the filesystem layer, not the image bytes.)

### Format detection (signature)

A JPEG starts with three bytes:

| index | hex | meaning |
|-------|-----|---------|
| `data[0]` | 0xFF | SOI marker start |
| `data[1]` | 0xD8 | SOI (Start Of Image) |
| `data[2]` | 0xFF | start of the following marker |

```python
if data[:3] == b"\xff\xd8\xff":
    fmt = "JPEG"
```

Slices are 0-based with an exclusive end, so `data[:3]` is indices 0,1,2; and a
*slice* compared to a `bytes` literal is correct, whereas `data[0]` alone is the
int `0xFF` (see the overview's "slice vs index").

### Dimensions (why a scan is needed)

Unlike the other three formats, JPEG does **not** store width/height at a fixed
offset. The size lives in the **Start Of Frame (SOF)** marker, which appears
only after the compression tables, so you must walk the markers (Part A.1) until
an SOF (`FF C0`–`FF CF`, excluding `C4`/`C8`/`CC`) and then read:

```
height = big-endian uint16 at SOF + 5
width  = big-endian uint16 at SOF + 7
```

The byte at SOF+4 is the sample precision; SOF+9 is the component count
(1 = grayscale, 3 = YCbCr/RGB, 4 = CMYK) — effectively the "mode".

### In PIL

`img.format` (`"JPEG"`), `img.size` (Pillow walked the SOF for you), `img.mode`;
file size/timestamps from `os.stat(path)`.

---

## Part A — General theory (byte structure)

### A.1 The marker container

A JPEG is a sequence of **marker segments**. Each marker is `0xFF` followed by a
marker byte. Most markers then carry a **2-byte big-endian length** and that
many bytes of payload (the length *includes* the 2 length bytes themselves).

```
FF D8                      SOI  (Start Of Image, standalone, no length)
FF E1  <len:2>  <payload>  APP1 (EXIF or XMP)
FF E0  <len:2>  <payload>  APP0 (JFIF)
...
FF DA  <len:2>  ...        SOS  (Start Of Scan) -> compressed pixel data follows
FF D9                      EOI  (End Of Image, standalone)
```

Markers relevant to metadata and dimensions:

| Marker | Name | Holds |
|--------|------|-------|
| `FF D8` | SOI | start (no length) |
| `FF E0` | APP0 | JFIF (density/thumbnail) |
| `FF E1` | APP1 | **EXIF** or **XMP** |
| `FF ED` | APP13 | IPTC / Photoshop IRB |
| `FF FE` | COM | free-text comment |
| `FF C0`–`FF CF` | SOFn | frame header → **dimensions** |
| `FF DA` | SOS | pixel data begins (stop scanning for metadata) |
| `FF D9` | EOI | end |

**Walking the segments** (the core loop of a raw parser):

```
pos = 2                      # skip SOI
while pos + 4 <= len(data):
    if data[pos] != 0xFF: pos += 1; continue
    marker = data[pos+1]
    if marker in (0xD8,0xD9) or 0xD0 <= marker <= 0xD7:
        pos += 2; continue   # standalone markers, no length
    if marker == 0xDA: break # SOS -> metadata is all before here
    seg_len = (data[pos+2] << 8) | data[pos+3]      # big-endian
    payload = data[pos+4 : pos+2+seg_len]
    handle(marker, payload)
    pos += 2 + seg_len
```

**Identifying what an APP1 payload is** — by its leading signature:

- EXIF: begins with `45 78 69 66 00 00` = `Exif\0\0`
- XMP: begins with `http://ns.adobe.com/xap/1.0/\0`

**Dimensions** are *not* at a fixed offset: scan to an **SOF** marker
(`C0`–`CF`, excluding `C4`/`C8`/`CC`) and read, from the marker, height then
width as big-endian uint16 at offsets +5 and +7.

### A.2 The TIFF/EXIF tree (inside APP1)

After the `Exif\0\0` signature comes a self-contained **TIFF block**. This is
where Make, Model, DateTime, exposure, and GPS actually live.

```
TIFF header (8 bytes):
  +0  2  byte-order mark: "II" (little-endian) or "MM" (big-endian)
  +2  2  magic 0x002A (read with the byte order just set)
  +4  4  offset to IFD0  (relative to the TIFF header start)

IFD (Image File Directory):
  +0  2   entry count N
  +2  12*N entries
  +...  4  offset to next IFD (0 = none)

one 12-byte entry:
  +0  2  tag id
  +2  2  type   (1=BYTE 2=ASCII 3=SHORT 4=LONG 5=RATIONAL 7=UNDEFINED 9=SLONG 10=SRATIONAL ...)
  +4  4  count  (number of values of that type)
  +8  4  value, OR an offset to the value if it doesn't fit in 4 bytes
```

Four facts a raw parser must get right:

1. **The byte-order mark is per file** (`II`/`MM`) and governs *every* integer
   in the TIFF block. Read it once, then use it everywhere.
2. **Offsets are relative to the TIFF header start**, not the file.
3. **Inline vs offset rule**: `size = typesize[type] * count`. If `size <= 4`,
   the value sits in the entry's last 4 bytes; otherwise those 4 bytes are an
   **offset** into the TIFF block where the value lives.
4. **Sub-IFD pointers**: tag `0x8769` → **Exif sub-IFD** (exposure, DateTimeOriginal,
   ISO, lens), tag `0x8825` → **GPS IFD** (lat/long as RATIONAL triples). These
   tags' "values" are offsets to another IFD you parse the same way.

Type sizes (bytes per element): `BYTE/ASCII/UNDEFINED/SBYTE = 1`,
`SHORT/SSHORT = 2`, `LONG/SLONG/FLOAT = 4`, `RATIONAL/SRATIONAL/DOUBLE = 8`.
RATIONAL = two LONGs (numerator, denominator).

### A.3 Raw parse, end to end

```
1. magic FF D8 FF            -> it's a JPEG
2. walk markers (A.1)        -> find APP1 whose payload starts "Exif\0\0"
3. T = payload[6:]           -> the TIFF block
4. read II/MM at T[0:2]      -> set endianness
5. ifd0_off = uint32(T[4:8]) -> parse_ifd(T, ifd0_off)
6. in each entry:
     - if tag 0x8769 -> remember Exif sub-IFD offset
     - if tag 0x8825 -> remember GPS IFD offset
     - else decode value by (type, count), inline-or-offset
7. parse the Exif sub-IFD and GPS IFD the same way
```

This is exactly what the hand-written scorpion's `find_exif` → `parse_exif` →
`parse_ifd` → value-decode chain does.

---

## Part B — Application to PIL

Pillow does all of Part A internally. The access points:

### B.1 The EXIF tree

```python
from PIL import Image, ExifTags

with Image.open(path) as img:
    exif = img.getexif()                     # IFD0 as a {tag_id: value} mapping
    exif_ifd = exif.get_ifd(0x8769)          # Exif sub-IFD (follows the pointer)
    gps_ifd  = exif.get_ifd(0x8825)          # GPS IFD
```

- `img.getexif()` returns IFD0, **including** the pointer tags `0x8769`/`0x8825`
  (that is why scorpion filters them out before printing IFD0, and calls
  `get_ifd` to descend). Observed keys on a sample: `[34665, 34853, 271]`
  i.e. `[0x8769, 0x8825, Make]`.
- `get_ifd(0x8769)` / `get_ifd(0x8825)` do the "follow the offset, parse that
  IFD" step for you. **Plain `getexif()` does not descend** — this is the bug in
  naive solutions that read only IFD0 and miss exposure/GPS.
- Names: `ExifTags.TAGS[tag_id]` for IFD0/Exif tags, `ExifTags.GPSTAGS[tag_id]`
  for GPS tags. Unknown ids have no entry (scorpion falls back to hex).

### B.2 The raw EXIF bytes

```python
raw = img.info.get("exif")          # bytes: the APP1 EXIF payload incl. "Exif\0\0"
```

This is the un-parsed block — handy for a lossless copy, or to hand to `piexif`.

### B.3 Other JPEG metadata

```python
img.info.get("jfif")            # JFIF present (APP0)
img.info.get("dpi")             # resolution, derived from JFIF/EXIF
img.info.get("comment")         # COM segment, if any
img.info.get("xmp")             # XMP packet bytes (APP1 XMP), if any
img.info.get("icc_profile")     # ICC colour profile bytes
```

### B.4 Dimensions and format

```python
img.format          # "JPEG"
img.size            # (width, height) -- Pillow read the SOF marker for you
img.mode            # "RGB", "CMYK", ...
```

### B.5 Mapping summary

| Byte structure (Part A) | PIL access |
|-------------------------|------------|
| walk markers → SOF | `img.size` |
| APP1 `Exif\0\0` → TIFF block | `img.info["exif"]` (raw) |
| TIFF IFD0 | `img.getexif()` |
| follow `0x8769` | `exif.get_ifd(0x8769)` |
| follow `0x8825` | `exif.get_ifd(0x8825)` |
| tag id → name | `ExifTags.TAGS` / `ExifTags.GPSTAGS` |
| APP1 XMP / COM / ICC | `img.info["xmp"/"comment"/"icc_profile"]` |

---

## Sources

- **CIPA DC-008** (Exif) — the TIFF tree, tags, types, sub-IFD pointers.
- **Adobe TIFF 6.0** — IFD/entry layout, byte-order mark, inline-vs-offset.
- **ITU-T T.81 (ISO/IEC 10918-1)** — JPEG markers and the SOF frame header.
- **ITU-T T.871** — JFIF (APP0).
