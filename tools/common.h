/* Shared helpers for the cross_cc build/release tools. Standard C11 plus the
 * POSIX or Win32 calls needed for processes and directories. Header-only: every
 * tool includes this file and nothing else is linked. The tools are short lived,
 * so allocations are intentionally never freed. */
#ifndef CROSS_CC_TOOLS_COMMON_H
#define CROSS_CC_TOOLS_COMMON_H

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define PATH_SEP ';'
#define EXE_SUFFIX ".exe"
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#define PATH_SEP ':'
#define EXE_SUFFIX ""
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define HOST_ARCH "amd64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define HOST_ARCH "arm64"
#else
#define HOST_ARCH "unknown"
#endif

#if defined(_WIN32)
#define HOST_OS "windows"
#elif defined(__APPLE__)
#define HOST_OS "darwin"
#elif defined(__linux__)
#define HOST_OS "linux"
#else
#define HOST_OS "unknown"
#endif

static const char *tool_name = "tool";

static void fail(const char *format, ...) {
    va_list ap;
    fprintf(stderr, "%s: error: ", tool_name);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) fail("out of memory");
    return p;
}
static void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) fail("out of memory");
    return p;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *r = (char *)xmalloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}
static char *sfmtv(const char *format, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (n < 0) fail("format error");
    char *r = (char *)xmalloc((size_t)n + 1);
    vsnprintf(r, (size_t)n + 1, format, ap);
    return r;
}
static char *sfmt(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    char *r = sfmtv(format, ap);
    va_end(ap);
    return r;
}

/* ---- growable string list ---- */
typedef struct {
    char **v;
    int n, cap;
} Strs;

static void strs_add(Strs *s, const char *x) {
    if (s->n + 2 > s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->v = (char **)xrealloc(s->v, sizeof(char *) * (size_t)s->cap);
    }
    s->v[s->n++] = xstrdup(x);
    s->v[s->n] = NULL; /* always NULL terminated, usable as argv */
}
static void strs_addf(Strs *s, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    char *t = sfmtv(format, ap);
    va_end(ap);
    strs_add(s, t);
    free(t);
}
static void strs_addv(Strs *s, const char *const *list) {
    for (int i = 0; list[i]; i++) strs_add(s, list[i]);
}
static void strs_cat(Strs *s, const Strs *o) {
    for (int i = 0; i < o->n; i++) strs_add(s, o->v[i]);
}

/* ---- growable byte/string builder ---- */
typedef struct {
    char *s;
    size_t n, cap;
} Buf;

static void buf_add(Buf *b, const void *data, size_t len) {
    if (b->n + len + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        while (c < b->n + len + 1) c *= 2;
        b->s = (char *)xrealloc(b->s, c);
        b->cap = c;
    }
    memcpy(b->s + b->n, data, len);
    b->n += len;
    b->s[b->n] = 0;
}
static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_char(Buf *b, char c) { buf_add(b, &c, 1); }
static void buf_addf(Buf *b, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    char *t = sfmtv(format, ap);
    va_end(ap);
    buf_str(b, t);
    free(t);
}
static const char *buf_cstr(Buf *b) {
    if (!b->s) buf_add(b, "", 0);
    return b->s;
}

