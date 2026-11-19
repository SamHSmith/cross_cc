/* Regenerate runtime/ubsan/runtime_source.h, the compact embedded copy of
 * runtime/ubsan/runtime.c that the driver compiles for each UBSan target.
 *
 *   cc -std=c11 -o .cache/ubsan-generate runtime/ubsan/generate.c
 *   .cache/ubsan-generate [/path/to/clang]      (default: $CLANG or "clang")
 *
 * Run at the repository root. Compaction removes comments, indentation and
 * blank lines; string/character literals and preprocessor lines are kept.
 * Before anything is replaced, the original and the compact source are both
 * compiled for every runtime variant with the driver's flags (-Werror), and
 * each pair of objects must be byte-identical. The runtime must stay free of
 * inline assembly and target-specific code generation options. */
#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define SOURCE "runtime/ubsan/runtime.c"
#define OUTPUT "runtime/ubsan/runtime_source.h"
/* MSVC rejects a concatenated string literal of 65535 bytes or more. */
#define MAX_LITERAL 65535

typedef struct { char *data; size_t len, cap; } Buf;

static void fail(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void put(Buf *b, char c) {
    if (b->len + 2 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4096;
        b->data = realloc(b->data, b->cap);
        if (!b->data) fail("out of memory");
    }
    b->data[b->len++] = c;
    b->data[b->len] = 0;
}

static void puts_buf(Buf *b, const char *s) { while (*s) put(b, *s++); }

static Buf read_file(const char *path) {
    Buf b = {0};
    FILE *f = fopen(path, "rb");
    if (!f) fail("cannot read %s: %s", path, strerror(errno));
    int c;
    while ((c = fgetc(f)) != EOF) put(&b, (char)c);
    fclose(f);
    if (!b.data) put(&b, 0), b.len = 0;
    return b;
}

static void write_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, len, f) != len || fclose(f) != 0) fail("cannot write %s", path);
}

/* Strip comments; trim each line and collapse blank runs outside literals.
 * Newlines are kept so preprocessor directives and continuations survive. */
static Buf compact(const Buf *in) {
    Buf stage = {0};
    size_t i = 0, n = in->len;
    const char *s = in->data;
    while (i < n) {
        char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            if (i + 1 >= n) fail("unterminated comment in " SOURCE);
            i += 2;
            put(&stage, ' ');
            continue;
        }
        if (c == '"' || c == '\'') {
            char quote = c;
            put(&stage, s[i++]);
            while (i < n && s[i] != quote) {
                if (s[i] == '\n') fail("unterminated literal in " SOURCE);
                if (s[i] == '\\' && i + 1 < n) put(&stage, s[i++]);
                put(&stage, s[i++]);
            }
            if (i >= n) fail("unterminated literal in " SOURCE);
            put(&stage, s[i++]);
            continue;
        }
        put(&stage, c);
        i++;
    }
    /* Second pass: whitespace, still literal-aware. */
    Buf out = {0};
    s = stage.data;
    n = stage.len;
    int line_start = 1, pending_space = 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\n') {
            if (!line_start) put(&out, '\n');
            line_start = 1;
            pending_space = 0;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            if (!line_start) pending_space = 1;
            continue;
        }
        if (pending_space) put(&out, ' ');
        pending_space = 0;
        line_start = 0;
        if (c == '"' || c == '\'') {
            char quote = c;
            put(&out, s[i++]);
            while (i < n && s[i] != quote) {
                if (s[i] == '\\') put(&out, s[i++]);
                put(&out, s[i++]);
            }
            put(&out, s[i]);
            continue;
        }
        put(&out, c);
    }
    if (out.len == 0 || out.data[out.len - 1] != '\n') put(&out, '\n');
    free(stage.data);
    return out;
}

typedef struct { const char *triple, *defines, *extra; } Variant;

/* Every runtime variant the driver builds: portable little/big endian 32/64,
 * the libbacktrace symbol variants, and the Windows DbgHelp variants. */
