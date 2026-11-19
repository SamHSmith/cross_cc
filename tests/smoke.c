/* Compact end-to-end and differential smoke suite against the pinned Zig.
 * Generated fixtures stay on disk under .cache/smoke.
 *
 *   cc -std=c11 -Isrc -Itests -o build/smoke tests/smoke.c
 *   build/smoke [repository-root]       (default: current directory)
 *
 * Needs bin/cross_cc, bin/cross_c++ and
 * .cache/reference/zig-x86_64-linux-0.17.1/zig (setup --reference, then build). */
#include "integration_process.h"

typedef struct {
    const char *root, *dir, *cc, *cxx, *reference;
    int passed, failed;
} Suite;

typedef struct { char *out, *err; int code; } Result;

static Result capture(Suite *s, const Argv *cmd) {
    Result r;
    char *tmp = ip_join(s->dir, "tmp");
    r.code = ip_capture(cmd, tmp, &r.out, &r.err);
    free(tmp);
    return r;
}

static Result capture_list(Suite *s, const char *const *args) {
    Argv cmd = {0};
    for (size_t i = 0; args[i]; i++) ip_push(&cmd, args[i]);
    Result r = capture(s, &cmd);
    free(cmd.data);
    return r;
}

static void check(Suite *s, const char *name, int ok, const char *details) {
    if (ok) { s->passed++; printf("PASS %s\n", name); }
    else { s->failed++; fprintf(stderr, "FAIL %s\n%s\n", name, details ? details : ""); }
    fflush(stdout);
}

static char *write_fixture(Suite *s, const char *name, const char *text) {
    char *path = ip_join(s->dir, name);
    if (!ip_write(path, text)) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    return path;
}

/* The reference is driven as `zig cc` / `zig c++`. */
static Result compile(Suite *s, const char *compiler, int cxx, const char *const *args) {
    Argv cmd = {0};
    ip_push(&cmd, compiler);
    if (strcmp(compiler, s->reference) == 0) ip_push(&cmd, cxx ? "c++" : "cc");
    for (size_t i = 0; args[i]; i++) ip_push(&cmd, args[i]);
    Result r = capture(s, &cmd);
    free(cmd.data);
    return r;
}

static void compare_program(Suite *s, const char *name, const char *source, int cxx, const char *const *flags) {
    char *own = ip_join(s->dir, name);
    char *ref = ip_format("%s/%s-reference", s->dir, name);
    const char *args[32];
    size_t n = 0;
    for (; flags[n]; n++) args[n] = flags[n];
    args[n] = source; args[n + 1] = "-o"; args[n + 3] = NULL;
    args[n + 2] = own;
    Result r = compile(s, cxx ? s->cxx : s->cc, cxx, args);
    if (r.code != 0) { check(s, name, 0, r.err); return; }
    args[n + 2] = ref;
    Result rr = compile(s, s->reference, cxx, args);
    if (rr.code != 0) { char *d = ip_format("reference failed: %s", rr.err); check(s, name, 0, d); return; }
    const char *own_run[] = {own, NULL}, *ref_run[] = {ref, NULL};
    Result o = capture_list(s, own_run), x = capture_list(s, ref_run);
    char *details = ip_format("own exit=%d out=\"%s\" err=%s; reference exit=%d out=\"%s\" err=%s",
        o.code, o.out, o.err, x.code, x.out, x.err);
    check(s, name, o.code == 0 && x.code == 0 && strcmp(o.out, x.out) == 0, details);
    if (cxx) {
        const char *readelf[] = {"readelf", "-d", own, NULL};
        Result d = capture_list(s, readelf);
        char *label = ip_format("%s uses libc++ rather than libstdc++", name);
        check(s, label, d.code == 0 && strstr(d.out, "libstdc++") == NULL, d.out);
    }
}

