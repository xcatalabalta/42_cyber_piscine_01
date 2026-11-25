# GIF metadata byte structure (+ PIL)

GIF is metadata-poor: no camera, author, date, or GPS fields. What it has lives
in **extension blocks** — chiefly a free-text *Comment Extension* and an
*Application Extension* (famously used for the animation loop count).

See `metadata_bytes_00_overview.md` for shared primitives. GIF integers are
**little-endian**.

---

## Basic attributes: format + dimensions

scorpion reports the **format** and **dimensions** from the bytes; file size and
timestamps come from `os.stat` (the filesystem layer).

### Format detection (signature)

GIF opens with 6 ASCII bytes carrying the version:

| index | bytes | meaning |
|-------|-------|---------|
| `data[0:3]` | `GIF` | magic |
| `data[3:6]` | `87a` or `89a` | version |

```python
if data[:6] in (b"GIF87a", b"GIF89a"):
    fmt = "GIF"
```

`data[:6]` is a 6-byte **slice** (indices 0..5, end exclusive) compared to
`bytes` literals — not individual bytes, which would be ints (see the overview's
"slice vs index").

### Dimensions

The canvas size sits in the Logical Screen Descriptor, **immediately after** the
6-byte signature (no chunk header precedes it, so the offsets are fixed), as
**little-endian** uint16 at offsets 6 and 8:

```
w, h = struct.unpack("<HH", data[6:10])
```

### In PIL

`img.format` (`"GIF"`), `img.size`, `img.mode`; frame count via `img.n_frames`;
file size/timestamps from `os.stat(path)`.

---

## Part A — General theory (byte structure)

### A.1 Header + Logical Screen Descriptor

```
+0  6  signature "GIF87a" or "GIF89a"
+6  2  canvas width   (little-endian uint16)
+8  2  canvas height  (little-endian uint16)
+10 1  packed fields  (bit 7 = Global Color Table present; bits 0-2 = its size)
+11 1  background color index
+12 1  pixel aspect ratio
```

If the Global Color Table (GCT) flag is set, a GCT of `3 * 2^(size+1)` bytes
follows immediately, before the first block.

### A.2 Block structure

After the header/LSD (+GCT), the file is a stream of blocks. The **first byte of
each block** selects its type:

| Intro byte | Block |
|-----------|-------|
| `0x2C` | Image Descriptor (a frame) |
| `0x21` | Extension Introducer — next byte is the **label** |
| `0x3B` | Trailer (end of file) |

For `0x21` extensions, the **label** byte says which:

| Label | Extension |
|-------|-----------|
| `0xF9` | Graphic Control (per-frame delay, transparency) |
| `0xFE` | **Comment** |
| `0x01` | Plain Text |
| `0xFF` | **Application** |

**Sub-blocks**: extension and image data are stored as a chain of sub-blocks,
each `length(1) + length bytes`, terminated by a **zero-length** sub-block
(`0x00`). A parser reads `n = data[pos]`; if `n == 0` the chain ends, else it
consumes `n` bytes and repeats.

### A.3 The metadata-bearing extensions

**Comment Extension** — `21 FE` then sub-blocks of (Latin-1/ASCII) text,
terminated by `00`. Concatenate the sub-blocks to get the comment.

**Application Extension** — `21 FF`, then a sub-block of exactly **11 bytes**
(8-byte application identifier + 3-byte auth code), then app-specific
sub-blocks, terminated by `00`. The classic example is
`NETSCAPE2.0` followed by `03 01 <loop-count:2 LE> 00`, which encodes the
animation **loop count**.

**Graphic Control Extension** — `21 F9`, a 4-byte block carrying the frame
**delay** (2 bytes LE, in 1/100 s) and transparency info. This is animation
timing, not descriptive metadata, but it's what a tool reports as per-frame
duration.

### A.4 Raw parse, end to end

```
1. check signature GIF87a/GIF89a; read width/height at +6/+8 (LE)
2. if GCT flag set, skip 3 * 2^(size+1) bytes
3. pos at first block; loop:
     b = data[pos]
     if b == 0x3B: break                      # trailer
     if b == 0x2C: skip image descriptor + its sub-blocks (pixel data)
     if b == 0x21:
         label = data[pos+1]
         if label == 0xFE: comment = read_subblocks(pos+2)   # Comment
         if label == 0xFF: appid   = data[pos+3:pos+14]; ... # Application
         if label == 0xF9: delay   = uint16_le(...)          # Graphic Control
         advance past this extension's sub-blocks
```

`read_subblocks` repeatedly reads `n = data[p]; p += 1`, appends `data[p:p+n]`,
`p += n`, until `n == 0`.

---

## Part B — Application to PIL

### B.1 Comment + animation info via `img.info`

```python
with Image.open(path) as img:
    img.info.get("comment")     # bytes from the Comment Extension
    img.info.get("duration")    # frame delay in ms (from Graphic Control)
    img.info.get("loop")        # loop count (from the NETSCAPE Application ext)
    img.info.get("background")  # background color index (from the LSD)
    img.info.get("version")     # b"GIF87a" / b"GIF89a"
```

Observed keys on a 2-frame GIF written with a comment:
`['background', 'comment', 'duration', 'extension', 'loop', 'version']`, with
`img.info["comment"] == b"made by scorpion"`.

### B.2 Frames

```python
img.n_frames        # number of frames
img.seek(n)         # move to frame n (then read img.info["duration"], etc.)
```

### B.3 No EXIF

`img.getexif()` is empty for GIF — there is no EXIF/TIFF tree in the format. A
metadata tool shows only the comment/animation fields plus the filesystem
attributes.

### B.4 Mapping summary

| Byte structure (Part A) | PIL access |
|-------------------------|------------|
| Comment Extension `21 FE` | `img.info["comment"]` |
| Graphic Control delay | `img.info["duration"]` |
| NETSCAPE loop count | `img.info["loop"]` |
| LSD background / size | `img.info["background"]`, `img.size` |
| frames | `img.n_frames`, `img.seek()` |

---

## Sources

- **GIF89a specification** (CompuServe, 1990) — header, Logical Screen
  Descriptor, extension blocks and sub-block chaining.