/* ---- string helpers ---- */
static int starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static int ends_with(const char *s, const char *p) {
    size_t a = strlen(s), b = strlen(p);
    return a >= b && memcmp(s + a - b, p, b) == 0;
}
static int contains(const char *s, const char *p) { return strstr(s, p) != NULL; }
static char *trim(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    char *r = (char *)xmalloc(n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}
static char *replace_all(const char *s, const char *from, const char *to) {
    Buf b = {0};
    size_t fl = strlen(from);
    if (fl == 0) return xstrdup(s);
    for (;;) {
        const char *p = strstr(s, from);
        if (!p) break;
        buf_add(&b, s, (size_t)(p - s));
        buf_str(&b, to);
        s = p + fl;
    }
    buf_str(&b, s);
    return (char *)buf_cstr(&b);
}
/* Split into lines (handles \r\n); empty trailing line dropped. */
static Strs split_lines(const char *text) {
    Strs out = {0};
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        size_t l2 = len;
        if (l2 > 0 && p[l2 - 1] == '\r') l2--;
        char *line = (char *)xmalloc(l2 + 1);
        memcpy(line, p, l2);
        line[l2] = 0;
        strs_add(&out, line);
        free(line);
        if (!e) break;
        p = e + 1;
    }
    return out;
}
/* Whitespace separated fields. */
static Strs split_fields(const char *text) {
    Strs out = {0};
    const char *p = text;
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *s = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        char *t = (char *)xmalloc((size_t)(p - s) + 1);
        memcpy(t, s, (size_t)(p - s));
        t[p - s] = 0;
        strs_add(&out, t);
        free(t);
    }
    return out;
}
/* Split a linker-flags line into arguments, honoring double quotes (no escapes). */
static Strs split_quoted(const char *text) {
    Strs out = {0};
    const char *p = text;
    for (;;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        Buf b = {0};
        int quoted = 0;
        while (*p && (quoted || !isspace((unsigned char)*p))) {
            if (*p == '"') quoted = !quoted;
            else buf_char(&b, *p);
            p++;
        }
        strs_add(&out, buf_cstr(&b));
    }
    return out;
}
static char *last_segment(const char *url) {
    const char *s = strrchr(url, '/');
    return xstrdup(s ? s + 1 : url);
}
static char *lower(const char *s) {
    char *r = xstrdup(s);
    for (char *p = r; *p; p++) *p = (char)tolower((unsigned char)*p);
    return r;
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void strs_sort(Strs *s) {
    if (s->n > 1) qsort(s->v, (size_t)s->n, sizeof(char *), cmp_str);
}

/* ---- paths ---- */
static int is_sep(char c) { return c == '/' || c == '\\'; }
static int is_abs(const char *p) {
    if (is_sep(p[0])) return 1;
    return isalpha((unsigned char)p[0]) && p[1] == ':' && is_sep(p[2]);
}
/* Join with '/', skipping empty parts. The list is NULL terminated. */
static char *path_joinv(const char *first, ...) {
    Buf b = {0};
    va_list ap;
    va_start(ap, first);
    for (const char *p = first; p; p = va_arg(ap, const char *)) {
        if (!*p) continue;
        if (b.n > 0 && !is_sep(b.s[b.n - 1])) buf_char(&b, '/');
        if (b.n > 0 && is_sep(*p)) while (is_sep(*p)) p++;
        buf_str(&b, p);
    }
    va_end(ap);
    return (char *)buf_cstr(&b);
}
#define PJ(...) path_joinv(__VA_ARGS__, (const char *)NULL)

static char *path_dir(const char *p) {
    size_t n = strlen(p);
    while (n > 1 && is_sep(p[n - 1])) n--;
    while (n > 0 && !is_sep(p[n - 1])) n--;
    while (n > 1 && is_sep(p[n - 1])) n--;
    if (n == 0) return xstrdup(".");
    char *r = (char *)xmalloc(n + 1);
    memcpy(r, p, n);
    r[n] = 0;
    return r;
}
static char *path_base(const char *p) {
    size_t n = strlen(p);
    while (n > 1 && is_sep(p[n - 1])) n--;
    size_t s = n;
    while (s > 0 && !is_sep(p[s - 1])) s--;
    char *r = (char *)xmalloc(n - s + 1);
    memcpy(r, p + s, n - s);
    r[n - s] = 0;
    return r;
}
static char *path_stem(const char *p) {
    char *b = path_base(p);
    char *d = strrchr(b, '.');
    if (d && d != b) *d = 0;
    return b;
}
static char *getcwd_dup(void) {
    char tmp[4096];
#ifdef _WIN32
    if (!_getcwd(tmp, sizeof tmp)) fail("cannot read working directory");
#else
    if (!getcwd(tmp, sizeof tmp)) fail("cannot read working directory");
#endif
    return xstrdup(tmp);
}
static char *abs_path(const char *p) {
    if (is_abs(p)) return xstrdup(p);
    return PJ(getcwd_dup(), p);
}

/* ---- filesystem ---- */
static int path_stat(const char *p, struct stat *st) { return stat(p, st); }
static int exists(const char *p) {
    struct stat st;
    return path_stat(p, &st) == 0;
}
static int is_dir(const char *p) {
    struct stat st;
    return path_stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static int is_file(const char *p) {
    struct stat st;
    return path_stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
static long long file_size(const char *p) {
    struct stat st;
    if (path_stat(p, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (long long)st.st_size;
}
static int make_dir(const char *p) {
#ifdef _WIN32
    return _mkdir(p);
#else
    return mkdir(p, 0777);
#endif
}
static void mkdirs(const char *p) {
    char *t = xstrdup(p);
    size_t n = strlen(t);
    for (size_t i = 1; i <= n; i++) {
        if (t[i] == 0 || is_sep(t[i])) {
            char c = t[i];
            t[i] = 0;
            if (!is_dir(t) && make_dir(t) != 0 && errno != EEXIST && !is_dir(t)) fail("mkdir %s: %s", t, strerror(errno));
            t[i] = c;
        }
    }
    free(t);
}
static void remove_file(const char *p) {
    remove(p);
}
/* Entries of a directory, names only, sorted bytewise. Returns 0 on failure. */
static int list_dir(const char *dir, Strs *out) {
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    char *pat = PJ(dir, "*");
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) strs_add(out, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) strs_add(out, e->d_name);
    }
    closedir(d);
#endif
    strs_sort(out);
    return 1;
}
static int is_symlink(const char *p) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(p);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    struct stat st;
    return lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
#endif
}
static void remove_all(const char *p) {
    if (!exists(p) && !is_symlink(p)) return;
    if (is_dir(p) && !is_symlink(p)) {
        Strs names = {0};
        list_dir(p, &names);
        for (int i = 0; i < names.n; i++) remove_all(PJ(p, names.v[i]));
        rmdir(p);
    } else {
        remove(p);
    }
}
static char *read_file(const char *p, size_t *len_out) {
    FILE *f = fopen(p, "rb");
    if (!f) fail("cannot read %s: %s", p, strerror(errno));
    Buf b = {0};
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_add(&b, chunk, n);
    if (ferror(f)) fail("read error on %s", p);
    fclose(f);
    if (len_out) *len_out = b.n;
    return (char *)buf_cstr(&b);
}
static void write_file_n(const char *p, const void *data, size_t n) {
    FILE *f = fopen(p, "wb");
    if (!f) fail("cannot write %s: %s", p, strerror(errno));
    if (n && fwrite(data, 1, n, f) != n) fail("write error on %s", p);
    if (fclose(f) != 0) fail("write error on %s", p);
}
static void write_file(const char *p, const char *text) { write_file_n(p, text, strlen(text)); }
static void copy_file(const char *dst, const char *src) {
    FILE *in = fopen(src, "rb");
    if (!in) fail("copy %s: %s", src, strerror(errno));
    FILE *out = fopen(dst, "wb");
    if (!out) fail("copy to %s: %s", dst, strerror(errno));
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, in)) > 0) {
        if (fwrite(chunk, 1, n, out) != n) fail("copy write error on %s", dst);
    }
    fclose(in);
    if (fclose(out) != 0) fail("copy write error on %s", dst);
