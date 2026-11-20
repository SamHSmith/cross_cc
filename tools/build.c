/* cross_cc setup/build tool. Plain C, compiled by the launcher with the
 * selected seed: <seed> cc -O1 -o build/tools/build tools/build.c, then
 * build/tools/build [command] --compiler=<seed>. */
#include "common.h"

static const char USAGE[] =
    "cross_cc build tool (normally run through the launcher, which selects the seed)\n"
    "\n"
    "usage: build/tools/build [command] --compiler=/abs/cross_cc [options]   (default command: build)\n"
    "\n"
    "commands:\n"
    "  resources    download + SHA-256 verify the pinned source archive and populate lib/\n"
    "  setup        resources plus compiler-rt and libbacktrace sources (--reference: test binary)\n"
    "  prepare      setup, the pinned host LLVM SDK, the GNU-ABI support SDK (linux-release-deps),\n"
    "               native-dir sources and the host recipe .cache/native/selfhost.json; every\n"
    "               compilation uses the seed only. Hosts: linux-amd64 (pinned GNU-ABI support\n"
    "               SDK), darwin-arm64 (system Apple SDK; --sdk or SDKROOT), darwin-amd64\n"
    "               (LLVM_PREFIX from tools/macos-x64-llvm), windows-amd64 (--sdk=MSVC splat whose\n"
    "               license you accepted); --host-config=FILE supplies a complete recipe instead\n"
    "  build        prepare, then the staged release pipeline publishes bin/cross_cc\n"
    "               (--profile=debug|release) and the argv0 alias copies\n"
    "  release      prepare, then the release tool with every other --key=value forwarded\n"
    "  test         unit programs (compiled by the seed), then integration tests of bin/cross_cc\n"
    "  package      stage bin/cross_cc + lib/ into build/dist (--target, --single-executable,\n"
    "               --extra-licenses, --msvc-helper, --upstream, --binary)\n"
    "  tools        recompile the sibling helper tools with the seed\n"
    "  check-tools  compile every tool with -Wall using the seed and run the path helper tests\n"
    "  native       compatibility alias of build\n"
    "  help         this text\n"
    "\n"
    "options: --compiler=FILE --profile=debug|release --jobs=N --deps-prefix=DIR (exact\n"
    "  linux-release-deps prefix; default .cache/support/linux-amd64-<key>; darwin: holds lib/libzstd.a)\n"
    "  --host-sdk=DIR --host-config=FILE --reference\n"
    "  --sdk=DIR is a host-sdk alias except for release, where it selects the target SDK\n"
    "environment: LLVM_PREFIX (LLVM 22.1.8 static SDK instead of the pinned archive),\n"
    "  CROSS_CC_SEED_ROOT (seed resources; else found above the seed binary),\n"
    "  CROSS_CC_HOST_SDK (windows-amd64 MSVC splat, like --sdk)\n"
    "\n"
    "All temp/cache/build output stays under .cache/ and build/ (TMPDIR=.cache/tmp).\n"
    "Pins: upstream.lock.json, toolchain.lock.json, compiler-rt.lock.json.\n";

typedef struct {
    char *root;
    char *native_dir; /* optional private output; never mutate a shared native SDK */
    int reference;
    int pinned;
    char *opt;
    char *output;
    char *binary;
    char *upstream;
    char *target; /* package only: artifact platform, never the bootstrap host */
    int single_executable;
    char *extra_licenses; /* package only: notices for explicitly selected support SDK */
    char *msvc_helper;    /* package only: authenticated helper directory with notices */
    int jobs;
    char *compiler;    /* --compiler: absolute selected seed cross_cc */
    char *seed_root;   /* its packaged resources (require_seed) */
    char *profile;     /* --profile=debug|release, forwarded to the release pipeline */
    char *deps_prefix; /* --deps-prefix: exact linux-release-deps prefix (helper verifies its receipt) */
    char *self_dir;    /* directory of this executable: sibling release/deps helpers */
    Strs forward;      /* release: options forwarded verbatim to the release tool */
    char *host_config; /* --host-config: complete host recipe, bypasses generated recipes */
    char *host_sdk;    /* --sdk: Apple SDK or MSVC splat for darwin/windows hosts */
} Opts;

static const char *const ALIASES[] = {"cross_c++", "cross_clang", "ld.lld", "ld64.lld", "lld-link", "wasm-ld", "llvm-ar", "llvm-ranlib", NULL};

static char *the_tmpdir;
static Strs base_env(const Opts *o) {
    static const char *const drop[] = {"TMPDIR", NULL};
    Strs extra = {0};
    strs_addf(&extra, "TMPDIR=%s", PJ(o->root, ".cache", "tmp"));
    (void)the_tmpdir;
    return make_env(drop, &extra);
}

static int run(const Opts *o, const char *cwd, Strs *cmd) {
    Strs env = base_env(o);
    return run_cmd(cmd->v, cwd, env.v);
}
static void must(const Opts *o, const char *cwd, Strs *cmd) {
    int code = run(o, cwd, cmd);
    if (code != 0) fail("%s failed (exit %d)", cmd->v[0], code);
}
/* argv-literal convenience: must_l(o, cwd, "tar", "-xf", ..., NULL). */
static void must_l(const Opts *o, const char *cwd, ...) {
    Strs cmd = {0};
    va_list ap;
    va_start(ap, cwd);
    for (const char *a = va_arg(ap, const char *); a; a = va_arg(ap, const char *)) strs_add(&cmd, a);
    va_end(ap);
    must(o, cwd, &cmd);
}
static char *capture(const Opts *o, Strs *cmd) {
    Strs env = base_env(o);
    return must_capture(cmd->v, NULL, env.v);
}
static char *capture_l(const Opts *o, ...) {
    Strs cmd = {0};
    va_list ap;
    va_start(ap, o);
    for (const char *a = va_arg(ap, const char *); a; a = va_arg(ap, const char *)) strs_add(&cmd, a);
    va_end(ap);
    return capture(o, &cmd);
}

/* Download to <dest>.part, verify hash BEFORE it becomes <dest>. A verified
 * existing file is reused. */
static void fetch(const char *url, const char *sha, const char *dest) { download_verified(url, sha, dest, 1800); }

/* Extract archive into a staging dir, then rename `stage/<top>` to final. */
static void extract(const Opts *o, const char *archive, const char *tar_flag, const char *stage, const char *final, const char *top) {
    if (exists(final)) return;
    remove_all(stage);
    mkdirs(stage);
    Strs cmd = {0};
    strs_add(&cmd, "tar");
    if (tar_flag) strs_add(&cmd, tar_flag);
    strs_add(&cmd, "-xf");
    strs_add(&cmd, archive);
    strs_add(&cmd, "-C");
    strs_add(&cmd, stage);
    must(o, o->root, &cmd);
    char *src = *top ? PJ(stage, top) : xstrdup(stage);
    if (!exists(src)) fail("archive %s lacks %s", archive, top);
    mkdirs(path_dir(final));
    rename_path(src, final);
    remove_all(stage);
}

static const char *host_key(void) {
    if (!strcmp(HOST_OS, "unknown") || !strcmp(HOST_ARCH, "unknown")) fail("unsupported bootstrap host; provide LLVM_PREFIX and use Linux, macOS, or Windows");
    return sfmt("%s-%s", HOST_OS, HOST_ARCH);
}


/* Only setup/resources acquires source dependencies. Build/package stay offline.
 * The receipt binds the extracted cache to both the archive and selection recipe.
 * Never replace an unrecognized lib/ tree (it might contain local patches). */
static const char *const RESOURCE_DIRS[] = {"include", "libc", "libcxx", "libcxxabi", "libunwind", "libtsan", NULL};

static char *resource_identity(const Opts *o) {
    Json *pin = jobj(json_load(PJ(o->root, "upstream.lock.json")), "resources");
    Json *dirs = jarr_opt(pin, "extract");
    if (!dirs) fail("resources.extract is missing");
    Buf id = {0};
    buf_addf(&id, "cross_cc-resources-v1\n%s\n%s\n", jstr(pin, "sha256"), jstr(pin, "strip_prefix"));
    int n = 0;
    for (; RESOURCE_DIRS[n]; n++) {
        if (n >= dirs->n || dirs->items[n]->type != J_STR || strcmp(dirs->items[n]->str, RESOURCE_DIRS[n]))
            fail("resources.extract must list the six supported lib/ directories in order");
        buf_addf(&id, "%s\n", RESOURCE_DIRS[n]);
    }
    if (dirs->n != n) fail("resources.extract has unsupported directories");
    return xstrdup(buf_cstr(&id));
}

static int resources_ready(const Opts *o, const char *identity) {
    char *receipt = PJ(o->root, "lib", ".resources-receipt");
    if (!is_file(receipt) || strcmp(read_file(receipt, NULL), identity)) return 0;
    for (int i = 0; RESOURCE_DIRS[i]; i++)
        if (!is_dir(PJ(o->root, "lib", RESOURCE_DIRS[i]))) return 0;
    return 1;
}

static void require_resources(const Opts *o) {
    if (!resources_ready(o, resource_identity(o)))
        fail("lib/ resources missing or stale; run ./build.sh resources (build.cmd on Windows)");
}

static void resources(const Opts *o) {
    char *identity = resource_identity(o);
    if (resources_ready(o, identity)) {
        printf("lib/ resources ready (cached; no network)\n");
        return;
    }
    char *receipt = PJ(o->root, "lib", ".resources-receipt");
    int recognized = is_file(receipt) && !strcmp(read_file(receipt, NULL), identity);
    for (int i = 0; RESOURCE_DIRS[i]; i++) {
        char *dir = PJ(o->root, "lib", RESOURCE_DIRS[i]);
        if (exists(dir) && !recognized)
            fail("unrecognized or stale resources at %s; move the six generated lib/ directories and .resources-receipt aside, then rerun resources", dir);
    }
    Json *pin = jobj(json_load(PJ(o->root, "upstream.lock.json")), "resources");
    const char *url = jstr(pin, "url");
    char *archive = PJ(o->root, ".cache", "downloads", last_segment(url));
    char *stage = PJ(o->root, ".cache", "resources-stage");
    mkdirs(PJ(o->root, ".cache", "tmp"));
    /* Exclusive ownership covers download .part, extraction and publication. */
#ifdef _WIN32
    int claimed = _mkdir(stage) == 0;
#else
    int claimed = mkdir(stage, 0777) == 0;
#endif
    if (!claimed) fail("resource stage %s exists or cannot be claimed (active/interrupted run); move it aside only after that run stops", stage);
    fetch(url, jstr(pin, "sha256"), archive);
    Strs cmd = {0};
    strs_add(&cmd, "tar");
    strs_add(&cmd, "-xJf");
    strs_add(&cmd, archive);
    strs_add(&cmd, "-C");
    strs_add(&cmd, stage);
    strs_add(&cmd, "--strip-components=2");
    for (int i = 0; RESOURCE_DIRS[i]; i++)
        strs_addf(&cmd, "%s/%s", jstr(pin, "strip_prefix"), RESOURCE_DIRS[i]);
    must(o, o->root, &cmd);
    for (int i = 0; RESOURCE_DIRS[i]; i++)
        if (!is_dir(PJ(stage, RESOURCE_DIRS[i]))) fail("resource archive lacks %s", RESOURCE_DIRS[i]);
    mkdirs(PJ(o->root, "lib"));
    for (int i = 0; RESOURCE_DIRS[i]; i++) {
        char *dst = PJ(o->root, "lib", RESOURCE_DIRS[i]);
        if (!exists(dst)) rename_path(PJ(stage, RESOURCE_DIRS[i]), dst);
    }
    char *part = sfmt("%s.part", receipt);
    write_file(part, identity);
    rename_path(part, receipt);
    remove_all(stage);
    printf("lib/ resources ready (SHA-256 verified source archive)\n");
}

