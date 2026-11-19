/* Small POSIX process/file helpers shared by the standalone integration
 * programs (smoke.c, linker_flags_integration.c,
 * linker_flags_semantics_integration.c). Header-only; C11 + POSIX. */
#ifndef CROSS_CC_INTEGRATION_PROCESS_H
#define CROSS_CC_INTEGRATION_PROCESS_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Growable NULL-terminated argument vector. */
typedef struct { char **data; size_t len, cap; } Argv;

static inline char *ip_format(const char *format, ...) {
    va_list ap, copy;
    va_start(ap, format);
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    char *s = malloc((size_t)n + 1);
    if (!s) { fprintf(stderr, "out of memory\n"); exit(2); }
    vsnprintf(s, (size_t)n + 1, format, ap);
    va_end(ap);
    return s;
}

static inline char *ip_join(const char *a, const char *b) { return ip_format("%s/%s", a, b); }

static inline void ip_push(Argv *v, const char *arg) {
    if (v->len + 2 > v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->data = realloc(v->data, v->cap * sizeof *v->data);
        if (!v->data) { fprintf(stderr, "out of memory\n"); exit(2); }
    }
    v->data[v->len++] = (char *)arg;
    v->data[v->len] = NULL;
}

/* Join argv with a separator, for logs and messages. */
static inline char *ip_argv_text(const Argv *v, size_t from, const char *sep) {
    size_t n = 1;
    for (size_t i = from; i < v->len; i++) n += strlen(v->data[i]) + strlen(sep);
    char *s = malloc(n);
    if (!s) { fprintf(stderr, "out of memory\n"); exit(2); }
    s[0] = 0;
    for (size_t i = from; i < v->len; i++) {
        if (i != from) strcat(s, sep);
        strcat(s, v->data[i]);
    }
    return s;
}

/* Whole file as a NUL-terminated buffer; *len gets the byte count. NULL on error. */
static inline char *ip_read(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0, got;
    char *data = malloc(cap + 1);
    if (!data) { fclose(f); return NULL; }
    while ((got = fread(data + n, 1, cap - n, f)) > 0) {
        n += got;
        if (n == cap) {
            cap *= 2;
            char *grown = realloc(data, cap + 1);
            if (!grown) { free(data); fclose(f); return NULL; }
            data = grown;
        }
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) { free(data); return NULL; }
    data[n] = 0;
    if (len) *len = n;
    return data;
}

static inline int ip_write(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = strlen(text);
    int ok = fwrite(text, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static inline void ip_write_or_exit(const char *path, const char *text) {
    if (!ip_write(path, text)) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
}

/* Binary-safe substring search. */
static inline int ip_contains(const char *data, size_t len, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0) return 1;
    for (size_t i = 0; i + n <= len; i++) {
        if (data[i] == needle[0] && memcmp(data + i, needle, n) == 0) return 1;
    }
    return 0;
}

static inline int ip_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }

static inline void ip_mkdirs(const char *path) {
    char *buf = ip_format("%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        (void)mkdir(buf, 0777);
        *p = '/';
    }
    (void)mkdir(buf, 0777);
    free(buf);
}

/* Run argv with stdout/stderr redirected to the given files (NULL discards).
 * Returns the exit status, 128+signal for a signal death, or -1 when the
 * program could not be executed at all. */
static inline int ip_run(const Argv *v, const char *out_path, const char *err_path) {
    int report[2];
    if (pipe(report) != 0) return -1;
    (void)fcntl(report[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) { close(report[0]); close(report[1]); return -1; }
    if (pid == 0) {
        close(report[0]);
        int o = open(out_path ? out_path : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        int e = open(err_path ? err_path : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        int in = open("/dev/null", O_RDONLY);
        if (o >= 0) { dup2(o, 1); close(o); }
        if (e >= 0) { dup2(e, 2); close(e); }
        if (in >= 0) { dup2(in, 0); close(in); }
        execvp(v->data[0], v->data);
        int err = errno;
        ssize_t ignored = write(report[1], &err, sizeof err);
        (void)ignored;
        _exit(127);
    }
    close(report[1]);
    int err = 0;
    ssize_t got;
    do { got = read(report[0], &err, sizeof err); } while (got < 0 && errno == EINTR);
    close(report[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (got == (ssize_t)sizeof err) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

/* Run and capture both streams into heap strings (never NULL). */
static inline int ip_capture(const Argv *v, const char *scratch_dir, char **out, char **err) {
    char *out_path = ip_join(scratch_dir, ".capture.stdout");
    char *err_path = ip_join(scratch_dir, ".capture.stderr");
    int code = ip_run(v, out_path, err_path);
    *out = ip_read(out_path, NULL);
    *err = ip_read(err_path, NULL);
    if (!*out) *out = ip_format("%s", "");
    if (!*err) *err = ip_format("%s", "");
    if (code == -1) {
        free(*err);
        *err = ip_format("cannot execute %s: %s", v->data[0], strerror(ENOENT));
        code = 127;
    }
    remove(out_path);
    remove(err_path);
    free(out_path);
    free(err_path);
    return code;
}

#endif