static void cross_object(Suite *s, const char *name, const char *target, const char *source, unsigned machine, const char *format) {
    char *object = ip_format("%s/%s.o", s->dir, name);
    const char *args[] = {"-target", target, "-c", source, "-o", object, NULL};
    Result r = compile(s, s->cc, 0, args);
    if (r.code != 0) { check(s, name, 0, r.err); return; }
    size_t len = 0;
    unsigned char *data = (unsigned char *)ip_read(object, &len);
    int good = data != NULL && len >= 20;
    if (good) {
        if (strcmp(format, "elf") == 0) good = data[0] == 0x7f && data[1] == 'E' && (unsigned)(data[18] | data[19] << 8) == machine;
        else if (strcmp(format, "coff") == 0) good = (unsigned)(data[0] | data[1] << 8) == machine;
        else if (strcmp(format, "macho") == 0) good = data[0] == 0xcf && data[1] == 0xfa && data[2] == 0xed && data[3] == 0xfe;
    }
    char *ref_object = ip_format("%s/%s-reference.o", s->dir, name);
    const char *ref_args[] = {"-target", target, "-c", source, "-o", ref_object, NULL};
    Result rr = compile(s, s->reference, 0, ref_args);
    char *details = ip_format("%s%s", r.err, rr.err);
    check(s, name, good && rr.code == 0, details);
}

/* Whitespace-separated field i of text (static buffer), or "" if absent. */
static const char *field(const char *text, int index) {
    static char buf[256];
    const char *p = text;
    for (int i = 0;; i++) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) return "";
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (i == index) {
            size_t n = (size_t)(p - start) < sizeof buf - 1 ? (size_t)(p - start) : sizeof buf - 1;
            memcpy(buf, start, n);
            buf[n] = 0;
            return buf;
        }
    }
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

static const char *program_c =
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <pthread.h>\n"
    "#include <stdatomic.h>\n"
    "static _Thread_local int local;\n"
    "static atomic_int sum;\n"
    "static void *work(void *p) { local = 21; atomic_fetch_add(&sum, local); return p; }\n"
    "int main(void) {\n"
    "    pthread_t a, b;\n"
    "    if (pthread_create(&a, 0, work, 0) || pthread_create(&b, 0, work, 0)) return 1;\n"
    "    pthread_join(a, 0); pthread_join(b, 0);\n"
    "    char *p = malloc(32); if (!p) return 2;\n"
    "    snprintf(p, 32, \"answer=%d\", atomic_load(&sum)); puts(p); free(p);\n"
    "    return atomic_load(&sum) != 42;\n"
    "}\n";

static const char *program_cpp =
    "#include <iostream>\n"
    "#include <string>\n"
    "#include <vector>\n"
    "#include <thread>\n"
    "#include <stdexcept>\n"
    "struct Base { virtual ~Base() = default; }; struct Derived : Base {};\n"
    "thread_local std::string local;\n"
    "int main() {\n"
    "    std::vector<int> values{20, 22}; int total = 0;\n"
    "    std::thread worker([&] { local = \"thread\"; total = values[0] + values[1]; }); worker.join();\n"
    "    Derived d; Base *b = &d; if (!dynamic_cast<Derived *>(b)) return 2;\n"
    "    try { throw std::runtime_error(std::to_string(total)); }\n"
    "    catch (const std::exception &e) { std::cout << \"answer=\" << e.what() << '\\n'; }\n"
    "    return total != 42;\n"
    "}\n";

