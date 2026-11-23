/*
 * spider - recursively download images from a website.
 *
 * Usage:   ./spider [-r] [-l N] [-p PATH] URL
 *   -r         follow links recursively
 *   -l N       max recursion depth (default 5, needs -r)
 *   -p PATH    output directory (default ./data/)
 *
 * This variant needs NO libcurl headers: it invokes the system `curl`
 * binary for each HTTP request via fork + execvp (an argv array, never a
 * shell -- so URLs from pages can't inject commands). All spider logic
 * (HTML scanning, URL resolution, recursion, dedup) is implemented here.
 * wget/scrapy are not used. Build: `make` (no -lcurl, runs anywhere curl
 * is installed).
 */

#ifndef _GNU_SOURCE
# define _GNU_SOURCE   /* strcasestr, strndup */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#define DEFAULT_DEPTH 5
#define DEFAULT_PATH  "./data/"
#define UA            "arachnida-spider/1.0"

/* ----- a growable string list used for visited pages / downloaded imgs ---- */
typedef struct {
    char **items;
    size_t count;
    size_t cap;
} strlist;

/* ----- global config / state ---------------------------------------------- */
static int    g_recursive = 0;
static int    g_max_depth = DEFAULT_DEPTH;
static char  *g_out       = DEFAULT_PATH;
static char   g_host[512] = {0};
static strlist g_visited;
static strlist g_downloaded;
static int    g_count     = 0;

/* ===== Forward declarations ============================================== *
 * Prototypes are ordered to follow the program's control flow: entry ->
 * crawl a page -> fetch it -> gate/clean -> scan for refs -> handle each
 * ref -> resolve its URL -> download images, then the helpers underneath.
 * The function bodies appear further down (in their original order).
 * ========================================================================= */

/* ---- Program entry & control flow ---- */

/* main: program entry; parse options, validate the URL, set up state and
 *       start the crawl at depth 0.
 *   args:    argc, argv - command line (-r, -l N, -p PATH, URL)
 *   returns: 0 on success, 1 on a usage/argument error */
int main(int argc, char **argv);

/* usage: print the usage line to stderr and terminate.
 *   args:    prog - argv[0], the program name
 *   returns: does not return (calls exit(1)) */
static void usage(const char *prog);

/* crawl: process one page - skip if already visited, fetch it, keep only real
 *        HTML, strip inline script/style, then scan it for images and links
 *        (recursing into same-host links).
 *   args:    url   - absolute URL of the page to crawl
 *            depth - current recursion depth (0 = start page)
 *   returns: nothing */
static void crawl(const char *url, int depth);

/* ---- HTTP (delegated to the system curl binary) ---- */

/* http_get: fetch the body of url into a heap buffer (capped page download).
 *   args:    url - absolute URL to GET
 *   returns: malloc'd NUL-terminated body (caller frees), or NULL on error */
static char *http_get(const char *url);

/* http_download: fetch url straight to a file on disk (image download).
 *   args:    url  - absolute URL to GET
 *            dest - path of the file to write
 *   returns: 0 on success, -1 on failure */
static int http_download(const char *url, const char *dest);

/* ---- Page gate & cleanup ---- */

/* looks_html: cheap check that a fetched body is actually an HTML page.
 *   args:    s - the fetched body
 *   returns: 1 if the first non-space character is '<', else 0 */
static int looks_html(const char *s);

/* strip_blocks: copy html with the CONTENTS of <script>/<style> removed so
 *               inline JavaScript can't be mined for fake links.
 *   args:    html - the page source
 *   returns: malloc'd cleaned copy (caller frees), or NULL on alloc failure */
static char *strip_blocks(const char *html);

/* ---- Scanning a page for references ---- */

