# `spider.c` — C library functions reference

A function-by-function tutorial for the curl-subprocess image spider, grouped by
header. Each entry gives the prototype, what it does, what it returns, the line
from `spider.c` that uses it, and a common mistake to avoid.

The headers the program includes:

```c
#define _GNU_SOURCE        /* needed for strcasestr / strndup */
#include <stdio.h>         /* printf, fprintf, snprintf, sprintf, perror, remove */
#include <stdlib.h>        /* malloc, realloc, free, exit, atoi */
#include <string.h>        /* str*/mem* family                   */
#include <strings.h>       /* strncasecmp                        */
#include <ctype.h>         /* tolower, isspace                   */
#include <unistd.h>        /* getopt, fork, execvp, pipe, dup2, read, close, _exit */
#include <sys/stat.h>      /* mkdir, stat, struct stat           */
#include <sys/types.h>     /* pid_t, size_t, ssize_t             */
#include <sys/wait.h>      /* waitpid, WIFEXITED, WEXITSTATUS     */
#include <errno.h>         /* errno, EEXIST                      */
```

`_GNU_SOURCE` must be defined
*before* any include, because it switches on the GNU extensions `strcasestr`
and `strndup`. That's why the `Makefile` also passes `-D_GNU_SOURCE`.

---

## `<stdio.h>` — input / output

### printf
- **Library:** `#include <stdio.h>`
- **Syntax:** `int printf(const char *format, ...);`
- **Description:** Writes formatted text to standard output. `%d` = int, `%s` = C string, `%u` = unsigned, `%c` = char, `\n` = newline.
- **Returns:** The number of characters printed, or a negative value on error.
- **In spider.c:** `printf("[page] depth %d: %s\n", depth, url);` — the progress line for each page visited.
- **Gotcha:** The format string must match the arguments. `printf("%s", 42)` (passing an int where `%s` expects a `char *`) is undefined behaviour and usually crashes — the compiler only catches it with `-Wformat` (included in `-Wall`).

### fprintf
- **Library:** `#include <stdio.h>`
- **Syntax:** `int fprintf(FILE *stream, const char *format, ...);`
- **Description:** Like `printf`, but you choose the stream. The spider sends progress to `stdout` and errors to `stderr`.
- **Returns:** Characters written, or negative on error.
- **In spider.c:** `fprintf(stderr, "  [skip] %s\n", url);` — a skip notice that won't pollute `stdout` if you pipe the image list to a file.
- **Gotcha:** `stderr` is unbuffered and `stdout` is line-buffered; when you redirect both to the same file the lines can interleave out of order. That's why in the real-site runs the `curl: (22)` error sometimes appeared *before* its `[page]` line.

### snprintf
- **Library:** `#include <stdio.h>`
- **Syntax:** `int snprintf(char *str, size_t size, const char *format, ...);`
- **Description:** Formats into the buffer `str` but never writes more than `size` bytes (including the terminating `'\0'`). The safe way to build strings.
- **Returns:** The number of characters that *would* have been written if the buffer were big enough (so a value `>= size` means it was truncated).
- **In spider.c:** `snprintf(dest, sizeof(dest), "%s/%s", g_out, name);` — builds the output path, capped at `sizeof(dest)`.
- **Gotcha:** The return value is the *intended* length, not the written length. The code guards against this in `normalize_in_place`: `for (... && off < sizeof(rebuilt); ...)` — without that check, accumulating the return value could run `off` past the buffer.

### sprintf
- **Library:** `#include <stdio.h>`
- **Syntax:** `int sprintf(char *str, const char *format, ...);`
- **Description:** Same as `snprintf` but with **no size limit** — it writes until the format is done.
- **Returns:** Number of characters written (excluding `'\0'`).
- **In spider.c:** `sprintf(res, "%.*s:%s", (int)sl, base, r);` in `resolve_url` — safe here only because `res` was `malloc`'d to exactly the needed length just above.
- **Gotcha:** `sprintf` is the classic buffer-overflow function; it's only safe when you've *proven* the destination is large enough (as the `malloc` sizing does here). Prefer `snprintf` everywhere else. Note `%.*s` prints at most a given number of bytes — the `(int)sl` argument supplies that length.

