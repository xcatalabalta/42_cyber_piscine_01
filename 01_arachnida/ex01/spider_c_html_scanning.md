# `spider.c` — HTML scanning

Once a page is fetched, the spider has to pull the image and link references out
of the markup. It does **not** build a real HTML parse tree — it scans the text
for the attributes it cares about (`src`, `href`, `srcset`) and reads their
values. This doc explains that scanning pipeline, the trade-offs of the
text-scan approach, and the guards that keep it from being fooled.

The pipeline inside `crawl`, once the HTML is in hand:

```
http_get → looks_html → strip_blocks → scan_attr("src")      → read_attr_value → handle_ref
                                      → scan_attr("href")     → read_attr_value → handle_ref
                                      → scan_attr("srcset")   → read_attr_value → handle_ref
```

---

## 1. The strategy: text scanning, not parsing

A browser builds a DOM tree; this spider doesn't. It treats the page as one long
string and repeatedly searches it for the literal text `src`, `href`, `srcset`,
then reads whatever value follows each. That's the whole idea.

Why do it this way? It's tiny, has no dependencies, and the subject only needs
three attributes. The cost is **imprecision** — the scanner sees `href=`
wherever it appears, with no idea which HTML *tag* it's in. Section 6 covers what
that costs and how the guards compensate.

The three passes, from `crawl`:

```c
scan_attr(clean, url, depth, "src",    0, 0);  /* images only  (can_recurse=0) */
scan_attr(clean, url, depth, "href",   0, 1);  /* links+images (can_recurse=1) */
scan_attr(clean, url, depth, "srcset", 1, 0);  /* images, srcset-split          */
```

Two flags ride along: `is_srcset` (the value is a space/comma list, not a single
URL) and `can_recurse` (may this reference be followed as a page? — only `href`
may).

---

## 2. `strip_blocks` — remove script/style first

Inline JavaScript is full of `.href=` and `.src=`:

```html
<script>document.location.href = "/somewhere"; el.src = data;</script>
```

A naive scan would mine those as if they were links. So **before** scanning, the
spider deletes the *contents* of every `<script>` and `<style>` block:

```c
static char *strip_blocks(const char *html)
{
    char *out = malloc(strlen(html) + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    const char *p = html;
    while (*p) {
        if (!strncasecmp(p, "<script", 7)) {
            const char *e = strcasestr(p, "</script>");
            if (e)                           /* jump past </script> (9 chars) */
                p = e + 9;
            else
                p = p + strlen(p);
            continue;
        }
        if (!strncasecmp(p, "<style", 6)) {
            const char *e = strcasestr(p, "</style>");
            if (e)                           /* jump past </style> (8 chars) */
                p = e + 8;
            else
                p = p + strlen(p);
            continue;
        }
        out[o++] = *p++;                     /* otherwise copy the byte */
    }
    out[o] = '\0';
    return out;
}
```

It copies the page byte by byte into a fresh buffer, but whenever it hits an
opening `<script`/`<style` it fast-forwards `p` past the matching close tag,
skipping everything between. The `9` and `8` are the lengths of `</script>` and
`</style>`. If a close tag is missing, it jumps to the end of the string
(`p + strlen(p)`), which is safe.

The result (`clean`) is HTML with the tag structure intact but the scripts and
styles emptied out — so the scanner can't be fooled by code.

---

## 3. `scan_attr` — find each attribute and dispatch its value

```c
static void scan_attr(const char *html, const char *base, int depth,
                      const char *attr, int is_srcset, int can_recurse)
{
    size_t alen = strlen(attr);
    const char *p = html;
    while ((p = strcasestr(p, attr)) != NULL) {   /* (a) next "src"/"href"... */
        const char *q = p + alen;
        while (*q && isspace((unsigned char)*q))  /* (b) skip spaces */
            q++;
        if (*q != '=') {                          /* (c) must be '=' */
            p += alen;
            continue;
        }
        q++;
        char *val = read_attr_value(&q);          /* (d) read value */
        if (is_srcset) {
            /* (e) split "a.jpg 1x, b.jpg 2x" -> first token of each part */
            char *comma;
            char *tok = val;
            while (tok) {
                comma = strchr(tok, ',');
                if (comma)
                    *comma = '\0';
                while (*tok && isspace((unsigned char)*tok))
                    tok++;
                char *sp = tok;
                while (*sp && !isspace((unsigned char)*sp))
                    sp++;
                *sp = '\0';
                if (*tok)
                    handle_ref(base, tok, depth, can_recurse);
                if (comma)
                    tok = comma + 1;
                else
                    tok = NULL;
            }
        }
        else if (*val) {
            handle_ref(base, val, depth, can_recurse);  /* (f) single value */
        }
        free(val);
        p = q;                                    /* (g) continue past it */
    }
}
```

