# `spider.c` — URL resolution and normalization

Every reference the scanner pulls out of a page (`src`, `href`, `srcset`) is
*raw* — it might be a full URL, or `/logo.png`, or `../img/a.jpg`, or
`photo.png`. Before the spider can fetch it, dedupe it, or compare its host, it
has to become one thing: an **absolute, canonical URL**. Two functions do that:

- `resolve_url(base, ref)` — join a raw reference onto the page it was found on,
  producing an absolute URL.
- `normalize_in_place(url)` — collapse `.` and `..` path segments so two URLs
  that mean the same file become the same string.

A browser does this for you automatically. In C you do it by hand, and getting
it right is what makes recursion and dedup reliable.

---

## 1. Why it matters: dedup is string comparison

The visited-pages set and downloaded-images set are just lists of strings
compared with `strcmp`:

```c
static int sl_contains(const strlist *l, const char *s) {
    for (size_t i = 0; i < l->count; i++)
        if (strcmp(l->items[i], s) == 0) return 1;
    return 0;
}
```

That means two URLs are "the same" only if they're **byte-for-byte identical**.
So `http://x/a/./b.png` and `http://x/a/b.png` — the same file — would be treated
as two different images and downloaded twice, and `http://x/p` crawled via one
link and `http://x/./p` via another would be visited twice. Normalization exists
to make equal URLs *look* equal, so the string-compare dedup actually works.

---

## 2. Anatomy of a URL

```
   https://host.example:443/dir/page.html?q=1#section
   └─┬─┘   └──────┬─────┘  └──────┬──────┘ └┬─┘ └──┬──┘
   scheme       host(:port)      path      query  fragment
```

The pieces the code cares about:

- **scheme** — `http` or `https` (everything before `://`).
- **host** — up to the next `/` (or end). The same-host check compares this.
- **path** — the `/dir/page.html` part; this is what gets normalized.
- **query** — `?q=1`; kept, but not part of the path.
- **fragment** — `#section`; a client-side anchor, **dropped entirely** (it
  never changes what you fetch).

---

## 3. `resolve_url` — making a reference absolute

```c
static char *resolve_url(const char *base, const char *ref) {
    size_t rl = strcspn(ref, "#");       /* (0) drop the fragment */
    char *r = strndup(ref, rl);
    char *res = NULL;
    ...
    free(r);
    normalize_in_place(res);             /* (5) collapse ./.. */
    /* (6) host-only -> add "/" */
    return res;
}
```

### Step 0 — drop the fragment

`strcspn(ref, "#")` gives the length of `ref` before any `#`; `strndup` copies
just that part. So `photo.png#top` becomes `photo.png` before anything else
happens. The fragment is meaningless for fetching, so it's gone immediately.

Then the reference falls into exactly one of **four shapes**, tested in order.

### Case 1 — already absolute

```c
if (!strncmp(r, "http://", 7) || !strncmp(r, "https://", 8)) {
    res = strdup(r);                     /* nothing to join; copy as-is */
}
```

`ref = "https://cdn.site/x.png"` → used verbatim. `base` is irrelevant.

### Case 2 — protocol-relative (`//host/...`)

```c
} else if (!strncmp(r, "//", 2)) {
    size_t sl = strcspn(base, ":");      /* length of base's scheme */
    res = malloc(sl + strlen(r) + 2);
    sprintf(res, "%.*s:%s", (int)sl, base, r);   /* scheme + ":" + ref */
}
```

A `//` reference means "same scheme as the current page, this host." The code
takes the scheme from `base` (everything up to the first `:`) and prepends it.

`base = "https://a.com/p"`, `ref = "//cdn.com/x.png"` → `https://cdn.com/x.png`.
(`%.*s` prints `sl` bytes of `base`, i.e. `https`.)

### Case 3 — root-relative (`/path`)

```c
} else if (r[0] == '/') {
    const char *p = strstr(base, "://");
    p += 3;                              /* skip past "://" */
    const char *slash = strchr(p, '/');  /* first '/' after the host */
    size_t prefix = slash ? (size_t)(slash - base) : strlen(base);
    res = malloc(prefix + strlen(r) + 1);
    sprintf(res, "%.*s%s", (int)prefix, base, r);  /* scheme://host + ref */
}
```

A leading `/` means "from the site root." The code keeps `base`'s
`scheme://host` and replaces the whole path with `ref`.

`base = "https://a.com/dir/page.html"`, `ref = "/img/a.png"` →
`https://a.com/img/a.png`. (`prefix` is the length up to the first `/` after the
host, i.e. `https://a.com`.)

