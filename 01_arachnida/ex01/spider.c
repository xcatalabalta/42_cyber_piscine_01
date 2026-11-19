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

static void sl_init(strlist *l) { l->items = NULL; l->count = 0; l->cap = 0; }

static int sl_contains(const strlist *l, const char *s) {
    for (size_t i = 0; i < l->count; i++)
        if (strcmp(l->items[i], s) == 0) return 1;
    return 0;
}

static void sl_add(strlist *l, const char *s) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->items = realloc(l->items, l->cap * sizeof(char *));
        if (!l->items) { perror("realloc"); exit(1); }
    }
    l->items[l->count++] = strdup(s);
}

static void sl_free(strlist *l) {
    for (size_t i = 0; i < l->count; i++) free(l->items[i]);
    free(l->items);
}

/* ----- global config / state ---------------------------------------------- */
static int    g_recursive = 0;
static int    g_max_depth = DEFAULT_DEPTH;
static char  *g_out       = DEFAULT_PATH;
static char   g_host[512] = {0};
static strlist g_visited;
static strlist g_downloaded;
static int    g_count     = 0;

/* ----- HTTP via the system curl binary (fork + execvp, no shell) ---------- */

/* Download url straight to dest with `curl -o`. Returns 0 on success. */
static int http_download(const char *url, const char *dest) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char *argv[] = { "curl", "-fsSL", "-A", UA,
                         "--max-time", "20", "-o", (char *)dest,
                         (char *)url, NULL };
        execvp("curl", argv);
        _exit(127);                      /* only reached if exec fails */
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Fetch url's body into a malloc'd buffer via a pipe. NULL on error. */
static char *http_get(const char *url) {
    int fd[2];
    if (pipe(fd) < 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return NULL; }
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
    char *buf = NULL, tmp[4096];
    size_t len = 0, cap = 0;
    ssize_t n;
    while ((n = read(fd[0], tmp, sizeof(tmp))) > 0) {
        if (len + (size_t)n + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            while (cap < len + (size_t)n + 1) cap *= 2;
            char *p = realloc(buf, cap);
            if (!p) { free(buf); close(fd[0]); waitpid(pid, NULL, 0); return NULL; }
            buf = p;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    close(fd[0]);
    int status;
    if (waitpid(pid, &status, 0) < 0) { free(buf); return NULL; }
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) { free(buf); return NULL; }
    if (!buf) return strdup("");         /* empty but successful body */
    buf[len] = '\0';
    return buf;
}

/* ----- helpers ------------------------------------------------------------ */
static int has_image_ext(const char *url) {
    const char *end = url;
    while (*end && *end != '?' && *end != '#') end++;
    const char *dot = NULL;
    for (const char *p = url; p < end; p++) if (*p == '.') dot = p;
    if (!dot) return 0;
    size_t n = end - dot;
    char ext[8] = {0};
    if (n >= sizeof(ext)) return 0;
    for (size_t i = 0; i < n; i++) ext[i] = tolower((unsigned char)dot[i]);
    return !strcmp(ext, ".jpg")  || !strcmp(ext, ".jpeg") ||
           !strcmp(ext, ".png")  || !strcmp(ext, ".gif")  ||
           !strcmp(ext, ".bmp");
}

static void host_of(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "://");
    out[0] = '\0';
    if (!p) return;
    p += 3;
    size_t i = 0;
    while (*p && *p != '/' && i + 1 < cap) out[i++] = *p++;
    out[i] = '\0';
}

/* collapse "." and ".." path segments in place (normalization only shortens) */
static void normalize_in_place(char *url) {
    char *p = strstr(url, "://");
    if (!p) return;
    p += 3;
    char *path = strchr(p, '/');
    if (!path) return;

    char qsave[2048] = "";
    char *query = strpbrk(path, "?#");
    if (query) { snprintf(qsave, sizeof(qsave), "%s", query); *query = '\0'; }

    char *tmp = strdup(path);
    char *segs[512];
    int n = 0;
    for (char *tok = strtok(tmp, "/"); tok; tok = strtok(NULL, "/")) {
        if (n >= 512) break;                    /* guard: never overflow segs[] */
        if (!strcmp(tok, "."))        continue;
        else if (!strcmp(tok, "..")) { if (n > 0) n--; }
        else                          segs[n++] = tok;
    }
    char rebuilt[2048];
    size_t off = 0;
    rebuilt[0] = '\0';
    for (int i = 0; i < n && off < sizeof(rebuilt); i++)  /* clamp: no size_t underflow */
        off += snprintf(rebuilt + off, sizeof(rebuilt) - off, "/%s", segs[i]);
    if (n == 0) { rebuilt[0] = '/'; rebuilt[1] = '\0'; }

    strcpy(path, rebuilt);
    strcat(url, qsave);
    free(tmp);
}

