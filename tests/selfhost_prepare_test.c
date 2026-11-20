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
        } else if (!strcmp(argv[1], "--host-config")) {
            SelfhostCfg c = {0};
            o.host_config = argv[2];
            load_host_config(&o, &c);
        } else if (!strcmp(argv[1], "--darwin-nozstd")) {
            Strs a = {0};
            sp_link_tail(selfhost_host("darwin-arm64"), &a, "-lm -lzstd", NULL, NULL);
        } else if (!strcmp(argv[1], "--abs-syslib")) {
            Strs a = {0};
            sp_link_tail(selfhost_host("windows-amd64"), &a, "/opt/builder/libfoo.a", NULL, NULL);
        } else if (!strcmp(argv[1], "--msvc-cross")) {
            const char *compiler = env_get("SELFHOST_PROBE_COMPILER");
            if (!is_file(compiler)) fail("set SELFHOST_PROBE_COMPILER to an existing cross_cc seed");
            o.compiler = (char *)compiler;
            require_seed(&o);
            char *probe = PJ(o.root, "build", "tests", "msvc-sdk-probe");
            if (exists(probe)) fail("probe directory already exists: %s", probe);
            mkdirs(probe);
            char *source = PJ(probe, "probe.c"), *exe = PJ(probe, "probe.exe");
            write_file(source, "#include <windows.h>\n#include <stdio.h>\nint main(void) { printf(\"%lu\\n\", (unsigned long)GetCurrentProcessId()); return 0; }\n");
            Strs env = seed_env(&o);
            sp_msvc_env(&env, abs_path(argv[2]), PJ(probe, "libc.txt"));
            char *cmd[] = {(char *)compiler, "cc", "-target", "x86_64-windows-msvc", (char *)source, "-o", exe, NULL};
            if (run_cmd(cmd, o.root, env.v)) fail("supplied-SDK Windows probe link failed");
            size_t n;
            char *pe = read_file(exe, &n);
            CHECK(n > 2 && pe[0] == 'M' && pe[1] == 'Z');
            char *main_cmd[] = {(char *)compiler, "cc", "-target", "x86_64-windows-msvc", "-std=c11", "-O0", "-DCROSS_CC_RELEASE_OBJECT=1", "-DCROSS_CC_WINDOWS_CANONICAL_PATHS=1", "-c", PJ(o.root, "main.c"), "-o", PJ(probe, "main.o"), NULL};
            if (run_cmd(main_cmd, o.root, env.v)) fail("supplied-SDK Windows main.c compile failed");
            CHECK(file_size(PJ(probe, "main.o")) > 0);
            /* Use the generated tail with an immutable seed too: the CRT's
             * combined chkstk/_alloca_probe member must precede auto builtins,
             * while O0's automatically supplied UBSan must remain available. */
            write_file(source, "extern void _alloca_probe(void);\nvoid (*volatile probe_ref)(void) = _alloca_probe;\n__declspec(noinline) void touch(volatile char *p) { p[0] = 9; p[16383] = 3; }\nint main(int argc, char **argv) { volatile char stack[16384]; touch(stack); volatile int divisor = argc; return stack[0] / divisor + stack[16383] - 12 + (probe_ref == 0); }\n");
            for (int mode = 0; mode < 2; mode++) {
                char *obj = PJ(probe, "stack.o");
                char *compile[] = {(char *)compiler, "cc", "-target", "x86_64-windows-msvc", mode ? "-O0" : "-O2", "-fms-runtime-lib=static", "-c", source, "-o", obj, NULL};
                if (run_cmd(compile, o.root, env.v)) fail("stackprobe fixture compile failed");
                Strs link = {0}, tail = {0};
                strs_add(&link, compiler); strs_add(&link, "cc");
                strs_add(&link, "-target"); strs_add(&link, "x86_64-windows-msvc");
                strs_add(&link, mode ? "-O0" : "-O2");
                strs_add(&link, obj);
                sp_link_tail(selfhost_host("windows-amd64"), &tail, "", NULL, NULL);
                for (int k = 0; k < tail.n; k++)
                    strs_add(&link, sp_expand(tail.v[k], o.seed_root, abs_path(argv[2]), "", ""));
                strs_add(&link, "-s"); strs_add(&link, "-o"); strs_add(&link, exe);
                if (run_cmd(link.v, o.root, env.v)) fail("generated Windows CRT recipe failed (%s)", mode ? "debug" : "release");
            }
            puts("generated MSVC recipe: release and O0 debug stackprobe links passed");
            remove_all(probe);
            return failures ? 1 : 0;
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
    SelfhostCfg lc = {selfhost_host("linux-amd64"), llvm, sdk, ndir, NULL, NULL, "x86-64-v2", "2.34", 1};
    mkdirs(PJ(sdk, "licenses"));
    write_file(PJ(sdk, "licenses", "zlib-LICENSE"), "fixture notice\n");
    write_selfhost(&o, &lc, NULL, NULL);
    Json *cfg = json_load(selfhost_config(&o));
    CHECK(!strcmp(jstr(cfg, "extra-licenses"), "sdk/licenses"));
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

    /* Host table and per-host recipes (mocks: no Apple or Microsoft SDK). */
    CHECK(selfhost_host("linux-amd64") && selfhost_host("darwin-arm64") && selfhost_host("darwin-amd64") && selfhost_host("windows-amd64"));
    CHECK(!selfhost_host("linux-arm64"));
    CHECK(!strcmp(selfhost_host("darwin-arm64")->cc_target, "aarch64-macos") && !strcmp(selfhost_host("darwin-arm64")->cpu_flag, "-mcpu"));
    Strs lb = sp_bridge_args(selfhost_host("linux-amd64"));
    CHECK(lb.n == bridge.n);
    for (int i = 0; i < lb.n && i < bridge.n; i++) CHECK(!strcmp(lb.v[i], bridge.v[i]));
    Strs db = sp_bridge_args(selfhost_host("darwin-arm64"));
    CHECK(db.n == 2 && !strcmp(db.v[0], "-isysroot") && !strcmp(db.v[1], "{sdk}"));
    Strs wb = sp_bridge_args(selfhost_host("windows-amd64"));
    CHECK(has(&wb, "{sdk}/crt/include") && has(&wb, "{sdk}/sdk/include/ucrt") && has(&wb, "-fms-runtime-lib=static") && has(&wb, "-D_ITERATOR_DEBUG_LEVEL=0") && has(&wb, "{root}/lib/include"));
    Strs wh = {0};
    sp_link_head(selfhost_host("windows-amd64"), &wh);
    CHECK(has(&wh, "{windows_path}") && has(&wh, "{bridge}") && !has(&wh, "-Wl,--start-group"));
    Strs dt = {0};
    sp_link_tail(selfhost_host("darwin-arm64"), &dt, "-lm -lz -lzstd -lxml2", "/pinned/zstd/lib/libzstd.a", NULL);
    CHECK(has(&dt, "/pinned/zstd/lib/libzstd.a") && !has(&dt, "-lzstd") && has(&dt, "-lc++") && has(&dt, "-L{sdk}/usr/lib") && has(&dt, "-lxml2"));
    Strs wt = {0};
    sp_link_tail(selfhost_host("windows-amd64"), &wt, "psapi.lib shell32.lib -lole32 xml2s.lib", NULL, "/x/libxml2s.lib");
    CHECK(has(&wt, "/x/libxml2s.lib") && !has(&wt, "-lxml2s"));
    CHECK(wt.n > 5 && !strcmp(wt.v[0], "-nodefaultlibs"));
    CHECK(!strcmp(wt.v[1], "{sdk}/crt/lib/x86_64/libcmt.lib"));
    CHECK(!strcmp(wt.v[2], "{sdk}/crt/lib/x86_64/libvcruntime.lib"));
    CHECK(!strcmp(wt.v[3], "{sdk}/crt/lib/x86_64/libcpmt.lib"));
    CHECK(!strcmp(wt.v[4], "{sdk}/sdk/lib/ucrt/x86_64/libucrt.lib"));
    CHECK(!has(&wt, "-nostdlib")); /* Preserve automatic debug UBSan. */
    CHECK(has(&wt, "-lkernel32")); /* Required even without LLVM system-libs. */
    CHECK(!has(&dt, "-nodefaultlibs") && !has(&ll, "-nodefaultlibs"));
    CHECK(has(&wt, "-lpsapi") && has(&wt, "-lshell32") && has(&wt, "-lole32") && has(&wt, "-L{sdk}/crt/lib/x86_64"));
    CHECK(child(self, "--darwin-nozstd", "x") != 0);
    CHECK(child(self, "--abs-syslib", "x") != 0);
    CHECK(sp_release_only("{object}") && sp_release_only("{windows_path}") && !sp_release_only("{llvm}/lib/libLLVMSupport.a"));
    CHECK(!strcmp(sp_expand("{root}/a:{sdk}/b:{llvm}/c", "R", "S", "L", "N"), "R/a:S/b:L/c"));
    char *why = NULL;
    char *apple = PJ(root, "MacOSX.sdk");
    CHECK(!sp_sdk_ok(selfhost_host("darwin-arm64"), apple, &why) && why);
    mkdirs(PJ(apple, "usr", "include", "c++", "v1"));
    write_file(PJ(apple, "SDKSettings.json"), "{}\n");
    CHECK(sp_sdk_ok(selfhost_host("darwin-arm64"), apple, &why));
    char *splat = PJ(root, "splat");
    CHECK(!sp_sdk_ok(selfhost_host("windows-amd64"), splat, &why) && contains(why, "license"));
    for (int i = 0; MSVC_INCLUDES[i]; i++) mkdirs(PJ(splat, MSVC_INCLUDES[i]));
    for (int i = 0; MSVC_LIBDIRS[i]; i++) mkdirs(PJ(splat, MSVC_LIBDIRS[i]));
    CHECK(sp_sdk_ok(selfhost_host("windows-amd64"), splat, &why));

    /* Generated darwin and windows recipes from the fake SDK. */
    write_file(config, "#!/bin/sh\ncase \"$2\" in --libnames) echo libLLVMSupport.a libLLVMDemangle.a;; --system-libs) echo -lm -lz psapi.lib;; esac\n");
    remove_file(PJ(llvm, "lib", "libclangBasic.a"));
    remove_file(PJ(llvm, "lib", "liblldCommon.a"));
    write_file(PJ(llvm, "lib", "clangBasic.lib"), "");
    write_file(PJ(llvm, "lib", "lldCommon.lib"), "");
    SelfhostCfg wc = {selfhost_host("windows-amd64"), llvm, splat, PJ(root, "wnative"), NULL, NULL, "x86-64-v2", NULL, 0};
    write_selfhost(&o, &wc, NULL, "/x/libxml2s.lib");
    Json *wj = json_load(selfhost_config(&o));
    CHECK(!strcmp(jstr(wj, "target"), "windows-amd64") && !strcmp(jstr(wj, "path-args"), "wnative/selfhost/bridge.args") && !*jstr_opt(wj, "linux-glibc"));
    Strs wl = split_lines(read_file(PJ(root, "wnative", "selfhost", "link.args"), NULL));
    CHECK(has(&wl, "{windows_path}") && has(&wl, "-lpsapi") && has(&wl, "{llvm}/lib/libLLVMSupport.a"));
    CHECK(has(&wl, "-nodefaultlibs") && has(&wl, "-lkernel32") && !has(&wl, "-nostdlib"));
    for (int i = 1; i <= 4; i++) CHECK(has(&wl, wt.v[i]));
    CHECK(has(&wl, "{llvm}/lib/clangBasic.lib") && has(&wl, "{llvm}/lib/lldCommon.lib"));
    CHECK(!has(&wl, "{llvm}/lib/libclangBasic.a") && !has(&wl, "-Wl,--start-group"));
    Strs we = {0};
    strs_add(&we, "INCLUDE=/poison"); strs_add(&we, "LIB=/poison");
    strs_add(&we, "CROSS_CC_MSVC=managed"); strs_add(&we, "ZIG_LIBC=/poison");
    char *libc = PJ(root, "windows-libc.txt");
    sp_msvc_env(&we, splat, libc);
    CHECK(has(&we, "CROSS_CC_MSVC=system") && has(&we, sfmt("ZIG_LIBC=%s", libc)));
    CHECK(!has(&we, "INCLUDE=/poison") && !has(&we, "LIB=/poison") && !has(&we, "CROSS_CC_MSVC=managed"));
    CHECK(contains(read_file(libc, NULL), sfmt("kernel32_lib_dir=%s", PJ(splat, "sdk", "lib", "um", "x86_64"))));
    write_file(PJ(llvm, "lib", "libclangBasic.a"), "");
    write_file(PJ(llvm, "lib", "liblldCommon.a"), "");
    SelfhostCfg dc = {selfhost_host("darwin-arm64"), llvm, apple, PJ(root, "dnative"), NULL, NULL, "generic", NULL, 0};
    char *zprefix = PJ(root, "apple-zstd");
    mkdirs(PJ(zprefix, "licenses"));
    write_file(PJ(zprefix, "licenses", "zstd-LICENSE"), "fixture notice\n");
    write_file(PJ(llvm, "lib", "libLTO.dylib"), "");
    write_selfhost(&o, &dc, PJ(zprefix, "lib", "libzstd.a"), NULL);
    Strs dl = split_lines(read_file(PJ(root, "dnative", "selfhost", "link.args"), NULL));
    CHECK(has(&dl, "{llvm}/lib/libclangBasic.a") && has(&dl, "{llvm}/lib/liblldCommon.a"));
    for (int i = 0; i < dl.n; i++) CHECK(!contains(dl.v[i], "lto_library") && !contains(dl.v[i], "libLTO"));
    Json *dj = json_load(selfhost_config(&o));
    CHECK(!strcmp(jstr(dj, "extra-licenses"), "apple-zstd/licenses"));
    CHECK(!strcmp(jstr(dj, "target"), "darwin-arm64") && !strcmp(jstr(dj, "cpu"), "generic") && !strcmp(jstr(dj, "sdk"), "MacOSX.sdk"));

    /* --host-config: complete recipes are read, mismatches refused. */
    char *hc = PJ(root, "host.json");
    mkdirs(PJ(llvm, "include", "llvm", "Config"));
    write_file(PJ(llvm, "include", "llvm", "Config", "llvm-config.h"), "#define LLVM_VERSION_MAJOR 22\n#define LLVM_VERSION_MINOR 1\n#define LLVM_VERSION_PATCH 8\n");
    write_file(hc, sfmt("{\"target\":\"%s\",\"llvm-prefix\":\".cache/native/llvm\",\"bridge-args\":\"wnative/selfhost/bridge.args\",\"link-args\":\"wnative/selfhost/link.args\",\"native-dir\":\"hcnative\"}", host_key()));
    if (selfhost_host(host_key())) {
        Opts ho = o;
        ho.host_config = hc;
        SelfhostCfg hcfg = {0};
        load_host_config(&ho, &hcfg);
        CHECK(!strcmp(hcfg.native, PJ(root, "hcnative")) && !strcmp(hcfg.llvm, llvm) && !hcfg.auto_linux && !strcmp(hcfg.cpu, hcfg.h->cpu));
        CHECK(!strcmp(selfhost_config(&ho), hc));
        CHECK(child(self, "--host-config", hc) != 0); /* child root is cwd: recipe paths missing */
    }
    write_file(hc, "{\"target\":\"no-such-host\",\"llvm-prefix\":\"x\",\"bridge-args\":\"x\",\"link-args\":\"x\"}");
    CHECK(child(self, "--host-config", hc) != 0);
    write_file(hc, sfmt("{\"target\":\"%s\"}", host_key()));
    CHECK(child(self, "--host-config", hc) != 0);

    if (failures) return 1;
    remove_all(root);
    puts("selfhost prepare tests passed (offline)");
    return 0;
#endif
}