/* scan_attr: find every occurrence of attr= in html and hand each value to
 *            handle_ref (splitting a srcset list into its candidate URLs).
 *   args:    html        - page source to scan
 *            base        - page URL, used to resolve relative refs
 *            depth       - current recursion depth
 *            attr        - attribute name to look for ("src"/"href"/"srcset")
 *            is_srcset   - 1 if the value is a srcset list, else 0
 *            can_recurse - 1 if these refs may drive recursion (href), else 0
 *   returns: nothing */
static void scan_attr(const char *html, const char *base, int depth,
                      const char *attr, int is_srcset, int can_recurse);

/* read_attr_value: read one attribute value (quoted or bare) at *pp.
 *   args:    pp - address of the cursor just past '='; advanced past the value
 *   returns: malloc'd copy of the value (caller frees) */
static char *read_attr_value(const char **pp);

/* ---- Handling each reference ---- */

/* handle_ref: resolve one raw ref; if it is an image, download it; otherwise,
 *             when allowed, recurse into it (same host, within depth).
 *   args:    base        - page URL the ref was found on
 *            raw         - the raw attribute value
 *            depth       - current recursion depth
 *            can_recurse - 1 if this ref may be followed as a page link
 *   returns: nothing */
static void handle_ref(const char *base, const char *raw, int depth, int can_recurse);

/* valid_ref: decide whether a raw ref is a real, fetchable URL (rejects empty,
 *            over-long, non-http schemes, fragments and URL-illegal chars).
 *   args:    raw - the raw attribute value
 *   returns: 1 if usable, 0 if it should be ignored */
static int valid_ref(const char *raw);

/* is_asset_ext: tell whether a URL's extension is a non-HTML asset that must
 *               never be recursed into (.js, .css, .mp4, .pdf, ...).
 *   args:    url - absolute URL to test
 *   returns: 1 if it is such an asset, else 0 */
static int is_asset_ext(const char *url);

/* ---- Turning a raw ref into an absolute URL ---- */

/* resolve_url: join a (possibly relative) ref onto base and normalize it.
 *   args:    base - the page URL the ref came from
 *            ref  - the raw reference (absolute, //host, /root, or relative)
 *   returns: malloc'd absolute, normalized URL (caller frees) */
static char *resolve_url(const char *base, const char *ref);

/* normalize_in_place: collapse "." and ".." path segments within url.
 *   args:    url - the URL to rewrite in place (only ever shortened)
 *   returns: nothing */
static void normalize_in_place(char *url);

/* host_of: copy the host part of a URL into a caller-provided buffer.
 *   args:    url - the URL to inspect
 *            out - destination buffer for the host
 *            cap - size of out
 *   returns: nothing (out receives the host, or "" if none) */
static void host_of(const char *url, char *out, size_t cap);

/* basename_of: locate the final path component (file name) of a URL.
 *   args:    url - the URL to inspect
 *   returns: pointer INTO url at the file name (not a new allocation) */
static const char *basename_of(const char *url);

/* has_image_ext: tell whether a URL's path ends in a wanted image extension.
 *   args:    url - absolute URL to test
 *   returns: 1 for .jpg/.jpeg/.png/.gif/.bmp, else 0 */
static int has_image_ext(const char *url);

/* ---- Downloading an image ---- */

/* download_image: download one image, skipping duplicates and avoiding
 *                 filename clashes, then report it.
 *   args:    url - absolute URL of the image
 *   returns: nothing */
static void download_image(const char *url);

/* ---- Filesystem helper ---- */

/* make_dirs: create a directory path, parents included (like mkdir -p).
 *   args:    path - directory path to create
 *   returns: 0 on success, -1 on failure */
static int make_dirs(const char *path);

/* ---- String list (the "visited" and "downloaded" sets) ---- */

/* sl_init: initialize an empty string list.
 *   args:    l - the list to initialize
 *   returns: nothing */
static void sl_init(strlist *l);

/* sl_contains: test whether a string is already in the list.
 *   args:    l - the list; s - the string to look for
 *   returns: 1 if present, else 0 */
static int sl_contains(const strlist *l, const char *s);

