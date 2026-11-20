/* UBSan runtime behavior test: builds the embedded runtime source
 * (runtime/ubsan/runtime_source.h, exactly what the driver compiles) for the
 * host with a host Clang, checks it is not stale against runtime/ubsan/runtime.c,
 * links it into small instrumented programs (without Clang's own sanitizer
 * runtime), and checks each diagnostic and the abort.
 *
 *   cc -std=c11 -Isrc -Itests -o build/ubsan_runtime_test tests/ubsan_runtime_test.c
 *   build/ubsan_runtime_test [clang]        (default: $CLANG or "clang")
 *   CROSS_CC_TEST_LINKER=/path/to/ld.lld selects the embedded linker for links.
 *
 * POSIX hosts only; run from the repository root. Without a host clang the
 * test reports SKIP and succeeds. */
#ifdef _WIN32
/* Unsupported on Windows (see "POSIX hosts only" above): the harness needs
 * fork/exec, signals and wait status. The test runner compiles every *_test.c,
 * so Windows builds an explicit SKIP stub; this is not a Windows UBSan pass. */
#include <stdio.h>
int main(void) {
    puts("ubsan_runtime_test: SKIP (POSIX-only harness; not run on Windows)");
    return 0;
}
#else
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../runtime/ubsan/runtime_source.h"

typedef struct {
    const char *name, *flags, *source, *expect;
} Case;

