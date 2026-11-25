# Image metadata byte structure — overview

A base for building the **raw** scorpion (parsing the bytes by hand) and a map
from that byte-level view to the **PIL/Pillow** calls that do the same thing.

There is one doc per format:

- `metadata_bytes_jpeg.md` — markers + EXIF/TIFF/IFD (the deep one)
- `metadata_bytes_png.md` — chunks + text/tIME/eXIf
- `metadata_bytes_gif.md` — blocks + extensions (comment, application)
- `metadata_bytes_bmp.md` — headers (almost no metadata)

This overview covers the ideas common to all four.

---

## 1. Two layers of metadata

- **Filesystem metadata** — timestamps, owner, permissions. Lives in the
  filesystem, read with `os.stat()`. Not in the file's bytes.
- **Embedded metadata** — stored *inside* the file and travels with it. This is
  what the raw parser reads, and what these docs describe.

Scorpion's basic attributes (size, mtime) come from the first layer; EXIF and
friends come from the second.

---

## 2. The three facts you need for any field

To read a fixed field out of a binary file you always need three things, often
documented in three different places:

1. **Where the structure starts** — a file signature / header of known length.
2. **How records are framed** — the per-chunk/segment header that precedes data.
3. **The field's position, size, and encoding** — including **byte order**.

Everything below is a specialisation of these three facts.

---

## 3. Byte order (endianness)

A multi-byte integer is stored either **big-endian** (most-significant byte
first) or **little-endian** (least-significant first). The same bytes
`00 00 00 20` are `32` read big-endian and `0x20000000` read little-endian.

| Format | Integer byte order |
|--------|--------------------|
| PNG    | **big-endian** (network order) |
| GIF    | **little-endian** |
| BMP    | **little-endian** |
| JPEG container markers | **big-endian** |
| EXIF/TIFF inside JPEG/PNG | **either — declared per file** by an `II`/`MM` mark |

In Python's `struct`: `>` = big-endian, `<` = little-endian. A raw parser reads
each integer with the right prefix; get it wrong and `32` becomes garbage.

---

## 4. Signatures (magic numbers)

Each format opens with a fixed byte signature — this is how you detect the
format by **content**, not by extension:

| Format | First bytes (hex) | ASCII |
|--------|-------------------|-------|
| JPEG | `FF D8 FF` | — |
| PNG  | `89 50 4E 47 0D 0A 1A 0A` | `.PNG....` |
| GIF  | `47 49 46 38 37/39 61` | `GIF87a` / `GIF89a` |
| BMP  | `42 4D` | `BM` |

Compare raw **bytes to bytes** (`data[:3] == b"\xff\xd8\xff"`); never decode to
text first (decoding drops the non-ASCII bytes a magic number relies on).

### Reading bytes in Python: slice vs index

Two facts about `bytes` access underlie every comparison in a raw parser:

- **Indexing is 0-based and a slice's end is exclusive.** `data[:8]` is
  positions **0..7** — eight bytes; index 8 is the first byte *not* included.
  So the PNG signature `data[:8] == b"\x89PNG\r\n\x1a\n"` maps like this:

  | index | byte | hex | role |
  |-------|------|-----|------|
  | `data[0]` | `\x89` | 0x89 | high-bit guard |
  | `data[1]` | `P` | 0x50 | "PNG" |
  | `data[2]` | `N` | 0x4E | |
  | `data[3]` | `G` | 0x47 | |
  | `data[4]` | `\r` | 0x0D | CR |
  | `data[5]` | `\n` | 0x0A | LF |
  | `data[6]` | `\x1a` | 0x1A | Ctrl-Z |
  | `data[7]` | `\n` | 0x0A | LF |

- **A slice returns `bytes`; a single index returns an `int`.** `data[:3]` is a
  3-byte `bytes` object (compare to `b"\xff\xd8\xff"`), but `data[0]` is the
  integer `137` (`0x89`). So `data[0] == 0x89` is `True` while
  `data[0] == b"\x89"` is `False`. This is exactly why format detection compares
  *slices* to byte literals, while byte-scanning loops compare *single bytes* as
  ints (`data[i] == 0xFF`).

---

## 5. Two shapes of container

