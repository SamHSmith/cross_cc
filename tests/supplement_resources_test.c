/* Offline regression tests for pinned upstream supplements (musl, WASI).
 * Builds tiny local archives, manifests and locks under build/tests; never uses
 * the network or the developer's real vendor/ tree. Each failing preparation
 * runs in a child process so fail() cannot end the whole harness. */
#define main cross_cc_build_main
#include "../tools/build.c"
#undef main

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

typedef struct {
    int kind; /* index into SUPPLEMENTS */
    const char *archive, *manifest, *lock, *prefix;
    const char *sel[5], *extra[4];
} Kind;

static const Kind KINDS[] = {
    {0, "musl-fixture.tar.gz", "vendor/musl.files.sha256", "musl.lock.json", "musl-1.0",
     {"arch/x86_64/bits.h", "include/stdio.h", "src/stdio/puts.c", "COPYRIGHT", NULL},
     {"src/stdio/unselected.c", "include/unselected.h", "README", "tools/gen.sh"}},
    {1, "wasi-fixture.tar.gz", "vendor/wasi/dlmalloc.files.sha256", "vendor/wasi/dlmalloc.lock.json", "wasi-1.0",
     {"dlmalloc/src/dlmalloc.c", "libc-bottom-half/sources/a.c", "libc-top-half/sources/b.c", NULL, NULL},
     {"libc-top-half/sources/unselected.c", "README", NULL, NULL}}
};

typedef struct { const Kind *k; char *root, *archive, *input; const char *archive_sha, *manifest_sha; } Fx;

static char *body(const char *rel) { return sfmt("selected %s\n", rel); }

static void fx_put(const char *base, const char *rel, const char *text) {
    char *p = PJ(base, rel);
    mkdirs(path_dir(p));
    write_file(p, text);
}

static void lock_write(const Fx *f, const char *note) {
    fx_put(f->root, f->k->lock, sfmt(
        "{\"name\":\"fixture\",\"url\":\"https://invalid.invalid/%s\",\"archive\":\"%s\","
        "\"sha256\":\"%s\",\"strip_prefix\":\"%s\",\"files_manifest\":\"%s\","
        "\"files_manifest_sha256\":\"%s\",\"note\":\"%s\"}\n",
        f->k->archive, f->k->archive, f->archive_sha, f->k->prefix, f->k->manifest, f->manifest_sha, note));
}

static void manifest_write(Fx *f, const char *extra_line, int bad_hash) {
    Buf m = {0};
    for (int i = 0; f->k->sel[i]; i++) {
        char *h = sha256_file(PJ(f->input, f->k->prefix, f->k->sel[i]));
        buf_addf(&m, "%s  %s\n", bad_hash && i == 0 ? "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" : h, f->k->sel[i]);
    }
    if (extra_line) buf_addf(&m, "%s\n", extra_line);
    fx_put(f->root, f->k->manifest, buf_cstr(&m));
    f->manifest_sha = sha256_file(PJ(f->root, f->k->manifest));
    lock_write(f, "");
}

static Fx fx_setup(const char *base, const char *name, int kind, const char *extra_line, int bad_hash) {
    Fx f = {0};
    f.k = &KINDS[kind];
    f.root = PJ(base, name);
    remove_all(f.root);
    f.input = PJ(f.root, "input");
    for (int i = 0; f.k->sel[i]; i++) fx_put(PJ(f.input, f.k->prefix), f.k->sel[i], body(f.k->sel[i]));
    for (int i = 0; i < 4 && f.k->extra[i]; i++) fx_put(PJ(f.input, f.k->prefix), f.k->extra[i], "upstream source we do not ship\n");
    f.archive = PJ(f.root, ".cache", "downloads", f.k->archive);
    mkdirs(path_dir(f.archive));
    char *tar[] = {"tar", "-czf", f.archive, "-C", f.input, (char *)f.k->prefix, NULL};
    must_run(tar, NULL, NULL);
    f.archive_sha = sha256_file(f.archive);
    manifest_write(&f, extra_line, bad_hash);
    if (kind == 1) { /* tracked local WASI files live beside generated dirs */
        fx_put(f.root, "vendor/wasi/uname.c", "local uname\n");
        fx_put(f.root, "vendor/wasi/valloc.c", "local valloc\n");
        fx_put(f.root, "vendor/wasi/LICENSE", "local license\n");
    }
    return f;
}

