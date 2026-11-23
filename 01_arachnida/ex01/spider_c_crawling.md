# `spider.c` — Web crawling: direct and recursive

How the spider turns one starting URL into a set of downloaded images. This
doc explains the crawl model, the two modes (direct and recursive), the depth
system, and the guards that keep recursion from running away. It assumes you've
read the function reference; here the focus is the *logic*, not the library
calls.

---

## 1. What "crawling" means here

A web crawler starts from one URL and discovers more by reading what that page
links to. For an image spider there are two different jobs hiding in that
sentence:

- **Collect images** that a page references (`<img src>`, `srcset`, favicon
  `<link>`, or an `<a href>` that points straight at a `.jpg`/`.png`/…).
- **Discover more pages** to repeat the process on, by following `<a href>`
  links — but only when recursion is switched on.

The whole program is one function, `crawl()`, applied to a page, that does the
first job always and the second job conditionally. Everything else (fetching,
parsing, URL resolution, downloading) is machinery in service of those two.

```
                 ┌──────────── crawl(url, depth) ─────────────┐
   start URL ──► │ fetch → is it HTML? → scan for refs        │
                 │   ├─ image ref      → download it          │
                 │   └─ page link (-r) → crawl(link, depth+1) │  ◄─ recursion
                 └────────────────────────────────────────────┘
```

---

## 2. Two modes

### Direct mode (no `-r`)

Without `-r`, the spider does **not** follow any links. It processes exactly
one thing:

- If the URL is a **page**, it downloads the images *on that single page* and
  stops.
- If the URL points **straight at an image** (ends in `.jpg`/`.png`/…), it
  downloads that one file.

That second case is handled before `crawl` is even called, in `main`:

```c
if (has_image_ext(url))   /* URL points straight at an image */
    download_image(url);
else                      /* it's a page: crawl it for images */
    crawl(url, 0);
```

Why the split? `crawl` only knows how to *parse a page*. If you hand it an
image, the fetch succeeds but `looks_html` returns false and it stops without
saving anything. So a URL that is itself the target is a special case: download
it directly. (The same reasoning applies to the PDF variant.)

In direct mode, `crawl` still runs for a page start URL — it just never reaches
the recursion branch, because the `g_recursive` flag is `0`.

### Recursive mode (`-r`)

With `-r`, after collecting a page's images the spider also **follows its
same-host `<a href>` links** and repeats the whole process on each, up to the
depth limit. One page becomes a tree of pages, and images are gathered from all
of them.

The switch is a single flag set during argument parsing:

```c
case 'r': g_recursive = 1; break;
```

and it gates the recursion branch inside `handle_ref`:

```c
} else if (can_recurse && g_recursive && depth < g_max_depth && !is_asset_ext(abs)) {
    ...
    if (strcmp(h, g_host) == 0 && !sl_contains(&g_visited, abs))
        crawl(abs, depth + 1);
}
```

---

## 3. The recursion engine

Three functions form the loop. Follow the calls:

```
crawl(url, depth)
  └─ scan_attr(...)         finds each src / href / srcset value
       └─ handle_ref(...)   decides: download? or recurse?
            └─ crawl(...)   ← calls back into crawl at depth+1
```

`crawl` → `scan_attr` → `handle_ref` → `crawl` is **indirect recursion** (the
cycle we needed the forward declaration for). Here's what each link does.

**`crawl(url, depth)`** — process one page:

```c
static void crawl(const char *url, int depth) {
    if (sl_contains(&g_visited, url)) return;    /* (a) already done? */
    sl_add(&g_visited, url);                      /* (b) mark visited */
    printf("[page] depth %d: %s\n", depth, url);

    char *html = http_get(url);                   /* (c) fetch        */
    if (!html) { fprintf(stderr, "  [skip] %s\n", url); return; }
    if (!looks_html(html)) { free(html); return; }/* (d) HTML only    */

    char *clean = strip_blocks(html);             /* (e) drop JS/CSS  */
    free(html);
    if (!clean) return;

    scan_attr(clean, url, depth, "src",    0, 0); /* (f) images only  */
    scan_attr(clean, url, depth, "href",   0, 1); /*     links+images */
    scan_attr(clean, url, depth, "srcset", 1, 0); /*     images only  */
    free(clean);
}
```

**`handle_ref(base, raw, depth, can_recurse)`** — classify one reference:

```c
static void handle_ref(const char *base, const char *raw, int depth, int can_recurse) {
    if (!valid_ref(raw)) return;
    char *abs = resolve_url(base, raw);           /* make it absolute */
    if (has_image_ext(abs)) {
        download_image(abs);                      /* it's an image    */
    } else if (can_recurse && g_recursive && depth < g_max_depth &&
               !is_asset_ext(abs)) {
        char h[512];
        host_of(abs, h, sizeof(h));
        if (strcmp(h, g_host) == 0 && !sl_contains(&g_visited, abs))
            crawl(abs, depth + 1);                /* recurse one deeper */
    }
    free(abs);
}
```

The key design choice: **only `href` references drive recursion** (`can_recurse
= 1`); `src` and `srcset` are image-only (`can_recurse = 0`). That's why a
`<script src="app.js">` is never crawled — `src` can't recurse.

---

