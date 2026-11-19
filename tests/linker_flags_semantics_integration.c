/* Offline archive extraction checks; no execution of cross-target programs.
 *
 *   cc -std=c11 -Isrc -Itests -o build/linker_flags_semantics tests/linker_flags_semantics_integration.c
 *   build/linker_flags_semantics CC REFERENCE OUTDIR TARGET
 *
 * CC and REFERENCE are both driven as `TOOL cc ...` (archives via `CC ar`). */
#include "integration_process.h"

static const char *markers[2] = {"CROSS_CC_ARCHIVE_ONE_UNREFERENCED_CTOR", "CROSS_CC_ARCHIVE_TWO_UNREFERENCED_CTOR"};

/* Run with logs LOG.command/.stdout/.stderr; true when the outcome matches. */
static int run(const Argv *command, const char *log, int success) {
    char *text = ip_argv_text(command, 0, "\n");
    ip_write_or_exit(ip_format("%s.command", log), text);
    int code = ip_run(command, ip_format("%s.stdout", log), ip_format("%s.stderr", log));
    int ok = code == 0;
    if (ok != success) {
        char *line = ip_argv_text(command, 0, " ");
        fprintf(stderr, "unexpected status %d: %s (logs %s)\n", code, line, log);
        free(line);
    }
    free(text);
    return ok == success;
}

static Argv list(const char *first, ...) {
    Argv v = {0};
    va_list ap;
    va_start(ap, first);
    for (const char *a = first; a; a = va_arg(ap, const char *)) ip_push(&v, a);
    va_end(ap);
    return v;
}

static void base_link(Argv *args, const char *tool, const char *target, const char *main_c, const char *output) {
    ip_push(args, tool); ip_push(args, "cc"); ip_push(args, "-target"); ip_push(args, target);
    ip_push(args, "-O2"); ip_push(args, "-g0"); ip_push(args, main_c); ip_push(args, "-o"); ip_push(args, output);
}