#ifndef _WIN32
    struct stat st;
    if (stat(src, &st) == 0) chmod(dst, st.st_mode & 07777);
#endif
}
static void copy_tree(const char *dst, const char *src) {
    if (is_dir(src)) {
        mkdirs(dst);
        Strs names = {0};
        if (!list_dir(src, &names)) fail("cannot list %s", src);
        for (int i = 0; i < names.n; i++) copy_tree(PJ(dst, names.v[i]), PJ(src, names.v[i]));
    } else {
        mkdirs(path_dir(dst));
        copy_file(dst, src);
    }
}
static void rename_path(const char *from, const char *to) {
#ifdef _WIN32
    if (!MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)) fail("rename %s -> %s failed", from, to);
#else
    if (rename(from, to) != 0) fail("rename %s -> %s: %s", from, to, strerror(errno));
#endif
}
static void make_executable(const char *p) {
#ifndef _WIN32
    chmod(p, 0755);
#else
    (void)p;
#endif
}
/* Hard link, falling back to a copy. */
static void link_or_copy(const char *dst, const char *src) {
#ifndef _WIN32
    if (link(src, dst) == 0) return;
#endif
    copy_file(dst, src);
}
static int cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* Checkout root: the working directory when it holds toolchain.lock.json, else the
 * nearest ancestor of the executable that does. */