## 4. Depth: what `-l` actually counts

Depth is the distance, in page-hops, from the start URL.

- The start page is **depth 0**.
- A page you reach by following one link from it is **depth 1**.
- A page one link further is **depth 2**, and so on.

`-l N` sets the maximum: the spider follows links **while** `depth < N`. The
default is 5. The check is literally `depth < g_max_depth`, and each recursive
call passes `depth + 1`.

```
./spider -r        -l 2  URL        ./spider -r -l 1 URL        ./spider URL
depth 0  (start)   ✓ scanned        depth 0  ✓                  depth 0 ✓
depth 1            ✓ scanned        depth 1  ✓ (last)            (no -r:
depth 2            ✓ scanned        depth 2  ✗ not followed       recursion off)
depth 3            ✗ not followed
```

Two things people get wrong at the defense:

- **Images are still collected at the deepest allowed level.** `-l 2` means
  pages at depth 2 are fetched and their images downloaded; it only stops the
  spider from following links *out of* depth-2 pages.
- **`-l` without `-r` does nothing.** The depth limit only matters once
  recursion is enabled. The subject writes them together (`-r -l`) for that
  reason.

---

## 5. The guards that make recursion safe

Naive recursion on the web either loops forever or tries to download the entire
internet. Five guards prevent that:

| Guard | Code | Prevents |
|-------|------|----------|
| **Visited set** | `sl_contains(&g_visited, url)` | re-crawling a page; infinite loops when pages link back |
| **Depth limit** | `depth < g_max_depth` | descending forever down deep link chains |
| **Same host** | `strcmp(h, g_host) == 0` | wandering off onto the whole web |
| **Asset skip** | `!is_asset_ext(abs)` | recursing into `.js`/`.css`/`.pdf`/… that aren't pages |
| **Image dedup** | `sl_contains(&g_downloaded, url)` | downloading the same image twice |

The first two are the classic pair every crawler needs. Without the **visited
set**, two pages that link to each other (`A → B → A → B …`) recurse until the
stack overflows. Without the **depth limit**, a site with deep pagination never
ends. The **same-host** check keeps the crawl inside the site you started on.
The last two are efficiency/correctness refinements specific to this spider.

There's also a content guard — `looks_html` — that isn't about recursion but
about *parsing*: even if a link slips through, the spider only scans a body that
actually looks like HTML, so a binary file is never mined for fake links.

---

## 6. Traversal order: depth-first

The recursion happens **inline, while scanning** — `handle_ref` calls `crawl`
the moment it meets a page link, before looking at the rest of the current
page's links. So the spider goes **depth-first**: it dives all the way down the
first link it finds before coming back for the second.

Trace it on the 3-level test site (`index → page2 → page3`, one image each),
with `-r`:

```
crawl(index, 0)
  download a.png
  meet <a href=page2> → crawl(page2, 1)
        download b.png
        meet <a href=page3> → crawl(page3, 2)
              download c.png
              (back out)
        (back out)
  (done)
```

Output:

```
[page] depth 0: .../            [img ] .../img/a.png
[page] depth 1: .../page2.html  [img ] .../img/b.png
[page] depth 2: .../page3.html  [img ] .../img/c.png
Done. 3 image(s) saved to ...
```

Depth-first has one visible consequence worth knowing: the order in which pages
appear in the output depends on link order in the HTML, and a link encountered
early can "claim" a page at a shallow depth before a later link would reach it.
That's exactly what produced the trailing-slash surprise — a `<link
rel="canonical">` in the `<head>` was followed first and used up a depth level.

---

## 7. Putting the two jobs together

On every page, `crawl` runs its three `scan_attr` passes, and each reference
flows through `handle_ref`, which answers one question — *image, or page?*

```
                         reference found
                               │
                    resolve to absolute URL
                               │
                 ┌─────────────┴─────────────┐
            ends in image ext?          otherwise
                 │                           │
            download it          came from href? and -r?
                                 and depth<max? and same host?
                                 and not an asset? and not visited?
                                     │                    │
                                    yes                   no
                                     │                    │
                               crawl deeper            ignore
```

- **Direct mode** is the left branch only (plus the single-image start URL):
  images on one page, nothing followed.
- **Recursive mode** adds the right branch: HTML links feed back into `crawl`,
  turning one page into a bounded, de-duplicated, same-host tree.

---

## 8. Rebuild it yourself, in order

If you're writing this from scratch, add crawling in this sequence and test each
step against a local `python3 -m http.server`:

1. **Direct, one page.** Make `crawl` fetch the start page and download its
   `<img>` images. No recursion yet. Verify the count on a single page.
2. **Direct, image URL.** Add the `main` branch that downloads the start URL
   directly when it's an image.
3. **Recursion, unguarded.** Let `href` links call `crawl` again at `depth+1`.
   Point it at your 3-page site — it should reach `page3`.
4. **Add the visited set.** Make two pages link to each other and confirm it no
   longer loops forever.
5. **Add the depth limit.** Check `-l 1` stops before the deepest page.
6. **Add the same-host and asset guards.** Confirm it won't follow an external
   link or a `.css`.

By step 6 you've built the whole crawl engine, one guarantee at a time, each one
provable on a site whose right answer you already know.