/* sl_add: append a copy of a string to the list (grows as needed).
 *   args:    l - the list; s - the string to copy in
 *   returns: nothing */
static void sl_add(strlist *l, const char *s);

/* sl_free: free every stored string and the list's backing array.
 *   args:    l - the list to release
 *   returns: nothing */
static void sl_free(strlist *l);

/* ===== Function definitions ============================================== */

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "rl:p:")) != -1) {
        switch (opt) {
            case 'r':
                g_recursive = 1;
                break;
            case 'l':
                g_max_depth = atoi(optarg);
                break;
            case 'p':
                g_out = optarg;
                break;
            default:
                usage(argv[0]);
        }
    }
    if (optind != argc - 1)
        usage(argv[0]);
    char *url = argv[optind];

    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
        fprintf(stderr, "URL must start with http:// or https://\n");
        return 1;
    }
    if (g_max_depth < 0) {
        fprintf(stderr, "-l must be >= 0\n");
        return 1;
    }

    /* canonicalize a host-only start URL: "scheme://host" -> "scheme://host/"
     * so it matches the "scheme://host/" form the page's own canonical <link>
     * uses; otherwise the two are distinct and recursion wastes a depth level
     * detouring through the slash form. */
    static char starturl[2048];
    snprintf(starturl, sizeof(starturl), "%s", url);
    {
        char *afterhost = strstr(starturl, "://");
        if (afterhost && !strchr(afterhost + 3, '/')) {
            size_t ul = strlen(starturl);
            if (ul + 1 < sizeof(starturl)) {
                starturl[ul] = '/';
                starturl[ul + 1] = '\0';
            }
        }
    }
    url = starturl;

    static char outbuf[1024];
    snprintf(outbuf, sizeof(outbuf), "%s", g_out);
    size_t ol = strlen(outbuf);
    while (ol > 1 && outbuf[ol - 1] == '/')
        outbuf[--ol] = '\0';
    g_out = outbuf;

    if (make_dirs(g_out) != 0) {
        perror("mkdir");
        return 1;
    }
    host_of(url, g_host, sizeof(g_host));
    sl_init(&g_visited);
    sl_init(&g_downloaded);

    if (has_image_ext(url))   /* URL points straight at an image */
        download_image(url);
    else                      /* it's a page: crawl it for images */
        crawl(url, 0);

    printf("\nDone. %d image(s) saved to %s\n", g_count, g_out);
    sl_free(&g_visited);
    sl_free(&g_downloaded);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-r] [-l N] [-p PATH] URL\n", prog);
    exit(1);
}

static void crawl(const char *url, int depth)
{
    if (sl_contains(&g_visited, url))
        return;
    sl_add(&g_visited, url);
    printf("[page] depth %d: %s\n", depth, url);

    char *html = http_get(url);
    if (!html) {
        fprintf(stderr, "  [skip] %s\n", url);
        return;
    }
    if (!looks_html(html)) {    /* fetched, but not a page */
        free(html);
        return;
    }

    char *clean = strip_blocks(html);    /* drop inline JS/CSS */
    free(html);
    if (!clean)
        return;

    /* only <a href> links drive recursion; src/srcset collect images only */
    scan_attr(clean, url, depth, "src",    0, 0);
    scan_attr(clean, url, depth, "href",   0, 1);
    scan_attr(clean, url, depth, "srcset", 1, 0);
    free(clean);
}