The four formats fall into two structural families, which decides how you walk
them:

- **Chunk / segment streams — JPEG, PNG, GIF.** After the signature the file is
  a sequence of records, each with a header saying what it is and how long it
  is. You **walk** record by record (`pos += header + length`), dispatching the
  ones that hold metadata and skipping the rest, until an end marker.
  - PNG: `length(4) + type(4) + data + CRC(4)`
  - JPEG: `0xFF + marker + length(2) + payload`
  - GIF: an introducer byte selects a block type, then sub-blocks
- **Fixed-offset headers — BMP.** No records; dimensions and the little
  metadata there is sit at constant offsets from the start.

The metadata-rich formats (JPEG, and PNG via `eXIf`) nest a **TIFF/EXIF tree**
inside one of their records — see the JPEG doc, which the PNG doc refers to.

---

## 6. The generic raw-parser skeleton

Every format handler in a raw scorpion follows the same outline:

```python
with open(path, "rb") as f:
    data = f.read()                 # whole file in memory -> random access

fmt = detect_format(data)           # magic-number sniff (section 4)

if fmt == "JPEG":
    walk_jpeg_markers(data)         # find APP1/Exif -> parse TIFF tree
elif fmt == "PNG":
    walk_png_chunks(data)           # find tEXt/iTXt/tIME/eXIf
elif fmt == "GIF":
    walk_gif_blocks(data)           # find comment/application extensions
elif fmt == "BMP":
    read_bmp_headers(data)          # fixed offsets; little metadata
```

Reading the whole file into `bytes` is the simplest choice: metadata parsing
needs **random access by offset** (e.g. "jump to the value at offset N"), which
is trivial on a `bytes` object and awkward on a streaming handle.

Two safety habits for a hand parser:

- **Bounds-check every read** — a truncated or hostile file must not index past
  the end. Guard with `if pos + need > len(data): break`.
- **Treat offsets as relative to the documented base** — e.g. TIFF offsets are
  relative to the start of the TIFF header, *not* the file.

---

## 7. How PIL abstracts all of this

Pillow does steps 3–6 internally. Three access points cover almost everything,
and each format doc maps its bytes to these:

| PIL access | What it exposes |
|------------|-----------------|
| `Image.open(path)` | sniffs the format by content; `img.format`, `img.mode`, `img.size` |
| `img.info` (dict) | per-format odds and ends: `dpi`, `icc_profile`, `comment`, `duration`, `loop`, `transparency`, raw `exif` bytes, … |
| `img.getexif()` | the EXIF/TIFF tree as a mapping; `.get_ifd(0x8769/0x8825)` descends into the Exif/GPS sub-IFDs |

Plus a few format-specific extras:

- **PNG** → `img.text` (a dict of the `tEXt`/`zTXt`/`iTXt` chunks)
- **GIF** → `img.n_frames`, `img.seek(n)`, and `img.info["comment"/"duration"/"loop"]`
- **JPEG** → `img.info["exif"]` (the raw APP1 EXIF bytes), `ExifTags.TAGS` / `ExifTags.GPSTAGS` (id → name)

The mental model: **the raw parser reproduces, by hand, what `Image.open` +
`getexif()` + `img.info` do for you.** Each format doc shows both sides so you
can check one against the other while building the raw version.

---

## 8. Where metadata lives, at a glance

| Format | Dimensions | Timestamps | Camera/EXIF | GPS | Author/text | Where it's stored |
|--------|:---------:|:----------:|:-----------:|:---:|:-----------:|-------------------|
| JPEG | SOF marker | EXIF | ✓ | ✓ | EXIF/IPTC/XMP | APP0/APP1/APP13 segments |
| PNG  | IHDR | `tIME` | via `eXIf` | via `eXIf` | `tEXt`/`iTXt` | ancillary chunks |
| GIF  | LSD | ✗ | ✗ | ✗ | Comment ext | extension blocks |
| BMP  | DIB header | ✗ | ✗ | ✗ | ✗ | fixed header fields |

This is why scorpion promises **EXIF for JPEG** and only **basic attributes**
for the rest: PNG *can* carry EXIF but usually doesn't, and GIF/BMP have no
descriptive-metadata fields to speak of.
