/* Apple host support: static zstd for amd64 or arm64, compiled from a fixed
 * source list with the selected cross_cc, without Make/configure/system compilers.
 *   tools/macos-x64-llvm --compiler=/abs/cross_cc --arch=arm64 --sdk=DIR --work=DIR
 * Without --compiler the first cross_cc on PATH is used. Produces WORK/zstd,
 * including headers, notices and a compiler/recipe/output-bound receipt.
 *
 * OPTIONAL --build-llvm additionally builds the Intel LLVM SDK in WORK/prefix.
 * LLVM publishes no official macOS x86_64 archive after 20.1.7. This separate
 * dependency-source utility uses the compiler-rt.lock.json LLVM source pin and
 * needs Apple's clang, Xcode, CMake and Ninja. Normal builds never invoke it;
 * Intel hosts supply an already prepared LLVM_PREFIX instead. */
#include "common.h"
#include "support_deps.h"

#define DEPLOYMENT_TARGET "11.0"
#define USAGE "usage: tools/macos-x64-llvm --compiler=/abs/cross_cc [--arch=amd64|arm64] [--sdk=DIR] [--work=DIR] [--jobs=N] [--build-llvm]\n"

static void run(const char *cwd, const Strs *cmd) {
    for (int i = 0; i < cmd->n; i++) printf("%s%s", i ? " " : "", cmd->v[i]);
    printf("\n");
    int code = run_cmd(cmd->v, cwd, NULL);
    if (code != 0) fail("%s failed (exit %d)", cmd->v[0], code);
}
static void run_l(const char *cwd, ...) {
    Strs cmd = {0};
    va_list ap;
    va_start(ap, cwd);
    for (const char *a = va_arg(ap, const char *); a; a = va_arg(ap, const char *)) strs_add(&cmd, a);
    va_end(ap);
    run(cwd, &cmd);
}
static char *capture_l(const char *cwd, ...) {
    Strs cmd = {0};
    va_list ap;
    va_start(ap, cwd);
    for (const char *a = va_arg(ap, const char *); a; a = va_arg(ap, const char *)) strs_add(&cmd, a);
    va_end(ap);
    return must_capture(cmd.v, cwd, NULL);
}

/* SDKSettings.json, not xcrun: xcrun cannot query an SDK outside the selected developer dir. */
static const char *sdk_version(const char *sdk) {
    Json *obj = json_load(PJ(sdk, "SDKSettings.json"));
    if (obj->type != J_OBJ) fail("%s/SDKSettings.json is not an object", sdk);
    return jstr(obj, "Version");
}