static const Variant variants[] = {
    {"x86_64-unknown-linux-gnu", "", "-fPIC"},
    {"x86_64-unknown-linux-musl", "", "-fPIC"},
    {"i386-unknown-linux-gnu", "", "-fPIC"},
    {"aarch64-unknown-linux-gnu", "", "-fPIC"},
    {"aarch64_be-unknown-linux-gnu", "", "-fPIC"},
    {"armv7a-unknown-linux-gnueabihf", "", "-fPIC"},
    {"armebv7a-unknown-linux-gnueabi", "", "-fPIC"},
    {"thumbv7-unknown-linux-gnueabihf", "", "-fPIC"},
    {"riscv32-unknown-linux-gnu", "", "-fPIC"},
    {"riscv64-unknown-linux-gnu", "", "-fPIC"},
    {"loongarch64-unknown-linux-gnu", "", "-fPIC"},
    {"powerpc64le-unknown-linux-gnu", "", "-fPIC"},
    {"powerpc64-unknown-linux-gnu", "", "-fPIC"},
    {"s390x-unknown-linux-gnu", "", "-fPIC"},
    {"mips-unknown-linux-musleabi", "", "-fPIC -msoft-float"},
    {"mipsel-unknown-linux-musleabi", "", "-fPIC -msoft-float"},
    {"mips64-unknown-linux-muslabi64", "", "-fPIC -msoft-float"},
    {"mips64el-unknown-linux-muslabi64", "", "-fPIC -msoft-float"},
    {"x86_64-unknown-freebsd", "", "-fPIC"},
    {"aarch64-unknown-netbsd", "", "-fPIC"},
    {"x86_64-unknown-linux-gnu", "-DCROSS_CC_UBSAN_LINUX_SYMBOLS=1", "-fPIC"},
    {"x86_64-unknown-linux-musl", "-DCROSS_CC_UBSAN_LINUX_SYMBOLS=1", "-fPIC"},
    {"x86_64-apple-macos11", "-DCROSS_CC_UBSAN_DARWIN_SYMBOLS=1", "-fPIC"},
    {"arm64-apple-macos11", "-DCROSS_CC_UBSAN_DARWIN_SYMBOLS=1", "-fPIC"},
    {"i686-pc-windows-gnu", "-DCROSS_CC_UBSAN_WINDOWS_SYMBOLS=1",
     "-fno-omit-frame-pointer -fno-asynchronous-unwind-tables -fno-unwind-tables"},
    {"i686-pc-windows-msvc", "-DCROSS_CC_UBSAN_WINDOWS_SYMBOLS=1",
     "-mno-incremental-linker-compatible -fno-omit-frame-pointer -fno-asynchronous-unwind-tables -fno-unwind-tables"},
    {"x86_64-pc-windows-gnu", "-DCROSS_CC_UBSAN_WINDOWS_SYMBOLS=1", ""},
    {"x86_64-pc-windows-msvc", "-DCROSS_CC_UBSAN_WINDOWS_SYMBOLS=1", "-mno-incremental-linker-compatible"},
    {"aarch64-pc-windows-gnu", "-DCROSS_CC_UBSAN_WINDOWS_SYMBOLS=1", ""},
    {"aarch64-pc-windows-msvc", "-DCROSS_CC_UBSAN_WINDOWS_SYMBOLS=1", "-mno-incremental-linker-compatible"},
};

static void compile(const char *clang, const Variant *v, const char *src, const char *obj) {
    char cmd[8192];
    snprintf(cmd, sizeof cmd,
        "'%s' --no-default-config --target=%s -x c -std=c11 -O2 -g0 -nostdinc -ffreestanding "
        "-fno-sanitize=all -fno-stack-protector -fno-builtin -Wall -Wextra -Werror %s %s -c '%s' -o '%s'",
        clang, v->triple, v->extra, v->defines, src, obj);
    if (system(cmd) != 0) fail("runtime does not compile for %s %s:\n  %s", v->triple, v->defines, cmd);
}