static char *self_path;

/* Runs a child action; stdout and (where sh exists) stderr come back in *out. */
static int fx_child(const Fx *f, const char *action, char **out) {
    char *idx = sfmt("%d", f->k->kind);
#ifdef _WIN32
    char *args[] = {self_path, (char *)action, f->root, idx, NULL};
#else
    char *args[] = {"sh", "-c", "exec \"$0\" \"$@\" 2>&1", self_path, (char *)action, f->root, idx, NULL};
#endif
    *out = NULL;
    return run_capture(args, NULL, NULL, out);
}

static int fx_prepare(const Fx *f, const char *msg) {
    char *out;
    int code = fx_child(f, "--prepare", &out);
#ifndef _WIN32
    if (msg) CHECK(out && contains(out, msg));
#else
    if (msg && code == 0) CHECK(out && contains(out, msg));
#endif
    return code;
}

static int fx_ready(const Fx *f) {
    Opts o = {0};
    o.root = f->root;
    SupplementInput in = supplement_input(&o, &SUPPLEMENTS[f->k->kind]);
    return supplement_ready(&o, &SUPPLEMENTS[f->k->kind], &in);
}

static char *fx_stage(const Fx *f) { return PJ(f->root, ".cache", sfmt("supplements-%s-stage", SUPPLEMENTS[f->k->kind].name)); }
static char *fx_gen(const Fx *f, const char *rel) { return PJ(f->root, SUPPLEMENTS[f->k->kind].root, rel); }

/* Exactly the selected files exist, with their content, and nothing else. */
static void check_exact(const Fx *f) {
    int total = 0;
    const Supplement *s = &SUPPLEMENTS[f->k->kind];
    for (int i = 0; i < 4 && s->dirs[i]; i++) total += supplement_file_count(fx_gen(f, s->dirs[i]));
    int n = 0;
    for (int i = 0; f->k->sel[i]; i++, n++) {
        CHECK(is_file(fx_gen(f, f->k->sel[i])));
        CHECK(!strcmp(read_file(fx_gen(f, f->k->sel[i]), NULL), body(f->k->sel[i])));
    }
    CHECK(total == n);
    for (int i = 0; i < 4 && f->k->extra[i]; i++) CHECK(!exists(fx_gen(f, f->k->extra[i])));
}

static int nothing_published(const Fx *f) {
    const Supplement *s = &SUPPLEMENTS[f->k->kind];
    for (int i = 0; i < 4 && s->dirs[i]; i++) if (exists(fx_gen(f, s->dirs[i]))) return 0;
    return !exists(PJ(f->root, "vendor", sfmt(".resources-%s-receipt", s->name)));
}

static void check_wasi_local(const Fx *f) {
    CHECK(!strcmp(read_file(PJ(f->root, "vendor/wasi/uname.c"), NULL), "local uname\n"));
    CHECK(!strcmp(read_file(PJ(f->root, "vendor/wasi/valloc.c"), NULL), "local valloc\n"));
    CHECK(!strcmp(read_file(PJ(f->root, "vendor/wasi/LICENSE"), NULL), "local license\n"));
    CHECK(is_file(PJ(f->root, f->k->lock)) && is_file(PJ(f->root, f->k->manifest)));
}