static const Case cases[] = {
    {"add", "-fsanitize=signed-integer-overflow",
     "volatile int a = 2147483647; int main(void) { return a + 1; }",
     "panic: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'\n"},
    {"sub", "-fsanitize=signed-integer-overflow",
     "volatile int a = -2147483647 - 1; int main(void) { return a - 1; }",
     "panic: signed integer overflow: -2147483648 - 1 cannot be represented in type 'int'\n"},
    {"mul", "-fsanitize=signed-integer-overflow",
     "volatile long long a = 4611686018427387904LL; int main(void) { return (int)(a * -4); }",
     "panic: signed integer overflow: 4611686018427387904 * -4 cannot be represented in type 'long long'\n"},
    {"unsigned-add", "-fsanitize=unsigned-integer-overflow",
     "volatile unsigned a = 4294967295u; int main(void) { return (int)(a + 1u); }",
     "panic: unsigned integer overflow: 4294967295 + 1 cannot be represented in type 'unsigned int'\n"},
    {"int128-add", "-fsanitize=signed-integer-overflow",
     "volatile __int128 a; int main(void) { a = (__int128)(((unsigned __int128)1 << 127) - 1); return (int)(a + 1); }",
     "panic: signed integer overflow: 170141183460469231731687303715884105727 + 1 cannot be represented in type '__int128'\n"},
    {"int128-min", "-fsanitize=signed-integer-overflow",
     "volatile __int128 a; int main(void) { a = (__int128)((unsigned __int128)1 << 127); return (int)(a - 1); }",
     "panic: signed integer overflow: -170141183460469231731687303715884105728 - 1 cannot be represented in type '__int128'\n"},
    {"negate", "-fsanitize=signed-integer-overflow",
     "volatile int a = -2147483647 - 1; int main(void) { return -a; }",
     "panic: negation of -2147483648 cannot be represented in type 'int'\n"},
    {"divzero", "-fsanitize=integer-divide-by-zero",
     "volatile int a = 1, b = 0; int main(void) { return a / b; }",
     "panic: division by zero\n"},
    {"divmin", "-fsanitize=signed-integer-overflow",
     "volatile int a = -2147483647 - 1, b = -1; int main(void) { return a / b; }",
     "panic: division of -2147483648 by -1 cannot be represented in type 'int'\n"},
    {"shift-large", "-fsanitize=shift",
     "volatile int a = 1, b = 40; int main(void) { return a << b; }",
     "panic: shift exponent 40 is too large for 32-bit type 'int'\n"},
    {"shift-negative-exponent", "-fsanitize=shift",
     "volatile int a = 1, b = -3; int main(void) { return a << b; }",
     "panic: shift exponent -3 is negative\n"},
    {"shift-negative-value", "-fsanitize=shift",
     "volatile int a = -1, b = 2; int main(void) { return a << b; }",
     "panic: left shift of negative value -1\n"},
    {"shift-overflow", "-fsanitize=shift",
     "volatile int a = 3, b = 31; int main(void) { return a << b; }",
     "panic: left shift of 3 by 31 places cannot be represented in type 'int'\n"},
    {"bounds", "-fsanitize=bounds",
     "int arr[4]; volatile int i = 5; int main(void) { return arr[i]; }",
     "panic: index 5 out of bounds for type 'int[4]'\n"},
    {"null-load", "-fsanitize=null",
     "int *volatile p; int main(void) { return *p; }",
     "panic: load of null pointer of type 'int'\n"},
    {"misaligned-store", "-fsanitize=alignment",
     "char buf[16] __attribute__((aligned(16))); volatile int off = 1;\n"
     "int main(void) { int *p = (int *)(buf + off); *p = 1; return 0; }",
     " misaligned address 0x"},
    {"misaligned-message", "-fsanitize=alignment",
     "char buf[16] __attribute__((aligned(16))); volatile int off = 2;\n"
     "int main(void) { int *p = (int *)(buf + off); return *p; }",
     " for type 'int', which requires 4 byte alignment\n"},
    {"pointer-null-offset", "-fsanitize=pointer-overflow",
     "char *volatile p; volatile long n = 5; int main(void) { char *q = p + n; return q != 0; }",
     "panic: applying non-zero offset 5 to null pointer\n"},
    {"vla", "-fsanitize=vla-bound",
     "volatile int n = -1; int main(void) { int v[n]; v[0] = 0; return v[0]; }",
     "panic: variable length array bound evaluates to non-positive value -1\n"},
    {"float-cast", "-fsanitize=float-cast-overflow",
     "volatile double d = 1e20; int main(void) { return (int)d; }",
     "panic: 100000000000000000000 is outside the range of representable values of type 'int'\n"},
    {"float-cast-fraction", "-fsanitize=float-cast-overflow",
     "volatile double d = -12500000000.5; int main(void) { return (int)d; }",
     "panic: -12500000000.5 is outside the range of representable values of type 'int'\n"},
    /* Pinned quirk: x86 long double descriptors are read as binary128. */
    {"float-cast-long-double", "-fsanitize=float-cast-overflow",
     "volatile long double d = 1e30L; int main(void) { return (int)d; }",
     "panic: (float) is outside the range of representable values of type 'int'\n"},
    {"float-cast-nan", "-fsanitize=float-cast-overflow",
     "volatile double d = __builtin_nan(\"\"); int main(void) { return (int)d; }",
     "panic: nan is outside the range of representable values of type 'int'\n"},
    /* Pinned quirk: an inline f32 on 64-bit targets is bitcast as f64. */
    {"float-cast-inf", "-fsanitize=float-cast-overflow",
     "volatile float f = -__builtin_inff(); int main(void) { return (int)f; }",
     "panic: (float) is outside the range of representable values of type 'int'\n"},
    {"bool", "-fsanitize=bool",
     "volatile unsigned char raw = 5; int main(void) { _Bool b; __builtin_memcpy(&b, (const void *)&raw, 1); return b ? 1 : 2; }",
     "panic: load of value 5, which is not valid for type '"},
    {"nonnull-arg", "-fsanitize=nonnull-attribute",
     "__attribute__((nonnull, noinline)) int f(int *p) { return p != 0; } int *volatile q;\n"
     "int main(void) { return f(q); }",
     "panic: null pointer passed as argument 1, which is declared to never be null\n"},
    {"nonnull-return", "-fsanitize=returns-nonnull-attribute",
     "int *volatile q; __attribute__((returns_nonnull, noinline)) int *f(void) { return q; }\n"
     "int main(void) { return f() != 0; }",
     "panic: null pointer returned from function declared to never return null\n"},
    {"unreachable", "-fsanitize=unreachable",
     "volatile int x = 1; int main(void) { if (x) __builtin_unreachable(); return 0; }",
     "panic: execution reached an unreachable program point\n"},
    {"invalid-builtin", "-fsanitize=builtin",
     "volatile unsigned x = 0; int main(void) { return __builtin_ctz(x); }",
     "panic: passing zero to ctz(), which is not a valid argument\n"},
    {"alignment-assumption", "-fsanitize=alignment",
     "char buf[64] __attribute__((aligned(64))); volatile int off = 4;\n"
     "int main(void) { char *p = __builtin_assume_aligned(buf + off, 32); return *p; }",
     /* The alignment value is described by the pointer's (non-integer) type. */
     "panic: assumption of (unknown) byte alignment for pointer of type 'char *' failed\naddress is 4 aligned, misalignment offset is 4 bytes\n"},
};

static int failed;

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *data = malloc(cap + 1);
    size_t n;
    while ((n = fread(data + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len == cap) { cap *= 2; data = realloc(data, cap + 1); }
    }
    fclose(f);
    data[len] = 0;
    return data;
}

static int write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(text, f);
    return fclose(f) == 0;
}

static int header_ok(const char *err) {
    /* Linux reports a real gettid: "thread <digits> panic: ". */
    if (strncmp(err, "thread ", 7) != 0) return strncmp(err, "panic: ", 7) == 0;
    const char *p = err + 7;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') p++;
    return strncmp(p, " panic: ", 8) == 0;
}

