/* Native Linux x86/x86_64 differential UBSan panic-header regression.
 *
 *   cc -std=c11 -o .cache/check_thread_header runtime/ubsan/check_thread_header.c
 *   .cache/check_thread_header /absolute/cross_cc /absolute/pinned/zig
 *
 * Run from the repository root. Set CROSS_CC_ROOT/CROSS_CC_LIB_DIR to existing
 * pinned dependencies if needed. All compiler caches and generated fixtures
 * remain private to this checkout (.cache/ubsan-thread-header). */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if !defined(__linux__) || !defined(__x86_64__)
#error "check_thread_header requires native x86_64 Linux with IA32 execution support"
#endif

static void fail(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static char *cat(const char *a, const char *b) {
    size_t n = strlen(a) + strlen(b) + 1;
    char *r = malloc(n);
    if (!r) fail("out of memory");
    snprintf(r, n, "%s%s", a, b);
    return r;
}

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return cat("", "");
    size_t cap = 4096, len = 0, n;
    char *data = malloc(cap + 1);
    if (!data) fail("out of memory");
    while ((n = fread(data + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len == cap) {
            cap *= 2;
            data = realloc(data, cap + 1);
            if (!data) fail("out of memory");
        }
    }
    fclose(f);
    data[len] = 0;
    return data;
}

static void make_dirs(const char *path) {
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(buf, 0777) != 0 && errno != EEXIST) fail("mkdir %s: %s", buf, strerror(errno));
        *p = '/';
    }
    if (mkdir(buf, 0777) != 0 && errno != EEXIST) fail("mkdir %s: %s", buf, strerror(errno));
}

static const char *capture_dir;

/* Run argv; return its stdout and stderr (as heap strings) and exit code.
 * A signal death reports 128 + signal, like a shell. */
static int capture(char *const argv[], char **out, char **errout) {
    char *out_path = cat(capture_dir, "/capture.out");
    char *err_path = cat(capture_dir, "/capture.err");
    pid_t pid = fork();
    if (pid < 0) fail("fork: %s", strerror(errno));
    if (pid == 0) {
        int o = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        int e = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (o < 0 || e < 0) _exit(126);
        dup2(o, 1);
        dup2(e, 2);
        close(o);
        close(e);
        execvp(argv[0], argv);
        fprintf(stderr, "cannot execute %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) fail("waitpid: %s", strerror(errno));
    }
    *out = read_all(out_path);
    *errout = read_all(err_path);
    free(out_path);
    free(err_path);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 255;
}

static int parse_uint(const char *s, unsigned long *v) {
    char *end;
    if (*s < '0' || *s > '9') return 0;
    errno = 0;
    *v = strtoul(s, &end, 10);
    return errno == 0 && *end == 0;
}

static const char *fixture =
    "#define _GNU_SOURCE\n"
    "#include <pthread.h>\n"
    "#include <stdio.h>\n"
    "#include <limits.h>\n"
    "#include <unistd.h>\n"
    "#include <sys/syscall.h>\n"
    "static void *overflow(void *unused) {\n"
    "    (void)unused;\n"
    "    printf(\"%ld %ld\\n\", (long)getpid(), syscall(SYS_gettid));\n"
    "    fflush(stdout);\n"
    "    volatile int n = INT_MAX;\n"
    "    n += 1;\n"
    "    return 0;\n"
    "}\n"
    "int main(int argc, char **argv) {\n"
    "    (void)argv;\n"
    "    if (argc > 1) { pthread_t t; if (pthread_create(&t, 0, overflow, 0)) return 2; return pthread_join(t, 0); }\n"
    "    overflow(0);\n"
    "    return 0;\n"
    "}\n";

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f || fputs(text, f) < 0 || fclose(f) != 0) fail("write fixture %s", path);
}