### perror
- **Library:** `#include <stdio.h>`
- **Syntax:** `void perror(const char *s);`
- **Description:** Prints your message `s`, then `": "`, then a human-readable text for the current `errno` (e.g. "Permission denied").
- **Returns:** Nothing.
- **In spider.c:** `perror("mkdir");` — reports *why* directory creation failed.
- **Gotcha:** It reads the global `errno`, which is only meaningful **immediately** after a failed call. Any library call in between can overwrite `errno`, so call `perror` right away.

### remove
- **Library:** `#include <stdio.h>`
- **Syntax:** `int remove(const char *pathname);`
- **Description:** Deletes a file (for a regular file it's equivalent to `unlink`).
- **Returns:** `0` on success, `-1` on error (sets `errno`).
- **In spider.c:** `remove(dest);` — if the image download failed, delete the empty/partial file curl may have left.
- **Gotcha:** Removing a file that doesn't exist returns `-1`; the code ignores the result here because a missing file is fine (nothing to clean up).

---

## `<stdlib.h>` — general utilities

### malloc
- **Library:** `#include <stdlib.h>`
- **Syntax:** `void *malloc(size_t size);`
- **Description:** Allocates `size` bytes of uninitialised heap memory.
- **Returns:** A pointer to the block, or `NULL` if the allocation failed.
- **In spider.c:** `char *out = malloc(strlen(html) + 1);` in `strip_blocks` — room for the cleaned copy plus the `'\0'`.
- **Gotcha:** The memory is **not** zeroed (use `calloc` for that), and you must always check for `NULL` before using it — the spider does: `if (!out) return NULL;`. The `+ 1` for the terminator is the single most common off-by-one in C strings.

### realloc
- **Library:** `#include <stdlib.h>`
- **Syntax:** `void *realloc(void *ptr, size_t size);`
- **Description:** Grows (or shrinks) a previously allocated block to `size` bytes, copying the old contents. Used to build a growable buffer/array.
- **Returns:** A pointer to the resized block (possibly **moved**), or `NULL` on failure (the old block is left intact).
- **In spider.c:** `char *p = realloc(buf, cap);` in `http_get` — enlarges the page buffer as more bytes stream in from curl.
- **Gotcha:** Assign the result to a *temporary* first (`p`), then check it, then replace `buf`. Writing `buf = realloc(buf, ...)` leaks the original block if `realloc` returns `NULL`. The spider does it correctly: `if (!p) { free(buf); ... } buf = p;`.

### free
- **Library:** `#include <stdlib.h>`
- **Syntax:** `void free(void *ptr);`
- **Description:** Releases memory obtained from `malloc`/`realloc`/`strdup`/`strndup`.
- **Returns:** Nothing.
- **In spider.c:** `free(abs);` at the end of `handle_ref` — every `resolve_url` result is freed once used.
- **Gotcha:** Three classic bugs: using memory after `free` (use-after-free), freeing twice (double-free), and forgetting to free (leak). `free(NULL)` is explicitly safe and does nothing. Running the program under `valgrind` is how the spider was proven leak-free.

### exit
- **Library:** `#include <stdlib.h>`
- **Syntax:** `void exit(int status);`
- **Description:** Ends the whole program immediately, flushing `stdio` buffers and running `atexit` handlers. `0` means success; non-zero means error.
- **Returns:** Never returns.
- **In spider.c:** `exit(1);` inside `usage` — bad arguments end the program with a failure code.
- **Gotcha:** `exit` flushes buffers; its sibling `_exit` (below) does **not**. In a `fork`ed child after a failed `exec` you want `_exit`, not `exit`, to avoid flushing the parent's buffered output twice.

### atoi
- **Library:** `#include <stdlib.h>`
- **Syntax:** `int atoi(const char *nptr);`
- **Description:** Converts the leading decimal digits of a string to an `int`.
- **Returns:** The integer value, or `0` if the string isn't a number.
- **In spider.c:** `g_max_depth = atoi(optarg);` — turns the `-l` argument text into a depth number.
- **Gotcha:** `atoi` can't tell "0" from "not a number" (both give 0) and ignores overflow. For stricter parsing use `strtol`, which reports where parsing stopped and detects range errors. Here it's acceptable because the depth is sanity-checked afterwards (`if (g_max_depth < 0) ...`).

---

## `<string.h>` — strings and memory

### strlen
- **Library:** `#include <string.h>`
- **Syntax:** `size_t strlen(const char *s);`
- **Description:** Counts the bytes in a C string *before* the terminating `'\0'`.
- **Returns:** The length as a `size_t` (unsigned).
- **In spider.c:** `malloc(strlen(html) + 1)` — size a buffer to hold a copy of the string.
- **Gotcha:** It walks the whole string every call (O(n)); don't call it repeatedly in a loop condition. And it requires a real `'\0'`-terminated string — calling it on non-terminated bytes reads past the end.

### strcmp
- **Library:** `#include <string.h>`
- **Syntax:** `int strcmp(const char *s1, const char *s2);`
- **Description:** Compares two strings byte by byte.
- **Returns:** `0` if equal, negative if `s1 < s2`, positive if `s1 > s2` (lexicographic).
- **In spider.c:** `if (strcmp(l->items[i], s) == 0) return 1;` — the "have I already seen this URL?" check in `sl_contains`.
- **Gotcha:** Equal strings give `0`, which is *falsy* — `if (strcmp(a, b))` means "if **different**". Reading it as "if equal" is a very common bug. The spider writes `== 0` explicitly to stay clear.

### strncmp
- **Library:** `#include <string.h>`
- **Syntax:** `int strncmp(const char *s1, const char *s2, size_t n);`
- **Description:** Like `strcmp`, but compares at most the first `n` bytes — perfect for testing a prefix.
- **Returns:** `0` if the first `n` bytes match, else non-zero.
- **In spider.c:** `if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))` — reject a URL that starts with neither scheme.
- **Gotcha:** Same truthiness trap as `strcmp` (`0` = match). Also make sure `n` matches the prefix length you mean (`"http://"` is 7 characters, `"https://"` is 8).

### strstr
- **Library:** `#include <string.h>`
- **Syntax:** `char *strstr(const char *haystack, const char *needle);`
- **Description:** Finds the first occurrence of the substring `needle` inside `haystack`.
- **Returns:** A pointer to the start of the match inside `haystack`, or `NULL` if not found.
- **In spider.c:** `const char *p = strstr(url, "://");` — locate the scheme separator so the host can be read just after it (`p + 3`).
- **Gotcha:** Unlike `strpbrk`, the argument is a *substring*, not a set of characters. Always check for `NULL` before dereferencing — the code does `if (!p) return;` in `host_of`.

### strchr
- **Library:** `#include <string.h>`
- **Syntax:** `char *strchr(const char *s, int c);`
- **Description:** Finds the first occurrence of a single character `c` in `s`.
- **Returns:** A pointer to that character, or `NULL` if it isn't present.
- **In spider.c:** `char *path = strchr(p, '/');` in `normalize_in_place` — find where the path begins after the host (and if there's none, the URL is host-only).
- **Gotcha:** `c` is passed as an `int` but treated as a `char`. Also, `strchr(s, '\0')` legitimately returns a pointer to the terminator — occasionally useful, occasionally surprising.

### strrchr
- **Library:** `#include <string.h>`
- **Syntax:** `char *strrchr(const char *s, int c);`
- **Description:** Like `strchr`, but finds the **last** occurrence (the extra `r` is "reverse").
- **Returns:** Pointer to the last match, or `NULL`.
- **In spider.c:** `char *dot = strrchr(base, '.');` in `download_image` — split a filename at its last dot to insert `_1`, `_2`, … before the extension on a name clash.
- **Gotcha:** Use `strrchr` (last dot), not `strchr` (first dot), for extensions — `archive.tar.gz` must split at the final dot. Still check for `NULL`: a name with no dot has no extension.

### strpbrk
- **Library:** `#include <string.h>`
- **Syntax:** `char *strpbrk(const char *s, const char *accept);`
- **Description:** Finds the first byte in `s` that matches **any** byte from the set `accept`.
- **Returns:** Pointer to that byte, or `NULL` if none of the `accept` bytes occur.
- **In spider.c:** `char *query = strpbrk(path, "?#");` — find where a `?query` or `#fragment` begins, whichever comes first, so the path can be cut there.
- **Gotcha:** `accept` is a *set of single characters*, not a substring. `strpbrk("abc", "xy")` looks for `x` or `y`, not the text `"xy"`. Use it when "any of these delimiters" is what you mean.

### strcspn
- **Library:** `#include <string.h>`
- **Syntax:** `size_t strcspn(const char *s, const char *reject);`
- **Description:** Measures the length of the initial run of `s` that contains **none** of the `reject` characters (the "complement span"). Effectively "how far until one of these delimiters".
- **Returns:** That length (a `size_t`). If no delimiter is present, it returns `strlen(s)`.
- **In spider.c:** `size_t n = strcspn(url, "?#");` in `basename_of` — length of the path part before any query/fragment, so the filename search stays within it.
- **Gotcha:** It returns a **length**, not a pointer (its cousin `strspn` measures the run that *does* consist of the set). `strpbrk` gives you the pointer; `strcspn` gives you the offset — pick whichever the surrounding code needs.

### strdup
- **Library:** `#include <string.h>` (POSIX / `_GNU_SOURCE`)
- **Syntax:** `char *strdup(const char *s);`
- **Description:** Allocates new memory and copies the string `s` into it — a `malloc` + `strcpy` in one call.
- **Returns:** Pointer to the new copy (you must `free` it), or `NULL` on allocation failure.
- **In spider.c:** `l->items[l->count++] = strdup(s);` in `sl_add` — the set stores its *own* copy of each URL, independent of the caller's buffer.
- **Gotcha:** The result is heap memory you own — every `strdup` needs a matching `free` (that's what `sl_free` does). Forgetting is a leak.

### strndup
- **Library:** `#include <string.h>` (POSIX / `_GNU_SOURCE`)
- **Syntax:** `char *strndup(const char *s, size_t n);`
- **Description:** Like `strdup`, but copies at most `n` bytes and always adds a `'\0'`. Good for copying a *slice* of a larger string.
- **Returns:** Pointer to the new (≤ `n`+1 byte) copy, or `NULL`.
- **In spider.c:** `char *val = strndup(start, p - start);` in `read_attr_value` — copy exactly the attribute value between the quotes, without the rest of the page.
- **Gotcha:** The pointer-difference `p - start` is the length of the slice; this idiom (`strndup(start, end - start)`) is how you extract a token from inside a buffer. Needs `_GNU_SOURCE` or a POSIX-2008 system.

### strtok
- **Library:** `#include <string.h>`
- **Syntax:** `char *strtok(char *str, const char *delim);`
- **Description:** Splits a string into tokens separated by any of the `delim` characters. First call passes the string; subsequent calls pass `NULL` to continue.
- **Returns:** Pointer to the next token, or `NULL` when there are no more.
- **In spider.c:** `for (char *tok = strtok(tmp, "/"); tok; tok = strtok(NULL, "/"))` in `normalize_in_place` — walk a path one `/`-separated segment at a time.
- **Gotcha:** `strtok` **modifies its input** (it writes `'\0'` over delimiters) and keeps **hidden static state**, so it isn't reentrant and can't be nested or used across threads. That's exactly why the code calls it on a disposable `strdup`'d copy (`tmp`), never on the original URL.

### strcpy
- **Library:** `#include <string.h>`
- **Syntax:** `char *strcpy(char *dest, const char *src);`
- **Description:** Copies the string `src` (including `'\0'`) into `dest`.
- **Returns:** `dest`.
- **In spider.c:** `strcpy(path, rebuilt);` in `normalize_in_place` — write the normalized path back over the original, which is safe because normalization only ever makes it **shorter**.
- **Gotcha:** `strcpy` does no bounds checking — if `src` is longer than `dest`, it overflows. It's only safe when you can prove `dest` is big enough (as the "only shortens" argument does here). Otherwise use `snprintf`.

### strcat
- **Library:** `#include <string.h>`
- **Syntax:** `char *strcat(char *dest, const char *src);`
- **Description:** Appends `src` onto the end of the string already in `dest`.
- **Returns:** `dest`.
- **In spider.c:** `strcat(url, qsave);` in `normalize_in_place` — re-attach the saved `?query`/`#fragment` after the path has been rebuilt.
- **Gotcha:** Like `strcpy`, no bounds checking, **and** it re-scans `dest` from the start to find its end every call — repeatedly `strcat`-ing in a loop is accidentally O(n²). Here it's a single append onto a buffer known to have room.

### strcasestr
- **Library:** `#include <string.h>` (**GNU extension — needs `_GNU_SOURCE`**)
- **Syntax:** `char *strcasestr(const char *haystack, const char *needle);`
- **Description:** Case-insensitive `strstr` — finds `needle` in `haystack` ignoring upper/lower case.
- **Returns:** Pointer to the match, or `NULL`.
- **In spider.c:** `while ((p = strcasestr(p, attr)) != NULL)` in `scan_attr` — find every `src`/`href`/`srcset` regardless of how the HTML cased it (`HREF`, `Href`, `href` all match).
- **Gotcha:** It's a GNU extension: without `_GNU_SOURCE` (or on non-glibc systems) it won't be declared and the build fails. That's the whole reason for the `#define _GNU_SOURCE` and `-D_GNU_SOURCE`.

### memcpy
- **Library:** `#include <string.h>`
- **Syntax:** `void *memcpy(void *dest, const void *src, size_t n);`
- **Description:** Copies exactly `n` raw bytes from `src` to `dest`. Works on any data, not just strings — it doesn't care about `'\0'`.
- **Returns:** `dest`.
- **In spider.c:** `memcpy(buf + len, tmp, n);` in `http_get` — append the `n` bytes just read from curl onto the growing page buffer.
- **Gotcha:** The regions must **not overlap** (use `memmove` if they can). And `memcpy` copies a fixed byte count — it won't stop at a `'\0'`, which is exactly why it's right for binary/streamed data where `strcpy` would stop early.

---

## `<strings.h>` — case-insensitive compare

### strncasecmp
- **Library:** `#include <strings.h>`
- **Syntax:** `int strncasecmp(const char *s1, const char *s2, size_t n);`
- **Description:** Compares the first `n` bytes of two strings, ignoring case.
- **Returns:** `0` if the first `n` bytes match case-insensitively, else non-zero.
- **In spider.c:** `if (!strncasecmp(raw, "javascript:", 11) ...)` in `valid_ref` — reject `javascript:`/`JavaScript:`/`DATA:` scheme links whatever their case.
- **Gotcha:** It lives in `<strings.h>` (plural), not `<string.h>` — forgetting that include gives an implicit-declaration error under `-Werror`. Same `0` = match truthiness trap as `strcmp`.

---

## `<ctype.h>` — character classification

### tolower
- **Library:** `#include <ctype.h>`
- **Syntax:** `int tolower(int c);`
- **Description:** If `c` is an uppercase letter, returns its lowercase form; otherwise returns `c` unchanged.
- **Returns:** The (possibly) lowercased character, as an `int`.
- **In spider.c:** `ext[i] = tolower((unsigned char)dot[i]);` in `has_image_ext` — normalize an extension so `.JPG` and `.jpg` compare equal.
- **Gotcha:** Always cast the argument to `unsigned char` first. A plain `char` can be negative (for bytes ≥ 0x80), and passing a negative value other than `EOF` to `tolower` is undefined behaviour. The code casts every time.

### isspace
- **Library:** `#include <ctype.h>`
- **Syntax:** `int isspace(int c);`
- **Description:** Tests whether `c` is whitespace (space, tab, newline, carriage return, vertical tab, form feed).
- **Returns:** Non-zero (true) if `c` is whitespace, `0` otherwise.
- **In spider.c:** `while (*p && isspace((unsigned char)*p)) p++;` in `read_attr_value` — skip spaces between `=` and the attribute value.
- **Gotcha:** Same `unsigned char` cast rule as `tolower`. Also it returns *non-zero*, not necessarily `1`, so compare with `if (isspace(c))`, never `if (isspace(c) == 1)`.

---

## `<unistd.h>` — option parsing

### getopt
- **Library:** `#include <unistd.h>`
- **Syntax:** `int getopt(int argc, char * const argv[], const char *optstring);`
  Globals it uses: `char *optarg; int optind;`
- **Description:** Parses single-letter command-line options. In `optstring`, a letter alone is a flag; a letter followed by `:` takes an argument (delivered in `optarg`).
- **Returns:** The option letter each call, `-1` when options are exhausted, or `'?'` for an unknown option / missing argument.
- **In spider.c:** `while ((opt = getopt(argc, argv, "rl:p:")) != -1)` — `r` is a flag, `l` and `p` each take an argument; after the loop, `optind` indexes the first non-option (the URL).
- **Gotcha:** `optstring` `"rl:p:"` means only `l` and `p` take arguments — the `:` placement is the whole specification. After the loop you must read the positional argument via `argv[optind]`, and check `optind == argc - 1` to ensure exactly one URL was given (the code does).

---

## `<unistd.h>` / `<sys/wait.h>` — processes (the curl subprocess)

These six functions are how the spider runs `curl` without a shell. The pattern
is always: `fork` a child, in the child redirect output and `execvp` curl, in
the parent read the result and `waitpid` for the child.

### fork
- **Library:** `#include <unistd.h>`
- **Syntax:** `pid_t fork(void);`
- **Description:** Creates a new process that is a near-exact copy of the caller. Execution continues in **both** processes from the point of the call.
- **Returns:** In the **parent**, the child's PID (> 0); in the **child**, `0`; on failure, `-1` in the parent and no child is made.
- **In spider.c:** `pid_t pid = fork();` in `http_get`/`http_download` — split off a child that will become curl.
- **Gotcha:** The return value is how each side knows who it is: `if (pid == 0)` is the child branch, else the parent. Forgetting to handle `-1` (fork failure) leaks the intent; the code returns an error in that case.

### execvp
- **Library:** `#include <unistd.h>`
- **Syntax:** `int execvp(const char *file, char *const argv[]);`
- **Description:** Replaces the current process image with a new program, searching `PATH` for `file`. `argv` is a `NULL`-terminated array of arguments (`argv[0]` is the program name).
- **Returns:** On success it **does not return** (the process is now curl); it returns `-1` only if the exec failed.
- **In spider.c:** `execvp("curl", argv);` with `argv = { "curl", "-fsSL", ..., (char *)url, NULL }`.
- **Gotcha:** Because arguments are passed as a real array, **no shell parses them** — so a URL containing `;`, `{`, or spaces can't inject a command. That safety is the whole reason to use `execvp` over `system()`. Any code after `execvp` runs **only if it failed**, hence the `_exit(127)` right below it.

### _exit
- **Library:** `#include <unistd.h>`
- **Syntax:** `void _exit(int status);`
- **Description:** Terminates the process **immediately**, without flushing `stdio` buffers or running `atexit` handlers.
- **Returns:** Never returns.
- **In spider.c:** `_exit(127);` — reached only in the child if `execvp` failed to launch curl.
- **Gotcha:** Use `_exit` (not `exit`) in a forked child after a failed `exec`. `exit` would flush the `stdio` buffers the child *inherited* from the parent, printing the parent's pending output a second time. `127` is the conventional "command not found" status.

### pipe
- **Library:** `#include <unistd.h>`
- **Syntax:** `int pipe(int pipefd[2]);`
- **Description:** Creates a one-way byte channel. It fills `pipefd[0]` with the **read** end and `pipefd[1]` with the **write** end.
- **Returns:** `0` on success, `-1` on error.
- **In spider.c:** `if (pipe(fd) < 0) return NULL;` in `http_get` — curl writes the page into the pipe, the parent reads it out.
- **Gotcha:** Remember the direction: index `0` = read, index `1` = write. Each process should **close the end it doesn't use** — leaving the write end open in the reader means the reader never sees end-of-file and `read` blocks forever. The code closes `fd[1]` in the parent and `fd[0]` in the child.

### dup2
- **Library:** `#include <unistd.h>`
- **Syntax:** `int dup2(int oldfd, int newfd);`
- **Description:** Makes `newfd` refer to the same open file as `oldfd`, closing `newfd` first if it was open. Used to redirect a standard stream.
- **Returns:** `newfd` on success, `-1` on error.
- **In spider.c:** `dup2(fd[1], STDOUT_FILENO);` in the child — curl's standard output now goes into the pipe instead of the terminal.
- **Gotcha:** After `dup2`, the original `fd[1]` is still open and should be closed (the code does `close(fd[1])` next), or the pipe keeps an extra writer and EOF never arrives. `STDOUT_FILENO` is the constant `1`.

### read
- **Library:** `#include <unistd.h>`
- **Syntax:** `ssize_t read(int fd, void *buf, size_t count);`
- **Description:** Reads up to `count` raw bytes from `fd` into `buf`.
- **Returns:** The number of bytes actually read (may be **fewer** than asked), `0` at end-of-file, or `-1` on error. Note the signed `ssize_t`.
- **In spider.c:** `while ((n = read(fd[0], tmp, sizeof(tmp))) > 0) { ... }` — drain the pipe in chunks until curl closes it.
- **Gotcha:** `read` can return fewer bytes than requested even mid-stream; you must loop until it returns `0` (EOF). Treating one `read` as "the whole thing" is a classic bug. The loop condition `> 0` handles both EOF (`0`) and error (`-1`).

### close
- **Library:** `#include <unistd.h>`
- **Syntax:** `int close(int fd);`
- **Description:** Closes a file descriptor, releasing it.
- **Returns:** `0` on success, `-1` on error.
- **In spider.c:** `close(fd[1]);` (parent closes the write end so it will see EOF once curl finishes).
- **Gotcha:** Closing the wrong end, or forgetting to close an end, is the usual cause of a pipe program that hangs. Each descriptor should be closed exactly once.

### waitpid
- **Library:** `#include <sys/wait.h>`
- **Syntax:** `pid_t waitpid(pid_t pid, int *wstatus, int options);`
- **Description:** Waits for a child process to change state (normally: to finish) and collects its exit status into `*wstatus`.
- **Returns:** The PID of the child that changed state, or `-1` on error.
- **In spider.c:** `if (waitpid(pid, &status, 0) < 0) return -1;` — wait for curl to finish and reap it.
- **Gotcha:** You **must** reap every child, or finished children pile up as "zombies". `0` for `options` means "block until it exits". Pair it with the macros below to read the status — don't compare `status` to numbers directly.

### WIFEXITED / WEXITSTATUS
- **Library:** `#include <sys/wait.h>`
- **Syntax:** `int WIFEXITED(int wstatus);` and `int WEXITSTATUS(int wstatus);` (macros)
- **Description:** `WIFEXITED` is true if the child ended normally (via `return`/`exit`). `WEXITSTATUS` then extracts the low 8 bits it exited with.
- **Returns:** `WIFEXITED`: non-zero/zero. `WEXITSTATUS`: the exit code `0`–`255`.
- **In spider.c:** `return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;` — success only if curl exited normally **and** with code 0.
- **Gotcha:** Only call `WEXITSTATUS` **after** `WIFEXITED` is true — otherwise the bits are meaningless (the child might have been killed by a signal, which `WIFSIGNALED` detects instead). The raw `status` is a packed bitfield, never a plain exit code.

---

## `<sys/stat.h>` — filesystem

### mkdir
- **Library:** `#include <sys/stat.h>`
- **Syntax:** `int mkdir(const char *pathname, mode_t mode);`
- **Description:** Creates one directory with permission bits `mode` (e.g. `0755` = `rwxr-xr-x`).
- **Returns:** `0` on success, `-1` on error (sets `errno`).
- **In spider.c:** `if (mkdir(tmp, 0755) && errno != EEXIST) return -1;` in `make_dirs` — create each path component, treating "already exists" as fine.
- **Gotcha:** `mkdir` makes **one** level only — it fails with `ENOENT` if a parent is missing. That's why `make_dirs` splits the path on `/` and creates each segment in turn (a hand-rolled `mkdir -p`). The `0755` is octal — the leading `0` matters.

### stat
- **Library:** `#include <sys/stat.h>`
- **Syntax:** `int stat(const char *pathname, struct stat *statbuf);`
- **Description:** Fills `statbuf` with metadata about a file (size, times, type, permissions…). The spider only uses it to test *existence*.
- **Returns:** `0` if the file exists and was stat-ed, `-1` otherwise.
- **In spider.c:** `if (stat(dest, &st) == 0) { ... }` in `download_image` — if a file of that name already exists, pick `name_1`, `name_2`, … instead of clobbering it.
- **Gotcha:** You must pass the address of a `struct stat` you own (`&st`). Using `stat` purely as an existence test is fine, but note there's a race window between checking and creating — acceptable here, but never rely on it for security-sensitive code.

---

## `<errno.h>` — error reporting

### errno / EEXIST
- **Library:** `#include <errno.h>`
- **Syntax:** `extern int errno;` and the macro `EEXIST`
- **Description:** `errno` is a thread-local integer that library calls set to a specific code when they fail. `EEXIST` is the code for "file already exists".
- **Returns:** Not a function — a variable you read after a failed call.
- **In spider.c:** `if (mkdir(tmp, 0755) && errno != EEXIST) return -1;` — ignore the "already exists" failure, report any other.
- **Gotcha:** `errno` is only valid **immediately after** a call that is documented to set it, and only when that call signalled failure (usually by returning `-1`). Don't read it after an *unrelated* call — anything in between may have changed it.

---

## How it all fits (reading order)

Natural order to understand the pieces:

1. **Arguments** — `getopt` reads `-r/-l/-p`, `atoi` parses the depth, `strncmp` validates the scheme.
2. **One HTTP fetch** — `fork` + `pipe` + `dup2` + `execvp` run curl; the parent loops on `read` and grows a buffer with `realloc`/`memcpy`; `waitpid` + `WIFEXITED` check the result.
3. **Scan the HTML** — `strcasestr` finds each `src`/`href`, `strndup` copies the value, `isspace` trims it.
4. **Resolve each URL** — `strstr`/`strchr`/`strncmp` classify the reference, `malloc`+`sprintf` build the absolute URL, `strtok`/`strpbrk`/`strcpy`/`strcat` normalize the path.
5. **Download or recurse** — `strcspn`/`strrchr` build a safe filename, `stat` avoids clobbering, a second curl subprocess saves the file; or `strcmp` on the host decides whether to recurse.
6. **Cleanup** — every `strdup`/`malloc` is matched by `free`; `make_dirs` used `mkdir`+`errno`; `perror`/`fprintf` report problems.

Build it piece by piece in that order, testing each stage against a tiny local
`python3 -m http.server`

See instructions in next section

## Test page

To ilustrate the above section, you can find a small bash script and a set of 3 images in the test_page folder.
Run the do_page.sh script. Open a new terminal and run the tests you want to perform.
For example:
./spider -r http://localhost:8000/          # default depth 5 → img_01.jpg, img_02.PNG, img_03.jpg
./spider -r -l 1 http://localhost:8000/      # stop at depth 1 → img_01.jpg, img_02.PNG only
./spider http://localhost:8000/               # only → img_01.jpg
(Keep on mind the program renames the files to avoid rewritting)