/* resolve a (possibly relative) ref against base, result malloc'd */
static char *resolve_url(const char *base, const char *ref) {
    size_t rl = strcspn(ref, "#");
    char *r = strndup(ref, rl);

    char *res = NULL;
    if (!strncmp(r, "http://", 7) || !strncmp(r, "https://", 8)) {
        res = strdup(r);
    } else if (!strncmp(r, "//", 2)) {                 /* protocol-relative */
        size_t sl = strcspn(base, ":");
        res = malloc(sl + strlen(r) + 2);
        sprintf(res, "%.*s:%s", (int)sl, base, r);
    } else if (r[0] == '/') {                          /* root-relative */
        const char *p = strstr(base, "://");
        p += 3;
        const char *slash = strchr(p, '/');
        size_t prefix = slash ? (size_t)(slash - base) : strlen(base);
        res = malloc(prefix + strlen(r) + 1);
        sprintf(res, "%.*s%s", (int)prefix, base, r);
    } else {                                           /* path-relative */
        const char *q = base + strcspn(base, "?#");
        const char *lastslash = NULL;
        for (const char *p = base; p < q; p++) if (*p == '/') lastslash = p;
        const char *afterscheme = strstr(base, "://");
        if (afterscheme && lastslash && lastslash < afterscheme + 3)
            lastslash = NULL;
        size_t prefix;
        if (lastslash) prefix = lastslash - base + 1;
        else           prefix = q - base;
        res = malloc(prefix + strlen(r) + 2);
        if (lastslash) sprintf(res, "%.*s%s", (int)prefix, base, r);
        else           sprintf(res, "%.*s/%s", (int)prefix, base, r);
    }
    free(r);
    normalize_in_place(res);
    return res;
}

static const char *basename_of(const char *url) {
    size_t n = strcspn(url, "?#");
    const char *last = url;
    for (const char *p = url; p < url + n; p++) if (*p == '/') last = p + 1;
    return last;
}

static int make_dirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
    return 0;
}

static void download_image(const char *url) {
    if (sl_contains(&g_downloaded, url)) return;
    sl_add(&g_downloaded, url);

    const char *name = basename_of(url);
    char dest[2048];
    if (!*name) name = "image";
    snprintf(dest, sizeof(dest), "%s/%s", g_out, name);

    struct stat st;
    if (stat(dest, &st) == 0) {
        char base[1024]; snprintf(base, sizeof(base), "%s", name);
        char *dot = strrchr(base, '.');
        char ext[64] = "";
        if (dot) { snprintf(ext, sizeof(ext), "%s", dot); *dot = '\0'; }
        for (int i = 1; ; i++) {
            snprintf(dest, sizeof(dest), "%s/%s_%d%s", g_out, base, i, ext);
            if (stat(dest, &st) != 0) break;
        }
    }

    if (http_download(url, dest) == 0) {
        g_count++;
        printf("  [img ] %s -> %s\n", url, dest);
    } else {
        fprintf(stderr, "  [skip] %s\n", url);
        remove(dest);
    }
}

/* ----- HTML scan + crawl --------------------------------------------------- */
static void crawl(const char *url, int depth);

/* reject refs that aren't real, fetchable URLs: empty, over-long, non-http
 * schemes, fragments, and anything with characters a URL can't contain
 * unencoded. This is what stops JavaScript snippets (e.g. "a.href,b({...})")
 * from being mistaken for links. */
static int valid_ref(const char *raw) {
    if (!raw || !*raw || raw[0] == '#') return 0;
    if (strlen(raw) > 1024) return 0;
    if (!strncasecmp(raw, "javascript:", 11) || !strncasecmp(raw, "data:", 5) ||
        !strncasecmp(raw, "mailto:", 7)     || !strncasecmp(raw, "tel:", 4))
        return 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p; p++) {
        if (*p <= 0x20 || *p >= 0x7F) return 0;            /* ctrl/space/non-ascii */
        if (strchr("{}<>\"'`\\|^[] ", *p)) return 0;       /* invalid in a URL */
    }
    return 1;
}

/* path extension that is NOT an HTML page -> never recurse into it */
static int is_asset_ext(const char *url) {
    const char *end = url;
    while (*end && *end != '?' && *end != '#') end++;
    const char *dot = NULL;
    for (const char *p = url; p < end; p++) if (*p == '.') dot = p;
    if (!dot) return 0;
    size_t n = end - dot;
    char ext[12] = {0};
    if (n >= sizeof(ext)) return 0;
    for (size_t i = 0; i < n; i++) ext[i] = tolower((unsigned char)dot[i]);
    static const char *assets[] = {
        ".js",".css",".mp4",".webm",".mov",".avi",".svg",".json",".xml",".pdf",
        ".zip",".gz",".woff",".woff2",".ttf",".eot",".ico",".mp3",".wav",".m4a",
        ".map",".txt",".webp",NULL };
    for (int i = 0; assets[i]; i++) if (!strcmp(ext, assets[i])) return 1;
    return 0;
}