### Case 4 — path-relative (`photo.png`, `../img/a.png`)

The hardest case: the reference is relative to the **directory** of the current
page.

```c
} else {
    const char *q = base + strcspn(base, "?#");   /* base without query/frag */
    const char *lastslash = NULL;
    for (const char *p = base; p < q; p++) if (*p == '/') lastslash = p;
    const char *afterscheme = strstr(base, "://");
    if (afterscheme && lastslash && lastslash < afterscheme + 3)
        lastslash = NULL;                /* the only '/'s were in "://" */
    size_t prefix;
    if (lastslash) prefix = lastslash - base + 1;  /* keep through last '/' */
    else           prefix = q - base;              /* base has no path slash */
    res = malloc(prefix + strlen(r) + 2);
    if (lastslash) sprintf(res, "%.*s%s", (int)prefix, base, r);
    else           sprintf(res, "%.*s/%s", (int)prefix, base, r);
}
```

The logic: take `base` up to and **including its last `/`** (that's the current
directory), then append `ref`.

`base = "https://a.com/dir/page.html"`, `ref = "photo.png"` →
`https://a.com/dir/photo.png` (the last `/` is before `page.html`, so the
directory is `/dir/`).

The `afterscheme` guard handles the tricky case where the *only* slashes in
`base` are the `//` in `https://`. For `base = "https://a.com"` (no path),
without the guard the "last slash" would be the second `/` of `://`, giving
nonsense. The guard notices `lastslash` is inside `://` and sets it to `NULL`, so
the code takes the other branch and inserts a `/` itself:
`https://a.com` + `/` + `photo.png` → `https://a.com/photo.png`.

### Steps 5 & 6 — normalize, then fix host-only

After building `res`, `resolve_url` calls `normalize_in_place` (next section),
then canonicalizes a host-only result to end in `/`:

```c
char *afterhost = strstr(res, "://");
if (afterhost && !strchr(afterhost + 3, '/')) {   /* no '/' after host */
    size_t rl2 = strlen(res);
    char *r2 = realloc(res, rl2 + 2);
    if (r2) { res = r2; res[rl2] = '/'; res[rl2 + 1] = '\0'; }
}
```

This is the slash fix: `https://a.com` and `https://a.com/` must be the same
string, or the dedup set treats them as two pages (which wasted a recursion
level — see the crawling doc).

---

## 4. `normalize_in_place` — collapsing `.` and `..`

Relative references routinely produce paths with `.` (current directory) and
`..` (parent directory): `../img/a.png` resolved against `/dir/sub/page.html`
first becomes `/dir/sub/../img/a.png`, which *means* `/dir/img/a.png`. Until
those are collapsed, dedup can't tell it from any other spelling of the same
file.

```c
static void normalize_in_place(char *url) {
    char *p = strstr(url, "://");
    if (!p) return;
    p += 3;
    char *path = strchr(p, '/');         /* start of the path */
    if (!path) return;                   /* host-only URL: nothing to do */

    char qsave[2048] = "";               /* (1) save & detach the query */
    char *query = strpbrk(path, "?#");
    if (query) { snprintf(qsave, sizeof(qsave), "%s", query); *query = '\0'; }

    char *tmp = strdup(path);            /* (2) tokenize a COPY */
    char *segs[512];
    int n = 0;
    for (char *tok = strtok(tmp, "/"); tok; tok = strtok(NULL, "/")) {
        if (n >= 512) break;
        if (!strcmp(tok, "."))        continue;        /* drop "."       */
        else if (!strcmp(tok, "..")) { if (n > 0) n--; }/* pop on ".."   */
        else                          segs[n++] = tok;  /* keep normal seg*/
    }
    char rebuilt[2048];                  /* (3) rejoin the survivors */
    size_t off = 0;
    rebuilt[0] = '\0';
    for (int i = 0; i < n && off < sizeof(rebuilt); i++)
        off += snprintf(rebuilt + off, sizeof(rebuilt) - off, "/%s", segs[i]);
    if (n == 0) { rebuilt[0] = '/'; rebuilt[1] = '\0'; }

    strcpy(path, rebuilt);               /* (4) write back (only shrinks) */
    strcat(url, qsave);                  /*     reattach the query */
    free(tmp);
}
```

**The algorithm is a stack.** Walk the path segment by segment:

- `.` → skip it (current directory means "stay here").
- `..` → pop the last kept segment (go up a level); if the stack is empty, ignore
  it (you can't go above the root).
- anything else → push it.

Then glue the surviving segments back with `/`.

Worked example — path `/a/b/../c/./d.png`:

```
segment   action         stack
  a       push            [a]
  b       push            [a, b]
  ..      pop             [a]
  c       push            [a, c]
  .       skip            [a, c]
  d.png   push            [a, c, d.png]
rebuilt → /a/c/d.png
```

**Four details that matter:**

1. **The query is detached first and reattached last.** `?a=/b` could contain a
   `/`, which would wreck the segment logic — so `strpbrk(path, "?#")` splits it
   off into `qsave`, normalization runs on the path alone, and `strcat` puts it
   back.
2. **`strtok` runs on a `strdup`'d copy (`tmp`), never the original.** `strtok`
   overwrites its input with `'\0'`s; the real `url` must stay intact until the
   rebuilt path is ready to overwrite it.
3. **It only ever shrinks the path.** Dropping `.` and popping `..` can only make
   the path shorter or equal, so writing `rebuilt` back over `path` with `strcpy`
   can never overflow — a deliberate safety argument.
4. **Host-only URLs return early.** `strchr(p, '/')` is `NULL` when there's no
   path, so there's nothing to normalize (the `/`-appending happens back in
   `resolve_url`).

---

## 5. End-to-end examples

Base page: `https://a.com/dir/page.html`

| Raw reference        | Shape             | After resolve + normalize            |
|----------------------|-------------------|--------------------------------------|
| `https://b.com/x.png`| absolute          | `https://b.com/x.png`                |
| `//cdn.com/x.png`    | protocol-relative | `https://cdn.com/x.png`              |
| `/img/a.png`         | root-relative     | `https://a.com/img/a.png`            |
| `photo.png`          | path-relative     | `https://a.com/dir/photo.png`        |
| `../up.png`          | path-relative     | `https://a.com/up.png`               |
| `./here.png`         | path-relative     | `https://a.com/dir/here.png`         |
| `sub/x.png#top`      | path-rel + frag   | `https://a.com/dir/sub/x.png`        |
| `a/b/../../c.png`    | path-relative     | `https://a.com/dir/c.png`            |

Every one comes out as a single canonical string, so the dedup sets work and the
same-host check (`strcmp(host, g_host)`) is meaningful.

---

## 6. Gotchas and simplifications

- **This is a practical subset of RFC 3986, not the full spec.** It handles the
  four reference shapes real pages use and collapses `.`/`..`, which is plenty
  for the spider. It does **not** implement every corner of the standard.
- **It ignores `<base href>`.** A page can override the base URL for relative
  links with a `<base>` tag; this code always uses the page's own URL as the
  base. Rare in practice, worth knowing.
- **No host lowercasing, default-port or percent-encoding normalization.**
  `HTTP://A.COM` vs `http://a.com`, or `:80` vs none, would be treated as
  different. The spider doesn't need that because it resolves against a single
  consistent base.
- **`..` above the root is silently dropped** (`if (n > 0) n--`), matching what
  browsers and RFC 3986 do — you can't escape above the host.
- **Fixed buffers (`2048`, `segs[512]`).** These are guarded (`n >= 512` break,
  `off < sizeof(rebuilt)`), so a pathological path can't overflow them — it just
  gets truncated. Those guards were added after a real crash; see the git
  history / the crawling story.
- **`sprintf` is used, not `snprintf`** — safe *only* because each `res` is
  `malloc`'d to the exact computed length just above it. If you change the
  sizing, change the `sprintf` too, or switch to `snprintf`.

---

## 7. Build it yourself, in order

1. **Absolute passthrough.** Handle `http://`/`https://` refs (just copy). Test
   with an `<a href="http://other/x.png">`.
2. **Root-relative.** Handle `/path` by keeping `scheme://host`. Test with
   `/img/a.png`.
3. **Path-relative.** Handle `photo.png` and `sub/x.png` by keeping the current
   directory. This is the fiddly one — test every shape.
4. **Protocol-relative.** Handle `//host/...`.
5. **Fragment + query.** Strip `#...` from the ref up front; make sure `?query`
   survives.
6. **Normalize.** Add the `.`/`..` stack. Test `../up.png` and `a/b/../c.png`.
7. **Host-only slash.** Append `/` to a `scheme://host` result.

At each step, print the resolved URL and eyeball it against a page whose base you
know — the resolution table above is a ready-made set of test cases.
