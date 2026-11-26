# `spider.c` — Running curl as a subprocess

This version of the spider has **no libcurl**. Instead of linking a library, it
runs the `curl` command-line program for each HTTP request. That's why it builds
with a plain `cc` and no `-lcurl` — the only requirement is that the `curl`
binary exists on the machine (it does, on every 42 Linux box).

Running an external program from C is a four-verb dance: **`fork`** a child,
**`dup2`** to wire up its output, **`execvp`** to turn it into curl, and
**`waitpid`** to collect its result. This doc walks through both callers —
`http_get` (fetch a page into memory) and `http_download` (fetch an image
straight to disk) — and the OS mechanics behind them.

---

## 1. The idea: fork then exec

A running program is a *process*. To run another program you first make a copy
of yourself (`fork`), then have the copy *become* the other program (`execvp`).

```
          fork()
   parent ───────► parent  (keeps running the spider)
          └──────► child   (a clone, about to become curl)
                      │ execvp("curl", ...)
                      ▼
                    curl    (child's code+memory replaced by curl)
```

- **`fork()`** creates the child and returns **twice**: the child's PID in the
  parent, and `0` in the child. That return value is how each side knows who it
  is.
- **`execvp()`** replaces the child's program image with curl. On success it
  never returns — the child *is* curl now. It returns only if the launch failed.