int main(int argc, char **argv) {
    tool_name = "supplement_resources_test";
    if (argc == 4) {
        Opts o = {0};
        o.root = argv[2];
        const Supplement *s = &SUPPLEMENTS[atoi(argv[3])];
        if (!strcmp(argv[1], "--prepare")) supplement_prepare(&o, s);
        else fail("unknown test action");
        return 0;
    }
    self_path = abs_path(argv[0]);
    char *base = abs_path(sfmt("build/tests/supplement-fixture-%lu", (unsigned long)(size_t)&failures));
    if (exists(base)) fail("test fixture already exists: %s", base);

    /* Normal extraction: only the manifest's files, no other upstream source. */
    Fx f = fx_setup(base, "musl-normal", 0, NULL, 0);
    CHECK(!fx_ready(&f));
    CHECK(fx_prepare(&f, "SHA-256 verified archive") == 0);
    CHECK(fx_ready(&f));
    check_exact(&f);
    CHECK(!exists(fx_stage(&f)));

    /* Verified reuse needs neither the archive nor a downloader. */
    char *saved = PJ(f.root, "saved.tar.gz");
    rename_path(f.archive, saved);
    CHECK(fx_prepare(&f, "verified cache; no network") == 0);
    CHECK(!exists(f.archive));

    /* A missing whole generated directory is not ready... */
    remove_all(fx_gen(&f, "include"));
    CHECK(!fx_ready(&f));
    /* ...but is repaired from the verified cached archive, keeping the others. */
    rename_path(saved, f.archive);
    CHECK(fx_prepare(&f, "SHA-256 verified archive") == 0);
    CHECK(fx_ready(&f));
    check_exact(&f);

    /* Modified, extra and symlinked cached files are preserved and refused. */
    fx_put(f.root, "vendor/musl/src/stdio/puts.c", "local edit\n");
    CHECK(!fx_ready(&f));
    CHECK(fx_prepare(&f, "modified musl supplement") != 0);
    CHECK(!strcmp(read_file(fx_gen(&f, "src/stdio/puts.c"), NULL), "local edit\n"));
    fx_put(f.root, "vendor/musl/src/stdio/puts.c", body("src/stdio/puts.c"));
    CHECK(fx_ready(&f));
    fx_put(f.root, "vendor/musl/src/stdio/mine.c", "mine\n");
    CHECK(!fx_ready(&f));
    remove_all(fx_gen(&f, "include")); /* repairable dir + extra file elsewhere */
    CHECK(fx_prepare(&f, "missing or extra files") != 0);
    CHECK(!strcmp(read_file(fx_gen(&f, "src/stdio/mine.c"), NULL), "mine\n"));
    remove_all(fx_stage(&f));
    remove_file(fx_gen(&f, "src/stdio/mine.c"));
    CHECK(fx_prepare(&f, NULL) == 0);
    CHECK(fx_ready(&f));
#ifndef _WIN32
    remove_file(fx_gen(&f, "COPYRIGHT"));
    CHECK(symlink(fx_gen(&f, "src/stdio/puts.c"), fx_gen(&f, "COPYRIGHT")) == 0);
    CHECK(!fx_ready(&f));
    CHECK(fx_prepare(&f, "modified musl supplement") != 0);
    CHECK(is_symlink(fx_gen(&f, "COPYRIGHT")));
    remove_file(fx_gen(&f, "COPYRIGHT"));
    CHECK(symlink(PJ(f.root, "missing"), fx_gen(&f, "COPYRIGHT")) == 0);
    CHECK(fx_prepare(&f, "modified musl supplement") != 0);
    CHECK(is_symlink(fx_gen(&f, "COPYRIGHT")));

    f = fx_setup(base, "musl-dangling-dir", 0, NULL, 0);
    CHECK(fx_prepare(&f, NULL) == 0);
    remove_all(fx_gen(&f, "arch"));
    CHECK(symlink(PJ(f.root, "missing"), fx_gen(&f, "arch")) == 0);
    CHECK(fx_prepare(&f, "modified musl supplement") != 0);
    CHECK(is_symlink(fx_gen(&f, "arch")));

    f = fx_setup(base, "musl-root-link", 0, NULL, 0);
    CHECK(fx_prepare(&f, NULL) == 0);
    char *tree = PJ(f.root, "saved-musl");
    rename_path(fx_gen(&f, ""), tree);
    CHECK(symlink(tree, fx_gen(&f, "")) == 0);
    CHECK(!fx_ready(&f));
    CHECK(fx_prepare(&f, "symlink supplement root") != 0);
    CHECK(is_symlink(fx_gen(&f, "")));
    CHECK(is_file(PJ(tree, "src/stdio/puts.c")));
#endif

    /* A changed lock invalidates the receipt but never deletes local files. */
    f = fx_setup(base, "musl-stale", 0, NULL, 0);
    CHECK(fx_prepare(&f, NULL) == 0);
    lock_write(&f, "changed");
    CHECK(!fx_ready(&f));
    CHECK(fx_prepare(&f, "unrecognized or stale musl supplements") != 0);
    check_exact(&f);
    lock_write(&f, "");
    CHECK(fx_ready(&f));
    fx_put(f.root, f.k->manifest, "garbage\n"); /* manifest no longer matches the lock */
    CHECK(fx_prepare(&f, "manifest SHA-256 mismatch") != 0);

    /* Unknown preexisting generated paths are never clobbered. */
    f = fx_setup(base, "musl-unknown", 0, NULL, 0);
    fx_put(f.root, "vendor/musl/src/user.c", "user data\n");
    CHECK(fx_prepare(&f, "unrecognized or stale musl supplements") != 0);
    CHECK(!strcmp(read_file(fx_gen(&f, "src/user.c"), NULL), "user data\n"));
    CHECK(!exists(fx_gen(&f, "include")) && !exists(fx_gen(&f, "arch")) && !exists(fx_gen(&f, "COPYRIGHT")));
    CHECK(!exists(fx_stage(&f)));

    /* A bad archive is rejected before anything is published. */
    f = fx_setup(base, "musl-corrupt", 0, NULL, 0);
    write_file(f.archive, "corrupt archive");
    CHECK(fx_prepare(&f, "hash mismatch") != 0);
    CHECK(nothing_published(&f));
    CHECK(!is_dir(PJ(fx_stage(&f), "selected")));

    /* A selected-file hash that disagrees with the archive is rejected. */
    f = fx_setup(base, "musl-badfile", 0, NULL, 1);
    CHECK(fx_prepare(&f, "source SHA-256 mismatch") != 0);
    CHECK(nothing_published(&f));

    /* Unsafe manifest paths are rejected before any stage or output exists. */
    const char *h = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const char *bad[] = {"../evil.c", "src/../../evil.c", "/abs/evil.c", "other/file.c", "src//x.c", "src/x/",
                         "src\\x.c", "C:/x.c", "src/./x.c", "srcx/file.c", "src/stdio/puts.c", NULL};
    for (int i = 0; bad[i]; i++) {
        f = fx_setup(base, "musl-badpath", 0, sfmt("%s  %s", h, bad[i]), 0);
        CHECK(fx_prepare(&f, i == 10 ? "duplicate supplement path" : "unsafe supplement path") != 0);
        CHECK(nothing_published(&f));
        CHECK(!exists(fx_stage(&f)));
        CHECK(!exists(PJ(base, "evil.c")) && !exists(PJ(f.root, "evil.c")));
    }
    f = fx_setup(base, "musl-badhashfmt", 0, "nothex  src/x.c", 0);
    CHECK(fx_prepare(&f, "invalid supplement manifest line") != 0);
    CHECK(nothing_published(&f));

    /* An interrupted or concurrent stage claim is refused and left intact. */
    f = fx_setup(base, "musl-stage", 0, NULL, 0);
    mkdirs(fx_stage(&f));
    CHECK(fx_prepare(&f, "exists or cannot be claimed") != 0);
    CHECK(is_dir(fx_stage(&f)));
    CHECK(nothing_published(&f));

    /* WASI: three generated dirs are rebuilt; tracked local files are untouched. */
    f = fx_setup(base, "wasi", 1, NULL, 0);
    CHECK(fx_prepare(&f, "wasi supplements ready (SHA-256 verified") == 0);
    CHECK(fx_ready(&f));
    check_exact(&f);
    check_wasi_local(&f);
    CHECK(is_file(PJ(f.root, "vendor", ".resources-wasi-receipt")));
    rename_path(f.archive, PJ(f.root, "saved.tar.gz"));
    CHECK(fx_prepare(&f, "verified cache; no network") == 0);
    rename_path(PJ(f.root, "saved.tar.gz"), f.archive);
    remove_all(fx_gen(&f, "libc-top-half"));
    CHECK(fx_prepare(&f, "SHA-256 verified") == 0);
    check_exact(&f);
    check_wasi_local(&f);
    /* Local files in the supplement root are not generated paths, so they never
     * make the tree "unknown"; modified generated files still are refused. */
    fx_put(f.root, "vendor/wasi/libc-bottom-half/sources/a.c", "edit\n");
    CHECK(fx_prepare(&f, "modified wasi supplement") != 0);
    check_wasi_local(&f);

    remove_all(base);
    if (failures) { fprintf(stderr, "%d supplement test failures\n", failures); return 1; }
    puts("supplement resource tests passed (offline)");
    return 0;
}