static char *find_root(const char *argv0) {
    char *cwd = getcwd_dup();
    if (exists(PJ(cwd, "toolchain.lock.json"))) return cwd;
    char *dir = path_dir(abs_path(argv0));
    for (int i = 0; i < 8; i++) {
        if (exists(PJ(dir, "toolchain.lock.json"))) return dir;
        char *up = path_dir(dir);
        if (!strcmp(up, dir)) break;
        dir = up;
    }
    fail("run from the repository root (toolchain.lock.json not found)");
    return NULL;
}

/* ---- environment ---- */
static const char *env_get(const char *name) {
    const char *v = getenv(name);
    return v ? v : "";
}

/* Child environment: copy of ours without the listed names (case insensitive on
 * Windows), then the extra NAME=VALUE entries appended. Entries without a
 * nonempty name are dropped. */
static Strs make_env(const char *const *drop, const Strs *extra) {
    Strs out = {0};
#ifdef _WIN32
    char *block = GetEnvironmentStringsA();
    for (const char *e = block; *e; e += strlen(e) + 1) {
#else
    for (char **pe = environ; *pe; pe++) {
        const char *e = *pe;
#endif
        const char *eq = strchr(e, '=');
        if (!eq || eq == e) continue;
        int skip = 0;
        for (int i = 0; drop && drop[i]; i++) {
            size_t n = strlen(drop[i]);
            if ((size_t)(eq - e) != n) continue;
#ifdef _WIN32
            if (_strnicmp(e, drop[i], n) == 0) skip = 1;
#else
            if (strncmp(e, drop[i], n) == 0) skip = 1;
#endif
        }
        if (!skip) strs_add(&out, e);
    }
    if (extra) strs_cat(&out, extra);
    return out;
}

/* ---- processes ---- */
#ifdef _WIN32
typedef HANDLE ProcHandle;
#else
typedef pid_t ProcHandle;
#endif

static void print_command(const char *prefix, char *const *argv) {
    printf("%s", prefix);
    for (int i = 0; argv[i]; i++) printf(" \"%s\"", argv[i]);
    printf("\n");
    fflush(stdout);
}

#ifdef _WIN32
static char *win_quote(const char *a) {
    Buf b = {0};
    int needs = (*a == 0) || strpbrk(a, " \t\"") != NULL;
    if (!needs) return xstrdup(a);
    buf_char(&b, '"');
    for (const char *p = a;; p++) {
        int slashes = 0;
        while (*p == '\\') { slashes++; p++; }
        if (*p == 0) {
            for (int i = 0; i < slashes * 2; i++) buf_char(&b, '\\');
            break;
        }
        if (*p == '"') {
            for (int i = 0; i < slashes * 2 + 1; i++) buf_char(&b, '\\');
            buf_char(&b, '"');
        } else {
            for (int i = 0; i < slashes; i++) buf_char(&b, '\\');
            buf_char(&b, *p);
        }
    }
    buf_char(&b, '"');
    return (char *)buf_cstr(&b);
}
#endif

/* Start a process with inherited stdio (or stdout/stderr redirected to a file
 * descriptor / handle when capture_fd >= 0, POSIX only through proc_capture). */
static ProcHandle proc_start(char *const *argv, const char *cwd, char *const *env) {
#ifdef _WIN32
    Buf cmd = {0};
    for (int i = 0; argv[i]; i++) {
        if (i) buf_char(&cmd, ' ');
        buf_str(&cmd, win_quote(argv[i]));
    }
    Buf envblock = {0};
    if (env) {
        for (int i = 0; env[i]; i++) buf_add(&envblock, env[i], strlen(env[i]) + 1);
        buf_add(&envblock, "", 1);
    }
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(NULL, (char *)buf_cstr(&cmd), NULL, NULL, TRUE, 0, env ? envblock.s : NULL, cwd, &si, &pi)) {
        fail("cannot start %s (error %lu; Windows builds need an MSVC developer shell)", argv[0], (unsigned long)GetLastError());
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
#else
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) fail("fork failed: %s", strerror(errno));
    if (pid == 0) {
        if (cwd && chdir(cwd) != 0) {
            fprintf(stderr, "%s: cannot enter %s\n", tool_name, cwd);
            _exit(127);
        }
        if (env) environ = (char **)env;
        execvp(argv[0], argv);
        fprintf(stderr, "%s: cannot start %s: %s\n", tool_name, argv[0], strerror(errno));
        _exit(127);
    }
    return pid;
#endif
}
static int proc_wait(ProcHandle h) {
#ifdef _WIN32
    DWORD code = 1;
    WaitForSingleObject(h, INFINITE);
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return (int)code;
#else
    int status = 0;
    while (waitpid(h, &status, 0) < 0) {
        if (errno != EINTR) fail("wait failed: %s", strerror(errno));
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
}
static int run_cmd(char *const *argv, const char *cwd, char *const *env) {
    return proc_wait(proc_start(argv, cwd, env));
}
static void must_run(char *const *argv, const char *cwd, char *const *env) {
    int code = run_cmd(argv, cwd, env);
    if (code != 0) fail("%s failed (exit %d)", argv[0], code);
}

/* Run and capture stdout (stderr stays inherited unless merge_err). Returns the
 * exit code; output is trimmed of trailing whitespace by the caller if wanted. */
static int run_capture(char *const *argv, const char *cwd, char *const *env, char **out) {
    char *tmpl = NULL;
    Buf cmd = {0};
    (void)cmd;
#ifdef _WIN32
    char tmpdir[MAX_PATH], tmpname[MAX_PATH];
    GetTempPathA(sizeof tmpdir, tmpdir);
    GetTempFileNameA(tmpdir, "cc", 0, tmpname);
    tmpl = xstrdup(tmpname);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE fh = CreateFileA(tmpl, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (fh == INVALID_HANDLE_VALUE) fail("cannot create capture file");
    Buf line = {0};
    for (int i = 0; argv[i]; i++) {
        if (i) buf_char(&line, ' ');
        buf_str(&line, win_quote(argv[i]));
    }
    Buf envblock = {0};
    if (env) {
        for (int i = 0; env[i]; i++) buf_add(&envblock, env[i], strlen(env[i]) + 1);
        buf_add(&envblock, "", 1);
    }
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = fh;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessA(NULL, (char *)buf_cstr(&line), NULL, NULL, TRUE, 0, env ? envblock.s : NULL, cwd, &si, &pi)) {
        fail("cannot start %s", argv[0]);
    }
    CloseHandle(pi.hThread);
    int code = proc_wait(pi.hProcess);
    CloseHandle(fh);
#else
    char name[] = "/tmp/cross_cc_capture_XXXXXX";
    const char *td = getenv("TMPDIR");
    if (td && *td) {
        tmpl = sfmt("%s/cross_cc_capture_XXXXXX", td);
    } else {
        tmpl = xstrdup(name);
    }
    int fd = mkstemp(tmpl);
    if (fd < 0) fail("cannot create capture file: %s", strerror(errno));
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) fail("fork failed");
    if (pid == 0) {
        dup2(fd, 1);
        close(fd);
        if (cwd && chdir(cwd) != 0) _exit(127);
        if (env) environ = (char **)env;
        execvp(argv[0], argv);
        fprintf(stderr, "%s: cannot start %s: %s\n", tool_name, argv[0], strerror(errno));
        _exit(127);
    }
    close(fd);
    int code = proc_wait(pid);
#endif
    *out = read_file(tmpl, NULL);
    remove(tmpl);
    return code;
}
static char *must_capture(char *const *argv, const char *cwd, char *const *env) {
    char *out;
    int code = run_capture(argv, cwd, env, &out);
    if (code != 0) fail("%s failed (exit %d)", argv[0], code);
    return trim(out);
}