int main(int argc, char **argv) {
    Suite suite = {0};
    Suite *s = &suite;
    char cwd[4096];
    if (argc > 1) s->root = argv[1];
    else if (getcwd(cwd, sizeof cwd)) s->root = cwd;
    else { perror("getcwd"); return 1; }
    s->dir = ip_format("%s/.cache/smoke", s->root);
    s->cc = ip_format("%s/bin/cross_cc", s->root);
    s->cxx = ip_format("%s/bin/cross_c++", s->root);
    s->reference = ip_format("%s/.cache/reference/zig-x86_64-linux-0.17.1/zig", s->root);
    const char *required[] = {s->cc, s->cxx, s->reference};
    for (int i = 0; i < 3; i++) {
        if (!ip_exists(required[i])) { fprintf(stderr, "missing %s; run setup --reference and build first\n", required[i]); return 1; }
    }
    char *tmp = ip_join(s->dir, "tmp");
    ip_mkdirs(tmp);
    setenv("TMPDIR", tmp, 1);
    setenv("ZIG_GLOBAL_CACHE_DIR", ip_format("%s/.cache/zig-global", s->root), 1);
    setenv("ZIG_LOCAL_CACHE_DIR", ip_format("%s/.cache/zig-local", s->root), 1);
    char *c = write_fixture(s, "program.c", program_c);
    char *cpp = write_fixture(s, "program.cpp", program_cpp);
    char *simple = write_fixture(s, "object.c", "#include <stddef.h>\nint answer(void) { return sizeof(void*) == 8 ? 42 : 21; }\n");
    char *bad = write_fixture(s, "invalid.c", "int main(void) { this is not C; }\n");

    {
        const char *own_cmd[] = {s->cc, "--version", NULL};
        const char *ref_cmd[] = {s->reference, "cc", "--version", NULL};
        Result own = capture_list(s, own_cmd), ref = capture_list(s, ref_cmd);
        char own_field[256];
        snprintf(own_field, sizeof own_field, "%s", field(own.out, 2));
        const char *ref_field = field(ref.out, 2);
        check(s, "frontend version matches reference",
            own.code == 0 && ref.code == 0 && own_field[0] && ref_field[0] && strcmp(own_field, ref_field) == 0,
            ip_format("%s%s", own.out, ref.out));
    }
    {
        /* A child selected by an explicit argv[0] alias must not depend on the
         * executable's self-path (macOS can return a different hard-link name). */
        char *dsymutil = ip_join(s->dir, "cross_dsymutil");
        char *clang = ip_join(s->dir, "cross_clang");
        const char *aliases[] = {dsymutil, clang};
        for (int i = 0; i < 2; i++) {
            remove(aliases[i]);
            if (link(s->cc, aliases[i]) != 0) { fprintf(stderr, "cannot create backend alias: %s\n", strerror(errno)); return 1; }
        }
        const char *d_cmd[] = {dsymutil, "--version", NULL};
        const char *c_cmd[] = {clang, "dsymutil", "--version", NULL};
        const char *p_cmd[] = {clang, "--cross-cc-dsymutil", "--version", NULL};
        Result d = capture_list(s, d_cmd), cl = capture_list(s, c_cmd), p = capture_list(s, p_cmd);
        check(s, "embedded dSYM alias and raw Clang dispatch stay distinct",
            d.code == 0 && strstr(d.out, "LLVM version") && cl.code == 0 && strstr(cl.out, "clang version") &&
            p.code != 0 && strstr(p.err, "unknown argument"),
            ip_format("%s%s%s", d.err, cl.err, p.err));
    }
    {
        const char *f1[] = {"-pthread", NULL};
        const char *f2[] = {"-std=c++20", "-pthread", NULL};
        const char *f3[] = {"-target", "x86_64-linux-musl", "-pthread", NULL};
        compare_program(s, "native-c", c, 0, f1);
        compare_program(s, "native-cxx", cpp, 1, f2);
        compare_program(s, "static-musl", c, 0, f3);
        const char *readelf[] = {"readelf", "-l", ip_join(s->dir, "static-musl"), NULL};
        Result deps = capture_list(s, readelf);
        check(s, "musl executable has no dynamic interpreter", deps.code == 0 && strstr(deps.out, "INTERP") == NULL, deps.out);
    }

    cross_object(s, "aarch64-linux-object", "aarch64-linux-musl", simple, 183, "elf");
    cross_object(s, "windows-object", "x86_64-windows-gnu", simple, 0x8664, "coff");
    cross_object(s, "macos-object", "aarch64-macos", simple, 0, "macho");
    {
        char *obj = ip_join(s->dir, "wasm.o");
        const char *args[] = {"-target", "wasm32-freestanding", "-c", simple, "-o", obj, NULL};
        Result r = compile(s, s->cc, 0, args);
        size_t len = 0;
        char *wasm = ip_read(obj, &len);
        check(s, "wasm object", r.code == 0 && wasm && len > 4 && memcmp(wasm, "\0asm", 4) == 0, r.err);
    }

    {
        char *hello = write_fixture(s, "cross-hello.c",
            "#include <stdio.h>\nint main(void) { puts(\"hello\"); printf(\"answer=%d %.1f\\n\", 42, 1.5); return 0; }\n");
        struct { const char *name, *target, *magic; size_t magic_len; } tests[] = {
            {"windows-c", "x86_64-windows-gnu", "MZ", 2}, {"wasi-c", "wasm32-wasi", "\0asm", 4},
        };
        for (int i = 0; i < 2; i++) {
            char *output = ip_join(s->dir, tests[i].name);
            const char *args[] = {"-target", tests[i].target, hello, "-o", output, NULL};
            Result r = compile(s, s->cc, 0, args);
            size_t len = 0;
            char *data = ip_read(output, &len);
            int good = r.code == 0 && data && len >= tests[i].magic_len && memcmp(data, tests[i].magic, tests[i].magic_len) == 0;
            char *ref_output = ip_format("%s/%s-reference", s->dir, tests[i].name);
            const char *ref_args[] = {"-target", tests[i].target, hello, "-o", ref_output, NULL};
            Result rr = compile(s, s->reference, 0, ref_args);
            size_t ref_len = 0;
            char *ref_data = ip_read(ref_output, &ref_len);
            int ref_good = rr.code == 0 && ref_data && ref_len >= tests[i].magic_len && memcmp(ref_data, tests[i].magic, tests[i].magic_len) == 0;
            check(s, ip_format("%s cross-link", tests[i].name), good && ref_good, ip_format("%s%s", r.err, rr.err));
        }
    }

    {
        char *object = ip_join(s->dir, "response object.o");
        char *depfile_path = ip_join(s->dir, "response.d");
        char *rsp = write_fixture(s, "compile.rsp",
            ip_format("-c \"%s\" -o \"%s\" -MMD -MF \"%s\" -DMSG='hello world'", simple, object, depfile_path));
        const char *cmd[] = {s->cc, ip_format("@%s", rsp), NULL};
        Result r = capture_list(s, cmd);
        char *depfile = ip_read(depfile_path, NULL);
        check(s, "response quoting and dependency files",
            r.code == 0 && ip_exists(object) && depfile && strstr(depfile, "object.c"), r.err);
    }
    {
        const char *pp[] = {"-E", "-P", simple, NULL};
        Result own = compile(s, s->cc, 0, pp), ref = compile(s, s->reference, 0, pp);
        check(s, "preprocessor output matches reference",
            own.code == 0 && ref.code == 0 && strcmp(trim(own.out), trim(ref.out)) == 0, ip_format("%s%s", own.err, ref.err));
        const char *bad_args[] = {"-c", bad, "-o", ip_join(s->dir, "invalid.o"), NULL};
        const char *ref_bad_args[] = {"-c", bad, "-o", ip_join(s->dir, "invalid-reference.o"), NULL};
        Result b = compile(s, s->cc, 0, bad_args), rb = compile(s, s->reference, 0, ref_bad_args);
        check(s, "compiler failure propagates", b.code != 0 && rb.code != 0 && strstr(b.err, "error:"), b.err);
        const char *build[] = {s->cc, "build", NULL};
        Result u = capture_list(s, build);
        check(s, "build subcommand explicitly rejected", u.code != 0 && strstr(u.err, "explicitly excluded"), u.err);
    }
    {
        /* MSVC link transports: Windows drive paths must survive -Wl and -Xlinker intact. */
        char *msvc_src = write_fixture(s, "msvc_start.c", "int mainCRTStartup(void) { return 0; }\n");
        const char *cmd[] = {s->cc, "-v", "-target", "x86_64-windows-msvc", "-nostdlib",
            "-Wl,/implib:C:/dir/x.lib,/pdb:C:/dir/x.pdb", "-Xlinker", "/subsystem:console", "-Xlinker", "-Brepro",
            "-Wl,--entry,mainCRTStartup", "-o", ip_join(s->dir, "msvc.exe"), msvc_src, NULL};
        Result r = capture_list(s, cmd);
        check(s, "MSVC link keeps drive-letter paths",
            strstr(r.err, "/implib:C:/dir/x.lib") && strstr(r.err, "/pdb:C:/dir/x.pdb") && strstr(r.err, "/subsystem:console,6.0") &&
            strstr(r.err, "/entry:mainCRTStartup") && strstr(r.err, "/Brepro"), r.err);
        const char *cmd2[] = {s->cc, "-target", "x86_64-windows-msvc", "-nostdlib", "-Xlinker", "/subsystem:console,6.0",
            "-o", ip_join(s->dir, "msvc2.exe"), msvc_src, NULL};
        Result v = capture_list(s, cmd2);
        check(s, "MSVC subsystem with version rejected", v.code != 0 && strstr(v.err, "console,6.0"), v.err);
    }
    printf("\n%d passed, %d failed\n", s->passed, s->failed);
    return s->failed != 0;
}
