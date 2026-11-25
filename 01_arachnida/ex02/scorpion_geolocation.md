# scorpion — geolocation implementation

How `scorpion` turns the GPS data embedded in a photo's EXIF into a human
`Coordinates` + `Location` line. The feature is a small pipeline of five steps,
each a dedicated function.

```
EXIF GPS IFD
   │  (raw degrees/minutes/seconds + N/S/E/W refs)
   ▼
clean_ref()        normalise the hemisphere reference
   ▼
dms_to_decimal()   D/M/S  ->  signed decimal degrees
   ▼
gps_to_latlon()    validate the fix, return (lat, lon) or None
   ▼
reverse_geocode()  (lat, lon) -> "City, Region, Country"
   ▼
print_gps_location()   print Coordinates and (if resolved) Location
```

---

## Prerequisites

- **Pillow** decodes the EXIF and exposes the GPS IFD (see *Step 1*).
- **reverse_geocoder** — offline nearest-city lookup from a coordinate.
- **pycountry** — ISO country-code → country-name lookup.

```
reverse_geocoder
pycountry
```

Both geocoding packages are optional in spirit — if the lookup fails the program
still prints the decimal coordinates — but this build imports them at startup,
so they must be installed.

---

## Step 1 — Locating GPS data inside EXIF

EXIF is a tree of IFDs (Image File Directories). The GPS data lives in its own
sub-directory, reached through a **pointer tag** in the main directory:

```python
GPS_IFD_TAG = 0x8825        # IFD0 tag whose value points to the GPS IFD
```

`print_exif` follows that pointer with Pillow's `get_ifd`:

```python
gps_ifd = exif.get_ifd(GPS_IFD_TAG)
```

The result, `gps_ifd`, is a dict keyed by the GPS IFD's own tag numbers (a
namespace separate from IFD0):

```python
GPS_LAT_REF = 1     # 'N' / 'S'
GPS_LAT     = 2     # (degrees, minutes, seconds)
GPS_LON_REF = 3     # 'E' / 'W'
GPS_LON     = 4     # (degrees, minutes, seconds)
```

So latitude is stored as **two** entries — a magnitude (`GPSLatitude`, a D/M/S
triple) and a hemisphere letter (`GPSLatitudeRef`) that carries the sign. Same
for longitude.

---

## Step 2 — Normalising the reference (`clean_ref`)

The hemisphere reference can arrive as `bytes` or `str`, and phone firmware
often NUL-pads fixed-width fields. `clean_ref` reduces it to a clean letter:

```python
def clean_ref(ref):
    if isinstance(ref, bytes):
        ref = ref.decode(errors="ignore")
    return (ref or "").replace("\x00", "").strip().upper()
```

It returns `"N"`, `"S"`, `"E"`, `"W"`, or `""` when the field is absent or
padding-only — the `""` is what later lets the code detect "no real fix."

---

## Step 3 — Degrees/minutes/seconds → decimal (`dms_to_decimal`)

EXIF stores a coordinate as three rational numbers (degrees, minutes, seconds).
Converting to the decimal degrees a map expects is unit arithmetic, with the
hemisphere deciding the sign:

```python
def dms_to_decimal(dms, ref):
    deg, minutes, sec = (float(x) for x in dms)
    dec = deg + minutes / 60 + sec / 3600
    if clean_ref(ref) in ("S", "W"):
        dec = -dec
    return dec
```

- `degrees + minutes/60 + seconds/3600` collapses the triple to one number.
- Southern/Western hemispheres are negative, so `S` and `W` flip the sign.
- `float(x)` is needed because Pillow yields the values as rational objects
  (`IFDRational`), not plain numbers.

Example: `N 48° 51′ 29.74″` → `48 + 51/60 + 29.74/3600 = 48.858261`.

---

## Step 4 — Validating the fix (`gps_to_latlon`)

A GPS IFD being *present* does not mean the photo has a real location. A phone
with location services off still writes the block, but with empty refs and
`0/0` rationals that decode to `NaN`. This function rejects those rather than
emitting a nonsense coordinate:

```python
def gps_to_latlon(gps_ifd):
    rlat = clean_ref(gps_ifd.get(GPS_LAT_REF))
    rlon = clean_ref(gps_ifd.get(GPS_LON_REF))
    if rlat not in ("N", "S") or rlon not in ("E", "W"):
        return None                                   # no/invalid hemisphere
    try:
        lat = dms_to_decimal(gps_ifd[GPS_LAT], rlat)
        lon = dms_to_decimal(gps_ifd[GPS_LON], rlon)
    except (KeyError, TypeError, ValueError, ZeroDivisionError):
        return None                                   # missing/garbage values
    if not (math.isfinite(lat) and math.isfinite(lon)):
        return None                                   # NaN/inf -> no real fix
    return lat, lon
```

Three gates: valid hemisphere letters, no exception while converting, and a
**finite** result (`math.isfinite` filters out `NaN`/`inf`). Only a coordinate
that passes all three comes back as `(lat, lon)`; otherwise `None`.

---

## Step 5 — Coordinate → place (`reverse_geocode`)

With a real coordinate in hand, the last step names it:

```python
def reverse_geocode(lat, lon):
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
```

- **`_rg.search((lat, lon), mode=1)`** returns a list of result dicts (one per
  queried point); `[0]` takes ours. Each hit has `name` (city), `admin1`
  (region), `admin2` (county) and `cc` (ISO alpha-2 country code). `mode=1`
  forces the single-threaded path — the default `mode=2` spawns processes, which
  is overkill for one lookup.
- **`os.devnull` + `redirect_stdout`** silence the one-time
  `Loading formatted geocoded file...` banner that `reverse_geocoder` prints to
  stdout on its first search, so it can't pollute scorpion's report. The
  redirect wraps only the `search()` call and is undone automatically on block
  exit.
- **`pycountry.countries.get(alpha_2=hit["cc"])`** turns `"FR"` into a country
  record whose `.name` is `"France"`; falls back to the raw code if unknown.
- **`place`** joins city and region, dropping any empty field, then the result
  is `"City, Region, Country"` (or just the country if the local parts are
  blank).
- The broad **`except Exception: return None`** keeps a geocoding hiccup from
  breaking the report — the location line is a nice-to-have, so on any failure
  the program falls back to coordinates only.

---

## Step 6 — Printing (`print_gps_location`)

The orchestrator ties it together and decides what actually appears:

```python
def print_gps_location(gps_ifd):
    latlon = gps_to_latlon(gps_ifd)
    if not latlon:
        return                                        # no valid fix -> print nothing
    lat, lon = latlon
    print(f"      {'Coordinates':<22}: {lat:.6f}, {lon:.6f}")
    where = reverse_geocode(lat, lon)
    if where:
        print(f"      {'Location':<22}: {where}")
```

- No valid fix → neither line is printed (the raw GPS tags still show above,
  from `print_ifd`).
- A valid fix → the `Coordinates` line always prints; the `Location` line prints
  only if reverse geocoding succeeded.

---

## Output example

```
    [GPS IFD]
      GPSLatitudeRef        : 'N'
      GPSLatitude           : (48.0, 51.0, 29.74)
      GPSLongitudeRef       : 'E'
      GPSLongitude          : (2.0, 17.0, 40.2)
      Coordinates           : 48.858261, 2.294500
      Location              : Vanves, Ile-de-France, France
```

---

## Limitations & notes

- **`Location` is approximate.** `reverse_geocoder` snaps to the nearest city in
  its offline dataset, so it names a place *near* the coordinate (here, the Paris
  suburb *Vanves*), not the exact landmark.
- **First lookup is slower.** `reverse_geocoder` loads its dataset on the first
  `search()` of a run; subsequent lookups are fast.
- **No-fix GPS is suppressed by design.** A GPS IFD full of zeros/NaN yields no
  `Coordinates`/`Location` line, so the tool never invents a location.
- **Precision.** Coordinates are printed to 6 decimal places (~0.1 m), far finer
  than the dataset's city-level resolution — the extra digits reflect the EXIF
  value, not the geocoding accuracy.
