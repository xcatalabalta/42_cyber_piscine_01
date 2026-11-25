# PNG metadata byte structure (+ PIL)

PNG keeps metadata in optional **ancillary chunks**. The structure is very
regular, which makes it one of the easiest formats to hand-parse.

See `metadata_bytes_00_overview.md` for shared primitives. PNG integers are
**big-endian**.

---

## Basic attributes: format + dimensions

scorpion reports the **format** and **dimensions** from the bytes; file size and
timestamps come from `os.stat` (the filesystem layer).

### Format detection (signature)

PNG opens with an 8-byte signature. Because Python slices are **0-based** and
the end index is **exclusive**, `data[:8]` is positions 0..7:

| index | byte | hex | dec | role |
|-------|------|-----|-----|------|
| `data[0]` | `\x89` | 0x89 | 137 | high-bit byte — detects 7-bit-stripping channels |
| `data[1]` | `P` | 0x50 | 80 | "PNG" in ASCII |
| `data[2]` | `N` | 0x4E | 78 | |
| `data[3]` | `G` | 0x47 | 71 | |
| `data[4]` | `\r` | 0x0D | 13 | CR (the CRLF-corruption trap) |
| `data[5]` | `\n` | 0x0A | 10 | LF |
| `data[6]` | `\x1a` | 0x1A | 26 | Ctrl-Z (stops DOS `type`) |
| `data[7]` | `\n` | 0x0A | 10 | LF |

```python
if data[:8] == b"\x89PNG\r\n\x1a\n":
    fmt = "PNG"
```

Two byte-access facts in play: `data[:8]` is a **slice** → a `bytes` object you
compare to the literal; a single `data[0]` would instead be the **int** `137`
(`0x89`), so `data[0] == 0x89` is `True` but `data[0] == b"\x89"` is `False`.

### Dimensions

Width and height are the first two fields of the IHDR chunk's data, each a
**big-endian** uint32, at absolute offsets **16** and **20**:

```
offset 16 = 8 (signature) + 4 (IHDR length) + 4 (type "IHDR")
w, h = struct.unpack(">II", data[16:24])
```

Bit depth is at +24, colour type at +25 (which maps to the "mode").

### In PIL

`img.format` (`"PNG"`), `img.size`, `img.mode`; file size/timestamps from
`os.stat(path)`.

---

## Part A — General theory (byte structure)

### A.1 Signature + chunk framing

```
offset 0: 8-byte signature  89 50 4E 47 0D 0A 1A 0A

then a series of chunks, each:
  +0  4  Length   (big-endian uint32 = size of Data only)
  +4  4  Type     (4 ASCII letters, e.g. "IHDR", "tEXt")
  +8  L  Data     (Length bytes)
  +8+L 4 CRC      (covers Type + Data)
```

So a raw parser walks chunks with `pos += 12 + length` (the 12 = 4 length + 4
type + 4 CRC). `IHDR` is always first; `IEND` marks the end.

**Critical vs ancillary** is encoded in the **case of the first letter** of the
type: uppercase = critical (IHDR, PLTE, IDAT, IEND), lowercase = ancillary
(tEXt, tIME, pHYs, eXIf, …). Metadata lives in ancillary chunks, which a decoder
may skip — perfect for a metadata tool to pick out.

### A.2 Dimensions (for completeness)

`IHDR`'s data begins with width (4) then height (4), both big-endian — at
absolute file offsets **16** and **20** (8 signature + 4 length + 4 type). See
the overview / separate formats reference.

### A.3 The metadata chunks

**Text chunks** — keyword/value pairs (Title, Author, Description, Copyright,
Creation Time, Software, Comment, …):

- `tEXt` — `keyword \0 text`, both Latin-1, uncompressed.
- `zTXt` — `keyword \0 method(1) compressed-text`; text is zlib-compressed.
- `iTXt` — UTF-8 and international:
  `keyword \0 compression-flag(1) compression-method(1) language-tag \0 translated-keyword \0 text`

**`tIME`** — last-modification time, 7 bytes:
`year(2, big-endian) month(1) day(1) hour(1) minute(1) second(1)`.

**`pHYs`** — physical pixel size: `x-ppu(4) y-ppu(4) unit(1)` (unit 1 = metres);
this is where "DPI" comes from.

**`eXIf`** — (added to the spec in 2017) a **complete TIFF/EXIF block**, byte-for-byte
the same structure as inside a JPEG's APP1 (minus the `Exif\0\0` prefix). So PNG
*can* carry the full camera/GPS EXIF tree — parse it exactly as in
`metadata_bytes_jpeg.md` Part A.2.

**`iCCP`** — embedded ICC colour profile (`name \0 method(1) compressed-profile`).

### A.4 Raw parse, end to end

```
1. check 8-byte signature
2. pos = 8
3. while pos + 12 <= len(data):
       length = uint32_be(data[pos:pos+4])
       ctype  = data[pos+4:pos+8]          # e.g. b"tEXt"
       cdata  = data[pos+8:pos+8+length]
       if   ctype == b"IHDR": width,height = uint32_be(cdata[0:4]), uint32_be(cdata[4:8])
       elif ctype in (b"tEXt",b"zTXt",b"iTXt"): parse_text(ctype, cdata)
       elif ctype == b"tIME": year=uint16_be(cdata[0:2]); month,day,... = cdata[2:7]
       elif ctype == b"eXIf": parse_tiff(cdata)      # same as JPEG EXIF
       elif ctype == b"IEND": break
       pos += 12 + length                   # 4 len + 4 type + Length + 4 CRC
```

Bounds-check `length` against the remaining bytes before slicing.

---

## Part B — Application to PIL

### B.1 Text chunks

```python
with Image.open(path) as img:
    img.text          # dict of tEXt/zTXt/iTXt as {keyword: text}
```

Observed on a sample written with `Author`/`Comment` text chunks:
`img.text == {'Author': 'Xavier', 'Comment': 'hello'}`. Pillow decompresses
`zTXt`/`iTXt` for you.

### B.2 pHYs / iCCP via `img.info`

```python
img.info.get("dpi")            # from pHYs, e.g. (72.0, 72.0)
img.info.get("icc_profile")    # from iCCP, raw bytes
img.info.get("transparency")   # tRNS (image data, not really metadata)
```

### B.3 eXIf

```python
exif = img.getexif()           # reads a PNG eXIf chunk if present (Pillow >= 6)
```

`getexif()` works on PNG too — it returns the eXIf TIFF tree, and `get_ifd(...)`
descends into Exif/GPS just like for JPEG. On a PNG with **no** eXIf chunk,
`len(img.getexif()) == 0` (as scorpion observes for ordinary PNGs).

### B.4 Mapping summary

| Byte structure (Part A) | PIL access |
|-------------------------|------------|
| `tEXt` / `zTXt` / `iTXt` chunks | `img.text` (dict) |
| `pHYs` | `img.info["dpi"]` |
| `iCCP` | `img.info["icc_profile"]` |
| `tIME` | (not surfaced directly; read the chunk raw) |
| `eXIf` | `img.getexif()` + `get_ifd(...)` |
| `IHDR` width/height | `img.size` |

Note scorpion's `has_metadata()` checks `img.text` and `img.getexif()` for PNG —
exactly the Part A text and eXIf chunks.

---

## Sources

- **RFC 2083** / **W3C PNG (3rd ed.)** — signature, chunk layout, IHDR, text and
  time chunks.
- **PNG eXIf registration (2017)** — the embedded EXIF/TIFF chunk.