int main(int argc, char **argv) {
    const char *clang = argc > 1 ? argv[1] : getenv("CLANG") ? getenv("CLANG") : "clang";
    char cmd[16384], runtime[4096], link_flag[8192] = "";
    const char *linker = getenv("CROSS_CC_TEST_LINKER");
    if (linker && *linker) {
        if (access(linker, X_OK) != 0) { fprintf(stderr, "FAIL test linker is not executable: %s\n", linker); return 1; }
        snprintf(link_flag, sizeof link_flag, "--ld-path='%s'", linker);
    }
    snprintf(cmd, sizeof cmd, "%s --version >/dev/null 2>&1", clang);
    if (system(cmd) != 0) { printf("SKIP ubsan runtime test: no host clang (%s)\n", clang); return 0; }
    /* Large outputs stay off tmpfs: $TMPDIR, else ~/.cache/cross-cc-build-tmp. */
    char dir[1024];
    const char *tmp = getenv("TMPDIR");
    const char *home = getenv("HOME");
    if (tmp && *tmp) snprintf(dir, sizeof dir, "%s/cross_cc-ubsan-XXXXXX", tmp);
    else {
        snprintf(dir, sizeof dir, "%s/.cache/cross-cc-build-tmp", home ? home : ".");
        snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
        if (system(cmd) != 0) { fprintf(stderr, "cannot create %s\n", dir); return 1; }
        strcat(dir, "/cross_cc-ubsan-XXXXXX");
    }
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    /* CROSS_CC_UBSAN_RUNTIME_OBJECT tests a prebuilt runtime object instead
     * (for example one produced by the driver's runtime preparation). */
    const char *prebuilt = getenv("CROSS_CC_UBSAN_RUNTIME_OBJECT");
    if (prebuilt && *prebuilt) {
        snprintf(runtime, sizeof runtime, "%s", prebuilt);
    } else {
        snprintf(runtime, sizeof runtime, "%s/runtime.o", dir);
        char src[2048], check[2048];
        snprintf(src, sizeof src, "%s/ubsan.c", dir);
        snprintf(check, sizeof check, "%s/check.o", dir);
        const char *flags = "-x c -std=c11 -O2 -g0 -nostdinc -ffreestanding -fno-builtin -fno-sanitize=all "
            "-fno-stack-protector -fPIC -Wall -Wextra -Werror -c";
        /* Same path for both builds, so the objects compare byte for byte. */
        snprintf(cmd, sizeof cmd, "cp runtime/ubsan/runtime.c '%s' && %s %s '%s' -o '%s'", src, clang, flags, src, check);
        if (system(cmd) != 0) { fprintf(stderr, "FAIL runtime.c build: %s\n", cmd); return 1; }
        FILE *f = fopen(src, "wb");
        if (!f || fwrite(cross_cc_ubsan_runtime_source, 1, CROSS_CC_UBSAN_RUNTIME_SOURCE_LEN, f) != CROSS_CC_UBSAN_RUNTIME_SOURCE_LEN || fclose(f) != 0) {
            fprintf(stderr, "FAIL cannot write %s\n", src);
            return 1;
        }
        snprintf(cmd, sizeof cmd, "%s %s '%s' -o '%s' && cmp -s '%s' '%s'", clang, flags, src, runtime, runtime, check);
        if (system(cmd) != 0) {
            fprintf(stderr, "FAIL embedded runtime build or stale runtime_source.h (rerun runtime/ubsan/generate.c): %s\n", cmd);
            return 1;
        }
        printf("PASS runtime build\n");
    }
    const char *opts[] = {"-O0", "-O2"};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const Case *c = &cases[i];
        for (size_t o = 0; o < 2; o++) {
            char src[2048], exe[2048], err[4096];
            snprintf(src, sizeof src, "%s/%s.c", dir, c->name);
            snprintf(exe, sizeof exe, "%s/%s%s", dir, c->name, opts[o]);
            snprintf(err, sizeof err, "%s.err", exe);
            write_file(src, c->source);
            snprintf(cmd, sizeof cmd, "%s -std=gnu11 %s -w %s -fno-sanitize-link-runtime %s %s %s -o %s",
                clang, opts[o], c->flags, link_flag, src, runtime, exe);
            if (system(cmd) != 0) { fprintf(stderr, "FAIL %s%s build: %s\n", c->name, opts[o], cmd); failed++; continue; }
            snprintf(cmd, sizeof cmd, "%s 2>%s", exe, err);
            int status = system(cmd);
            char *out = read_file(err);
            int aborted = status != -1 && ((WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) ||
                (WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGABRT));
            int ok = out && aborted && header_ok(out) && strstr(out, c->expect) != NULL;
            if (ok) printf("PASS %s%s\n", c->name, opts[o]);
            else { failed++; fprintf(stderr, "FAIL %s%s status=%d\nexpected: %s\ngot: %s\n", c->name, opts[o], status, c->expect, out ? out : "(none)"); }
            free(out);
        }
    }
    /* CROSS_CC_UBSAN_KEEP=1 keeps the programs and their stderr for comparison. */
    const char *keep = getenv("CROSS_CC_UBSAN_KEEP");
    if (keep && *keep && strcmp(keep, "0") != 0) {
        printf("kept %s\n", dir);
    } else {
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) fprintf(stderr, "warning: cannot remove %s\n", dir);
    }
    printf("\n%d failed\n", failed);
    return failed != 0;
}
#endif /* _WIN32 */
