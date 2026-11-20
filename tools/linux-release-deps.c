/* Linux release support archives. Plain C; no Make, configure, Python or CMake.
 *   tools/linux-release-deps --compiler=/abs/cross_cc [--lock=FILE] [--prefix=ABSOLUTE-DIR] [--jobs=N]
 * zlib, zstd and libxml2 are compiled from fixed source lists with the selected
 * cross_cc (`cross_cc cc`, `cross_cc ar`); without --compiler the first cross_cc on
 * PATH is used. Pins are linux_release_deps in toolchain.lock.json. The only other
 * programs run are curl and tar. Does not modify the LLVM SDK. */
#include "common.h"
#include "support_deps.h"

typedef struct {
    char *root, *prefix, *cc;
    int jobs;
    SdCtx sd;
    Strs env;
} Options;

static void run(const Options *o, const char *cwd, ...) {
    Strs cmd = {0};
    va_list ap;
    va_start(ap, cwd);
    for (const char *a = va_arg(ap, const char *); a; a = va_arg(ap, const char *)) strs_add(&cmd, a);
    va_end(ap);
    for (int i = 0; i < cmd.n; i++) printf("%s%s", i ? " " : "", cmd.v[i]);
    printf("\n");
    int code = run_cmd(cmd.v, cwd, o->env.v);
    if (code != 0) fail("%s failed (exit %d)", cmd.v[0], code);
}

static char *fetch(const Options *o, const Json *pin, const char *name) {
    char *dest = PJ(o->prefix, "downloads", name);
    const char *expected = jstr(pin, "sha256");
    if (!exists(dest)) {
        char *part = sfmt("%s.part", dest);
        run(o, o->root, "curl", "-fL", "--proto", "=https", "--retry", "3", "--connect-timeout", "30", "--max-time", "600", "-o", part, jstr(pin, "url"), NULL);
        if (strcmp(sha256_file(part), expected) != 0) fail("SHA-256 mismatch: %s", part);
        rename_path(part, dest);
    }
    if (strcmp(sha256_file(dest), expected) != 0) fail("SHA-256 mismatch: %s", dest);
    return dest;
}

static char *source(const Options *o, const Json *pins, const char *key, const char *suffix) {
    Json *pin = jobj(pins, key);
    char *name = sfmt("%s-%s", key, jstr(pin, "version"));
    char *archive = fetch(o, pin, sfmt("%s%s", name, suffix));
    char *dst = PJ(o->prefix, "src", name);
    if (!exists(dst)) run(o, o->root, "tar", "-xf", archive, "-C", PJ(o->prefix, "src"), NULL);
    if (!is_dir(dst)) fail("archive lacks %s", dst);
    return dst;
}

static void deb(const Options *o, const Json *pins, const char *key) {
    char *archive = fetch(o, jobj(pins, key), sfmt("%s.deb", key));
    char *dir = PJ(o->prefix, "deb", key);
    mkdirs(dir);
    Strs x = {0};
    strs_add(&x, "x");
    strs_add(&x, archive);
    sd_ar(&o->sd, dir, &x);
    const char *payload = exists(PJ(dir, "data.tar.xz")) ? "data.tar.xz" : "data.tar.zst";
    run(o, o->root, "tar", "-xf", PJ(dir, payload), "-C", PJ(o->prefix, "sysroot"), NULL);
}

static void copy_to(const char *dst, const char *src) { copy_file(dst, src); }


static char *find_compiler_checked(const char *arg) { return sd_find_compiler(arg); }

