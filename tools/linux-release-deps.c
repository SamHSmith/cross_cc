/* Linux release support archives. Plain C, no Python or shell script required.
 *   cc -O1 -o tools/linux-release-deps tools/linux-release-deps.c
 *   tools/linux-release-deps [--lock=FILE] [--prefix=ABSOLUTE-DIR] [--cc=EXECUTABLE]
 * Pins are linux_release_deps in toolchain.lock.json. Does not modify the LLVM SDK. */
#include "common.h"

typedef struct {
    char *root, *prefix, *cc;
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
    run(o, dir, "ar", "x", archive, NULL);
    const char *payload = exists(PJ(dir, "data.tar.xz")) ? "data.tar.xz" : "data.tar.zst";
    run(o, o->root, "tar", "-xf", PJ(dir, payload), "-C", PJ(o->prefix, "sysroot"), NULL);
}

static void copy_to(const char *dst, const char *src) { copy_file(dst, src); }

int main(int argc, char **argv) {
    tool_name = "linux-release-deps";
    Options o;
    memset(&o, 0, sizeof o);
    o.root = find_root(argv[0]);
    o.prefix = PJ(o.root, ".cache", "linux-release-deps");
    o.cc = "gcc";
    char *lock = PJ(o.root, "toolchain.lock.json");
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (starts_with(a, "--prefix=")) o.prefix = xstrdup(a + 9);
        else if (starts_with(a, "--lock=")) lock = xstrdup(a + 7);
        else if (starts_with(a, "--cc=")) o.cc = xstrdup(a + 5);
        else if (!strcmp(a, "--help")) {
            puts("usage: tools/linux-release-deps [--lock=FILE] [--prefix=ABSOLUTE-DIR] [--cc=EXECUTABLE]");
            return 0;
        } else fail("unknown argument %s", a);
    }
    if (strcmp(HOST_OS, "linux") || strcmp(HOST_ARCH, "amd64")) fail("only Linux amd64 is pinned and verified");
    if (!is_abs(o.prefix)) fail("--prefix must be absolute");
    static const char *const dirs[] = {"downloads", "src", "lib", "licenses", "tmp", "sysroot", NULL};
    for (int i = 0; dirs[i]; i++) mkdirs(PJ(o.prefix, dirs[i]));
    static const char *const drop[] = {"TMPDIR", "CC", "CFLAGS", "CPPFLAGS", "LDFLAGS", "LIBRARY_PATH", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "MAKEFLAGS", NULL};
    Strs extra = {0};
    strs_addf(&extra, "TMPDIR=%s", PJ(o.prefix, "tmp"));
    strs_addf(&extra, "CC=%s", o.cc);
    strs_add(&extra, "CFLAGS=-O2 -std=gnu11");
    strs_add(&extra, "CPPFLAGS=");
    strs_add(&extra, "LDFLAGS=");
    o.env = make_env(drop, &extra);
    Json *top = json_load(lock);
    if (top->type != J_OBJ) fail("lock is not an object");
    Json *pins = jobj(top, "linux_release_deps");
    static const char *const notices[] = {"GPL-3.0.txt", "GCC-Runtime-Library-Exception-3.1.txt", NULL};
    for (int i = 0; notices[i]; i++) copy_to(PJ(o.prefix, "licenses", notices[i]), PJ(o.root, "licenses", notices[i]));
    /* A completed invocation is reusable only with the exact same lock file. */
    char *receipt = PJ(o.prefix, "complete.sha256");
    if (exists(receipt)) {
        char *contents = trim(read_file(receipt, NULL));
        if (strcmp(contents, sha256_file(lock)) != 0) fail("dependency prefix belongs to another lock; choose a fresh --prefix");
        static const char *const libs[] = {"z", "zstd", "xml2", "stdc++", "gcc", "gcc_eh", NULL};
        for (int i = 0; libs[i]; i++) {
            if (!exists(PJ(o.prefix, "lib", sfmt("lib%s.a", libs[i])))) fail("incomplete prefix: missing %s", libs[i]);
        }
        printf("verified existing dependency prefix %s\n", o.prefix);
        return 0;
    }
    char *prefix_arg = sfmt("--prefix=%s", o.prefix);
    char *z = source(&o, pins, "zlib", ".tar.gz");
    run(&o, z, PJ(z, "configure"), "--static", prefix_arg, NULL);
    run(&o, z, "make", "-j2", NULL);
    run(&o, z, "make", "install", NULL);
    char *zs = source(&o, pins, "zstd", ".tar.gz");
    run(&o, zs, "make", "-j2", "-C", "lib", "libzstd.a-mt", NULL);
    run(&o, zs, "make", "-C", "lib", "install-static", "install-includes", sfmt("PREFIX=%s", o.prefix), NULL);
    char *xml = source(&o, pins, "libxml2", ".tar.xz");
    /* Keep the complete XML parser and schemas. Only unused external protocols,
     * language bindings and optional external libraries are excluded. LLVM's
     * Windows manifest merger retains its libxml2 implementation. */
    run(&o, xml, PJ(xml, "configure"), prefix_arg, "--disable-shared", "--enable-static", "--without-python", "--without-lzma", "--without-zlib", "--without-iconv", "--without-icu", "--without-readline", "--without-http", "--without-modules", NULL);
    run(&o, xml, "make", "-j2", NULL);
    run(&o, xml, "make", "install", NULL);
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
    write_file(receipt, sfmt("%s\n", sha256_file(lock)));
    printf("built static release support archives: %s\n", PJ(o.prefix, "lib"));
    puts("Final executable still requires a readelf dependency/version audit; host glibc is not pinned by this helper.");
    return 0;
}