static char *http_get(const char *url)
{
    int fd[2];
    if (pipe(fd) < 0)
        return NULL;
    pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        return NULL;
    }
    if (pid == 0) {
        close(fd[0]);
        dup2(fd[1], STDOUT_FILENO);      /* curl writes to the pipe */
        close(fd[1]);
        char *argv[] = { "curl", "-fsSL", "-A", UA, "--max-time", "20",
                         "--max-filesize", "5000000", (char *)url, NULL };
        execvp("curl", argv);
        _exit(127);
    }
    close(fd[1]);
    char *buf = NULL;
    char tmp[4096];
    size_t len = 0;
    size_t cap = 0;
    ssize_t n;
    while ((n = read(fd[0], tmp, sizeof(tmp))) > 0) {
        if (len + (size_t)n + 1 > cap) {
            if (cap)
                cap = cap * 2;
            else
                cap = 8192;
            while (cap < len + (size_t)n + 1)
                cap *= 2;
            char *p = realloc(buf, cap);
            if (!p) {
                free(buf);
                close(fd[0]);
                waitpid(pid, NULL, 0);
                return NULL;
            }
            buf = p;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    close(fd[0]);
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        free(buf);
        return NULL;
    }
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        free(buf);
        return NULL;
    }
    if (!buf)
        return strdup("");         /* empty but successful body */
    buf[len] = '\0';
    return buf;
}

static int http_download(const char *url, const char *dest)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char *argv[] = { "curl", "-fsSL", "-A", UA,
                         "--max-time", "20", "-o", (char *)dest,
                         (char *)url, NULL };
        execvp("curl", argv);
        _exit(127);                      /* only reached if exec fails */
    }
    int status;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    return -1;
}

static int looks_html(const char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return *s == '<';
}

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
            if (e)
                p = e + 9;
            else
                p = p + strlen(p);
            continue;
        }
        if (!strncasecmp(p, "<style", 6)) {
            const char *e = strcasestr(p, "</style>");
            if (e)
                p = e + 8;
            else
                p = p + strlen(p);
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return out;
}

static void scan_attr(const char *html, const char *base, int depth,
                      const char *attr, int is_srcset, int can_recurse)
{
    size_t alen = strlen(attr);
    const char *p = html;
    while ((p = strcasestr(p, attr)) != NULL) {
        const char *q = p + alen;
        while (*q && isspace((unsigned char)*q))
            q++;
        if (*q != '=') {
            p += alen;
            continue;
        }
        q++;
        char *val = read_attr_value(&q);
        if (is_srcset) {
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
            handle_ref(base, val, depth, can_recurse);
        }
        free(val);
        p = q;
    }
}

static char *read_attr_value(const char **pp)
{
    const char *p = *pp;
    while (*p && isspace((unsigned char)*p))
        p++;
    char quote = 0;
    if (*p == '"' || *p == '\'') {
        quote = *p;
        p++;
    }
    const char *start = p;
    if (quote)
        while (*p && *p != quote)
            p++;
    else
        while (*p && !isspace((unsigned char)*p) && *p != '>')
            p++;
    char *val = strndup(start, p - start);
    if (*p)
        *pp = p + 1;
    else
        *pp = p;
    return val;
}

static void handle_ref(const char *base, const char *raw, int depth, int can_recurse)
{
    if (!valid_ref(raw))
        return;
    char *abs = resolve_url(base, raw);
    if (has_image_ext(abs)) {
        download_image(abs);
    }
    else if (can_recurse && g_recursive && depth < g_max_depth &&
             !is_asset_ext(abs)) {
        char h[512];
        host_of(abs, h, sizeof(h));
        if (strcmp(h, g_host) == 0 && !sl_contains(&g_visited, abs))
            crawl(abs, depth + 1);
    }
    free(abs);
}

static int valid_ref(const char *raw)
{
    if (!raw || !*raw || raw[0] == '#')
        return 0;
    if (strlen(raw) > 1024)
        return 0;
    if (!strncasecmp(raw, "javascript:", 11) || !strncasecmp(raw, "data:", 5) ||
        !strncasecmp(raw, "mailto:", 7)     || !strncasecmp(raw, "tel:", 4))
        return 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p; p++) {
        if (*p <= 0x20 || *p >= 0x7F)            /* ctrl/space/non-ascii */
            return 0;
        if (strchr("{}<>\"'`\\|^[] ", *p))       /* invalid in a URL */
            return 0;
    }
    return 1;
}