- **`waitpid()`** lets the parent wait for the child to finish and read its exit
  status (and reap it, so it doesn't linger as a zombie).

The simplest caller, `http_download`, is exactly this with no pipe — curl writes
the file itself.

---

## 2. `http_download` — fetch straight to a file

```c
static int http_download(const char *url, const char *dest)
{
    pid_t pid = fork();
    if (pid < 0)                                 /* fork failed */
        return -1;
    if (pid == 0) {                              /* ---- child ---- */
        char *argv[] = { "curl", "-fsSL", "-A", UA,
                         "--max-time", "20", "-o", (char *)dest,
                         (char *)url, NULL };
        execvp("curl", argv);                    /* become curl */
        _exit(127);                              /* only if exec failed */
    }
    int status;                                  /* ---- parent ---- */
    if (waitpid(pid, &status, 0) < 0)            /* wait for curl, reap it */
        return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    return -1;
}
```

- The child builds an **argv array** — `curl -fsSL -A <ua> --max-time 20 -o <dest>
  <url>` — and execs it. `curl -o dest` writes the downloaded bytes to the file
  directly, so the parent doesn't have to handle any data.
- `_exit(127)` runs only if `execvp` failed (e.g. curl not installed). `127` is
  the conventional "command not found" code.
- The parent `waitpid`s, then reports success only if curl **exited normally and
  with status 0** — that's the `WIFEXITED(...) && WEXITSTATUS(...) == 0` test.

### The curl command line

The child builds and execs this command:

```
curl -fsSL -A arachnida-spider/1.0 --max-time 20 -o <dest> <url>
```

**The array mechanics.** `argv[0]` is `"curl"` (the program's own name); every
later element is one argument *token*; the terminating `NULL` is required so
`execvp` knows where the list ends. In `execvp`, the `v` means "arguments as a
**v**ector (array)" and the `p` means "search `PATH` for the `curl` binary" (so
the path isn't hard-coded). The `(char *)` casts on `dest`/`url` only drop
`const` to satisfy `execvp`'s `char * const argv[]` type — curl never modifies
them.

**No shell = no injection.** Because the arguments go to `execvp` as a real
array, **no shell parses them**. A URL containing `;`, `{`, spaces, or `$(...)`
is handed to curl as one opaque argument and can't inject a command. That safety
is the whole reason to use `fork`+`execvp` instead of `system("curl " + url)`,
which would let a malicious link run shell commands.

**The flags:**

| Option | Meaning | Why it's here |
|--------|---------|---------------|
| `-f` (`--fail`) | fail (non-zero exit, no body) on HTTP 4xx/5xx | a 404's error page isn't saved as if it were the image |
| `-s` (`--silent`) | no progress meter or chatter | keep output clean |
| `-S` (`--show-error`) | with `-s`, still print real errors | stay quiet without hiding failures |
| `-L` (`--location`) | follow 3xx redirects | http→https / CDN redirects resolve instead of returning an empty body |
| `-A UA` (`--user-agent`) | send `UA` as the `User-Agent` header | see below |
| `--max-time 20` | 20 s cap on the whole transfer | a slow/hung server can't stall the crawl |
| `-o <dest>` (`--output`) | write the body to the file `dest` | **this is the "save the image" step** — no pipe needed |

Note there is **no `--max-filesize`** here: images are expected to be large, so
the download isn't size-capped (only time-capped). That size cap lives in
`http_get` instead, where the body is a page to be parsed.

**Why a User-Agent at all.** `User-Agent` is an HTTP request header that
identifies the client. If you send none, curl sends its default `curl/8.x.x`,
which many servers treat as a bot — returning `403`, a CAPTCHA, or stripped-down
content — so you'd fetch an error page instead of the real bytes. Sending any
plausible UA avoids the laziest of those filters. The value is almost arbitrary
(the server just reads a string), so it's really a choice between an **honest
identifier** — `"arachnida-spider/1.0"` openly says "I am a crawler," transparent
and polite — and **browser impersonation** (`Mozilla/5.0 …`) to slip past bot
blocks, which is deceptive and can breach a site's terms. The honest identifier
is the right default for a school crawler; you can add contact info
(`"arachnida-spider/1.0 (+https://github.com/fcatala-/arachnida)"`) to be an even
better citizen.

---

## 3. `http_get` — fetch a page into memory through a pipe

Pages need to come *back* into the program so the scanner can read them. curl
writes to its standard output; the spider has to capture that. The tool for
"connect one process's output to another's input" is a **pipe**.

A pipe is a one-way channel with two ends: `fd[1]` is the **write** end, `fd[0]`
is the **read** end. Bytes written to `fd[1]` come out of `fd[0]`.

```
   pipe():   fd[1] ====================► fd[0]
             (write end)                (read end)

   wiring we want:

   ┌────────── child (curl) ──────────┐        ┌──────── parent ────────┐
   │ stdout ─► fd[1] ═══════════════════════════► fd[0] ─► read() loop  │
   └──────────────────────────────────┘        └────────────────────────┘
```

The code:

```c
static char *http_get(const char *url)
{
    int fd[2];
    if (pipe(fd) < 0)                            /* make the pipe */
        return NULL;
    pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        return NULL;
    }

    if (pid == 0) {                              /* ---- child ---- */
        close(fd[0]);                            /* won't read */
        dup2(fd[1], STDOUT_FILENO);              /* curl's stdout -> pipe */
        close(fd[1]);                            /* original copy no longer needed */
        char *argv[] = { "curl", "-fsSL", "-A", UA, "--max-time", "20",
                         "--max-filesize", "5000000", (char *)url, NULL };
        execvp("curl", argv);
        _exit(127);
    }

    close(fd[1]);                                /* ---- parent: won't write ---- */
    char *buf = NULL;
    char tmp[4096];
    size_t len = 0;
    size_t cap = 0;
    ssize_t n;
    while ((n = read(fd[0], tmp, sizeof(tmp))) > 0) {   /* drain the pipe */
        if (len + (size_t)n + 1 > cap) {                /* grow buffer */
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
        return strdup("");                        /* empty but successful body */
    buf[len] = '\0';
    return buf;
}
```

### The curl command line

The child builds and execs this command:

```
curl -fsSL -A arachnida-spider/1.0 --max-time 20 --max-filesize 5000000 <url>
```

**The array mechanics.** `argv[0]` is `"curl"` (the program's own name); every
later element is one argument *token*; the terminating `NULL` is required so
`execvp` knows where the list ends. In `execvp`, the `v` means "arguments as a
**v**ector (array)" and the `p` means "search `PATH` for the `curl` binary." The
`(char *)` cast on `url` only drops `const` to satisfy `execvp`'s
`char * const argv[]` type — curl never modifies it.

**No shell = no injection.** Because the arguments go to `execvp` as a real
array, **no shell parses them**. A URL containing `;`, `{`, spaces, or `$(...)`
is handed to curl as one opaque argument and can't inject a command. That is why
the fetch uses `fork`+`execvp`, never `system("curl " + url)`, which would let a
malicious link run shell commands.

**The flags:**

| Option | Meaning | Why it's here |
|--------|---------|---------------|
| `-f` (`--fail`) | fail (non-zero exit, no body) on HTTP 4xx/5xx | a 404's error page isn't parsed as if it were a real page |
| `-s` (`--silent`) | no progress meter or chatter | keep the piped bytes clean — only the body |
| `-S` (`--show-error`) | with `-s`, still print real errors | stay quiet without hiding failures |
| `-L` (`--location`) | follow 3xx redirects | http→https / CDN redirects resolve instead of returning an empty body |
| `-A UA` (`--user-agent`) | send `UA` as the `User-Agent` header | see below |
| `--max-time 20` | 20 s cap on the whole transfer | a slow/hung server can't stall the crawl |
| `--max-filesize 5000000` | abort if the body exceeds ~5 MB | the body is about to be parsed as a page — don't pull a giant blob into memory |

**The two differences from `http_download`:**

- **No `-o`.** curl writes the body to **stdout**, which `dup2` has redirected
  into the pipe, so the parent reads it back — that is the entire purpose of this
  function (a page must come *into* the program to be scanned).
- **`--max-filesize 5000000` is present.** A page is meant to be parsed, so an
  accidental huge binary should be refused rather than buffered into memory. One
  caveat: `--max-filesize` relies on the server sending a `Content-Length`
  header; without one, the cap can't be enforced before the download starts.

**Why a User-Agent at all.** `User-Agent` is an HTTP request header that
identifies the client. If you send none, curl sends its default `curl/8.x.x`,
which many servers treat as a bot — returning `403`, a CAPTCHA, or stripped-down
content — so you'd fetch an error page instead of the real HTML. Sending any
plausible UA avoids the laziest of those filters. The value is almost arbitrary
(the server just reads a string), so it's really a choice between an **honest
identifier** — `"arachnida-spider/1.0"` openly says "I am a crawler," transparent
and polite — and **browser impersonation** (`Mozilla/5.0 …`) to slip past bot
blocks, which is deceptive and can breach a site's terms. The honest identifier
is the right default for a school crawler; you can add contact info
(`"arachnida-spider/1.0 (+https://github.com/fcatala-/arachnida)"`) to be an even
better citizen.

### The `dup2` redirect

`dup2(fd[1], STDOUT_FILENO)` makes **file descriptor 1 (stdout)** refer to the
pipe's write end. After this, anything the child writes to stdout — which is
everything curl prints — goes into the pipe instead of the terminal. When
`execvp` replaces the child with curl, curl inherits this redirected stdout and
writes the page body straight into the pipe.

### The close discipline (the part everyone gets wrong)

A pipe sends end-of-file to the reader **only when every write end is closed**.
So each side must close the end it doesn't use:

- **Child:** closes `fd[0]` (never reads), and closes its copy of `fd[1]` right
  after `dup2` (the redirected stdout still points at the write end — the extra
  descriptor is redundant).
- **Parent:** closes `fd[1]` (never writes).

If the parent forgot to close `fd[1]`, there would still be an open write end, so
`read` would block forever waiting for more data and the program would hang. This
is the classic pipe bug. The spider closes exactly the right ends.

### The read loop

```c
while ((n = read(fd[0], tmp, sizeof(tmp))) > 0) { ... }
```

`read` returns however many bytes are available right now (up to 4096) — often
fewer than the full page. You **must loop**: append each chunk, keep reading,
until `read` returns `0` (EOF — curl finished and its write end closed). The
buffer grows geometrically (`cap *= 2`) with `realloc`, and each chunk is
appended with `memcpy`. Treating a single `read` as "the whole page" is a
classic bug; the loop handles arbitrarily large pages (up to curl's
`--max-filesize` cap).

### Reap and check

After EOF, the parent `waitpid`s to reap curl and inspect its status, returning
the buffer only if curl exited cleanly. `if (!buf) return strdup("")` handles a
successful-but-empty response (a zero-byte page), so callers never get a
spurious `NULL`.

---

## 4. The whole sequence (http_get)

```
 parent                              child
 ──────                              ─────
 pipe(fd)        make read+write ends
 fork() ───────────────────────────► (clone created)
 close(fd[1])   drop write end       close(fd[0])    drop read end
 read(fd[0]) …  (blocks, waiting)     dup2(fd[1], stdout)
                                      close(fd[1])
                                      execvp("curl", …)  ── becomes curl ──►
                                      curl writes page ──► fd[1] ──► pipe
 …receives chunks ◄───────────────────────────────────────────────┘
 read returns 0 (EOF)  ◄───────────── curl exits, its stdout(write end) closes
 waitpid(pid)  ◄───────────────────── child becomes reapable
 check WIFEXITED / WEXITSTATUS
 return buffer
```

The parent blocks in `read` while curl runs, wakes as data arrives, and sees EOF
the instant curl exits and the last write end closes.

---

## 5. Gotchas

- **`fork` returns twice.** `pid == 0` is the child, `pid > 0` is the parent,
  `pid < 0` is failure. Mixing these up runs the wrong code in the wrong process.
- **Use `_exit`, not `exit`, in the child after a failed `exec`.** `exit` flushes
  the `stdio` buffers the child inherited from the parent, which can re-print the
  parent's pending output. `_exit` skips that.
- **Close every pipe end you don't use, in both processes.** A leftover write end
  means the reader never sees EOF and `read` blocks forever — the program hangs.
- **`read` can return fewer bytes than asked** at any time; always loop until
  `0`. `ssize_t` (signed) is important because `-1` signals an error.
- **Always `waitpid`** every child, or finished children accumulate as zombies.
  The spider reaps in both callers, which is also why valgrind shows no leaked
  processes.
- **`WEXITSTATUS` is only valid after `WIFEXITED` is true.** A child killed by a
  signal didn't "exit with a code"; check `WIFEXITED` first.
- **Error cleanup must close fds and reap.** Notice the mid-loop `realloc`
  failure path does `free(buf); close(fd[0]); waitpid(pid, NULL, 0);` — it
  doesn't just `return NULL` and leak a descriptor and a zombie.

---

## 6. libcurl vs the subprocess approach

| | libcurl (linked library) | curl subprocess (this version) |
|---|---|---|
| Build | needs `curl/curl.h` + `-lcurl` | plain `cc`, no headers |
| Runtime dep | the libcurl shared library | the `curl` binary |
| HTTP done by | function calls in-process | a separate process |
| Data back | a write callback into memory | a pipe you `read` |
| Cost | — | one `fork`+`exec` per request |

The subprocess approach exists because the 42 cluster had no libcurl headers and
no root to install them, but did have the `curl` program. It trades a little
per-request overhead (spawning a process) for zero build dependencies. The spider
logic above it — crawling, scanning, resolution — is identical either way; only
these two fetch functions differ.

---

## 7. Build it yourself, in order

1. **`http_download` first.** `fork` + `execvp` curl with `-o file`, then
   `waitpid`. Test: does the file appear? Does a bad URL return non-zero?
2. **Add the status check.** `WIFEXITED` + `WEXITSTATUS`; confirm a 404 is
   reported as failure.
3. **`http_get` — pipe setup.** `pipe` + `fork` + `dup2(fd[1], STDOUT_FILENO)` +
   close the right ends + `execvp`. Have the parent `read` once and print what it
   got.
4. **The read loop.** Loop until EOF, growing the buffer. Test on a page bigger
   than one 4096 chunk.
5. **Reap + edge cases.** `waitpid`, the empty-body `strdup("")`, and the
   error-path cleanup.

Test each step with a local `python3 -m http.server`: step 1 downloads a known
file, step 3 should print a page's HTML to your terminal, step 4 should handle a
large page without truncation.