/* Parallel job pool: add processes, never more than `limit` run at once. */
typedef struct {
    ProcHandle h[256];
    char *names[256];
    int n;
    int limit;
} Pool;

static void pool_drain(Pool *p) {
    for (int i = 0; i < p->n; i++) {
        int code = proc_wait(p->h[i]);
        if (code != 0) fail("compile %s failed (exit %d)", p->names[i], code);
    }
    p->n = 0;
}
static void pool_add(Pool *p, char *const *argv, const char *cwd, char *const *env, const char *name) {
    if (p->limit < 1) p->limit = 1;
    if (p->limit > 256) p->limit = 256;
    p->h[p->n] = proc_start(argv, cwd, env);
    p->names[p->n] = xstrdup(name);
    p->n++;
    if (p->n >= p->limit) pool_drain(p);
}

/* ---- SHA-256 ---- */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t fill;
} Sha256;

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(Sha256 *s, const unsigned char *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha256_init(Sha256 *s) {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, init, sizeof init);
    s->len = 0;
    s->fill = 0;
}
static void sha256_update(Sha256 *s, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    s->len += n;
    while (n > 0) {
        size_t take = 64 - s->fill;
        if (take > n) take = n;
        memcpy(s->buf + s->fill, p, take);
        s->fill += take;
        p += take;
        n -= take;
        if (s->fill == 64) {
            sha256_block(s, s->buf);
            s->fill = 0;
        }
    }
}
static void sha256_final(Sha256 *s, char hex[65]) {
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80;
    sha256_update(s, &pad, 1);
    pad = 0;
    while (s->fill != 56) sha256_update(s, &pad, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(s, lenb, 8);
    for (int i = 0; i < 8; i++) sprintf(hex + i * 8, "%08x", (unsigned)s->h[i]);
    hex[64] = 0;
}
static char *sha256_bytes(const void *data, size_t n) {
    Sha256 s;
    char *hex = (char *)xmalloc(65);
    sha256_init(&s);
    sha256_update(&s, data, n);
    sha256_final(&s, hex);
    return hex;
}
static char *sha256_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) fail("cannot hash %s: %s", path, strerror(errno));
    Sha256 s;
    char *hex = (char *)xmalloc(65);
    unsigned char *chunk = (unsigned char *)xmalloc(1 << 20);
    size_t n;
    sha256_init(&s);
    while ((n = fread(chunk, 1, 1 << 20, f)) > 0) sha256_update(&s, chunk, n);
    if (ferror(f)) fail("read error on %s", path);
    fclose(f);
    free(chunk);
    sha256_final(&s, hex);
    return hex;
}