Step by step:

- **(a)** `strcasestr(p, attr)` finds the next occurrence of the attribute name,
  case-insensitively — so `SRC`, `Src`, `src` all match.
- **(b)+(c)** Confirm it's really `attr=`: skip any spaces, then require a `=`.
  If the next non-space char isn't `=`, this was a false hit — advance and keep
  looking. This single check is what stops `src` inside `srcset` from matching
  (after the `src` of `srcset` comes `set`, not `=`).
- **(d)** `read_attr_value` reads whatever follows the `=` (next section).
- **(e)** For `srcset`, the value is a comma-separated list like
  `"a.jpg 1x, b.jpg 2x"`; the loop splits on commas and, for each candidate,
  keeps the **first whitespace-delimited token** (the URL), discarding the `1x`
  descriptor. Each URL goes to `handle_ref`.
- **(f)** For `src`/`href`, the value is one URL; hand it straight to
  `handle_ref`.
- **(g)** `p = q` moves past the value so the next loop iteration finds the
  *next* attribute, not the same one again.

`handle_ref` then decides download-or-recurse (see the crawling doc); scanning's
only job is to produce clean reference strings.

---

## 4. `read_attr_value` — read one quoted or unquoted value

```c
static char *read_attr_value(const char **pp)
{
    const char *p = *pp;
    while (*p && isspace((unsigned char)*p))         /* skip spaces after '=' */
        p++;
    char quote = 0;
    if (*p == '"' || *p == '\'') {                   /* opening quote? */
        quote = *p;
        p++;
    }
    const char *start = p;
    if (quote)
        while (*p && *p != quote)                    /* quoted: until same quote */
            p++;
    else
        while (*p && !isspace((unsigned char)*p) && *p != '>')  /* bare */
            p++;
    char *val = strndup(start, p - start);           /* copy the value out */
    if (*p)                                          /* advance caller's cursor */
        *pp = p + 1;
    else
        *pp = p;
    return val;
}
```

It handles the three ways HTML writes an attribute value:

- `href="/a.png"` — double-quoted: read until the closing `"`.
- `href='/a.png'` — single-quoted: read until the closing `'`.
- `href=/a.png` — unquoted: read until whitespace or `>`.

The quote character it opened with (`"` or `'`) is remembered in `quote` so the
matching close is found. `strndup(start, p - start)` copies exactly the value
(the `end - start` slice idiom from the functions reference). The `**pp`
double-pointer lets the function **advance the caller's cursor** past the value —
that's how `scan_attr`'s `p = q` ends up pointing after the value.

The returned string is heap memory; `scan_attr` `free`s it after dispatching.

---

## 5. Worked example

Input (already `strip_blocks`-cleaned):

```html
<img src="img/a.png">
<picture><source srcset="img/b.png 1x, img/c.png 2x"></picture>
<a href='/page2.html'>next</a>
```

**Pass 1 — `scan_attr("src", is_srcset=0, can_recurse=0)`**
- finds `src="img/a.png"` → value `img/a.png` → `handle_ref(... , can_recurse=0)`
- finds the `src` inside `srcset` → next char is `s`, not `=` → rejected
- → one image candidate: `img/a.png`

**Pass 2 — `scan_attr("href", is_srcset=0, can_recurse=1)`**
- finds `href='/page2.html'` → value `/page2.html` → `handle_ref(..., can_recurse=1)`
- → one link (recursable): `/page2.html`

**Pass 3 — `scan_attr("srcset", is_srcset=1, can_recurse=0)`**
- finds `srcset="img/b.png 1x, img/c.png 2x"` → value split on commas:
  - `img/b.png 1x` → first token `img/b.png`
  - `img/c.png 2x` → first token `img/c.png`
- → two image candidates: `img/b.png`, `img/c.png`

Net: four references extracted — three images (`src` + `srcset`) and one
recursable link (`href`).

