# BMP metadata byte structure (+ PIL)

BMP is the simplest case: it is a **raw-pixel container with almost no
metadata**. There is no EXIF, no author/date, no GPS, no comment field. The only
"metadata-ish" values are the dimensions, bit depth, compression, and a physical
resolution — all at **fixed offsets** in two headers.

See `metadata_bytes_00_overview.md` for shared primitives. BMP integers are
**little-endian**.

---

## Basic attributes: format + dimensions

For BMP the basic attributes are essentially *all* there is — the format has no
descriptive metadata. They are read from the bytes; file size and timestamps
come from `os.stat` (the filesystem layer).

### Format detection (signature)

| index | byte | hex | meaning |
|-------|------|-----|---------|
| `data[0]` | `B` | 0x42 | "BM" magic |
| `data[1]` | `M` | 0x4D | |

```python
if data[:2] == b"BM":
    fmt = "BMP"
```

`data[:2]` is a 2-byte **slice** compared to a `bytes` literal. BMP's 2-byte
signature is the weakest of the four formats (two ASCII letters), but it is
compared the same way — never as single `int` bytes.

### Dimensions

Width and height live in the DIB header as **signed little-endian** int32 at
offsets **18** and **22**. A negative height means the rows are stored
top-to-bottom, so take the absolute value:

```
w, h = struct.unpack("<ii", data[18:26])
h = abs(h)
```

Bits-per-pixel is at +28; compression at +30.

### In PIL

`img.format` (`"BMP"`), `img.size`, `img.mode`; file size/timestamps from
`os.stat(path)`.

---

## Part A — General theory (byte structure)

### A.1 BITMAPFILEHEADER (14 bytes)

```
+0  2  signature "BM"            (0x42 0x4D)
+2  4  file size                 (little-endian uint32)
+6  4  reserved
+10 4  offset to pixel data      (little-endian uint32)
```

### A.2 DIB header — BITMAPINFOHEADER (40 bytes, the common one)

Starts at offset **14**:

```
+14 4  header size               (40 for BITMAPINFOHEADER)
+18 4  width                     (little-endian  int32, SIGNED)
+22 4  height                    (little-endian  int32, SIGNED; negative = top-down rows)
+26 2  color planes              (always 1)
+28 2  bits per pixel
+30 4  compression method
+34 4  image size (pixel bytes)
+38 4  horizontal resolution     (pixels per metre, little-endian int32)
+42 4  vertical resolution       (pixels per metre)
+46 4  colors in palette
+50 4  important colors
```

### A.3 What counts as "metadata" here

Only these, and they are structural rather than descriptive:

- **Dimensions** — width at +18, height at +22 (take `abs(height)`; a negative
  height means rows are stored top-to-bottom).
- **Bit depth** — +28.
- **Compression** — +30 (usually 0 = BI_RGB, uncompressed).
- **Resolution** — +38/+42 in **pixels per metre**; divide by ~39.37 to get DPI.

There is nothing else to extract. A raw BMP handler is just a handful of
fixed-offset reads.

### A.4 Raw parse, end to end

```
1. check "BM" at +0
2. width  = int32_le(data[18:22])
   height = abs(int32_le(data[22:26]))
   bpp    = uint16_le(data[28:30])
   comp   = uint32_le(data[30:34])
   xppm   = int32_le(data[38:42]); dpi_x = round(xppm / 39.3701)
3. done — no further metadata exists
```

(Other DIB header variants exist — BITMAPV4HEADER/V5HEADER add colour-space and,
in V5, an ICC profile — but the common BMP has only BITMAPINFOHEADER.)

---

## Part B — Application to PIL

### B.1 Everything via `img` + `img.info`

```python
with Image.open(path) as img:
    img.format                      # "BMP"
    img.size                        # (width, height)
    img.mode                        # "RGB", "P", "1", ...
    img.info.get("dpi")             # derived from the ppm resolution fields
    img.info.get("compression")     # compression method
```

Observed `img.info` keys for a plain BMP: `['compression', 'dpi']` — that's the
whole story.

### B.2 No EXIF, no text

```python
len(img.getexif())      # 0  -- BMP has no EXIF tree
# no img.text, no comment field
```

### B.3 Mapping summary

| Byte structure (Part A) | PIL access |
|-------------------------|------------|
| width/height (DIB +18/+22) | `img.size` |
| bits per pixel (+28) | `img.mode` (indirectly) |
| compression (+30) | `img.info["compression"]` |
| resolution ppm (+38/+42) | `img.info["dpi"]` |
| (nothing descriptive) | — |

For scorpion this is why BMP shows **basic attributes only**: there is no
embedded metadata to report beyond what `img.size` / `img.info` already give.

---

## Sources

- **Microsoft `BITMAPFILEHEADER` / `BITMAPINFOHEADER`** (Windows GDI reference).
- **Wikipedia "BMP file format"** — a consolidated offset table of all header
  variants.