static int is_asset_ext(const char *url)
{
    const char *end = url;
    while (*end && *end != '?' && *end != '#')
        end++;
    const char *dot = NULL;
    for (const char *p = url; p < end; p++)
        if (*p == '.')
            dot = p;
    if (!dot)
        return 0;
    size_t n = end - dot;
    char ext[12] = {0};
    if (n >= sizeof(ext))
        return 0;
    for (size_t i = 0; i < n; i++)
        ext[i] = tolower((unsigned char)dot[i]);
    static const char *assets[] = {
        ".js",".css",".mp4",".webm",".mov",".avi",".svg",".json",".xml",".pdf",
        ".zip",".gz",".woff",".woff2",".ttf",".eot",".ico",".mp3",".wav",".m4a",
        ".map",".txt",".webp",NULL };
    for (int i = 0; assets[i]; i++)
        if (!strcmp(ext, assets[i]))
            return 1;
    return 0;
}

static char *resolve_url(const char *base, const char *ref)
{
    size_t rl = strcspn(ref, "#");
    char *r = strndup(ref, rl);

    char *res = NULL;
    if (!strncmp(r, "http://", 7) || !strncmp(r, "https://", 8)) {
        res = strdup(r);
    }
    else if (!strncmp(r, "//", 2)) {                   /* protocol-relative */
        size_t sl = strcspn(base, ":");
        res = malloc(sl + strlen(r) + 2);
        sprintf(res, "%.*s:%s", (int)sl, base, r);
    }
    else if (r[0] == '/') {                            /* root-relative */
        const char *p = strstr(base, "://");
        p += 3;
        const char *slash = strchr(p, '/');
        size_t prefix;
        if (slash)
            prefix = (size_t)(slash - base);
        else
            prefix = strlen(base);
        res = malloc(prefix + strlen(r) + 1);
        sprintf(res, "%.*s%s", (int)prefix, base, r);
    }
    else {                                             /* path-relative */
        const char *q = base + strcspn(base, "?#");
        const char *lastslash = NULL;
        for (const char *p = base; p < q; p++)
            if (*p == '/')
                lastslash = p;
        const char *afterscheme = strstr(base, "://");
        if (afterscheme && lastslash && lastslash < afterscheme + 3)
            lastslash = NULL;
        size_t prefix;
        if (lastslash)
            prefix = lastslash - base + 1;
        else
            prefix = q - base;
        res = malloc(prefix + strlen(r) + 2);
        if (lastslash)
            sprintf(res, "%.*s%s", (int)prefix, base, r);
        else
            sprintf(res, "%.*s/%s", (int)prefix, base, r);
    }
    free(r);
    normalize_in_place(res);
    /* host-only result -> append "/" so "scheme://host" == "scheme://host/" */
    {
        char *afterhost = strstr(res, "://");
        if (afterhost && !strchr(afterhost + 3, '/')) {
            size_t rl2 = strlen(res);
            char *r2 = realloc(res, rl2 + 2);
            if (r2) {
                res = r2;
                res[rl2] = '/';
                res[rl2 + 1] = '\0';
            }
        }
    }
    return res;
}

static void normalize_in_place(char *url)
{
    char *p = strstr(url, "://");
    if (!p)
        return;
    p += 3;
    char *path = strchr(p, '/');
    if (!path)
        return;

    char qsave[2048] = "";
    char *query = strpbrk(path, "?#");
    if (query) {
        snprintf(qsave, sizeof(qsave), "%s", query);
        *query = '\0';
    }

    char *tmp = strdup(path);
    char *segs[512];
    int n = 0;
    for (char *tok = strtok(tmp, "/"); tok; tok = strtok(NULL, "/")) {
        if (n >= 512)                           /* guard: never overflow segs[] */
            break;
        if (!strcmp(tok, "."))
            continue;
        else if (!strcmp(tok, "..")) {
            if (n > 0)
                n--;
        }
        else
            segs[n++] = tok;
    }
    char rebuilt[2048];
    size_t off = 0;
    rebuilt[0] = '\0';
    for (int i = 0; i < n && off < sizeof(rebuilt); i++)  /* clamp: no size_t underflow */
        off += snprintf(rebuilt + off, sizeof(rebuilt) - off, "/%s", segs[i]);
    if (n == 0) {
        rebuilt[0] = '/';
        rebuilt[1] = '\0';
    }

    strcpy(path, rebuilt);
    strcat(url, qsave);
    free(tmp);
}