static void setup(Opts *o) {
    resources(o);
    char *dl = PJ(o->root, ".cache", "downloads");
    mkdirs(dl);
    mkdirs(PJ(o->root, ".cache", "tmp"));
    mkdirs(PJ(o->root, "build"));
    mkdirs(PJ(o->root, "bin"));
    Json *upstream = json_load(PJ(o->root, "upstream.lock.json"));
    Json *bt = jobj(upstream, "libbacktrace");
    char *btname = sfmt("libbacktrace-%s", jstr(bt, "commit"));
    char *btarc = PJ(dl, sfmt("%s.tar.gz", btname));
    fetch(jstr(bt, "url"), jstr(bt, "sha256"), btarc);
    extract(o, btarc, "-z", PJ(o->root, ".cache", "upstream", ".backtrace-stage"), PJ(o->root, ".cache", "upstream", btname), btname);
    printf("libbacktrace sources ready (%s)\n", btname);

    if (o->reference) {
        if (strcmp(HOST_OS, "linux") || strcmp(HOST_ARCH, "amd64")) fail("--reference currently pins a Linux amd64 test binary only; normal setup/native/build do not need it");
        Json *ref = jobj(upstream, "reference_binary");
        const char *rn = jstr(ref, "directory");
        char *rarc = PJ(dl, sfmt("%s.tar.xz", rn));
        fetch(jstr(ref, "url"), jstr(ref, "sha256"), rarc);
        extract(o, rarc, "-J", PJ(o->root, ".cache", "reference", ".stage"), PJ(o->root, ".cache", "reference", rn), rn);
        printf("reference binary ready (tests only, never a backend)\n");
    }

    char *crtlock = PJ(o->root, "compiler-rt.lock.json");
    if (exists(crtlock)) {
        Json *crt = json_load(crtlock);
        char *crtdir = PJ(o->root, ".cache", "upstream", sfmt("compiler-rt-%s", jstr(crt, "version")));
        if (!exists(PJ(crtdir, "compiler-rt", "lib", "builtins"))) {
            const char *url = jstr(crt, "url");
            char *carc = PJ(dl, last_segment(url));
            fetch(url, jstr(crt, "sha256"), carc);
            char *stage = PJ(o->root, ".cache", "upstream", ".crt-stage");
            remove_all(stage);
            mkdirs(stage);
            Strs cmd = {0};
            strs_add(&cmd, "tar");
            strs_add(&cmd, "-xJf");
            strs_add(&cmd, carc);
            strs_add(&cmd, "-C");
            strs_add(&cmd, stage);
            strs_add(&cmd, "--strip-components=1");
            const char *pfx = jstr(crt, "strip_prefix");
            Json *ex = jarr_opt(crt, "extract");
            for (int i = 0; ex && i < ex->n; i++) {
                if (ex->items[i]->type == J_STR) strs_addf(&cmd, "%s/%s", pfx, ex->items[i]->str);
            }
            must(o, o->root, &cmd);
            rename_path(stage, crtdir);
        }
        printf("compiler-rt sources ready (%s)\n", crtdir);
    }

}