/* can_recurse: only <a href> links drive recursion; src/srcset are images only */
static void handle_ref(const char *base, const char *raw, int depth, int can_recurse) {
    if (!valid_ref(raw)) return;
    char *abs = resolve_url(base, raw);
    if (has_image_ext(abs)) {
        download_image(abs);
    } else if (can_recurse && g_recursive && depth < g_max_depth &&
               !is_asset_ext(abs)) {
        char h[512];
        host_of(abs, h, sizeof(h));
        if (strcmp(h, g_host) == 0 && !sl_contains(&g_visited, abs))
            crawl(abs, depth + 1);
    }
    free(abs);
}

/* read an attribute value starting at p (which points just after '=') */
static char *read_attr_value(const char **pp) {
    const char *p = *pp;
    while (*p && isspace((unsigned char)*p)) p++;
    char quote = 0;
    if (*p == '"' || *p == '\'') { quote = *p; p++; }
    const char *start = p;
    if (quote) while (*p && *p != quote) p++;
    else       while (*p && !isspace((unsigned char)*p) && *p != '>') p++;
    char *val = strndup(start, p - start);
    *pp = (*p ? p + 1 : p);
    return val;
}

static void scan_attr(const char *html, const char *base, int depth,
                      const char *attr, int is_srcset, int can_recurse) {
    size_t alen = strlen(attr);
    const char *p = html;
    while ((p = strcasestr(p, attr)) != NULL) {
        const char *q = p + alen;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q != '=') { p += alen; continue; }
        q++;
        char *val = read_attr_value(&q);
        if (is_srcset) {
            char *comma;
            for (char *tok = val; tok; tok = comma ? comma + 1 : NULL) {
                comma = strchr(tok, ',');
                if (comma) *comma = '\0';
                while (*tok && isspace((unsigned char)*tok)) tok++;
                char *sp = tok;
                while (*sp && !isspace((unsigned char)*sp)) sp++;
                *sp = '\0';
                if (*tok) handle_ref(base, tok, depth, can_recurse);
            }
        } else if (*val) {
            handle_ref(base, val, depth, can_recurse);
        }
        free(val);
        p = q;
    }
}

/* cheap sniff: a real HTML page starts (after whitespace) with '<' */
static int looks_html(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return *s == '<';
}

/* return a copy of html with <script>/<style> block CONTENTS removed, so that
 * inline JavaScript (which is full of ".href=" and ".src=") can't be mined
 * for fake links. */
static char *strip_blocks(const char *html) {
    char *out = malloc(strlen(html) + 1);
    if (!out) return NULL;
    size_t o = 0;
    const char *p = html;
    while (*p) {
        if (!strncasecmp(p, "<script", 7)) {
            const char *e = strcasestr(p, "</script>");
            p = e ? e + 9 : p + strlen(p);
            continue;
        }
        if (!strncasecmp(p, "<style", 6)) {
            const char *e = strcasestr(p, "</style>");
            p = e ? e + 8 : p + strlen(p);
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return out;
}

static void crawl(const char *url, int depth) {
    if (sl_contains(&g_visited, url)) return;
    sl_add(&g_visited, url);
    printf("[page] depth %d: %s\n", depth, url);

    char *html = http_get(url);
    if (!html) { fprintf(stderr, "  [skip] %s\n", url); return; }
    if (!looks_html(html)) { free(html); return; }   /* fetched, but not a page */

    char *clean = strip_blocks(html);                /* drop inline JS/CSS */
    free(html);
    if (!clean) return;

    /* only <a href> links drive recursion; src/srcset collect images only */
    scan_attr(clean, url, depth, "src",    0, 0);
    scan_attr(clean, url, depth, "href",   0, 1);
    scan_attr(clean, url, depth, "srcset", 1, 0);
    free(clean);
}

/* ----- main --------------------------------------------------------------- */
static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-r] [-l N] [-p PATH] URL\n", prog);
    exit(1);
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "rl:p:")) != -1) {
        switch (opt) {
            case 'r': g_recursive = 1; break;
            case 'l': g_max_depth = atoi(optarg); break;
            case 'p': g_out = optarg; break;
            default:  usage(argv[0]);
        }
    }
    if (optind != argc - 1) usage(argv[0]);
    char *url = argv[optind];

    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
        fprintf(stderr, "URL must start with http:// or https://\n");
        return 1;
    }
    if (g_max_depth < 0) { fprintf(stderr, "-l must be >= 0\n"); return 1; }

    static char outbuf[1024];
    snprintf(outbuf, sizeof(outbuf), "%s", g_out);
    size_t ol = strlen(outbuf);
    while (ol > 1 && outbuf[ol - 1] == '/') outbuf[--ol] = '\0';
    g_out = outbuf;

    if (make_dirs(g_out) != 0) { perror("mkdir"); return 1; }
    host_of(url, g_host, sizeof(g_host));
    sl_init(&g_visited);
    sl_init(&g_downloaded);

    crawl(url, 0);

    printf("\nDone. %d image(s) saved to %s\n", g_count, g_out);
    sl_free(&g_visited);
    sl_free(&g_downloaded);
    return 0;
}