static int needed_libraries(const char *cc, const char *reference, const char *dir, const char *target, const char *main_c) {
    int apple = strcmp(target, "aarch64-macos") == 0;
    const char *suffix = apple ? "dylib" : "so";
    for (int i = 0; i < 2; i++) {
        char *source = ip_format("%s/needed%d.c", dir, i);
        char *library = ip_format("%s/libneeded%d.%s", dir, i, suffix);
        ip_write_or_exit(source, ip_format("int needed%d(void) { return %d; }\n", i, i));
        Argv c = list(cc, "cc", "-target", target, "-O2", "-g0", "-shared", "-nostdlib", "-fPIC", source,
            ip_format("-Wl,-soname,libneeded%d.%s", i, suffix), "-o", library, (const char *)NULL);
        int ok = run(&c, ip_format("%s/shared%d", dir, i), 1);
        free(c.data);
        if (!ok) return 0;
    }
    int ok = 1;
    const char *tools[2] = {reference, cc};
    for (int j = 0; j < 2; j++) {
        char *output = ip_format("%s/needed-%d.out", dir, j);
        Argv args = {0};
        base_link(&args, tools[j], target, main_c, output);
        ip_push(&args, apple ? "-Wl,-dead_strip_dylibs" : "-dynamic");
        const char *rest[] = {"-Wl,--no-as-needed", "-lneeded0", "-Xlinker", "--as-needed", "-lneeded1", "-L", dir};
        for (size_t k = 0; k < sizeof rest / sizeof rest[0]; k++) ip_push(&args, rest[k]);
        int linked = run(&args, ip_format("%s/needed-%d", dir, j), 1);
        free(args.data);
        if (!linked) { ok = 0; continue; }
        size_t len = 0;
        char *data = ip_read(output, &len);
        if (!data || !ip_contains(data, len, "libneeded0.") || ip_contains(data, len, "libneeded1.")) {
            fprintf(stderr, "%s as-needed tool%d: incorrect shared-library dependencies\n", target, j);
            ok = 0;
        }
        free(data);
        printf("%s as-needed tool%d: %s\n", target, j, ok ? "PASS" : "FAIL");
    }
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: linker_flags_semantics CC REFERENCE OUTDIR TARGET\n"); return 2; }
    const char *cc = argv[1], *reference = argv[2], *dir = argv[3], *target = argv[4];
    (void)mkdir(dir, 0777);
    char *main_c = ip_join(dir, "main.c");
    ip_write_or_exit(main_c, "int main(void) { return 0; }\n");
    char *archives[2];
    for (int i = 0; i < 2; i++) {
        char *source = ip_format("%s/member%d.c", dir, i);
        char *object = ip_format("%s/member%d.o", dir, i);
        archives[i] = ip_format("%s/libmember%d.a", dir, i);
        ip_write_or_exit(source, ip_format(
            "char marker%d[] = \"%s\"; __attribute__((constructor)) static void init(void) { volatile char *p = marker%d; *p = 'O'; }\n",
            i, markers[i], i));
        Argv c = list(cc, "cc", "-target", target, "-O2", "-g0", "-c", source, "-o", object, (const char *)NULL);
        if (!run(&c, ip_format("%s/compile%d", dir, i), 1)) return 1;
        Argv a = list(cc, "ar", "rcs", archives[i], object, (const char *)NULL);
        if (!run(&a, ip_format("%s/archive%d", dir, i), 1)) return 1;
        free(c.data); free(a.data);
    }
    typedef struct { const char *flags[8]; int markers[2]; int success; } Case;
    const Case cases[] = {
        {{archives[0], archives[1]}, {0, 0}, 1},
        {{"-Wl,--whole-archive", archives[0], "-Wl,--no-whole-archive", archives[1]}, {1, 0}, 1},
        {{archives[0], "-Xlinker", "--whole-archive", archives[1], "-Xlinker", "--no-whole-archive"}, {0, 1}, 1},
        {{"-Xlinker", "--whole-archive", "-lmember0", "-Xlinker", "--no-whole-archive", "-lmember1", "-L", dir}, {1, 0}, 1},
        {{"-lmember0", "-Wl,--whole-archive", "-lmember1", "-Wl,--no-whole-archive", "-L", dir}, {0, 1}, 1},
        {{"-Wl,--whole-archive", archives[0]}, {1, 0}, 1},
        {{"-Wl,--whole-archive", "-Wl,-lmember0", "-Wl,--no-whole-archive", "-L", dir}, {0, 0}, 0},
        {{"-Xlinker", "--whole-archive", "-Xlinker", archives[0], "-Xlinker", "--no-whole-archive"}, {0, 0}, 0},
        {{"-Wl,--version-script,--whole-archive"}, {0, 0}, 0},
        {{"-Wl,--push-state,--pop-state"}, {0, 0}, 0},
    };
    size_t count = sizeof cases / sizeof cases[0];
    int failures = 0;
    const char *tools[2] = {reference, cc};
    for (size_t i = 0; i < count; i++) {
        const Case *c = &cases[i];
        for (int j = 0; j < 2; j++) {
            char *output = ip_format("%s/extract-%zu-%d.out", dir, i, j);
            Argv args = {0};
            base_link(&args, tools[j], target, main_c, output);
            for (int k = 0; k < 8 && c->flags[k]; k++) ip_push(&args, c->flags[k]);
            int ok = run(&args, ip_format("%s/extract-%zu-%d", dir, i, j), c->success);
            free(args.data);
            if (c->success && ok) {
                size_t len = 0;
                char *data = ip_read(output, &len);
                if (!data) ok = 0;
                for (int k = 0; k < 2; k++) {
                    int found = data && ip_contains(data, len, markers[k]);
                    if (found != c->markers[k]) {
                        ok = 0;
                        fprintf(stderr, "%s case%zu tool%d marker%d: got %s, expected %s\n", target, i, j, k,
                            found ? "true" : "false", c->markers[k] ? "true" : "false");
                    }
                }
                free(data);
            }
            if (!ok) failures++;
            printf("%s archive case%zu tool%d: %s\n", target, i, j, ok ? "PASS" : "FAIL");
        }
    }
    printf("%s: %zu archive cases x two tools, %d failures; logs in %s\n", target, count, failures, dir);
    if (strcmp(target, "aarch64-macos") == 0 || strcmp(target, "x86_64-linux-musl") == 0) {
        if (!needed_libraries(cc, reference, dir, target, main_c)) failures++;
    }
    return failures != 0;
}