static int same_file(const char *a, const char *b) {
    Buf x = read_file(a), y = read_file(b);
    int same = x.len == y.len && memcmp(x.data, y.data, x.len) == 0;
    free(x.data);
    free(y.data);
    return same;
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

static int contains_word(const char *text, const char *word) {
    size_t n = strlen(word);
    for (const char *p = strstr(text, word); p; p = strstr(p + 1, word)) {
        int before = p == text || !(p[-1] == '_' || (p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= '0' && p[-1] <= '9'));
        char a = p[n];
        int after = !(a == '_' || (a >= 'a' && a <= 'z') || (a >= 'A' && a <= 'Z') || (a >= '0' && a <= '9'));
        if (before && after) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 2) fail("usage: generate [/path/to/clang]");
    const char *clang = argc == 2 ? argv[1] : getenv("CLANG") ? getenv("CLANG") : "clang";
    Buf source = read_file(SOURCE);
    for (size_t i = 0; i < source.len; i++) {
        unsigned char c = (unsigned char)source.data[i];
        if (c == 0 || c >= 0x80) fail(SOURCE " must be ASCII (byte %zu)", i);
    }
    Buf small = compact(&source);
    /* Target-neutral source: no assembly, no includes, no code-generation pragmas. */
    static const char *forbidden[] = {"asm", "__asm", "__asm__", "#include", "#pragma", "target"};
    for (size_t i = 0; i < sizeof forbidden / sizeof forbidden[0]; i++) {
        const char *w = forbidden[i];
        int hit = w[0] == '#' ? strstr(small.data, w) != NULL : contains_word(small.data, w);
        if (hit) fail("runtime contains target-specific construct '%s'", w);
    }
    if (small.len >= MAX_LITERAL) fail("compact runtime is %zu bytes; the embedded literal must stay below %d", small.len, MAX_LITERAL);

    const char *tmp = getenv("TMPDIR");
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/ubsan-generate", tmp && *tmp ? tmp : ".cache/tmp");
    make_dirs(dir);
    char src[4200], full_obj[4200], small_obj[4200];
    /* Both versions compile from the same path, so objects compare byte for byte. */
    snprintf(src, sizeof src, "%s/ubsan.c", dir);
    snprintf(full_obj, sizeof full_obj, "%s/full.o", dir);
    snprintf(small_obj, sizeof small_obj, "%s/compact.o", dir);
    for (size_t i = 0; i < sizeof variants / sizeof variants[0]; i++) {
        const Variant *v = &variants[i];
        write_file(src, source.data, source.len);
        compile(clang, v, src, full_obj);
        write_file(src, small.data, small.len);
        compile(clang, v, src, small_obj);
        if (!same_file(full_obj, small_obj)) fail("compaction changed the object for %s %s", v->triple, v->defines);
        printf("PASS %s %s\n", v->triple, v->defines);
    }
    remove(src);
    remove(full_obj);
    remove(small_obj);

    Buf header = {0};
    puts_buf(&header,
        "/* Generated from runtime.c by generate.c; do not edit. */\n"
        "#ifndef CROSS_CC_UBSAN_RUNTIME_SOURCE_H\n"
        "#define CROSS_CC_UBSAN_RUNTIME_SOURCE_H\n"
        "static const char cross_cc_ubsan_runtime_source[] =\n");
    size_t i = 0;
    while (i < small.len) {
        put(&header, '"');
        while (i < small.len) {
            char c = small.data[i++];
            if (c == '\n') { puts_buf(&header, "\\n"); break; }
            if (c == '\\' || c == '"') put(&header, '\\');
            /* Keep "??" sequences from forming trigraphs in older modes. */
            if (c == '?' && header.len > 0 && header.data[header.len - 1] == '?') put(&header, '\\');
            put(&header, c);
        }
        puts_buf(&header, "\"\n");
    }
    puts_buf(&header,
        ";\n"
        "#define CROSS_CC_UBSAN_RUNTIME_SOURCE_LEN (sizeof cross_cc_ubsan_runtime_source - 1)\n"
        "#endif\n");
    write_file(OUTPUT, header.data, header.len);
    printf("%s: %zu bytes from %zu source bytes (%zu compact)\n", OUTPUT, header.len, source.len, small.len);
    return 0;
}
