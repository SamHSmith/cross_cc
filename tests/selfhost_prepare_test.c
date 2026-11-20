/* Offline tests for the seeded host preparation in tools/build.c: seed
 * resource discovery, the generated host recipe (.cache/native/selfhost.json
 * plus bridge.args/link.args) and its GNU-ABI runtime. Fake fixtures only:
 * no network, no compiler, no real LLVM SDK. */
#define main cross_cc_build_main
#include "../tools/build.c"
#undef main

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

static int has(const Strs *a, const char *x) {
    for (int i = 0; i < a->n; i++) {
        if (!strcmp(a->v[i], x)) return 1;
    }
    return 0;
}

static void fake_seed(const char *dir) {
    mkdirs(PJ(dir, "lib", "include"));
    mkdirs(PJ(dir, "lib", "libc"));
    write_file(PJ(dir, "upstream.lock.json"), "{}\n");
    write_file(PJ(dir, "cross_cc"), "");
}

static int child(const char *self, const char *action, const char *arg) {
    char *args[] = {(char *)self, (char *)action, (char *)arg, NULL};
    char *out = NULL;
    return run_capture(args, NULL, NULL, &out);
}

int main(int argc, char **argv) {
    tool_name = "selfhost_prepare_test";
    if (argc == 3) {
        Opts o = {0};
        o.root = abs_path(".");
        if (!strcmp(argv[1], "--seed")) {
            o.compiler = argv[2];
            require_seed(&o);
        } else if (!strcmp(argv[1], "--sdk")) {
            check_host_sdk(argv[2]);
        } else if (!strcmp(argv[1], "--prepare")) {
            o.root = argv[2];
            prepare(&o);
        } else {
            fail("unknown test action");
        }
        return 0;
    }
#ifdef _WIN32
    puts("selfhost prepare tests skipped (linux-amd64 host recipe; POSIX fixtures)");
    return 0;
#else
    unsetenv("CROSS_CC_SEED_ROOT");
    char *self = abs_path(argv[0]);
    char *root = abs_path(sfmt("build/tests/selfhost-fixture-%ld", (long)getpid()));
    remove_all(root);
    mkdirs(root);

    /* Seed resources: the package directory itself, then a bin/ layout. */
    char *seed = PJ(root, "seed");
    fake_seed(seed);
    Opts o = {0};
    o.root = root;
    o.compiler = PJ(seed, "cross_cc");
    require_seed(&o);
    CHECK(o.seed_root && !strcmp(o.seed_root, real_or_abs(seed)));
    char *seed2 = PJ(root, "seed2");
    fake_seed(seed2);
    mkdirs(PJ(seed2, "bin"));
    write_file(PJ(seed2, "bin", "cross_cc"), "");
    Opts o2 = {0};
    o2.root = root;
    o2.compiler = PJ(seed2, "bin", "cross_cc");
    require_seed(&o2);
    CHECK(o2.seed_root && !strcmp(o2.seed_root, real_or_abs(seed2)));
    CHECK(child(self, "--seed", "relative/cross_cc") != 0);
    CHECK(child(self, "--seed", PJ(root, "missing", "cross_cc")) != 0);
    mkdirs(PJ(root, "bare"));
    write_file(PJ(root, "bare", "cross_cc"), "");
    CHECK(child(self, "--seed", PJ(root, "bare", "cross_cc")) != 0);
    /* No --compiler: prepare refuses before touching anything. */
    CHECK(child(self, "--prepare", root) != 0);
    CHECK(!exists(PJ(root, ".cache", "native")));

    /* Seed env: the seed's own resources, ambient compiler overrides dropped. */
    setenv("CPATH", "/nonexistent", 1);
    setenv("CROSS_CC_ROOT", "/checkout", 1);
    Strs env = seed_env(&o);
    CHECK(has(&env, sfmt("CROSS_CC_ROOT=%s", o.seed_root)));
    CHECK(!has(&env, "CROSS_CC_ROOT=/checkout"));
    CHECK(!has(&env, "CPATH=/nonexistent"));
    unsetenv("CPATH");
    unsetenv("CROSS_CC_ROOT");

    /* Bridge flags: explicit libstdc++-12 headers before the compiler's own. */
    Strs bridge = host_bridge_args("{sdk}", "{root}");
    CHECK(bridge.n > 0 && !strcmp(bridge.v[0], "-nostdinc"));
    CHECK(has(&bridge, "{sdk}/sysroot/usr/include/c++/12"));
    CHECK(has(&bridge, "{sdk}/sysroot/usr/include/x86_64-linux-gnu/c++/12"));
    CHECK(has(&bridge, "{root}/lib/libc/include/generic-glibc"));
    CHECK(has(&bridge, "-D__GLIBC_MINOR__=34"));

    /* Support SDK verification. */
    char *sdk = PJ(root, "sdk");
    mkdirs(sdk);
    CHECK(child(self, "--sdk", sdk) != 0);
    for (int i = 0; HOST_CXX_HEADERS[i]; i++) mkdirs(PJ(sdk, HOST_CXX_HEADERS[i]));
    mkdirs(PJ(sdk, "lib"));
    for (int i = 0; HOST_SDK_LIBS[i]; i++) write_file(PJ(sdk, "lib", HOST_SDK_LIBS[i]), "");
    for (int i = 0; HOST_CXX_RUNTIME[i]; i++) write_file(PJ(sdk, "lib", HOST_CXX_RUNTIME[i]), "");
    CHECK(child(self, "--sdk", sdk) == 0);

    /* Host recipe from a fake LLVM SDK. */
    write_file(PJ(root, "toolchain.lock.json"), "{\"native\":{\"clang_libs\":[\"clangBasic\"],\"lld_libs\":[\"lldCommon\"]}}\n");
    char *llvm = PJ(root, ".cache", "native", "llvm");
    mkdirs(PJ(llvm, "lib"));
    mkdirs(PJ(llvm, "bin"));
    static const char *const libs[] = {"libclangBasic.a", "liblldCommon.a", "libLLVMSupport.a", "libLLVMDemangle.a", NULL};
    for (int i = 0; libs[i]; i++) write_file(PJ(llvm, "lib", libs[i]), "");
    char *config = PJ(llvm, "bin", "llvm-config");
    write_file(config, "#!/bin/sh\necho libLLVMSupport.a libLLVMDemangle.a\n");
    chmod(config, 0755);
    char *ndir = PJ(root, ".cache", "native");
    write_selfhost(&o, ndir, llvm, sdk);
    Json *cfg = json_load(selfhost_config(&o));
    CHECK(!strcmp(jstr(cfg, "target"), "linux-amd64"));
    CHECK(!strcmp(jstr(cfg, "llvm-prefix"), ".cache/native/llvm"));
    CHECK(!strcmp(jstr(cfg, "sdk"), "sdk"));
    CHECK(!strcmp(jstr(cfg, "bridge-args"), ".cache/native/selfhost/bridge.args"));
    CHECK(!strcmp(jstr(cfg, "link-args"), ".cache/native/selfhost/link.args"));
    CHECK(!strcmp(jstr(cfg, "link-mode"), "cc"));
    CHECK(!strcmp(jstr(cfg, "cpu"), "x86-64-v2"));
    CHECK(!strcmp(jstr(cfg, "linux-glibc"), "2.34"));
    CHECK(!strcmp(jstr(cfg, "native-dir"), ".cache/native"));
    Strs bl = split_lines(read_file(PJ(ndir, "selfhost", "bridge.args"), NULL));
    CHECK(bl.n == bridge.n && has(&bl, "{sdk}/sysroot/usr/include/c++/12/backward"));
    Strs ll = split_lines(read_file(PJ(ndir, "selfhost", "link.args"), NULL));
    CHECK(ll.n > 4 && !strcmp(ll.v[0], "{object}") && !strcmp(ll.v[1], "{version}") && !strcmp(ll.v[2], "{lld_version}"));
    CHECK(has(&ll, "{bridge}"));
    CHECK(has(&ll, "{llvm}/lib/libclangBasic.a") && has(&ll, "{llvm}/lib/liblldCommon.a"));
    CHECK(has(&ll, "{llvm}/lib/libLLVMSupport.a") && has(&ll, "{llvm}/lib/libLLVMDemangle.a"));
    CHECK(has(&ll, "{sdk}/lib/libstdc++.a") && has(&ll, "{sdk}/lib/libgcc_eh.a"));
    /* -lstdc++ would be remapped to libc++ by cross_cc; no prebuilt backend either. */
    CHECK(!has(&ll, "-lstdc++") && !has(&ll, "-lc++") && !has(&ll, "-nostdlib"));
    for (int i = 0; i < ll.n; i++) CHECK(!contains(ll.v[i], root) && !contains(ll.v[i], "libcross_cc_backend"));

    if (failures) return 1;
    remove_all(root);
    puts("selfhost prepare tests passed (offline)");
    return 0;
#endif
}