int main(int argc, char **argv) {
    tool_name = "linux-release-deps";
    Options o;
    memset(&o, 0, sizeof o);
    o.root = find_root(argv[0]);
    o.prefix = PJ(o.root, ".cache", "linux-release-deps");
    o.jobs = cpu_count();
    const char *compiler_arg = NULL;
    char *lock = PJ(o.root, "toolchain.lock.json");
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (starts_with(a, "--prefix=")) o.prefix = xstrdup(a + 9);
        else if (starts_with(a, "--lock=")) lock = xstrdup(a + 7);
        else if (starts_with(a, "--compiler=")) compiler_arg = a + 11;
        else if (starts_with(a, "--jobs=")) {
            char *end;
            long n = strtol(a + 7, &end, 10);
            if (!a[7] || *end || n < 1) fail("--jobs needs a positive integer");
            o.jobs = (int)n;
        }
        else if (!strcmp(a, "--help")) {
            puts("usage: tools/linux-release-deps --compiler=/abs/cross_cc [--lock=FILE] [--prefix=ABSOLUTE-DIR] [--jobs=N]");
            return 0;
        } else fail("unknown argument %s", a);
    }
    if (strcmp(HOST_OS, "linux") || strcmp(HOST_ARCH, "amd64")) fail("only Linux amd64 is pinned and verified");
    if (!is_abs(o.prefix)) fail("--prefix must be absolute");
    static const char *const dirs[] = {"downloads", "src", "lib", "licenses", "tmp", "sysroot", NULL};
    for (int i = 0; dirs[i]; i++) mkdirs(PJ(o.prefix, dirs[i]));
    o.cc = find_compiler_checked(compiler_arg);
    o.env = sd_seed_env(o.cc, PJ(o.prefix, "tmp", "cross_cc-cache"), PJ(o.prefix, "tmp"), NULL);
    o.sd.compiler = o.cc;
    o.sd.env = o.env.v;
    o.sd.jobs = o.jobs;
    strs_add(&o.sd.target, "-target");
    strs_add(&o.sd.target, "x86_64-linux-gnu.2.34");
    strs_add(&o.sd.target, "-march=x86-64-v2");
    Json *top = json_load(lock);
    if (top->type != J_OBJ) fail("lock is not an object");
    Json *pins = jobj(top, "linux_release_deps");
    /* The receipt binds the lock, this helper and its recipe header, the compiler binary,
     * the target flags and every output file hash. Reuse recomputes all of it. */
    static const char *const manifest_dirs[] = {"lib", "include", "licenses", "sysroot/usr/include/c++", "sysroot/usr/include/x86_64-linux-gnu/c++", "sysroot/usr/lib/gcc", NULL};
    Buf hb = {0};
    buf_addf(&hb, "recipe linux-release-deps-v2\nlock %s\ncompiler %s\n", sha256_file(lock), sha256_file(o.cc));
    buf_addf(&hb, "source-file %s tools/linux-release-deps.c\nsource-file %s tools/support_deps.h\nsource-file %s tools/common.h\ntarget", sha256_file(PJ(o.root, "tools", "linux-release-deps.c")), sha256_file(PJ(o.root, "tools", "support_deps.h")), sha256_file(PJ(o.root, "tools", "common.h")));
    for (int i = 0; i < o.sd.target.n; i++) buf_addf(&hb, " %s", o.sd.target.v[i]);
    buf_addf(&hb, "\nmanifest\n");
    const char *header = buf_cstr(&hb);
    char *receipt = PJ(o.prefix, "complete.sha256");
    if (exists(receipt)) {
        char *want = sfmt("%s%s", header, sd_manifest(o.prefix, manifest_dirs));
        if (strcmp(read_file(receipt, NULL), want) != 0) fail("dependency prefix %s differs from its receipt (other lock, helper, compiler, flags, or a modified/missing output file); choose a fresh --prefix", o.prefix);
        static const char *const libs[] = {"z", "zstd", "xml2", "stdc++", "gcc", "gcc_eh", NULL};
        for (int i = 0; libs[i]; i++) {
            if (!exists(PJ(o.prefix, "lib", sfmt("lib%s.a", libs[i])))) fail("incomplete prefix: missing %s", libs[i]);
        }
        printf("verified existing dependency prefix %s\n", o.prefix);
        return 0;
    }
    static const char *const notices[] = {"GPL-3.0.txt", "GCC-Runtime-Library-Exception-3.1.txt", NULL};
    for (int i = 0; notices[i]; i++) copy_to(PJ(o.prefix, "licenses", notices[i]), PJ(o.root, "licenses", notices[i]));
    char *incdir = PJ(o.prefix, "include"), *libdir = PJ(o.prefix, "lib");
    char *bld = PJ(o.prefix, "build");
    char *z = source(&o, pins, "zlib", ".tar.gz");
    sd_build_zlib(&o.sd, z, PJ(bld, "zlib"), libdir, incdir, 1);
    char *zs = source(&o, pins, "zstd", ".tar.gz");
    sd_build_zstd(&o.sd, zs, PJ(bld, "zstd"), libdir, incdir, 1);
    /* Complete XML parser and schemas; only HTTP, iconv, ICU, lzma, zlib, modules and
     * language bindings are excluded (same options as the former configure line). */
    char *xml = source(&o, pins, "libxml2", ".tar.xz");
    sd_build_libxml2(&o.sd, jstr(jobj(pins, "libxml2"), "version"), xml, PJ(bld, "libxml2"), PJ(bld, "libxml2", "obj"), libdir, incdir);
    deb(&o, pins, "libstdcxx");
    deb(&o, pins, "libgcc");
    deb(&o, pins, "gcc_base");
    deb(&o, pins, "gcc11_base");
    deb(&o, pins, "libc");
    static const char *const archives[] = {"libstdc++.a", "libgcc.a", "libgcc_eh.a", NULL};
    for (int i = 0; archives[i]; i++) {
        const char *version = i == 0 ? "12" : "11";
        copy_to(PJ(o.prefix, "lib", archives[i]), PJ(o.prefix, "sysroot", "usr", "lib", "gcc", "x86_64-linux-gnu", version, archives[i]));
    }
    /* Link-time only: older libm selects its old symbol versions. The final
     * executable uses the destination OS's libm.so.6; do not package this file. */
    copy_to(PJ(o.prefix, "lib", "libm.so"), PJ(o.prefix, "sysroot", "lib", "x86_64-linux-gnu", "libm-2.31.so"));
    char *copies[][2] = {
        {"zlib-LICENSE", PJ(z, "LICENSE")},
        {"zstd-LICENSE", PJ(zs, "LICENSE")},
        {"zstd-COPYING", PJ(zs, "COPYING")},
        {"libxml2-Copyright", PJ(xml, "Copyright")},
        {"gcc-12-copyright", PJ(o.prefix, "sysroot", "usr", "share", "doc", "gcc-12-base", "copyright")},
        {"gcc-11-copyright", PJ(o.prefix, "sysroot", "usr", "share", "doc", "gcc-11-base", "copyright")},
    };
    for (size_t i = 0; i < sizeof copies / sizeof copies[0]; i++) copy_to(PJ(o.prefix, "licenses", copies[i][0]), copies[i][1]);
    write_file(receipt, sfmt("%s%s", header, sd_manifest(o.prefix, manifest_dirs)));
    printf("built static release support archives: %s\n", PJ(o.prefix, "lib"));
    puts("Final executable still requires a readelf dependency/version audit; host glibc is not pinned by this helper.");
    return 0;
}