int main(int argc, char **argv) {
    tool_name = "macos-x64-llvm";
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        fputs(USAGE, stdout);
        return 0;
    }
    char *root = find_root(argv[0]);
    char *work = NULL;
    int jobs = cpu_count(), build_llvm = 0;
    const char *compiler_arg = NULL, *arch = "amd64", *sdk_arg = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (starts_with(a, "--compiler=")) {
            compiler_arg = a + 11;
        } else if (starts_with(a, "--arch=")) {
            arch = a + 7;
            if (strcmp(arch, "amd64") && strcmp(arch, "arm64")) fail("--arch must be amd64 or arm64");
        } else if (starts_with(a, "--sdk=")) {
            sdk_arg = a + 6;
        } else if (!strcmp(a, "--build-llvm")) {
            build_llvm = 1;
        } else if (starts_with(a, "--work=")) {
            work = xstrdup(a + 7);
        } else if (starts_with(a, "--jobs=")) {
            char *end;
            long n = strtol(a + 7, &end, 10);
            if (!a[7] || *end || n < 1) fail("--jobs needs a positive integer, got \"%s\"", a + 7);
            jobs = (int)n;
        } else {
            fputs(USAGE, stderr);
            return 1;
        }
    }
    if (strcmp(HOST_OS, "darwin")) fail("builds with Apple's SDK; run on macOS");
    if (build_llvm && strcmp(arch, "amd64")) fail("--build-llvm is Intel-only; use the pinned official ARM64 SDK");
    if (!work) work = PJ(root, ".cache", sfmt("llvm-darwin-%s", arch));
    if (!is_abs(work)) work = abs_path(work);
    mkdirs(work);

    char *sdk = sdk_arg ? abs_path(sdk_arg) : xstrdup(env_get("SDKROOT"));
    if (!*sdk) sdk = trim(capture_l(root, "xcrun", "--show-sdk-path", NULL));
    char *compiler = sd_find_compiler(compiler_arg);

    /* Static zstd, as the official macOS ARM64 LLVM links it. The pin is shared with the
     * Linux release dependencies. Same recipe as lib/Makefile libzstd.a-mt. */
    Json *zpin = jobj(jobj(json_load(PJ(root, "toolchain.lock.json")), "linux_release_deps"), "zstd");
    char *zname = sfmt("zstd-%s", jstr(zpin, "version"));
    char *zarchive = PJ(root, ".cache", "downloads", sfmt("%s.tar.gz", zname));
    download_verified(jstr(zpin, "url"), jstr(zpin, "sha256"), zarchive, 600);
    char *zprefix = PJ(work, "zstd");
    char *zlib = PJ(zprefix, "lib", "libzstd.a");
    static const char *const dirs[] = {"lib", "include", "licenses", NULL};
    Buf identity = {0};
    buf_addf(&identity, "recipe macos-zstd-v2\narch %s\ncompiler %s\nsdk %s\nsdk-settings %s\nlock %s\nhelper %s\nrecipes %s\ncommon %s\nmanifest\n", arch, sha256_file(compiler), sdk, sha256_file(PJ(sdk, "SDKSettings.json")), sha256_file(PJ(root, "toolchain.lock.json")), sha256_file(PJ(root, "tools", "macos-x64-llvm.c")), sha256_file(PJ(root, "tools", "support_deps.h")), sha256_file(PJ(root, "tools", "common.h")));
    char *receipt = PJ(zprefix, "complete.sha256");
    if (exists(receipt)) {
        char *want = sfmt("%s%s", buf_cstr(&identity), sd_manifest(zprefix, dirs));
        if (strcmp(read_file(receipt, NULL), want)) fail("zstd prefix %s differs from its receipt; choose a fresh --work", zprefix);
        if (!is_file(zlib)) fail("zstd prefix lacks %s", zlib);
    } else {
        if (exists(zlib)) fail("unverified zstd prefix %s; choose a fresh --work", zprefix);
        char *zsrc = PJ(work, zname);
        remove_all(zsrc);
        run_l(root, "tar", "-xf", zarchive, "-C", work, NULL);
        static const char *const more[] = {"MACOSX_DEPLOYMENT_TARGET", NULL};
        Strs env = sd_seed_env(compiler, PJ(work, "cross_cc-cache"), PJ(work, "tmp"), more);
        SdCtx sd = {0};
        sd.compiler = compiler;
        sd.env = env.v;
        sd.jobs = jobs;
        strs_add(&sd.target, "-target");
        strs_addf(&sd.target, "%s-macos." DEPLOYMENT_TARGET, !strcmp(arch, "amd64") ? "x86_64" : "aarch64");
        strs_add(&sd.target, !strcmp(arch, "amd64") ? "-march=x86-64-v2" : "-mcpu=generic");
        strs_add(&sd.target, "-isysroot");
        strs_add(&sd.target, sdk);
        sd_build_zstd(&sd, zsrc, PJ(work, "zstd-obj"), PJ(zprefix, "lib"), PJ(zprefix, "include"), 0);
        if (!exists(zlib)) fail("zstd build did not produce %s", zlib);
        mkdirs(PJ(zprefix, "licenses"));
        copy_file(PJ(zprefix, "licenses", "zstd-LICENSE"), PJ(zsrc, "LICENSE"));
        copy_file(PJ(zprefix, "licenses", "zstd-COPYING"), PJ(zsrc, "COPYING"));
        write_file(receipt, sfmt("%s%s", buf_cstr(&identity), sd_manifest(zprefix, dirs)));
    }
    if (!build_llvm) {
        printf("darwin-%s zstd ready: %s\n(pass --build-llvm for the optional Intel LLVM-from-source SDK build)\n", arch, zlib);
        return 0;
    }

    Json *pin = json_load(PJ(root, "compiler-rt.lock.json"));
    const char *url = jstr(pin, "url"), *sha = jstr(pin, "sha256"), *strip = jstr(pin, "strip_prefix");
    char *archive = PJ(root, ".cache", "downloads", last_segment(url));
    if (!exists(archive)) fail("%s missing; run `tools/build setup` first", archive);
    if (strcmp(sha256_file(archive), sha) != 0) fail("SHA-256 mismatch: %s", archive);

    char *src = PJ(work, "src");
    if (!is_dir(src)) {
        char *tmp = PJ(work, "src.tmp");
        remove_all(tmp);
        mkdirs(tmp);
        Strs cmd = {0};
        strs_add(&cmd, "tar");
        strs_add(&cmd, "-xf");
        strs_add(&cmd, archive);
        strs_add(&cmd, "-C");
        strs_add(&cmd, tmp);
        strs_add(&cmd, "--strip-components=1");
        static const char *const dirs[] = {"llvm", "clang", "lld", "cmake", "third-party", "libunwind", NULL};
        for (int i = 0; dirs[i]; i++) strs_addf(&cmd, "%s/%s", strip, dirs[i]);
        run(root, &cmd);
        rename_path(tmp, src);
    }

    /* LLVM 22 TAPI cannot read the arm64e.x1 entries in newer SDK .tbd stubs (macOS 27 SDK),
     * so the final link could not use the SDK libz/libxml2. Pick an older SDK via SDKROOT. */
    char *tbd_path = PJ(sdk, "usr", "lib", "libz.tbd");
    if (!is_file(tbd_path) || contains(read_file(tbd_path, NULL), ".x1-")) fail("SDK %s has .tbd stubs LLVM 22 cannot read; set SDKROOT to a macOS 15 or 26 SDK", sdk);
    char *cc = capture_l(root, "xcrun", "--find", "clang", NULL);
    char *cxx = capture_l(root, "xcrun", "--find", "clang++", NULL);


    char *build = PJ(work, "build"), *prefix = PJ(work, "prefix");
    Strs configure = {0};
    strs_add(&configure, "cmake");
    strs_add(&configure, "-G");
    strs_add(&configure, "Ninja");
    strs_add(&configure, "-S");
    strs_add(&configure, PJ(src, "llvm"));
    strs_add(&configure, "-B");
    strs_add(&configure, build);
    strs_add(&configure, "-Wno-author");
    strs_add(&configure, "-DCMAKE_BUILD_TYPE=Release");
    strs_addf(&configure, "-DCMAKE_C_COMPILER=%s", cc);
    strs_addf(&configure, "-DCMAKE_CXX_COMPILER=%s", cxx);
    strs_add(&configure, "-DCMAKE_OSX_ARCHITECTURES=x86_64");
    strs_add(&configure, "-DCMAKE_OSX_DEPLOYMENT_TARGET=" DEPLOYMENT_TARGET);
    strs_addf(&configure, "-DCMAKE_OSX_SYSROOT=%s", sdk);
    strs_addf(&configure, "-DCMAKE_INSTALL_PREFIX=%s", prefix);
    strs_add(&configure, "-DLLVM_ENABLE_PROJECTS=clang;lld");
    /* Same external libraries as the official macOS ARM64 archive: SDK zlib and libxml2, static zstd. */
    strs_add(&configure, "-DLLVM_ENABLE_ZLIB=FORCE_ON");
    strs_add(&configure, "-DLLVM_ENABLE_LIBXML2=FORCE_ON");
    strs_add(&configure, "-DLLVM_ENABLE_ZSTD=FORCE_ON");
    strs_add(&configure, "-DLLVM_USE_STATIC_ZSTD=ON");
    strs_addf(&configure, "-Dzstd_INCLUDE_DIR=%s", PJ(zprefix, "include"));
    strs_addf(&configure, "-Dzstd_LIBRARY=%s", zlib);
    strs_addf(&configure, "-Dzstd_STATIC_LIBRARY=%s", zlib);
    static const char *const rest[] = {"-DLLVM_ENABLE_LIBEDIT=OFF", "-DLLVM_ENABLE_LIBPFM=OFF", "-DLLVM_ENABLE_CURL=OFF", "-DLLVM_ENABLE_HTTPLIB=OFF", "-DLLVM_ENABLE_Z3_SOLVER=OFF", "-DLLVM_ENABLE_FFI=OFF", "-DLLVM_INCLUDE_TESTS=OFF", "-DLLVM_INCLUDE_BENCHMARKS=OFF", "-DLLVM_INCLUDE_EXAMPLES=OFF", "-DLLVM_INCLUDE_DOCS=OFF", "-DCLANG_INCLUDE_TESTS=OFF", "-DCLANG_INCLUDE_DOCS=OFF", "-DLLVM_ENABLE_ASSERTIONS=OFF", "-DBUILD_SHARED_LIBS=OFF", "-DLLVM_BUILD_LLVM_DYLIB=OFF", "-DLLVM_APPEND_VC_REV=OFF", "-DCLANG_PLUGIN_SUPPORT=OFF", NULL};
    strs_addv(&configure, rest);
    run(root, &configure);
    run_l(root, "ninja", "-C", build, sfmt("-j%d", jobs), "install", NULL);

    char *config = PJ(prefix, "bin", "llvm-config");
    char *h = exists(PJ(prefix, "include", "llvm", "Config", "llvm-config.h")) ? read_file(PJ(prefix, "include", "llvm", "Config", "llvm-config.h"), NULL) : xstrdup("");
    if (!contains(h, "#define LLVM_VERSION_MAJOR 22") || !contains(h, "#define LLVM_VERSION_PATCH 8")) fail("installed prefix is not LLVM 22.1.8");
    if (!exists(config)) fail("installed prefix lacks %s", config);
    Buf llvm_receipt = {0};
    buf_addf(&llvm_receipt, "recipe macos-x64-llvm-v1\nsource %s\nsource-sha256 %s\nsdk %s\nsdk-version %s\ncompiler %s\n", url, sha, sdk, sdk_version(sdk), capture_l(root, cxx, "--version", NULL));
    for (int i = 1; i < configure.n; i++) buf_addf(&llvm_receipt, "configure-arg %s\n", configure.v[i]);
    write_file(PJ(prefix, "cross_cc-llvm-build.txt"), buf_cstr(&llvm_receipt));
    printf("darwin-amd64 LLVM SDK ready: %s\n", prefix);
    return 0;
}