---

## 6. Text-scan vs tag-aware parsing (the trade-off)

This is the single most important thing to understand — and the difference from
the Python version, which uses a real `HTMLParser`.

Because the scanner matches attribute *names* and knows nothing about *tags*:

- **It can't tell `<a href>` from `<link href>` or `<area href>`.** All `href`s
  share one `can_recurse` setting. That's why the C spider follows a `<link
  rel="canonical">` as if it were a page link — the cause of the trailing-slash
  depth surprise. Python's parser inspects the tag, so it recurses only on `<a>`.
- **It would match attribute-like text anywhere**, including inside `<script>`
  and HTML comments. `strip_blocks` neutralizes scripts/styles; `valid_ref`
  (rejecting braces, quotes, `javascript:`…) catches most of the rest.
- **It matches helpful things too.** `data-src="x.jpg"` (lazy-loaded images)
  ends in `src="..."`, so the scanner catches it — a `<link rel="icon">` favicon
  is found the same way. The tag-aware Python parser *misses* those unless you
  teach it each tag (which is why we had to add `<link>` handling to Python to
  match the favicon count).

The summary: **text scanning is greedier and simpler; tag parsing is precise but
blind to tags it doesn't code for.** Neither is strictly "more correct" — they
catch different things, and the guards (`strip_blocks`, `valid_ref`,
`is_asset_ext`, `looks_html`) are what make the greedy approach safe.

---

## 7. The guards around scanning

Scanning never runs on untrusted bytes unguarded:

- **`looks_html`** — before scanning at all, `crawl` checks the fetched body
  starts (after whitespace) with `<`. A binary file or JSON is never scanned, so
  stray `src`/`href` byte-sequences in binary data can't produce fake links.

  ```c
  static int looks_html(const char *s)
  {
      while (*s && isspace((unsigned char)*s))
          s++;
      return *s == '<';
  }
  ```

- **`strip_blocks`** — removes script/style bodies (section 2).
- **`valid_ref`** — every value `scan_attr` extracts is checked in `handle_ref`
  before use: no braces, quotes, spaces, control chars, or `javascript:`/`data:`
  schemes. This is the backstop that rejects any JS fragment the first two guards
  missed.

Together they mean: scan only real HTML, with scripts emptied, and distrust every
extracted value.

---

## 8. Gotchas

- **`strcasestr` needs `_GNU_SOURCE`** (set at the top of the file and in the
  Makefile) — it's a GNU extension.
- **The `*q != '='` check is load-bearing.** It's what distinguishes a real
  attribute from the name appearing inside another word (`srcset`,
  `data-source`, `href` inside `xlink:href`, etc.). Remove it and you get
  garbage.
- **`srcset` splitting keeps only the first token** of each candidate (the URL),
  dropping the `1x`/`2x`/`480w` descriptor — otherwise the descriptor would end
  up in the URL.
- **`read_attr_value` modifies nothing but copies out** with `strndup`; but
  `scan_attr`'s srcset branch **does** write `'\0'` into `val` (its own heap
  copy) while splitting — fine, because `val` is a private copy that gets
  `free`d.
- **Order matters: `strip_blocks` must run before `scan_attr`.** Scanning raw
  HTML first would mine the inline JS the stripping was meant to remove.
- **No entity decoding.** `&amp;` in a URL isn't turned back into `&`. Rare in
  image/link URLs and not needed for the subject, but a real browser would.

---

## 9. Build it yourself, in order

1. **`read_attr_value` first.** Given a pointer just past `=`, read a quoted
   value. Test on `"/a.png"` and `'/a.png'`, then unquoted `/a.png`.
2. **`scan_attr` for one attribute.** Loop with `strcasestr`, add the `=` check,
   call your reader, print each value. Test on a page with a few `<img src>`.
3. **Add `href`.** Same function, different attribute name; print those too.
4. **Add `srcset` splitting.** Handle the comma list and keep the first token of
   each part.
5. **Add `strip_blocks`.** Put an inline `<script>` with `location.href="/x"` on
   the page and confirm it's no longer mined.
6. **Add `looks_html` + `valid_ref`.** Point at a non-HTML URL and at a page with
   junk, and confirm nothing bogus comes through.

Test every step against a local `python3 -m http.server` with a page whose
attributes you wrote yourself, so you know exactly which references should come
out.