static void host_of(const char *url, char *out, size_t cap)
{
    const char *p = strstr(url, "://");
    out[0] = '\0';
    if (!p)
        return;
    p += 3;
    size_t i = 0;
    while (*p && *p != '/' && i + 1 < cap)
        out[i++] = *p++;
    out[i] = '\0';
}

static const char *basename_of(const char *url)
{
    size_t n = strcspn(url, "?#");
    const char *last = url;
    for (const char *p = url; p < url + n; p++)
        if (*p == '/')
            last = p + 1;
    return last;
}

static int has_image_ext(const char *url)
{
    const char *end = url;
    while (*end && *end != '?' && *end != '#')
        end++;
    const char *dot = NULL;
    for (const char *p = url; p < end; p++)
        if (*p == '.')
            dot = p;
    if (!dot)
        return 0;
    size_t n = end - dot;
    char ext[8] = {0};
    if (n >= sizeof(ext))
        return 0;
    for (size_t i = 0; i < n; i++)
        ext[i] = tolower((unsigned char)dot[i]);
    return !strcmp(ext, ".jpg")  || !strcmp(ext, ".jpeg") ||
           !strcmp(ext, ".png")  || !strcmp(ext, ".gif")  ||
           !strcmp(ext, ".bmp");
}

static void download_image(const char *url)
{
    if (sl_contains(&g_downloaded, url))
        return;
    sl_add(&g_downloaded, url);

    /* file name = last path segment, without any ?query or #fragment */
    const char *bn = basename_of(url);
    size_t nl = strcspn(bn, "?#");
    char name[1024];
    if (nl == 0 || nl >= sizeof(name))
        snprintf(name, sizeof(name), "image");
    else {
        memcpy(name, bn, nl);
        name[nl] = '\0';
    }

    char dest[2048];
    snprintf(dest, sizeof(dest), "%s/%s", g_out, name);

    struct stat st;
    if (stat(dest, &st) == 0) {
        char base[1024];
        snprintf(base, sizeof(base), "%s", name);
        char *dot = strrchr(base, '.');
        char ext[64] = "";
        if (dot) {
            snprintf(ext, sizeof(ext), "%s", dot);
            *dot = '\0';
        }
        for (int i = 1; ; i++) {
            snprintf(dest, sizeof(dest), "%s/%s_%d%s", g_out, base, i, ext);
            if (stat(dest, &st) != 0)
                break;
        }
    }

    if (http_download(url, dest) == 0) {
        g_count++;
        printf("  [img ] %s -> %s\n", url, dest);
    }
    else {
        fprintf(stderr, "  [skip] %s\n", url);
        remove(dest);
    }
}

static int make_dirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) && errno != EEXIST)
        return -1;
    return 0;
}

static void sl_init(strlist *l)
{
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

static int sl_contains(const strlist *l, const char *s)
{
    for (size_t i = 0; i < l->count; i++)
        if (strcmp(l->items[i], s) == 0)
            return 1;
    return 0;
}

static void sl_add(strlist *l, const char *s)
{
    if (l->count == l->cap) {
        if (l->cap)
            l->cap = l->cap * 2;
        else
            l->cap = 64;
        l->items = realloc(l->items, l->cap * sizeof(char *));
        if (!l->items) {
            perror("realloc");
            exit(1);
        }
    }
    l->items[l->count++] = strdup(s);
}

static void sl_free(strlist *l)
{
    for (size_t i = 0; i < l->count; i++)
        free(l->items[i]);
    free(l->items);
}