int main(int argc, char **argv) {
    if (argc != 3) fail("usage: check_thread_header /absolute/cross_cc /absolute/reference-compiler");
    char root[4096];
    if (!getcwd(root, sizeof root)) fail("getcwd: %s", strerror(errno));
    char *dir = cat(root, "/.cache/ubsan-thread-header");
    char *tmp = cat(dir, "/tmp");
    make_dirs(tmp);
    capture_dir = tmp;
    const char *env[4][2] = {
        {"TMPDIR", tmp}, {"CROSS_CC_CACHE_DIR", cat(dir, "/cc")},
        {"ZIG_GLOBAL_CACHE_DIR", cat(dir, "/zig-global")}, {"ZIG_LOCAL_CACHE_DIR", cat(dir, "/zig-local")},
    };
    for (int i = 0; i < 4; i++) {
        if (setenv(env[i][0], env[i][1], 1) != 0) fail("setenv: %s", strerror(errno));
    }
    char *source = cat(dir, "/overflow.c");
    /* C is only an instrumented client fixture here. */
    write_text(source, fixture);
    static const char *targets[] = {"x86_64-linux-gnu", "x86_64-linux-musl", "x86-linux-gnu", "x86-linux-musl"};
    int passed = 0;
    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < 2; i++) {
            const char *label = i == 0 ? "own" : "reference";
            char exe[4096];
            snprintf(exe, sizeof exe, "%s/%s-%s", dir, targets[t], label);
            char *cmd[16];
            int n = 0;
            cmd[n++] = argv[1 + i];
            if (i == 1) cmd[n++] = "cc";
            cmd[n++] = "-target"; cmd[n++] = (char *)targets[t]; cmd[n++] = "-O0";
            cmd[n++] = "-fsanitize=signed-integer-overflow"; cmd[n++] = "-pthread";
            cmd[n++] = source; cmd[n++] = "-o"; cmd[n++] = exe; cmd[n] = NULL;
            char *out, *errout;
            int code = capture(cmd, &out, &errout);
            if (code != 0) fail("compile %s %s: %d\n%s", targets[t], label, code, errout);
            for (int worker = 0; worker < 2; worker++) {
                char *run[3] = {exe, worker ? "worker" : NULL, NULL};
                int status = capture(run, &out, &errout);
                char pid_text[64] = "", tid_text[64] = "", extra[2] = "";
                int fields = sscanf(out, "%63s %63s %1s", pid_text, tid_text, extra);
                if (fields != 2) fail("missing PID/TID: %s\n%s", out, errout);
                unsigned long pid, tid;
                int ok_pid = parse_uint(pid_text, &pid), ok_tid = parse_uint(tid_text, &tid);
                if (!ok_pid || !ok_tid || pid == 0 || tid == 0 || ((pid != tid) != worker)) {
                    fail("wrong native identity: worker=%s pid=%s tid=%s", worker ? "true" : "false", pid_text, tid_text);
                }
                char *newline = strchr(errout, '\n');
                size_t first_len = newline ? (size_t)(newline - errout) : strlen(errout);
                char expected[256];
                snprintf(expected, sizeof expected,
                    "thread %lu panic: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'", tid);
                if (status == 0 || first_len != strlen(expected) || strncmp(errout, expected, first_len) != 0) {
                    fail("%s %s worker=%s status=%d\nexpected %s\nactual %s", targets[t], label,
                        worker ? "true" : "false", status, expected, errout);
                }
                printf("PASS %s %s worker=%s PID=%lu TID=%lu exit=%d: %.*s\n", targets[t], label,
                    worker ? "true" : "false", pid, tid, status, (int)first_len, errout);
                passed++;
            }
        }
    }
    printf("%d native first-line/PID/TID checks passed (stack traces intentionally not compared)\n", passed);
    char *lazy_source = cat(dir, "/lazy.c");
    write_text(lazy_source, "int entry(void) { return 0; }\n");
    for (int t = 0; t < 4; t++) {
        char exe[4096];
        snprintf(exe, sizeof exe, "%s/lazy-%s", dir, targets[t]);
        char *cmd[] = {argv[1], "-target", (char *)targets[t], "-O0", "-fsanitize=signed-integer-overflow",
            "-nostdlib", "-Wl,-e,entry", "-Wl,--no-undefined", lazy_source, "-o", exe, NULL};
        char *out, *errout;
        if (capture(cmd, &out, &errout) != 0) fail("lazy archive link %s: %s", targets[t], errout);
        char *nm[] = {"nm", "-u", exe, NULL};
        int code = capture(nm, &out, &errout);
        int empty = 1;
        for (const char *p = out; *p; p++) {
            if (*p != ' ' && *p != '\n' && *p != '\t' && *p != '\r') { empty = 0; break; }
        }
        if (code != 0 || !empty) fail("lazy archive undefineds %s: %s %s", targets[t], out, errout);
        printf("PASS %s no-handler -nostdlib lazy link: no undefined symbols\n", targets[t]);
    }
    return 0;
}