/* ---- minimal JSON (strings, numbers, objects, arrays, literals) ---- */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct Json Json;
struct Json {
    JType type;
    char *str;    /* J_STR value, J_NUM text */
    int b;        /* J_BOOL */
    Json **items; /* J_ARR elements, J_OBJ values */
    char **keys;  /* J_OBJ keys */
    int n;
};

typedef struct {
    const char *p;
    const char *file;
} JParser;

static void jfail(JParser *jp, const char *msg) { fail("%s: bad json: %s", jp->file, msg); }
static void jskip(JParser *jp) {
    while (*jp->p && isspace((unsigned char)*jp->p)) jp->p++;
}
static void put_utf8(Buf *b, unsigned long c) {
    if (c < 0x80) {
        buf_char(b, (char)c);
    } else if (c < 0x800) {
        buf_char(b, (char)(0xc0 | (c >> 6)));
        buf_char(b, (char)(0x80 | (c & 0x3f)));
    } else if (c < 0x10000) {
        buf_char(b, (char)(0xe0 | (c >> 12)));
        buf_char(b, (char)(0x80 | ((c >> 6) & 0x3f)));
        buf_char(b, (char)(0x80 | (c & 0x3f)));
    } else {
        buf_char(b, (char)(0xf0 | (c >> 18)));
        buf_char(b, (char)(0x80 | ((c >> 12) & 0x3f)));
        buf_char(b, (char)(0x80 | ((c >> 6) & 0x3f)));
        buf_char(b, (char)(0x80 | (c & 0x3f)));
    }
}
static unsigned long jhex4(JParser *jp) {
    unsigned long v = 0;
    for (int i = 0; i < 4; i++) {
        char c = *jp->p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned long)(c - 'A' + 10);
        else jfail(jp, "bad \\u escape");
    }
    return v;
}
static char *jstring(JParser *jp) {
    Buf b = {0};
    if (*jp->p != '"') jfail(jp, "expected string");
    jp->p++;
    while (*jp->p != '"') {
        char c = *jp->p++;
        if (!c) jfail(jp, "unterminated string");
        if (c != '\\') {
            buf_char(&b, c);
            continue;
        }
        c = *jp->p++;
        switch (c) {
        case 'n': buf_char(&b, '\n'); break;
        case 't': buf_char(&b, '\t'); break;
        case 'r': buf_char(&b, '\r'); break;
        case 'b': buf_char(&b, '\b'); break;
        case 'f': buf_char(&b, '\f'); break;
        case '/': case '\\': case '"': buf_char(&b, c); break;
        case 'u': {
            unsigned long u = jhex4(jp);
            if (u >= 0xd800 && u < 0xdc00 && jp->p[0] == '\\' && jp->p[1] == 'u') {
                jp->p += 2;
                unsigned long lo = jhex4(jp);
                u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
            }
            put_utf8(&b, u);
            break;
        }
        default: jfail(jp, "bad escape");
        }
    }
    jp->p++;
    return (char *)buf_cstr(&b);
}
static Json *jnew(JType t) {
    Json *j = (Json *)xmalloc(sizeof *j);
    memset(j, 0, sizeof *j);
    j->type = t;
    return j;
}
static Json *jvalue(JParser *jp, int depth) {
    if (depth > 100) jfail(jp, "nesting too deep");
    jskip(jp);
    char c = *jp->p;
    if (c == '"') {
        Json *j = jnew(J_STR);
        j->str = jstring(jp);
        return j;
    }
    if (c == '{' || c == '[') {
        int obj = c == '{';
        Json *j = jnew(obj ? J_OBJ : J_ARR);
        int cap = 0;
        jp->p++;
        jskip(jp);
        if (*jp->p == (obj ? '}' : ']')) {
            jp->p++;
            return j;
        }
        for (;;) {
            jskip(jp);
            char *key = NULL;
            if (obj) {
                key = jstring(jp);
                jskip(jp);
                if (*jp->p++ != ':') jfail(jp, "expected ':'");
            }
            Json *v = jvalue(jp, depth + 1);
            if (j->n + 1 > cap) {
                cap = cap ? cap * 2 : 8;
                j->items = (Json **)xrealloc(j->items, sizeof(Json *) * (size_t)cap);
                if (obj) j->keys = (char **)xrealloc(j->keys, sizeof(char *) * (size_t)cap);
            }
            j->items[j->n] = v;
            if (obj) j->keys[j->n] = key;
            j->n++;
            jskip(jp);
            if (*jp->p == ',') {
                jp->p++;
                continue;
            }
            if (*jp->p == (obj ? '}' : ']')) {
                jp->p++;
                return j;
            }
            jfail(jp, "expected ',' or end of container");
        }
    }
    if (starts_with(jp->p, "true") || starts_with(jp->p, "false")) {
        Json *j = jnew(J_BOOL);
        j->b = *jp->p == 't';
        jp->p += j->b ? 4 : 5;
        return j;
    }
    if (starts_with(jp->p, "null")) {
        jp->p += 4;
        return jnew(J_NULL);
    }
    if (c == '-' || isdigit((unsigned char)c)) {
        const char *s = jp->p;
        while (*jp->p && strchr("+-0123456789.eE", *jp->p)) jp->p++;
        Json *j = jnew(J_NUM);
        j->str = (char *)xmalloc((size_t)(jp->p - s) + 1);
        memcpy(j->str, s, (size_t)(jp->p - s));
        j->str[jp->p - s] = 0;
        return j;
    }
    jfail(jp, "unexpected character");
    return NULL;
}
static Json *json_parse_text(const char *text, const char *name) {
    JParser jp = {text, name};
    Json *j = jvalue(&jp, 0);
    jskip(&jp);
    if (*jp.p) jfail(&jp, "trailing data");
    return j;
}
static Json *json_load(const char *file) { return json_parse_text(read_file(file, NULL), file); }
/* NULL when absent or j is not an object. */
static Json *jget(const Json *o, const char *key) {
    if (!o || o->type != J_OBJ) return NULL;
    for (int i = 0; i < o->n; i++) {
        if (strcmp(o->keys[i], key) == 0) return o->items[i];
    }
    return NULL;
}
static Json *jobj(const Json *o, const char *key) {
    Json *v = jget(o, key);
    if (!v || v->type != J_OBJ) fail("lock file: missing object \"%s\"", key);
    return v;
}
static const char *jstr(const Json *o, const char *key) {
    Json *v = jget(o, key);
    if (!v || v->type != J_STR) fail("lock file: missing string \"%s\"", key);
    return v->str;
}
/* "" when absent. */
static const char *jstr_opt(const Json *o, const char *key) {
    Json *v = jget(o, key);
    return v && v->type == J_STR ? v->str : "";
}
static Json *jarr_opt(const Json *o, const char *key) {
    Json *v = jget(o, key);
    return v && v->type == J_ARR ? v : NULL;
}

/* ---- download + archive helpers shared by the setup tools ---- */
static void download_verified(const char *url, const char *sha, const char *dest, int max_time) {
    if (exists(dest)) {
        if (strcmp(sha256_file(dest), sha) == 0) return;
        fail("%s exists but hash mismatch; delete it and rerun", dest);
    }
    printf("download %s\n", url);
    fflush(stdout);
    char *part = sfmt("%s.part", dest);
    remove(part);
    mkdirs(path_dir(dest));
    char *argv[] = {"curl", "-fL", "--proto", "=https", "--retry", "3", "--connect-timeout", "30", "--max-time", sfmt("%d", max_time), "-sS", "-o", part, (char *)url, NULL};
    must_run(argv, NULL, NULL);
    char *got = sha256_file(part);
    if (strcmp(got, sha) != 0) {
        remove(part);
        fail("sha256 mismatch for %s: expected %s got %s", url, sha, got);
    }
    rename_path(part, dest);
}

#endif