/* ---- the selected seed compiler (never a system cc/clang/ld/ar/make) ---- */
static const char *const SEED_DROP[] = {"TMPDIR", "TMP", "TEMP", "CROSS_CC_ROOT", "CROSS_CC_LIB_DIR", "CROSS_CC_MSVC", "CROSS_CC_CACHE_DIR", "ZIG_LIB_DIR", "ZIG_GLOBAL_CACHE_DIR", "ZIG_LOCAL_CACHE_DIR", "ZIG_LIBC", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH", "LIBRARY_PATH", "CCC_OVERRIDE_OPTIONS", "SDKROOT", "CC", "CXX", "LD", "AR", "CFLAGS", "CXXFLAGS", "LDFLAGS", NULL};

static int seed_root_ok(const char *d) { return exists(PJ(d, "upstream.lock.json")) && is_dir(PJ(d, "lib", "include")) && is_dir(PJ(d, "lib", "libc")); }

static char *real_or_abs(const char *p) {
#ifndef _WIN32
    char *r = realpath(p, NULL);
    if (r) return r;
#endif
    return abs_path(p);
}

/* Validate --compiler and find its own packaged resources: CROSS_CC_SEED_ROOT
 * (exported by the launcher), else the nearest ancestor of the resolved binary
 * holding upstream.lock.json + lib/. Never the checkout's resources. */
static void require_seed(Opts *o) {
    if (o->seed_root) return;
    if (!o->compiler || !*o->compiler) fail("%s: pass --compiler=FILE (the selected cross_cc seed; the launcher does this)", tool_name);
    if (!is_abs(o->compiler)) fail("--compiler must be an absolute path, got %s", o->compiler);
    if (!is_file(o->compiler)) fail("--compiler %s does not exist", o->compiler);
    const char *e = env_get("CROSS_CC_SEED_ROOT");
    if (*e) {
        if (!seed_root_ok(e)) fail("CROSS_CC_SEED_ROOT=%s lacks upstream.lock.json and lib/", e);
        o->seed_root = real_or_abs(e);
        return;
    }
    char *d = path_dir(real_or_abs(o->compiler));
    for (int i = 0; i < 4; i++) {
        if (seed_root_ok(d)) {
            o->seed_root = d;
            return;
        }
        if (seed_root_ok(PJ(d, "share", "cross_cc"))) {
            o->seed_root = PJ(d, "share", "cross_cc");
            return;
        }
        char *up = path_dir(d);
        if (!strcmp(up, d)) break;
        d = up;
    }
    fail("cannot find resources of seed %s (upstream.lock.json + lib/ in an ancestor); set CROSS_CC_SEED_ROOT", o->compiler);
}

/* Environment for anything the seed runs: no ambient compiler/resource
 * overrides; resources from the seed's own package; caches under .cache. */
static Strs seed_env(const Opts *o) {
    Strs extra = {0};
    strs_addf(&extra, "TMPDIR=%s", PJ(o->root, ".cache", "tmp"));
    strs_addf(&extra, "CROSS_CC_ROOT=%s", o->seed_root);
    strs_addf(&extra, "CROSS_CC_CACHE_DIR=%s", PJ(o->root, ".cache", "seed-cc"));
    return make_env(SEED_DROP, &extra);
}
static void seed_must(const Opts *o, const char *cwd, Strs *cmd) {
    Strs env = seed_env(o);
    mkdirs(PJ(o->root, ".cache", "tmp"));
    int code = run_cmd(cmd->v, cwd, env.v);
    if (code != 0) fail("%s failed (exit %d)", cmd->v[0], code);
}

static void prepare(Opts *o);
static char *linux_deps_prefix(const Opts *o);
static void build(Opts *o);

/* Compile one standalone C test program and run it from the checkout root. */
static void run_c_test(Opts *o, const char *source, const char *name) {
    mkdirs(PJ(o->root, "build", "tests"));
    char *exe = PJ(o->root, "build", "tests", sfmt("%s%s", name, EXE_SUFFIX));
    require_seed(o);
    Strs cmd = {0};
    strs_add(&cmd, o->compiler);
    strs_add(&cmd, "cc");
    strs_add(&cmd, "-std=c11");
    strs_add(&cmd, "-O1");
    strs_addf(&cmd, "-I%s", PJ(o->root, "src"));
    strs_addf(&cmd, "-I%s", PJ(o->root, "tests"));
    strs_add(&cmd, source);
    strs_add(&cmd, "-o");
    strs_add(&cmd, exe);
    printf("test: build %s\n", name);
    seed_must(o, o->root, &cmd);
    Strs run_line = {0};
    strs_add(&run_line, exe);
    if (ends_with(source, "_integration.c")) {
        /* Differential linker programs take CC, reference, scratch, target. */
        Json *ref = jobj(json_load(PJ(o->root, "upstream.lock.json")), "reference_binary");
        char *scratch = PJ(o->root, "build", "tests", sfmt("%s-data", name));
        mkdirs(scratch);
        strs_add(&run_line, PJ(o->root, "bin", sfmt("cross_cc%s", EXE_SUFFIX)));
        strs_add(&run_line, PJ(o->root, ".cache", "reference", jstr(ref, "directory"), sfmt("zig%s", EXE_SUFFIX)));
        strs_add(&run_line, scratch);
        strs_add(&run_line, "x86_64-linux-musl");
    }
    printf("test: run %s\n", name);
    must(o, o->root, &run_line);
}

/* `test` runs, in order: the Windows path helper tests, every tests/ *_test.c
 * unit program (no binaries needed), then the integration programs that drive
 * the built bin/cross_cc: tests/smoke.c and every tests/ *_integration.c. */
static void test(Opts *o) {
    require_seed(o);
    /* Tests that drive a raw clang use the checkout's alias, never a system clang. */
    char *clang = PJ(o->root, "bin", sfmt("cross_clang%s", EXE_SUFFIX));
#ifdef _WIN32
    _putenv_s("CLANG", clang);
    _putenv_s("CROSS_CC_TEST_LINKER", PJ(o->root, "bin", "lld-link.exe"));
#else
    setenv("CLANG", clang, 1);
    setenv("CROSS_CC_TEST_LINKER", PJ(o->root, "bin", !strncmp(host_key(), "darwin-", 7) ? "ld64.lld" : "ld.lld"), 1);
#endif
    char *wp_src = PJ(o->root, "tools", "windows_path", "windows_path_test.c");
    if (exists(wp_src)) run_c_test(o, wp_src, "windows_path_test");
    Strs names = {0};
    char *tests = PJ(o->root, "tests");
    if (!list_dir(tests, &names)) fail("tests/ directory missing");
    int units = 0;
    for (int i = 0; i < names.n; i++) {
        if (ends_with(names.v[i], "_test.c")) {
            run_c_test(o, PJ(tests, names.v[i]), path_stem(names.v[i]));
            units++;
        }
    }
    printf("test: %d unit programs passed\n", units);
    /* Seeded support-SDK check: only once prepare has built the default prefix. */
    char *sd_src = PJ(tests, "support_deps_check.c");
    char *sd_prefix = linux_deps_prefix(o);
    if (is_file(sd_src) && !strcmp(host_key(), "linux-amd64") && is_file(PJ(sd_prefix, "complete.sha256"))) {
        char *exe = PJ(o->root, "build", "tests", "support_deps_check");
        Strs cc = {0};
        strs_add(&cc, o->compiler);
        strs_add(&cc, "cc");
        strs_add(&cc, "-std=c11");
        strs_add(&cc, "-O1");
        strs_add(&cc, "-o");
        strs_add(&cc, exe);
        strs_add(&cc, sd_src);
        seed_must(o, o->root, &cc);
        char *work = PJ(o->root, "build", "tests", "support-deps");
        mkdirs(work);
        Strs line = {0};
        strs_add(&line, exe);
        strs_add(&line, o->compiler);
        strs_add(&line, sd_prefix);
        strs_add(&line, work);
        printf("test: run support_deps_check\n");
        seed_must(o, o->root, &line);
    }
    static const char *const needed[] = {"cross_cc", "cross_c++", NULL};
    for (int i = 0; needed[i]; i++) {
        if (!exists(PJ(o->root, "bin", sfmt("%s%s", needed[i], EXE_SUFFIX)))) fail("bin/%s%s missing: run `build` first", needed[i], EXE_SUFFIX);
    }
    Json *ref = jobj(json_load(PJ(o->root, "upstream.lock.json")), "reference_binary");
    char *reference = PJ(o->root, ".cache", "reference", jstr(ref, "directory"), sfmt("zig%s", EXE_SUFFIX));
    if (!exists(reference)) fail("reference binary missing: run `setup --reference`");
    char *smoke = PJ(tests, "smoke.c");
    if (!exists(smoke)) fail("tests/smoke.c not found");
    run_c_test(o, smoke, "smoke");
    for (int i = 0; i < names.n; i++) {
        if (ends_with(names.v[i], "_integration.c")) run_c_test(o, PJ(tests, names.v[i]), path_stem(names.v[i]));
    }
}

static void must_copy_dir(const char *dst, const char *src) {
    mkdirs(path_dir(dst));
    copy_tree(dst, src);
}

static int dir_nonempty(const char *p) {
    Strs names = {0};
    return list_dir(p, &names) && names.n > 0;
}

/* package_dist assembles a self-contained tree: the compiler finds lib/ and
 * upstream.lock.json next to the executable, never the development checkout. */
static void package_dist(Opts *o) {
    require_resources(o);
    const char *target = o->target ? o->target : host_key();
    const char *suffix = "";
    if (!strcmp(target, "windows-amd64") || !strcmp(target, "windows-arm64")) suffix = ".exe";
    else if (strcmp(target, "linux-amd64") && strcmp(target, "linux-arm64") && strcmp(target, "darwin-amd64") && strcmp(target, "darwin-arm64")) fail("unsupported package target %s", target);
    const char *crt = jstr(json_load(PJ(o->root, "compiler-rt.lock.json")), "version");
    char *up = o->upstream ? o->upstream : PJ(o->root, ".cache", "upstream");
    char *libsrc = PJ(o->root, "lib");
    char *crtsrc = PJ(up, sfmt("compiler-rt-%s", crt), "compiler-rt");
    Json *bt = jobj(json_load(PJ(o->root, "upstream.lock.json")), "libbacktrace");
    char *btsrc = PJ(up, sfmt("libbacktrace-%s", jstr(bt, "commit")));
    if (!exists(PJ(btsrc, "backtrace.c"))) fail("libbacktrace sources missing at %s: run `setup`", btsrc);
    if (!exists(PJ(crtsrc, "lib", "builtins", "CMakeLists.txt"))) fail("compiler-rt sources missing at %s: run `setup`", crtsrc);
    char *bin = o->binary;
    if (!bin) {
        char *cands[] = {PJ(o->root, "bin", sfmt("cross_cc%s", suffix)), PJ(o->root, sfmt("cross_cc%s", suffix))};
        for (int i = 0; i < 2; i++) {
            if (exists(cands[i])) {
                bin = cands[i];
                break;
            }
        }
    }
    if (!bin || !exists(bin)) fail("no built cross_cc: run `build` or pass --binary=/abs/cross_cc");
    char *dest = o->output ? o->output : PJ(o->root, "dist", sfmt("cross_cc-%s", target));
    if (exists(dest)) {
        if (!is_dir(dest)) fail("%s exists and is not a directory", dest);
        if (dir_nonempty(dest)) fail("destination %s exists and is not empty; remove it yourself or choose --output=", dest);
    }
    /* Darwin has no upstream xwin release binary; its authenticated helper is
     * supplied by the release builder, never the Microsoft SDK itself. */
    char *helper = NULL;
    char *msvc_lock = PJ(o->root, "msvc.lock.json");
    if (exists(msvc_lock)) {
        Json *hosts = jobj(json_load(msvc_lock), "hosts");
        const char *expected = "";
        int bundled_only = 0;
        Json *hv = jget(hosts, target);
        if (hv && hv->type == J_OBJ) {
            expected = jstr_opt(hv, "binary_sha256");
            bundled_only = *expected && !*jstr_opt(hv, "url");
        }
        if ((starts_with(target, "darwin-") || bundled_only) && !o->msvc_helper) {
            fail("managed MSVC for %s uses a bundled helper and requires --msvc-helper=DIR (pinned xwin and notices)", target);
        }
        if (o->msvc_helper) {
            if (strlen(expected) != 64) fail("no bundled helper binary pin for %s", target);
            helper = PJ(o->msvc_helper, sfmt("xwin%s", suffix));
            if (!is_file(helper)) fail("managed MSVC helper missing: %s", helper);
            if (strcmp(sha256_file(helper), expected) != 0) fail("managed MSVC helper hash mismatch: %s", helper);
            static const char *const notices[] = {"LICENSE-APACHE", "LICENSE-MIT", "Cargo.lock", "dependency-notices.json", "rust-std-notices/LICENSE-APACHE", "rust-std-notices/LICENSE-MIT", "rust-std-notices/COPYRIGHT", NULL};
            for (int i = 0; notices[i]; i++) {
                if (file_size(PJ(o->msvc_helper, notices[i])) <= 0) fail("helper notice missing or empty: %s", notices[i]);
            }
            if (!dir_nonempty(PJ(o->msvc_helper, "dependency-notices"))) fail("helper dependency-notices directory missing or empty");
            if (file_size(PJ(o->msvc_helper, "sbom.spdx.json")) <= 0 && file_size(PJ(o->msvc_helper, "build-provenance.txt")) <= 0) {
                fail("helper needs sbom.spdx.json or build-provenance.txt");
            }
        }
    } else if (o->msvc_helper) {
        fail("msvc.lock.json required for --msvc-helper=");
    }
    size_t dl = strlen(dest);
    while (dl > 1 && is_sep(dest[dl - 1])) dl--;
    char *stage = sfmt("%.*s.stage", (int)dl, dest);
    if (exists(stage)) fail("stage %s exists from an interrupted run; remove it and retry", stage);
    mkdirs(stage);
    char *main_exe = PJ(stage, sfmt("cross_cc%s", suffix));
    copy_file(main_exe, bin);
    make_executable(main_exe);
    if (!o->single_executable) {
        for (int i = 0; ALIASES[i]; i++) link_or_copy(PJ(stage, sfmt("%s%s", ALIASES[i], suffix)), main_exe);
    }
    static const char *const libdirs[] = {"include", "libc", "libcxx", "libcxxabi", "libunwind", "libtsan", NULL};
    for (int i = 0; libdirs[i]; i++) must_copy_dir(PJ(stage, "lib", libdirs[i]), PJ(libsrc, libdirs[i]));
    must_copy_dir(PJ(stage, "lib", "compiler-rt"), crtsrc);
    /* Only the source/header closure used by the fixed recipe is shipped; no
     * configure, host-generated headers, scripts or test suite is needed. */
    mkdirs(PJ(stage, "lib", "libbacktrace"));
    static const char *const btfiles[] = {"atomic.c", "dwarf.c", "fileline.c", "posix.c", "sort.c", "state.c", "backtrace.c", "simple.c", "elf.c", "macho.c", "mmapio.c", "mmap.c", "backtrace.h", "internal.h", "filenames.h", "LICENSE", NULL};
    for (int i = 0; btfiles[i]; i++) copy_file(PJ(stage, "lib", "libbacktrace", btfiles[i]), PJ(btsrc, btfiles[i]));
    must_copy_dir(PJ(stage, "vendor"), PJ(o->root, "vendor"));
    must_copy_dir(PJ(stage, "licenses"), PJ(o->root, "licenses"));
    if (o->extra_licenses) must_copy_dir(PJ(stage, "licenses", "dependencies"), o->extra_licenses);
    if (helper) {
        mkdirs(PJ(stage, "libexec"));
        char *dst = PJ(stage, "libexec", sfmt("xwin%s", suffix));
        copy_file(dst, helper);
        make_executable(dst);
        mkdirs(PJ(stage, "licenses", "xwin"));
        static const char *const hfiles[] = {"LICENSE-APACHE", "LICENSE-MIT", "sbom.spdx.json", "Cargo.lock", "build-provenance.txt", "cargo-metadata.json", "dependency-notices.json", "source-verification.json", "release-static-crt.sha256", ".crates2.json", "formula-xwin.json", "xwin.rb", NULL};
        for (int i = 0; hfiles[i]; i++) {
            char *src = PJ(o->msvc_helper, hfiles[i]);
            if (file_size(src) < 0) continue;
            copy_file(PJ(stage, "licenses", "xwin", hfiles[i]), src);
        }
        static const char *const hdirs[] = {"notices", "dependency-notices", "rust-std-notices", NULL};
        for (int i = 0; hdirs[i]; i++) {
            char *nd = PJ(o->msvc_helper, hdirs[i]);
            if (is_dir(nd)) must_copy_dir(PJ(stage, "licenses", "xwin", hdirs[i]), nd);
        }
    }
    static const char *const toplevel[] = {"LICENSE", "COPYING", "upstream.lock.json", "compiler-rt.lock.json", "toolchain.lock.json", "musl.lock.json", "msvc.lock.json", NULL};
    for (int i = 0; toplevel[i]; i++) {
        if (exists(PJ(o->root, toplevel[i]))) copy_file(PJ(stage, toplevel[i]), PJ(o->root, toplevel[i]));
    }
    /* Preserve third-party license texts under licenses/. */
    char *third[][2] = {
        {"llvm-bridge-LICENSE", PJ(o->root, "deps", "llvm-bridge", "LICENSE")},
        {"compiler-rt-LICENSE.TXT", PJ(crtsrc, "LICENSE.TXT")},
        {"libcxx-LICENSE.TXT", PJ(libsrc, "libcxx", "LICENSE.TXT")},
        {"winpthreads-spinlock.c", PJ(o->root, "deps", "winpthreads", "spinlock.c")},
    };
    for (size_t i = 0; i < sizeof third / sizeof third[0]; i++) {
        if (exists(third[i][1])) copy_file(PJ(stage, "licenses", third[i][0]), third[i][1]);
    }
    if (exists(dest)) rmdir(dest);
    rename_path(stage, dest);
    printf("packaged %s\n", dest);
}

#ifdef _WIN32
#define IS_WIN 1
#else
#define IS_WIN 0
#endif
#define IS_DARWIN (!strcmp(HOST_OS, "darwin"))
#define IS_LINUX (!strcmp(HOST_OS, "linux"))

static char *static_lib(const char *libdir, const char *name) {
    if (IS_WIN) {
        char *winlib = PJ(libdir, sfmt("%s.lib", name));
        if (exists(winlib)) return winlib;
    }
    char *f = PJ(libdir, sfmt("lib%s.a", name));
    if (!exists(f)) {
        char *winlib = PJ(libdir, sfmt("%s.lib", name));
        if (exists(winlib)) return winlib;
        fail("missing static library %s (LLVM_PREFIX must contain static Clang, LLD and LLVM libraries)", f);
    }
    return f;
}






/* The pinned Windows LLVM needs libxml2 (its manifest merger) but does not
 * ship it. Build the pinned source like the bridge: the bridge compiler and
 * its static-CRT flags, one process per file up to --jobs, prefix-mapped
 * paths, and a receipt that forces a rebuild when any input or flag changes.
 * XML2_LIBRARY or an SDK copy still takes precedence. */
static const char *const LIBXML2_SOURCES[] = {"buf.c", "chvalid.c", "dict.c", "encoding.c", "entities.c", "error.c", "globals.c", "hash.c", "list.c", "parser.c", "parserInternals.c", "SAX2.c", "threads.c", "tree.c", "uri.c", "valid.c", "xmlIO.c", "xmlmemory.c", "xmlstring.c", "c14n.c", "catalog.c", "debugXML.c", "HTMLparser.c", "HTMLtree.c", "xmlmodule.c", "xmlsave.c", "pattern.c", "xmlreader.c", "xmlregexp.c", "xmlunicode.c", "relaxng.c", "xmlschemas.c", "xmlschemastypes.c", "schematron.c", "xmlwriter.c", "xinclude.c", "xpath.c", "xlink.c", "xpointer.c", NULL};
static const char *const LIBXML2_ON[] = {"C14N", "CATALOG", "DEBUG", "HTML", "ISO8859X", "OUTPUT", "PATTERN", "PUSH", "READER", "REGEXPS", "RELAXNG", "SAX1", "SCHEMAS", "SCHEMATRON", "THREADS", "VALID", "WRITER", "XINCLUDE", "XPATH", "XPTR", NULL};
static const char *const LIBXML2_OFF[] = {"HTTP", "ICONV", "ICU", "LZMA", "MODULES", "THREAD_ALLOC", "ZLIB", NULL};
static const char LIBXML2_CONFIG[] = "#define HAVE_DECL_GETENTROPY 0\n#define HAVE_DECL_GLOB 0\n#define HAVE_DECL_MMAP 0\n#define HAVE_STDINT_H 1\n#define XML_SYSCONFDIR \"/etc\"\n";
/* Bridge flags that only apply to C++. */
static const char *const LIBXML2_CXX_ONLY[] = {"-std=c++17", "-fno-exceptions", "-fno-rtti", "-fvisibility-inlines-hidden", NULL};

/* Seeded: cxx is the raw cross_clang alias and ar the seed llvm-ar alias. */
static char *prepare_libxml2(const Opts *o, const char *ndir, const char *cxx, const char *ar, const Strs *flags) {
    Json *pin = jobj(jobj(json_load(PJ(o->root, "toolchain.lock.json")), "linux_release_deps"), "libxml2");
    const char *version = jstr(pin, "version");
    char *dir = PJ(ndir, sfmt("libxml2-%s", version));
    char *lib = PJ(dir, "libxml2s.lib");
    const char *url = jstr(pin, "url");
    char *archive = PJ(o->root, ".cache", "downloads", last_segment(url));
    fetch(url, jstr(pin, "sha256"), archive);
    char *src = PJ(dir, "src");
    extract(o, archive, NULL, sfmt("%s.stage", dir), src, sfmt("libxml2-%s", version));
    char *gen = PJ(dir, "include");
    mkdirs(PJ(gen, "libxml"));
    char *text = read_file(PJ(src, "include", "libxml", "xmlversion.h.in"), NULL);
    int nums[3], got = sscanf(version, "%d.%d.%d", &nums[0], &nums[1], &nums[2]);
    if (got != 3) fail("libxml2 version %s is not MAJOR.MINOR.PATCH", version);
    int number = (nums[0] * 100 + nums[1]) * 100 + nums[2];
    text = replace_all(text, "@VERSION@", version);
    text = replace_all(text, "@LIBXML_VERSION_NUMBER@", sfmt("%d", number));
    text = replace_all(text, "@LIBXML_VERSION_EXTRA@", "");
    text = replace_all(text, "@MODULE_EXTENSION@", ".dll");
    for (int i = 0; LIBXML2_ON[i]; i++) text = replace_all(text, sfmt("@WITH_%s@", LIBXML2_ON[i]), "1");
    for (int i = 0; LIBXML2_OFF[i]; i++) text = replace_all(text, sfmt("@WITH_%s@", LIBXML2_OFF[i]), "0");
    /* Every @ in the pinned template is a placeholder; a leftover one means the pin changed. */
    char *at = strchr(text, '@');
    if (at) fail("libxml2 xmlversion.h.in has an unhandled placeholder near: %.40s", at);
    write_file(PJ(gen, "libxml", "xmlversion.h"), text);
    write_file(PJ(gen, "config.h"), LIBXML2_CONFIG);

    char *objd = PJ(dir, "obj");
    Buf receipt = {0};
    buf_addf(&receipt, "recipe libxml2-windows-v1\narchive-sha256 %s\n", jstr(pin, "sha256"));
    buf_addf(&receipt, "generated %s include/libxml/xmlversion.h\n", sha256_file(PJ(gen, "libxml", "xmlversion.h")));
    buf_addf(&receipt, "generated %s include/config.h\n", sha256_file(PJ(gen, "config.h")));
    Strs cmds[64];
    int ncmds = 0;
    for (int s = 0; LIBXML2_SOURCES[s]; s++) {
        const char *name = LIBXML2_SOURCES[s];
        Strs *cmd = &cmds[ncmds++];
        memset(cmd, 0, sizeof *cmd);
        strs_add(cmd, cxx);
        for (int i = 0; i < flags->n; i++) {
            int cxx_only = 0;
            for (int k = 0; LIBXML2_CXX_ONLY[k]; k++) {
                if (!strcmp(flags->v[i], LIBXML2_CXX_ONLY[k])) cxx_only = 1;
            }
            if (!cxx_only) strs_add(cmd, flags->v[i]);
        }
        /* After the bridge's root map, so the libxml2 tree gets its own virtual name. */
        strs_addf(cmd, "-ffile-prefix-map=%s=/cross_cc_libxml2", dir);
        strs_add(cmd, "-x");
        strs_add(cmd, "c");
        strs_add(cmd, "-std=c11");
        strs_add(cmd, "-DLIBXML_STATIC");
        strs_add(cmd, "-DWIN32");
        strs_add(cmd, "-D_WINDOWS");
        strs_add(cmd, "-DNDEBUG");
        strs_addf(cmd, "-I%s", gen);
        strs_addf(cmd, "-I%s", PJ(src, "include"));
        strs_add(cmd, "-o");
        strs_add(cmd, PJ(objd, sfmt("%s.o", name)));
        strs_add(cmd, PJ(src, name));
        for (int i = 0; i < cmd->n; i++) buf_addf(&receipt, "compile-arg %s %s\n", name, cmd->v[i]);
    }
    char *inputs = PJ(dir, "inputs.txt");
    const char *want = buf_cstr(&receipt);
    if (exists(inputs) && exists(lib) && strcmp(read_file(inputs, NULL), want) == 0) return lib;
    remove_file(inputs);
    remove_all(objd);
    mkdirs(objd);
    printf("compile libxml2 %s (%d files)\n", version, ncmds);
    Strs env = seed_env(o);
    mkdirs(PJ(o->root, ".cache", "tmp"));
    Pool pool = {0};
    pool.limit = o->jobs;
    for (int i = 0; i < ncmds; i++) pool_add(&pool, cmds[i].v, NULL, env.v, LIBXML2_SOURCES[i]);
    pool_drain(&pool);
    char *part = sfmt("%s.tmp.a", lib);
    remove_file(part);
    Strs acmd = {0};
    strs_add(&acmd, ar);
    strs_add(&acmd, "rcs");
    strs_add(&acmd, part);
    for (int i = 0; LIBXML2_SOURCES[i]; i++) strs_add(&acmd, PJ(objd, sfmt("%s.o", LIBXML2_SOURCES[i])));
    seed_must(o, o->root, &acmd);
    remove_file(lib);
    rename_path(part, lib);
    write_file(inputs, want);
    printf("libxml2 ready: %s\n", lib);
    return lib;
}


/* ---- seeded Linux host recipe (GNU ABI: the pinned LLVM SDK is libstdc++-built) ----
 * The same argument lists feed the checked recipe files the release pipeline
 * reads (placeholders) and the option-table generator prepare builds itself. */
static const char *const HOST_GLIBC = "2.34";
static const char *const HOST_CPU = "x86-64-v2";
static const char *const HOST_CXX_HEADERS[] = {"sysroot/usr/include/c++/12", "sysroot/usr/include/x86_64-linux-gnu/c++/12", "sysroot/usr/include/c++/12/backward", NULL};
static const char *const HOST_C_HEADERS[] = {"lib/include", "lib/libc/include/x86-linux-gnu", "lib/libc/include/generic-glibc", "lib/libc/include/x86-linux-any", "lib/libc/include/any-linux-any", NULL};
static const char *const HOST_SDK_LIBS[] = {"libz.a", "libzstd.a", "libxml2.a", NULL};
static const char *const HOST_CXX_RUNTIME[] = {"libstdc++.a", "libgcc_eh.a", "libgcc.a", NULL};
static const char *const HOST_SYSTEM_LIBS[] = {"-lm", "-lpthread", "-ldl", "-lrt", NULL};

/* Raw-clang bridge flags: explicit libstdc++ headers from the pinned
 * libstdc++-12-dev, then the compiler's builtin and glibc headers. */
static Strs host_bridge_args(const char *sdk, const char *root) {
    Strs a = {0};
    strs_add(&a, "-nostdinc");
    for (int i = 0; HOST_CXX_HEADERS[i]; i++) {
        strs_add(&a, "-isystem");
        strs_addf(&a, "%s/%s", sdk, HOST_CXX_HEADERS[i]);
    }
    for (int i = 0; HOST_C_HEADERS[i]; i++) {
        strs_add(&a, "-isystem");
        strs_addf(&a, "%s/%s", root, HOST_C_HEADERS[i]);
    }
    strs_addf(&a, "-D__GLIBC_MINOR__=%s", strchr(HOST_GLIBC, '.') + 1);
    strs_add(&a, "-fPIC");
    return a;
}

/* Everything after the LLVM archive group: support libraries, the explicit
 * GNU C++ runtime archives (never -lstdc++, which cross_cc maps to libc++),
 * and glibc libraries the cc driver provides for the pinned glibc. */
static void host_runtime_args(Strs *a, const char *sdk) {
    for (int i = 0; HOST_SDK_LIBS[i]; i++) strs_addf(a, "%s/lib/%s", sdk, HOST_SDK_LIBS[i]);
    strs_add(a, "-Wl,--start-group");
    for (int i = 0; HOST_CXX_RUNTIME[i]; i++) strs_addf(a, "%s/lib/%s", sdk, HOST_CXX_RUNTIME[i]);
    strs_add(a, "-Wl,--end-group");
    strs_addv(a, HOST_SYSTEM_LIBS);
}

static void check_host_sdk(const char *sdk) {
    for (int i = 0; HOST_CXX_HEADERS[i]; i++) {
        if (!is_dir(PJ(sdk, HOST_CXX_HEADERS[i]))) fail("host SDK %s lacks %s (libstdc++-12-dev headers)", sdk, HOST_CXX_HEADERS[i]);
    }
    const char *const *lists[] = {HOST_SDK_LIBS, HOST_CXX_RUNTIME};
    for (int k = 0; k < 2; k++) {
        for (int i = 0; lists[k][i]; i++) {
            if (!is_file(PJ(sdk, "lib", lists[k][i]))) fail("host SDK %s lacks lib/%s", sdk, lists[k][i]);
        }
    }
}

/* Seed aliases (argv0 selects the mode) in .cache/seed-tools. */
static char *seed_alias(const Opts *o, const char *name) {
    char *dir = PJ(o->root, ".cache", "seed-tools");
    mkdirs(dir);
    char *dst = PJ(dir, sfmt("%s%s", name, EXE_SUFFIX));
    remove_file(dst);
#ifndef _WIN32
    if (symlink(real_or_abs(o->compiler), dst) == 0) return dst;
#endif
    link_or_copy(dst, o->compiler);
    return dst;
}

/* Build the option-parser llvm-tblgen with the seed: raw cross_clang for the
 * two upstream sources, the seed's cc driver for the GNU-ABI link. */
static void seeded_tblgen(const Opts *o, const char *dir, const char *tg, const char *llvm, const char *sdk, const char *helper, Buf *receipt) {
    char *clang = seed_alias(o, "cross_clang");
    Strs bridge = host_bridge_args(sdk, o->seed_root);
    static const char *const flags[] = {"--no-default-config", "-std=c++17", "-O2", "-w", "-c", "-fno-exceptions", "-fno-rtti", "-D__STDC_CONSTANT_MACROS", "-D__STDC_FORMAT_MACROS", "-D__STDC_LIMIT_MACROS", "-DLLVM_BUILD_STATIC", "--target=x86_64-unknown-linux-gnu", NULL};
    static const char *const srcs[] = {"llvm-tblgen.cpp", "OptionParserEmitter.cpp", NULL};
    char *objd = PJ(dir, "tblgen-obj");
    mkdirs(objd);
    Strs link = {0};
    strs_add(&link, o->compiler);
    strs_add(&link, "cc");
    strs_addf(&link, "-target");
    strs_addf(&link, "x86_64-linux-gnu.%s", HOST_GLIBC);
    for (int i = 0; srcs[i]; i++) {
        Strs cmd = {0};
        strs_add(&cmd, clang);
        strs_addv(&cmd, flags);
        strs_addf(&cmd, "-march=%s", HOST_CPU);
        strs_cat(&cmd, &bridge);
        strs_addf(&cmd, "-I%s", PJ(llvm, "include"));
        strs_addf(&cmd, "-I%s", tg);
        char *obj = PJ(objd, sfmt("%s.o", srcs[i]));
        strs_add(&cmd, "-o");
        strs_add(&cmd, obj);
        strs_add(&cmd, PJ(tg, srcs[i]));
        for (int k = 0; k < cmd.n; k++) buf_addf(receipt, "generator-compile-arg %s\n", cmd.v[k]);
        seed_must(o, o->root, &cmd);
        strs_add(&link, obj);
    }
    strs_add(&link, "-Wl,--start-group");
    static const char *const tglibs[] = {"LLVMTableGenBasic", "LLVMTableGenCommon", "LLVMTableGen", "LLVMSupport", "LLVMDemangle", NULL};
    for (int i = 0; tglibs[i]; i++) strs_add(&link, static_lib(PJ(llvm, "lib"), tglibs[i]));
    strs_add(&link, "-Wl,--end-group");
    host_runtime_args(&link, sdk);
    strs_add(&link, "-o");
    strs_add(&link, helper);
    for (int k = 0; k < link.n; k++) buf_addf(receipt, "generator-build-arg %s\n", link.v[k]);
    seed_must(o, o->root, &link);
}

/* Unchanged pinned LLVM dependency sources. The only generated C++ is LLVM's
 * own driver template; all dependency orchestration and dispatch remain C. */

#include "selfhost_platform.h"

/* One host self-hosting recipe: generated by prepare or read from --host-config. */
typedef struct {
    const SelfhostHost *h;
    char *llvm, *sdk, *native, *bridge_args, *link_args, *cpu, *glibc;
    int auto_linux; /* the generated linux-amd64 recipe: tblgen built as before */
} SelfhostCfg;

/* Option-table generator from a recipe's own flags, for every other host
 * recipe: bridge-args compile, link-args minus release-only inputs. */
static void recipe_tblgen(const Opts *o, const SelfhostCfg *c, const char *dir, const char *tg, const char *helper, Buf *receipt) {
    char *clang = seed_alias(o, "cross_clang");
    static const char *const flags[] = {"--no-default-config", "-std=c++17", "-O2", "-w", "-c", "-fno-exceptions", "-fno-rtti", "-D__STDC_CONSTANT_MACROS", "-D__STDC_FORMAT_MACROS", "-D__STDC_LIMIT_MACROS", "-DLLVM_BUILD_STATIC", NULL};
    static const char *const srcs[] = {"llvm-tblgen.cpp", "OptionParserEmitter.cpp", NULL};
    Strs bridge = sp_recipe_lines(c->bridge_args);
    Strs linkr = sp_recipe_lines(c->link_args);
    char *objd = PJ(dir, "tblgen-obj");
    mkdirs(objd);
    Strs link = {0};
    strs_add(&link, o->compiler);
    strs_add(&link, "cc");
    strs_add(&link, "-target");
    if (!strcmp(c->h->kind, "linux")) strs_addf(&link, "%s.%s", c->h->cc_target, c->glibc);
    else strs_add(&link, c->h->cc_target);
    for (int i = 0; srcs[i]; i++) {
        Strs cmd = {0};
        strs_add(&cmd, clang);
        strs_addv(&cmd, flags);
        strs_addf(&cmd, "--target=%s", c->h->clang_target);
        strs_addf(&cmd, "%s=%s", c->h->cpu_flag, c->cpu);
        for (int k = 0; k < bridge.n; k++) strs_add(&cmd, sp_expand(bridge.v[k], o->seed_root, c->sdk, c->llvm, c->native));
        strs_addf(&cmd, "-I%s", PJ(c->llvm, "include"));
        strs_addf(&cmd, "-I%s", tg);
        char *obj = PJ(objd, sfmt("%s.o", srcs[i]));
        strs_add(&cmd, "-o");
        strs_add(&cmd, obj);
        strs_add(&cmd, PJ(tg, srcs[i]));
        for (int k = 0; k < cmd.n; k++) buf_addf(receipt, "generator-compile-arg %s\n", cmd.v[k]);
        seed_must(o, o->root, &cmd);
        strs_add(&link, obj);
    }
    static const char *const tglibs[] = {"LLVMTableGenBasic", "LLVMTableGenCommon", "LLVMTableGen", "LLVMSupport", "LLVMDemangle", NULL};
    int group = !strcmp(c->h->kind, "linux"); /* ELF only; ld64 and COFF resolve archives without groups */
    if (group) strs_add(&link, "-Wl,--start-group");
    for (int i = 0; tglibs[i]; i++) strs_add(&link, static_lib(PJ(c->llvm, "lib"), tglibs[i]));
    if (group) strs_add(&link, "-Wl,--end-group");
    for (int k = 0; k < linkr.n; k++) {
        if (!sp_release_only(linkr.v[k])) strs_add(&link, sp_expand(linkr.v[k], o->seed_root, c->sdk, c->llvm, c->native));
    }
    strs_add(&link, "-o");
    strs_add(&link, helper);
    for (int k = 0; k < link.n; k++) buf_addf(receipt, "generator-build-arg %s\n", link.v[k]);
    if (!strcmp(c->h->kind, "msvc")) {
        Strs env = seed_env(o);
        sp_msvc_env(&env, c->sdk, PJ(dir, "msvc-libc.txt"));
        int code = run_cmd(link.v, o->root, env.v);
        if (code) fail("MSVC option-table generator link failed (exit %d)", code);
    } else seed_must(o, o->root, &link);
}

static char *prepare_dsymutil(const Opts *o, const char *ndir, const char *llvm, const SelfhostCfg *seed) {
    Json *lock = json_load(PJ(o->root, "compiler-rt.lock.json"));
    const char *url = jstr(lock, "url");
    char *archive = PJ(o->root, ".cache", "downloads", last_segment(url));
    fetch(url, jstr(lock, "sha256"), archive);
    char *dir = PJ(ndir, "dsymutil");
    char *stage = sfmt("%s.stage", dir);
    remove_all(stage);
    mkdirs(stage);
    const char *prefix = jstr(lock, "strip_prefix");
    Strs cmd = {0};
    strs_add(&cmd, "tar");
    strs_add(&cmd, "-xf");
    strs_add(&cmd, archive);
    strs_add(&cmd, "-C");
    strs_add(&cmd, stage);
    strs_add(&cmd, "--strip-components=1");
    static const char *const members[] = {"llvm/tools/dsymutil", "llvm/utils/TableGen/llvm-tblgen.cpp", "llvm/utils/TableGen/OptionParserEmitter.cpp", "llvm/utils/TableGen/Basic", "llvm/utils/TableGen/Common", "llvm/cmake/modules/llvm-driver-template.cpp.in", NULL};
    for (int i = 0; members[i]; i++) strs_addf(&cmd, "%s/%s", prefix, members[i]);
    must(o, o->root, &cmd);
    remove_all(dir);
    rename_path(stage, dir);
    char *src = PJ(dir, "llvm", "tools", "dsymutil");
    char *tg = PJ(dir, "llvm", "utils", "TableGen");
    char *gen = PJ(dir, "gen");
    mkdirs(gen);
    /* The public SDK omits LLVM's private config.h. This dsymutil TU uses
     * no private config macros; forward the SDK's exported configuration only. */
    mkdirs(PJ(gen, "llvm", "Config"));
    write_file(PJ(gen, "llvm", "Config", "config.h"), "#include \"llvm/Config/llvm-config.h\"\n");
    Buf receipt = {0};
    buf_addf(&receipt, "recipe llvm-dsymutil-v1\narchive-sha256 %s\nextract strip-components=1 llvm/tools/dsymutil llvm/utils/TableGen/{llvm-tblgen.cpp,OptionParserEmitter.cpp,Basic,Common} llvm/cmake/modules/llvm-driver-template.cpp.in\n", jstr(lock, "sha256"));
    char *helper = PJ(dir, sfmt("llvm-opt-tblgen%s", EXE_SUFFIX));
    if (seed && seed->auto_linux) {
        seeded_tblgen(o, dir, tg, llvm, seed->sdk, helper, &receipt);
    } else if (seed) {
        recipe_tblgen(o, seed, dir, tg, helper, &receipt);
    } else {
        fail("prepare_dsymutil: no seeded host recipe");
    }
    memset(&cmd, 0, sizeof cmd);
    strs_add(&cmd, helper);
    strs_add(&cmd, "-gen-opt-parser-defs");
    strs_add(&cmd, "-I");
    strs_add(&cmd, PJ(llvm, "include"));
    strs_add(&cmd, PJ(src, "Options.td"));
    strs_add(&cmd, "-o");
    strs_add(&cmd, PJ(gen, "Options.inc"));
    for (int i = 0; i < cmd.n; i++) buf_addf(&receipt, "generator-arg %s\n", cmd.v[i]);
    must(o, o->root, &cmd);
    char *template_path = PJ(dir, "llvm", "cmake", "modules", "llvm-driver-template.cpp.in");
    char *driver = replace_all(read_file(template_path, NULL), "@TOOL_NAME@", "dsymutil");
    write_file(PJ(gen, "driver.cpp"), driver);
    /* The C driver reaches this through the bridge's extern "C" cross_cc_dsymutil_main wrapper. */
    buf_addf(&receipt, "driver-template TOOL_NAME=dsymutil compiler-define main=cross_cc_dsymutil_driver_main\n");
    /* Dependency build configuration only: preserve __APPLE__ for libc++ and
     * Darwin headers, but select upstream's portable bundle metadata fallback.
     * Never modify the authenticated original or add a replacement implementation. */
    char *cfpath = PJ(src, "CFBundle.cpp");
    char *cf = read_file(cfpath, NULL);
    const char *guard = "#ifdef __APPLE__";
    int count = 0;
    for (const char *p = cf; (p = strstr(p, guard)) != NULL; p += strlen(guard)) count++;
    if (count != 3) fail("pinned CFBundle.cpp must have exactly three Apple guards");
    char *configured = replace_all(cf, guard, "#if defined(__APPLE__) && !defined(CROSS_CC_DSYMUTIL_PORTABLE_BUNDLE)");
    char *cfgen = PJ(gen, "CFBundle.cpp");
    write_file(cfgen, configured);
    buf_addf(&receipt, "bundle-configuration replace exactly 3 #ifdef __APPLE__ with #if defined(__APPLE__) && !defined(CROSS_CC_DSYMUTIL_PORTABLE_BUNDLE); define only for CFBundle.cpp\ninput %s %s\nconfigured-source %s %s\n", sha256_file(cfpath), cfpath, sha256_file(cfgen), cfgen);
    char *inputs_list[] = {helper, template_path, PJ(gen, "driver.cpp"), PJ(gen, "Options.inc"), PJ(gen, "llvm", "Config", "config.h"), PJ(src, "Options.td"), PJ(llvm, "include", "llvm", "Option", "OptParser.td")};
    for (size_t i = 0; i < sizeof inputs_list / sizeof inputs_list[0]; i++) buf_addf(&receipt, "input %s %s\n", sha256_file(inputs_list[i]), inputs_list[i]);
    write_file(PJ(dir, "inputs.txt"), buf_cstr(&receipt));
    return dir;
}

/* Pinned host LLVM SDK (headers, static libraries, llvm-config): extracted
 * from the verified archive into <ndir>/llvm unless LLVM_PREFIX names one.
 * Never compiles anything. Returns the prefix; *header_out gets llvm-config.h. */
static char *prepare_llvm_sdk(const Opts *o, const char *ndir, char **header_out) {
    Json *nat = jobj(json_load(PJ(o->root, "toolchain.lock.json")), "native");
    char *llvm = xstrdup(env_get("LLVM_PREFIX"));
    if (*llvm && !is_abs(llvm)) llvm = PJ(o->root, llvm);
    int supplied = *llvm != 0;
    if (!supplied) llvm = PJ(ndir, "llvm");
    char *objd = PJ(ndir, "obj");
    mkdirs(PJ(o->root, ".cache", "downloads"));
    mkdirs(PJ(o->root, ".cache", "tmp"));
    mkdirs(objd);
    if (!exists(PJ(llvm, "include", "llvm", "Config", "llvm-config.h"))) {
        if (supplied) fail("LLVM_PREFIX=%s lacks include/llvm/Config/llvm-config.h", llvm);
        Json *host = jget(jobj(nat, "hosts"), host_key());
        if (!host || host->type != J_OBJ) fail("no pinned LLVM archive for %s; set LLVM_PREFIX to a matching LLVM 22.1.8 static installation", host_key());
        char *arc = PJ(o->root, ".cache", "downloads", jstr(host, "name"));
        fetch(jstr(host, "url"), jstr(host, "sha256"), arc);
        char *stage = PJ(ndir, ".stage");
        remove_all(stage);
        mkdirs(stage);
        const char *top = jstr(host, "top");
        /* Directory selection works with BSD tar (macOS/Windows) and GNU tar.
         * Only existing tool members are requested; Windows releases may omit llvm-config. */
        Strs members = split_lines(capture_l(o, "tar", "-tf", arc, NULL));
        Strs cmd = {0};
        strs_add(&cmd, "tar");
        strs_add(&cmd, "-xf");
        strs_add(&cmd, arc);
        strs_add(&cmd, "-C");
        strs_add(&cmd, stage);
        strs_add(&cmd, "--strip-components=1");
        strs_addf(&cmd, "%s/include", top);
        strs_addf(&cmd, "%s/lib", top);
        static const char *const tools[] = {"llvm-config", "llvm-ar", "clang", "clang++", "clang-22", NULL};
        for (int t = 0; tools[t]; t++) {
            char *member = sfmt("%s/bin/%s%s", top, tools[t], EXE_SUFFIX);
            for (int i = 0; i < members.n; i++) {
                char *line = xstrdup(members.v[i]);
                size_t n = strlen(line);
                while (n > 0 && line[n - 1] == '/') line[--n] = 0;
                if (strcmp(line, member) == 0) {
                    strs_add(&cmd, member);
                    break;
                }
            }
        }
        must(o, o->root, &cmd);
        remove_all(llvm);
        rename_path(stage, llvm);
    }
    char *header = read_file(PJ(llvm, "include", "llvm", "Config", "llvm-config.h"), NULL);
    static const char *const defines[] = {"#define LLVM_VERSION_MAJOR 22", "#define LLVM_VERSION_MINOR 1", "#define LLVM_VERSION_PATCH 8", NULL};
    for (int i = 0; defines[i]; i++) {
        if (!contains(header, defines[i])) fail("LLVM_PREFIX must contain LLVM 22.1.8 (%s missing)", defines[i]);
    }
    char *config = PJ(llvm, "bin", sfmt("llvm-config%s", EXE_SUFFIX));
    if (exists(config)) {
        char *got = capture_l(o, config, "--version", NULL);
        if (strcmp(got, "22.1.8") != 0) fail("LLVM_PREFIX version %s; bridge requires 22.1.8", got);
    }
    /* Diagnose incomplete SDKs before spending time compiling the bridge. */
    char *libdir = PJ(llvm, "lib");
    static const char *const lib_keys[] = {"clang_libs", "lld_libs", NULL};
    for (int k = 0; lib_keys[k]; k++) {
        Json *arr = jarr_opt(nat, lib_keys[k]);
        for (int i = 0; arr && i < arr->n; i++) static_lib(libdir, arr->items[i]->str);
    }
    (void)config;
    *header_out = header;
    return llvm;
}

/* Official clang/lld Version.cpp sources plus their generated headers under
 * <ndir>/version, from the pinned llvm-project archive. Sources stay unmodified. */
static void prepare_version_sources(const Opts *o, const char *ndir, const char *header) {
    Json *crt = json_load(PJ(o->root, "compiler-rt.lock.json"));
    const char *crt_url = jstr(crt, "url");
    char *crt_archive = PJ(o->root, ".cache", "downloads", last_segment(crt_url));
    char *vdir = PJ(ndir, "version");
    char *vsrc = PJ(vdir, "clang", "lib", "Basic", "Version.cpp");
    if (!exists(vsrc)) {
        fetch(crt_url, jstr(crt, "sha256"), crt_archive);
        char *vstage = PJ(ndir, ".version-stage");
        remove_all(vstage);
        mkdirs(vstage);
        const char *pfx = jstr(crt, "strip_prefix");
        must_l(o, o->root, "tar", "-xf", crt_archive, "-C", vstage, "--strip-components=1", sfmt("%s/clang/lib/Basic/Version.cpp", pfx), NULL);
        remove_all(vdir);
        rename_path(vstage, vdir);
    }
    char *vgen = PJ(vdir, "gen");
    mkdirs(vgen);
    write_file(PJ(vgen, "VCSVersion.inc"), "");
    /* Official lld Common/Version.cpp, same treatment. Its Version.inc is a CMake
     * product of Version.inc.in (LLD_VERSION == LLVM_VERSION) and is not installed;
     * the installed llvm/Support/VCSRevision.h carries the repository URL and commit,
     * so an empty one (what a build without VCS info generates) shadows it for this
     * object only. Sources stay unmodified. */
    char *lsrc = PJ(vdir, "lld", "Common", "Version.cpp");
    char *linc = PJ(vdir, "lld", "include", "lld", "Common", "Version.inc.in");
    if (!exists(lsrc) || !exists(linc)) {
        fetch(crt_url, jstr(crt, "sha256"), crt_archive);
        const char *pfx = jstr(crt, "strip_prefix");
        must_l(o, o->root, "tar", "-xf", crt_archive, "-C", vdir, "--strip-components=1", sfmt("%s/lld/Common/Version.cpp", pfx), sfmt("%s/lld/include/lld/Common/Version.inc.in", pfx), NULL);
    }
    char *lgen = PJ(vdir, "lld-gen");
    mkdirs(PJ(lgen, "lld", "Common"));
    mkdirs(PJ(lgen, "llvm", "Support"));
    write_file(PJ(lgen, "VCSVersion.inc"), "");
    write_file(PJ(lgen, "llvm", "Support", "VCSRevision.h"), "");
    {
        const char *prefix = "#define LLVM_VERSION_STRING \"";
        char *lver = NULL;
        Strs lines = split_lines(header);
        for (int i = 0; i < lines.n; i++) {
            if (starts_with(lines.v[i], prefix)) {
                lver = xstrdup(lines.v[i] + strlen(prefix));
                size_t n = strlen(lver);
                if (n > 0 && lver[n - 1] == '"') lver[n - 1] = 0;
            }
        }
        if (!lver || !*lver) fail("LLVM_VERSION_STRING missing from llvm-config.h");
        write_file(PJ(lgen, "lld", "Common", "Version.inc"), replace_all(read_file(linc, NULL), "@LLD_VERSION@", lver));
    }
}


/* ---- seeded preparation and the release pipeline ---- */
static char *root_rel(const Opts *o, const char *p) {
    size_t n = strlen(o->root);
    if (!strncmp(p, o->root, n) && (p[n] == '/' || p[n] == '\\')) return xstrdup(p + n + 1);
    return xstrdup(p);
}

static void json_field(Buf *b, const char *key, const char *val, int last) {
    buf_addf(b, "  \"%s\": \"", key);
    for (const char *p = val; *p; p++) {
        if (*p == '"' || *p == '\\') buf_char(b, '\\');
        buf_char(b, *p);
    }
    buf_addf(b, "\"%s\n", last ? "" : ",");
}

static void write_lines(const char *path, const Strs *a) {
    Buf b = {0};
    for (int i = 0; i < a->n; i++) {
        if (strchr(a->v[i], '\n')) fail("recipe argument contains a newline: %s", a->v[i]);
        buf_addf(&b, "%s\n", a->v[i]);
    }
    write_file(path, buf_cstr(&b));
}

/* Pinned GNU-ABI support SDK: zlib/zstd/libxml2 built by the seed plus the
 * libstdc++-12/libgcc archives and headers (tools/linux-release-deps.c). */
/* The support prefix: --deps-prefix exactly, else keyed by everything its
 * helper receipt binds, so another seed or helper never meets a stale prefix. */
static char *linux_deps_prefix(const Opts *o) {
    if (o->deps_prefix) return abs_path(o->deps_prefix);
    Buf k = {0};
    buf_addf(&k, "seed %s\n", sha256_file(o->compiler));
    static const char *const ins[] = {"toolchain.lock.json", "tools/linux-release-deps.c", "tools/support_deps.h", "tools/common.h", NULL};
    for (int i = 0; ins[i]; i++) {
        char *p = PJ(o->root, ins[i]);
        buf_addf(&k, "%s %s\n", ins[i], is_file(p) ? sha256_file(p) : "-");
    }
    const char *text = buf_cstr(&k);
    char *hex = sha256_bytes(text, strlen(text));
    hex[16] = 0;
    return PJ(o->root, ".cache", "support", sfmt("linux-amd64-%s", hex));
}

static char *prepare_linux_deps(const Opts *o) {
    char *prefix = linux_deps_prefix(o);
    char *helper = PJ(o->self_dir, sfmt("linux-release-deps%s", EXE_SUFFIX));
    if (!is_file(helper)) fail("%s missing: the launcher compiles it beside this tool", helper);
    /* Reuse already verified archives (the helper re-checks every SHA-256). */
    char *dl = PJ(prefix, "downloads");
    char *old = PJ(o->root, ".cache", "linux-release-deps", "downloads");
    Strs names = {0};
    if (!o->deps_prefix && !exists(dl) && list_dir(old, &names)) {
        mkdirs(dl);
        for (int i = 0; i < names.n; i++) {
            char *src = PJ(old, names.v[i]);
            if (is_file(src) && !ends_with(names.v[i], ".part")) link_or_copy(PJ(dl, names.v[i]), src);
        }
    }
    Strs cmd = {0};
    strs_add(&cmd, helper);
    strs_addf(&cmd, "--compiler=%s", o->compiler);
    strs_addf(&cmd, "--prefix=%s", prefix);
    strs_addf(&cmd, "--jobs=%d", o->jobs);
    seed_must(o, o->root, &cmd);
    check_host_sdk(prefix);
    printf("host support SDK ready: %s\n", prefix);
    return prefix;
}

/* {llvm}/lib/lib*.a for clang, lld and every LLVM component. */
static void host_llvm_libs(const Opts *o, Strs *a, const char *llvm) {
    Json *nat = jobj(json_load(PJ(o->root, "toolchain.lock.json")), "native");
    char *libdir = PJ(llvm, "lib");
    static const char *const keys[] = {"clang_libs", "lld_libs", NULL};
    for (int k = 0; keys[k]; k++) {
        Json *arr = jarr_opt(nat, keys[k]);
        for (int i = 0; arr && i < arr->n; i++) {
            char *archive = static_lib(libdir, arr->items[i]->str);
            strs_addf(a, "{llvm}/lib/%s", path_base(archive));
        }
    }
    char *config = PJ(llvm, "bin", sfmt("llvm-config%s", EXE_SUFFIX));
    if (!is_file(config)) fail("%s missing; the host recipe needs the SDK's llvm-config --libnames", config);
    Strs names = split_fields(capture_l(o, config, "--link-static", "--libnames", NULL));
    for (int i = 0; i < names.n; i++) {
        if (!is_file(PJ(libdir, names.v[i]))) fail("LLVM SDK lacks %s", PJ(libdir, names.v[i]));
        strs_addf(a, "{llvm}/lib/%s", names.v[i]);
    }
}

static char *selfhost_config(const Opts *o) { return o->host_config ? abs_path(o->host_config) : PJ(o->root, ".cache", "native", "selfhost.json"); }

static void write_selfhost(const Opts *o, SelfhostCfg *c, const char *zstd, const char *xml2) {
    const SelfhostHost *h = c->h;
    char *rdir = PJ(c->native, "selfhost");
    mkdirs(rdir);
    Strs bridge = sp_bridge_args(h);
    char *bridge_file = PJ(rdir, "bridge.args");
    write_lines(bridge_file, &bridge);
    Strs link = {0};
    sp_link_head(h, &link);
    host_llvm_libs(o, &link, c->llvm);
    char *syslibs = "";
    if (!strcmp(h->kind, "linux")) strs_add(&link, "-Wl,--end-group");
    else syslibs = capture_l(o, PJ(c->llvm, "bin", sfmt("llvm-config%s", EXE_SUFFIX)), "--link-static", "--system-libs", NULL);
    sp_link_tail(h, &link, syslibs, zstd, xml2);
    if (!strcmp(h->kind, "apple") && is_file(PJ(c->llvm, "lib", "libLTO.dylib"))) strs_add(&link, "-Wl,-lto_library,{llvm}/lib/libLTO.dylib");
    char *link_file = PJ(rdir, "link.args");
    write_lines(link_file, &link);
    c->bridge_args = bridge_file;
    c->link_args = link_file;
    Buf j = {0};
    buf_str(&j, "{\n");
    json_field(&j, "target", h->host, 0);
    json_field(&j, "llvm-prefix", root_rel(o, c->llvm), 0);
    json_field(&j, "sdk", root_rel(o, c->sdk), 0);
    json_field(&j, "bridge-args", root_rel(o, bridge_file), 0);
    if (!strcmp(h->kind, "msvc")) json_field(&j, "path-args", root_rel(o, bridge_file), 0);
    json_field(&j, "link-args", root_rel(o, link_file), 0);
    json_field(&j, "link-mode", "cc", 0);
    json_field(&j, "cpu", c->cpu, 0);
    if (c->glibc) json_field(&j, "linux-glibc", c->glibc, 0);
    char *notices = !strcmp(h->kind, "linux") ? PJ(c->sdk, "licenses") :
                    zstd ? PJ(path_dir(path_dir(zstd)), "licenses") : PJ(rdir, "licenses");
    if (is_dir(notices)) json_field(&j, "extra-licenses", root_rel(o, notices), 0);
    json_field(&j, "native-dir", root_rel(o, c->native), 1);
    buf_str(&j, "}\n");
    write_file(selfhost_config(o), buf_cstr(&j));
}

/* The pinned Windows LLVM needs libxml2 but ships none: build the pinned
 * source static (/MT) with the raw seed cross_clang and the host bridge flags. */
static char *msvc_libxml2(const Opts *o, const SelfhostCfg *c) {
    Strs flags = {0};
    static const char *const base[] = {"--no-default-config", "-O2", "-w", "-c", "-ffunction-sections", "-fdata-sections", NULL};
    strs_addv(&flags, base);
    strs_addf(&flags, "--target=%s", c->h->clang_target);
    strs_addf(&flags, "%s=%s", c->h->cpu_flag, c->cpu);
    Strs b = sp_bridge_args(c->h);
    for (int i = 0; i < b.n; i++) strs_add(&flags, sp_expand(b.v[i], o->seed_root, c->sdk, c->llvm, c->native));
    char *lib = prepare_libxml2(o, c->native, seed_alias(o, "cross_clang"), seed_alias(o, "llvm-ar"), &flags);
    char *notices = PJ(c->native, "selfhost", "licenses");
    mkdirs(notices);
    copy_file(PJ(notices, "libxml2-Copyright"), PJ(path_dir(lib), "src", "Copyright"));
    return lib;
}

/* System Apple SDK: --sdk, SDKROOT, else xcrun's (a locator, not a compiler). */
static char *apple_sdk(const Opts *o) {
    if (o->host_sdk) return abs_path(o->host_sdk);
    const char *e = env_get("SDKROOT");
    if (*e) return abs_path(e);
    char *argv[] = {"xcrun", "--sdk", "macosx", "--show-sdk-path", NULL};
    char *out = NULL;
    if (run_capture(argv, NULL, NULL, &out) == 0 && out && *trim(out)) return trim(out);
    fail("no macOS SDK: pass --sdk=DIR or SDKROOT (xcrun --show-sdk-path failed)");
    return NULL;
}

/* Both Apple hosts build their pinned zstd with the selected compiler. The
 * SDK itself remains an explicit, separately supplied Apple input. */
static char *prepare_apple_deps(const Opts *o, const SelfhostCfg *c) {
    if (o->deps_prefix) {
        char *lib = PJ(abs_path(o->deps_prefix), "lib", "libzstd.a");
        if (!is_file(lib)) fail("--deps-prefix lacks %s", lib);
        return lib;
    }
    Buf key = {0};
    buf_addf(&key, "seed %s\nhost %s\nsdk %s\nsettings %s\n", sha256_file(o->compiler), c->h->host, c->sdk, sha256_file(PJ(c->sdk, "SDKSettings.json")));
    static const char *const inputs[] = {"toolchain.lock.json", "tools/macos-x64-llvm.c", "tools/support_deps.h", "tools/common.h", NULL};
    for (int i = 0; inputs[i]; i++) buf_addf(&key, "%s %s\n", inputs[i], sha256_file(PJ(o->root, inputs[i])));
    const char *text = buf_cstr(&key);
    char *hash = sha256_bytes(text, strlen(text));
    hash[16] = 0;
    char *work = PJ(o->root, ".cache", "support", sfmt("%s-%s", c->h->host, hash));
    Strs cmd = {0};
    strs_add(&cmd, PJ(o->self_dir, sfmt("macos-x64-llvm%s", EXE_SUFFIX)));
    strs_addf(&cmd, "--compiler=%s", o->compiler);
    strs_addf(&cmd, "--arch=%s", ends_with(c->h->host, "-arm64") ? "arm64" : "amd64");
    strs_addf(&cmd, "--sdk=%s", c->sdk);
    strs_addf(&cmd, "--work=%s", work);
    strs_addf(&cmd, "--jobs=%d", o->jobs);
    seed_must(o, o->root, &cmd);
    char *lib = PJ(work, "zstd", "lib", "libzstd.a");
    if (!is_file(lib)) fail("Apple support helper did not produce %s", lib);
    return lib;
}

static char *msvc_sdk(const Opts *o) {
    if (o->host_sdk) return abs_path(o->host_sdk);
    const char *e = env_get("CROSS_CC_HOST_SDK");
    if (*e) return abs_path(e);
    fail("windows-amd64 host: pass --sdk=DIR (or CROSS_CC_HOST_SDK), an MSVC + Windows SDK in xwin splat layout (crt/, sdk/) whose Microsoft license you accepted yourself; build never accepts it for you");
    return NULL;
}

static char *config_path(const Opts *o, const char *v) { return is_abs(v) ? xstrdup(v) : PJ(o->root, v); }

/* --host-config=FILE: a complete recipe; prepare only adds native-dir inputs. */
static void load_host_config(const Opts *o, SelfhostCfg *c) {
    char *file = selfhost_config(o);
    if (!is_file(file)) fail("--host-config %s does not exist", file);
    Json *d = json_load(file);
    if (d->type != J_OBJ) fail("host config %s must be a JSON object", file);
    const char *target = jstr_opt(d, "target");
    if (strcmp(target, host_key())) fail("host config target '%s' does not match this host %s", target, host_key());
    c->h = selfhost_host(target);
    if (!c->h) fail("host config target %s has no seed compile/link target mapping", target);
    static const char *const need[] = {"llvm-prefix", "bridge-args", "link-args", NULL};
    for (int i = 0; need[i]; i++) {
        if (!*jstr_opt(d, need[i])) fail("host config %s lacks \"%s\"", file, need[i]);
    }
    const char *mode = jstr_opt(d, "link-mode");
    if (*mode && strcmp(mode, "cc")) fail("host config link-mode %s: prepare builds its generator from a cc-mode link recipe; use link-mode cc", mode);
    c->llvm = config_path(o, jstr_opt(d, "llvm-prefix"));
    c->bridge_args = config_path(o, jstr_opt(d, "bridge-args"));
    c->link_args = config_path(o, jstr_opt(d, "link-args"));
    if (!is_file(c->bridge_args) || !is_file(c->link_args)) fail("host config recipe files missing: %s, %s", c->bridge_args, c->link_args);
    c->sdk = *jstr_opt(d, "sdk") ? config_path(o, jstr_opt(d, "sdk")) : NULL;
    c->native = config_path(o, *jstr_opt(d, "native-dir") ? jstr_opt(d, "native-dir") : ".cache/native");
    c->cpu = *jstr_opt(d, "cpu") ? xstrdup(jstr_opt(d, "cpu")) : (char *)c->h->cpu;
    c->glibc = !strcmp(c->h->kind, "linux") ? (*jstr_opt(d, "linux-glibc") ? xstrdup(jstr_opt(d, "linux-glibc")) : (char *)HOST_GLIBC) : NULL;
    char *h = read_file(PJ(c->llvm, "include", "llvm", "Config", "llvm-config.h"), NULL);
    if (!h || !contains(h, "#define LLVM_VERSION_MAJOR 22") || !contains(h, "#define LLVM_VERSION_MINOR 1") || !contains(h, "#define LLVM_VERSION_PATCH 8")) fail("host config llvm-prefix %s is not LLVM 22.1.8", c->llvm);
}

/* Everything the release pipeline's seeded host stages need, compiled by
 * nothing but the seed: sources, LLVM SDK, host SDK, native-dir inputs
 * (version sources, dsymutil generated sources) and the host recipe. */
static void prepare(Opts *o) {
    require_seed(o);
    SelfhostCfg c = {0};
    char *zstd = NULL;
    if (o->host_config) {
        load_host_config(o, &c);
    } else {
        c.h = selfhost_host(host_key());
        if (!c.h) fail("prepare: no seeded host recipe for %s; pass --host-config=FILE with a complete recipe (target, llvm-prefix, bridge-args, link-args, link-mode cc)", host_key());
        c.cpu = (char *)c.h->cpu;
        c.glibc = !strcmp(c.h->kind, "linux") ? (char *)HOST_GLIBC : NULL;
        c.native = PJ(o->root, ".cache", "native");
    }
    setup(o);
    mkdirs(c.native);
    char *ndir = c.native;
    char *header = o->host_config ? read_file(PJ(c.llvm, "include", "llvm", "Config", "llvm-config.h"), NULL) : NULL;
    if (!o->host_config) {
        c.llvm = prepare_llvm_sdk(o, ndir, &header);
        if (!strcmp(c.h->kind, "linux")) {
            c.sdk = prepare_linux_deps(o);
            c.auto_linux = 1;
        } else {
            c.sdk = !strcmp(c.h->kind, "apple") ? apple_sdk(o) : msvc_sdk(o);
            char *why = NULL;
            if (!sp_sdk_ok(c.h, c.sdk, &why)) fail("%s", why);
            if (!strcmp(c.h->kind, "apple")) zstd = prepare_apple_deps(o, &c);
            char *xml2 = NULL;
            if (!strcmp(c.h->kind, "msvc")) xml2 = msvc_libxml2(o, &c);
            write_selfhost(o, &c, zstd, xml2);
        }
    }
    /* Identity of every input of the generated native-dir sources. */
    Buf id = {0};
    buf_addf(&id, "seed %s\nseed-root %s\nllvm %s\nsdk %s\n", sha256_file(o->compiler), o->seed_root, c.llvm, c.sdk ? c.sdk : "-");
    static const char *const ins[] = {"toolchain.lock.json", "compiler-rt.lock.json", "upstream.lock.json", "tools/build.c", NULL};
    for (int i = 0; ins[i]; i++) buf_addf(&id, "%s %s\n", sha256_file(PJ(o->root, ins[i])), ins[i]);
    buf_addf(&id, "llvm-config.h %s\n", sha256_file(PJ(c.llvm, "include", "llvm", "Config", "llvm-config.h")));
    if (!c.auto_linux) {
        buf_addf(&id, "platform %s\n", sha256_file(PJ(o->root, "tools", "selfhost_platform.h")));
        buf_addf(&id, "bridge-args %s\nlink-args %s\n", sha256_file(c.bridge_args), sha256_file(c.link_args));
    }
    char *stamp = PJ(ndir, "selfhost", "prepared.txt");
    char *want = (char *)buf_cstr(&id);
    int fresh = exists(stamp) && !strcmp(read_file(stamp, NULL), want) && is_file(PJ(ndir, "dsymutil", "inputs.txt")) && is_file(PJ(ndir, "version", "lld-gen", "lld", "Common", "Version.inc"));
    if (!fresh) {
        remove_file(stamp);
        prepare_version_sources(o, ndir, header);
        prepare_dsymutil(o, ndir, c.llvm, &c);
    }
    if (c.auto_linux) write_selfhost(o, &c, NULL, NULL);
    mkdirs(PJ(ndir, "selfhost"));
    write_file(stamp, want);
    printf("host prepared%s: %s\n", fresh ? " (verified existing)" : "", selfhost_config(o));
}

static Strs release_cmd(const Opts *o) {
    Strs cmd = {0};
    char *tool = PJ(o->self_dir, sfmt("release%s", EXE_SUFFIX));
    if (!is_file(tool)) fail("%s missing: the launcher compiles it beside this tool", tool);
    strs_add(&cmd, tool);
    strs_addf(&cmd, "--compiler=%s", o->compiler);
    strs_addf(&cmd, "--host-config=%s", selfhost_config(o));
    return cmd;
}

static void release_env_must(const Opts *o, Strs *cmd) {
    Strs extra = {0};
    strs_addf(&extra, "TMPDIR=%s", PJ(o->root, ".cache", "tmp"));
    strs_addf(&extra, "CROSS_CC_SEED_ROOT=%s", o->seed_root);
    static const char *const drop[] = {"TMPDIR", "CROSS_CC_SEED_ROOT", NULL};
    Strs env = make_env(drop, &extra);
    int code = run_cmd(cmd->v, o->root, env.v);
    if (code != 0) fail("%s failed (exit %d)", cmd->v[0], code);
}

/* Default build: prepare, then the same staged release pipeline as `release`,
 * publishing only the host compiler, then the argv0 alias copies. */
static void build(Opts *o) {
    prepare(o);
    mkdirs(PJ(o->root, "bin"));
    char *out = PJ(o->root, "bin", sfmt("cross_cc%s", EXE_SUFFIX));
    Strs cmd = release_cmd(o);
    strs_addf(&cmd, "--profile=%s", o->profile ? o->profile : "release");
    strs_addf(&cmd, "--build-only=%s", out);
    release_env_must(o, &cmd);
    if (!is_file(out)) fail("release pipeline did not publish %s", out);
    for (int i = 0; ALIASES[i]; i++) {
        char *dst = PJ(o->root, "bin", sfmt("%s%s", ALIASES[i], EXE_SUFFIX));
        remove_file(dst);
        copy_file(dst, out);
    }
    printf("built bin/cross_cc plus alias copies (argv0 selects mode)\n");
}

static void release_all(Opts *o) {
    prepare(o);
    Strs cmd = release_cmd(o);
    strs_cat(&cmd, &o->forward);
    release_env_must(o, &cmd);
}

/* Recompile the sibling helper tools with the seed (the launcher does this too). */
static void tools_cmd(Opts *o, int check) {
    require_seed(o);
    static const char *const names[] = {"build", "release", "linux-release-deps", "macos-x64-llvm", NULL};
    char *out = check ? PJ(o->root, "build", "check-tools") : o->self_dir;
    mkdirs(out);
    for (int i = 0; names[i]; i++) {
        char *src = PJ(o->root, "tools", sfmt("%s.c", names[i]));
        if (!is_file(src)) continue;
        if (!check && !strcmp(names[i], "build")) continue; /* never replace the running tool */
        char *dst = PJ(out, sfmt("%s%s", names[i], EXE_SUFFIX));
        Strs cmd = {0};
        strs_add(&cmd, o->compiler);
        strs_add(&cmd, "cc");
        strs_add(&cmd, "-std=c11");
        strs_add(&cmd, "-O1");
        if (check) strs_add(&cmd, "-Wall");
        strs_add(&cmd, "-o");
        strs_add(&cmd, dst);
        strs_add(&cmd, src);
        seed_must(o, o->root, &cmd);
        printf("%s: %s\n", check ? "checked" : "built", dst);
    }
    if (check) {
        char *wp = PJ(o->root, "tools", "windows_path", "windows_path_test.c");
        if (exists(wp)) run_c_test(o, wp, "windows_path_test");
    }
}

static int parse_jobs(const char *s) {
    char *end;
    long n = strtol(s, &end, 10);
    if (*s == 0 || *end || n < 1 || n > 4096) fail("--jobs needs a positive integer, got \"%s\"", s);
    return (int)n;
}

int main(int argc, char **argv) {
    tool_name = "build";
    Opts o;
    memset(&o, 0, sizeof o);
    o.opt = "2";
    o.jobs = cpu_count();
    /* The checkout root is the working directory when it holds the lock files;
     * otherwise the parent of the directory containing this executable. */
    o.root = find_root(argv[0]);
    o.self_dir = path_dir(real_or_abs(argv[0]));
    const char *cmd = argc > 1 && argv[1][0] != '-' ? argv[1] : "build";
    int first = argc > 1 && argv[1][0] != '-' ? 2 : 1;
    if (!strcmp(cmd, "help") || (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))) {
        fputs(USAGE, stdout);
        return 0;
    }
    int is_release = !strcmp(cmd, "release");
    for (int i = first; i < argc; i++) {
        const char *a = argv[i];
        if (starts_with(a, "--compiler=")) o.compiler = xstrdup(a + 11);
        else if (starts_with(a, "--deps-prefix=")) o.deps_prefix = xstrdup(a + 14);
        else if (starts_with(a, "--host-config=")) o.host_config = xstrdup(a + 14);
        else if (starts_with(a, "--host-sdk=")) o.host_sdk = xstrdup(a + 11);
        else if (starts_with(a, "--jobs=")) {
            o.jobs = parse_jobs(a + 7); /* local preparation only; release.c has no --jobs */
        } else if (is_release) strs_add(&o.forward, a);
        else if (starts_with(a, "--profile=")) {
            o.profile = xstrdup(a + 10);
            if (strcmp(o.profile, "debug") && strcmp(o.profile, "release")) fail("--profile must be debug or release");
        } else if (!strcmp(a, "--reference")) o.reference = 1;
        else if (starts_with(a, "--sdk=")) o.host_sdk = xstrdup(a + 6);
        else if (starts_with(a, "--llvm=")) fail("%s is obsolete: prepare always uses the pinned LLVM SDK (or LLVM_PREFIX)", a);
        else if (starts_with(a, "-O")) fail("%s is obsolete: use --profile=debug|release", a);
        else if (starts_with(a, "--output=")) o.output = xstrdup(a + 9);
        else if (starts_with(a, "--native-dir=")) fail("--native-dir is obsolete: set \"native-dir\" in a --host-config=FILE recipe");
        else if (starts_with(a, "--binary=")) o.binary = xstrdup(a + 9);
        else if (starts_with(a, "--upstream=")) o.upstream = xstrdup(a + 11);
        else if (starts_with(a, "--target=")) o.target = xstrdup(a + 9);
        else if (!strcmp(a, "--single-executable")) o.single_executable = 1;
        else if (starts_with(a, "--extra-licenses=")) o.extra_licenses = xstrdup(a + 17);
        else if (starts_with(a, "--msvc-helper=")) o.msvc_helper = xstrdup(a + 14);
        else fail("unknown option %s", a);
    }
    if (!strcmp(cmd, "setup")) setup(&o);
    else if (!strcmp(cmd, "resources")) resources(&o);
    else if (!strcmp(cmd, "prepare")) prepare(&o);
    else if (!strcmp(cmd, "build")) build(&o);
    else if (!strcmp(cmd, "release")) release_all(&o);
    else if (!strcmp(cmd, "native")) build(&o); /* compatibility alias */
    else if (!strcmp(cmd, "test")) test(&o);
    else if (!strcmp(cmd, "package")) package_dist(&o);
    else if (!strcmp(cmd, "tools")) tools_cmd(&o, 0);
    else if (!strcmp(cmd, "check-tools")) tools_cmd(&o, 1);
    else {
        fprintf(stderr, "unknown command %s\n", cmd);
        fputs(USAGE, stderr);
        return 2;
    }
    return 0;
}
