/* Offline pipeline regression: this executable also acts as a deterministic
 * mock cross_cc seed, raw Clang/ar aliases, and packaging helper. No SDK/network
 * or production system compiler is used. Compile the TEST HARNESS with cross_cc:
 * cross_cc cc -std=c11 -O0 -o build/tests/selfhost_pipeline_test tests/selfhost_pipeline_test.c
 *
 * Only the Windows source-acquisition boundary is replaced below. The real
 * helper (including its authentication checks) is compiled unchanged first;
 * release.c then calls the mock instead. This tests pipeline orchestration,
 * not authentication/compilation of actual LLVM sources. */
#include "../tools/windows_path/windows_path.c"
static void mock_wp_prepare(const char *, const char *, const char *, const char *, char **, char **);
#define wp_prepare mock_wp_prepare
#define main cross_cc_release_main
#include "../tools/release.c"
#undef main
#undef wp_prepare

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

static void put(const char *path, const char *text) {
    mkdirs(path_dir(path));
    write_file(path, text);
}

/* On Windows '\\' and '/' are the same separator; every other byte (case,
 * repeated separators) must match exactly. `windows` lets Linux test the rule. */
static int same_path_rule(const char *a, const char *b, int windows) {
    for (; *a && *b; a++, b++) {
        int sep = windows && (*a == '/' || *a == '\\') && (*b == '/' || *b == '\\');
        if (*a != *b && !sep) return 0;
    }
    return *a == *b;
}
#ifdef _WIN32
#define same_fs_path(a, b) same_path_rule((a), (b), 1)
#else
#define same_fs_path(a, b) same_path_rule((a), (b), 0)
#endif

#define MOCK_LLVM_ARCHIVE "mock LLVM source archive\n"
static void mock_wp_prepare(const char *archive, const char *expected, const char *strip_prefix, const char *dir, char **source, char **gen) {
    const char *root = env_get("SELFHOST_MOCK_ROOT");
    /* Assert the acquisition call is bound to the fixture's cached input. No
     * real source hash or authentication function is substituted globally. */
    if (!same_fs_path(archive, PJ(root, ".cache", "downloads", "mock-llvm.tar"))) fail("unexpected mock LLVM archive path: %s (expected under %s)", archive, root);
    if (strcmp(expected, sha256_bytes(MOCK_LLVM_ARCHIVE, strlen(MOCK_LLVM_ARCHIVE)))) fail("unexpected mock LLVM expected hash: %s", expected);
    if (strcmp(sha256_file(archive), expected)) fail("mock LLVM archive content hash differs from expected %s", expected);
    if (strcmp(strip_prefix, "mock-llvm")) fail("unexpected mock LLVM strip prefix: %s", strip_prefix);
    if (exists(dir)) fail("input stage already exists: %s", dir);
    *source = PJ(dir, "llvm", "lib", "Support", "Path.cpp");
    *gen = PJ(dir, "gen");
    put(*source, "mock LLVM Path.cpp\n");
    put(PJ(dir, "llvm", "lib", "Support", "Windows", "Path.inc"), "mock Windows Path.inc\n");
    put(PJ(dir, "llvm", "LICENSE.TXT"), "mock LLVM license\n");
    put(PJ(*gen, "llvm", "Config", "config.h"), WP_CONFIG);
    put(PJ(dir, "inputs.txt"), sfmt("recipe mock-llvm-windows-forward-path-v1\narchive-sha256 %s\nsource-sha256 %s\nwindows-path-inc-sha256 %s\nconfig-sha256 %s\noption LLVM_WINDOWS_PREFER_FORWARD_SLASH=1\n", expected, sha256_file(*source), sha256_file(PJ(dir, "llvm", "lib", "Support", "Windows", "Path.inc")), sha256_file(PJ(*gen, "llvm", "Config", "config.h"))));
}

static const char *argument(int argc, char **argv, const char *key) {
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], key)) return argv[i + 1];
    return "";
}
static int has_argument(int argc, char **argv, const char *key) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], key)) return 1;
    return 0;
}

static int mock_package(int argc, char **argv) {
    const char *output = "", *binary = "", *target = "";
    for (int i = 1; i < argc; i++) {
        if (starts_with(argv[i], "--output=")) output = argv[i] + 9;
        if (starts_with(argv[i], "--binary=")) binary = argv[i] + 9;
        if (starts_with(argv[i], "--target=")) target = argv[i] + 9;
    }
    if (!*output || !*binary || !*target) fail("mock package missing arguments");
    mkdirs(output);
    copy_file(PJ(output, starts_with(target, "windows-") ? "cross_cc.exe" : "cross_cc"), binary);
    put(PJ(output, "lib", "fixture.h"), "mock resources\n");
    return 0;
}

static int mock_compiler(int argc, char **argv) {
    const char *root = env_get("SELFHOST_MOCK_ROOT");
    const char *name = path_base(argv[0]);
    int ar = !strcmp(name, "llvm-ar" EXE_SUFFIX);
    int compile = has_argument(argc, argv, "-c");
    const char *out = ar ? (argc > 2 ? argv[2] : "") : argument(argc, argv, "-o");
    if (!*out) fail("mock compiler missing output");
    int seed_stage = contains(out, "/host-stage1.stage/");
    const char *expected = seed_stage ? PJ(root, "seed") : root;
    if (strcmp(env_get("CROSS_CC_ROOT"), expected)) fail("mock resource isolation: expected %s, got %s", expected, env_get("CROSS_CC_ROOT"));
    FILE *log = fopen(PJ(root, "commands.log"), "ab");
    if (!log) fail("mock log");
    fprintf(log, "%s %s %s resource-root=%s\n", seed_stage ? "seed" : "current", ar ? "ar" : compile ? "compile" : "link", out, expected);
    if (compile) {
        const char *last_opt = "";
        for (int i = 1; i < argc; i++) if (starts_with(argv[i], "-O")) last_opt = argv[i];
        int debug = !strcmp(env_get("SELFHOST_PROFILE"), "debug");
        if (strcmp(last_opt, debug ? "-O0" : "-O2")) fail("wrong compile profile for %s: %s", out, last_opt);
        int path = ends_with(out, "llvm-WindowsPath.cpp.o");
        if (debug && (!has_argument(argc, argv, "-g") || (!path && has_argument(argc, argv, "-DNDEBUG")))) fail("debug flags missing/contradictory");
        if (path) {
            for (int i = 0; wp_flags[i]; i++) if (!has_argument(argc, argv, wp_flags[i])) fail("missing Path.cpp flag %s", wp_flags[i]);
            if (!has_argument(argc, argv, "Path.cpp") || !is_file("Path.cpp")) fail("Path.cpp compile cwd/input not isolated");
            fprintf(log, "%s windows-path-profile %s\n", seed_stage ? "seed" : "current", last_opt);
        }
        int cpp = !strcmp(name, "cross_clang" EXE_SUFFIX);
        fprintf(log, "%s-profile %s\n", cpp ? "c++" : "c", last_opt);
        Buf object = {0};
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-o")) { i++; continue; }
            buf_addf(&object, "%s\n", argv[i]);
        }
        /* Changing only one C++ object must fail even if the mock link produces
         * identical executable bytes. The second pass uses the same paths. */
        if (*env_get("SELFHOST_FAIL_FRESH") && !seed_stage && cpp && ends_with(out, "bridge.cpp.o") && exists(out)) buf_str(&object, "nonrepeatable C++ object\n");
        write_file(out, buf_cstr(&object));
    } else if (ar) {
        Buf archive = {0};
        for (int i = 3; i < argc; i++) buf_str(&archive, read_file(argv[i], NULL));
        write_file(out, buf_cstr(&archive));
    } else {
        /* This is a copy, not an in-place write of any executing alias. */
        if (exists(out)) fail("link output was not unlinked before rebuild");
        copy_file(out, argv[0]);
        make_executable(out);
    }
    fclose(log);
    return 0;
}

static char *fixture(const char *base, const char *label, const char *self) {
    char *root = PJ(base, label);
    if (exists(root)) fail("fixture already exists %s", root);
    mkdirs(PJ(root, "tools"));
    copy_file(PJ(root, "tools", "release" EXE_SUFFIX), self);
    copy_file(PJ(root, "tools", "build" EXE_SUFFIX), self);
    mkdirs(PJ(root, "seed", "bin"));
    copy_file(PJ(root, "seed", "bin", "cross_cc" EXE_SUFFIX), self);
    mkdirs(PJ(root, "seed", "lib", "include"));
    mkdirs(PJ(root, "seed", "lib", "libc"));
    put(PJ(root, "seed", "upstream.lock.json"), "{}\n");
    put(PJ(root, "toolchain.lock.json"), "{\"native\":{\"sources\":[\"bridge.cpp\"]}}\n");
    put(PJ(root, "upstream.lock.json"), "{}\n");
    put(PJ(root, ".cache", "downloads", "mock-llvm.tar"), MOCK_LLVM_ARCHIVE);
    put(PJ(root, "compiler-rt.lock.json"), sfmt("{\"url\":\"https://invalid.example/mock-llvm.tar\",\"sha256\":\"%s\",\"strip_prefix\":\"mock-llvm\"}\n", sha256_bytes(MOCK_LLVM_ARCHIVE, strlen(MOCK_LLVM_ARCHIVE))));
    put(PJ(root, "tools", "windows_path", "windows_path.c"), "mock Path.cpp provenance input\n");
    put(PJ(root, "main.c"), "mock driver\n");
    put(PJ(root, "src", "driver.h"), "mock header\n");
    const char *tools[] = {"release.c", "build.c", "common.h", "linux-release-deps.c", "macos-x64-llvm.c", NULL};
    for (int i = 0; tools[i]; i++) put(PJ(root, "tools", tools[i]), "mock provenance input\n");
    put(PJ(root, "deps", "llvm-bridge", "bridge.cpp"), "mock bridge\n");
    put(PJ(root, "llvm", "include", "llvm", "Config", "llvm-config.h"), "#define LLVM_VERSION_MAJOR 22\n#define LLVM_VERSION_MINOR 1\n#define LLVM_VERSION_PATCH 8\n");
    mkdirs(PJ(root, "llvm", "lib"));
    const char *native = PJ(root, "native");
    put(PJ(native, "version", "clang", "lib", "Basic", "Version.cpp"), "mock clang version\n");
    put(PJ(native, "version", "lld", "Common", "Version.cpp"), "mock lld version\n");
    put(PJ(native, "version", "lld-gen", "lld", "Common", "Version.inc"), "mock lld version include\n");
    put(PJ(native, "version", "lld-gen", "llvm", "Support", "VCSRevision.h"), "");
    put(PJ(native, "version", "lld-gen", "VCSVersion.inc"), "");
    const char *d = PJ(native, "dsymutil");
    put(PJ(d, "inputs.txt"), "mock pinned source receipt\n");
    put(PJ(d, "gen", "Options.inc"), "mock options\n");
    put(PJ(d, "gen", "llvm", "Config", "config.h"), "mock config\n");
    put(PJ(d, "gen", "CFBundle.cpp"), "mock bundle\n");
    put(PJ(d, "gen", "driver.cpp"), "mock driver\n");
    for (int i = 0; DSYM_FILES[i]; i++) put(PJ(d, "llvm", "tools", "dsymutil", DSYM_FILES[i]), "mock dsymutil\n");
    /* Deliberately no native/libcross_cc_backend.a or linker-flags.txt. */
    put(PJ(root, "recipes", "bridge.args"), "-O3\n-fPIC\n");
    put(PJ(root, "recipes", "link.args"), "{object}\n{version}\n{lld_version}\n{bridge}\n");
    put(PJ(root, "recipes", "windows-link.args"), "{object}\n{version}\n{lld_version}\n{windows_path}\n{bridge}\n");
    put(PJ(root, "config", "host.json"), sfmt("{\"target\":\"%s\",\"llvm-prefix\":\"llvm\",\"native-dir\":\"native\",\"bridge-args\":\"recipes/bridge.args\",\"link-args\":\"recipes/%s\",\"linux-glibc\":\"2.34\"}\n", host(), starts_with(host(), "windows-") ? "windows-link.args" : "link.args"));
    return root;
}

static int invoke(const char *root, const char *profile, const char *bad, const Strs *extra, char **output) {
    Strs args = {0};
    strs_add(&args, PJ(root, "tools", "release" EXE_SUFFIX));
    strs_addf(&args, "--compiler=%s", PJ(root, "seed", "bin", "cross_cc" EXE_SUFFIX));
    strs_addf(&args, "--host-config=%s", PJ(root, "config", "host.json"));
    strs_addf(&args, "--profile=%s", profile);
    if (extra) strs_cat(&args, extra);
    const char *drop[] = {"CROSS_CC_SEED_ROOT", "SELFHOST_FAIL_FRESH", "SELFHOST_MOCK_ROOT", "SELFHOST_PROFILE", "CROSS_CC_ROOT", "CC", "CXX", NULL};
    Strs additions = {0};
    strs_addf(&additions, "SELFHOST_MOCK_ROOT=%s", root);
    strs_addf(&additions, "SELFHOST_PROFILE=%s", profile);
    strs_addf(&additions, "SELFHOST_FAIL_FRESH=%s", bad);
    strs_add(&additions, "CROSS_CC_ROOT=/poisoned-parent-root");
    strs_add(&additions, "CC=__forbidden_system_cc__");
    strs_add(&additions, "CXX=__forbidden_system_cxx__");
    Strs env = make_env(drop, &additions);
    return run_capture(args.v, root, env.v, output);
}

static void native_test(const char *base, const char *self, const char *profile, int bad) {
    char *root = fixture(base, bad ? "nonrepeatable" : profile, self);
    char *binary = PJ(root, "bin", "cross_cc" EXE_SUFFIX);
    Strs extra = {0};
    strs_addf(&extra, "--build-only=%s", binary);
    char *out = NULL;
    int code = invoke(root, profile, bad ? "1" : "", &extra, &out);
    if (bad) {
        CHECK(code != 0);
        CHECK(!exists(binary));
        CHECK(!exists(sfmt("%s.build-manifest.txt", binary)));
        CHECK(exists(PJ(root, "build", "release", "host-stage1.stage")));
    } else {
        CHECK(code == 0);
        if (code) fprintf(stderr, "%s\n", out);
        CHECK(is_file(binary));
        char *receipt = read_file(sfmt("%s.build-manifest.txt", binary), NULL);
        CHECK(contains(receipt, "verification native stage2/stage3 convergence"));
        CHECK(contains(receipt, "artifact "));
        CHECK(contains(receipt, "bridge.cpp.o"));
        CHECK(contains(receipt, "prebuilt-bridge false"));
        CHECK(contains(receipt, "host-stage1 seed-resource-lock "));
        CHECK(contains(receipt, "host-stage1 bridge-source "));
        CHECK(!exists(PJ(root, "dist")));
        CHECK(!exists(PJ(root, "build", "release", "host-stage1.stage")));
        char *log = read_file(PJ(root, "commands.log"), NULL);
        CHECK(contains(log, "seed compile"));
        CHECK(contains(log, "current compile"));
        CHECK(contains(log, !strcmp(profile, "debug") ? "c++-profile -O0" : "c++-profile -O2"));
        CHECK(contains(log, !strcmp(profile, "debug") ? "c-profile -O0" : "c-profile -O2"));
        /* Existing normal-build output may be atomically replaced on rebuild. */
        CHECK(invoke(root, profile, "", &extra, &out) == 0);
        Strs replace_seed = {0};
        strs_addf(&replace_seed, "--build-only=%s", PJ(root, "seed", "bin", "cross_cc" EXE_SUFFIX));
        CHECK(invoke(root, profile, "", &replace_seed, &out) == 0);
    }
}

static void cross_and_matrix_test(const char *base, const char *self) {
    char *root = fixture(base, "cross", self);
    /* Exercise a non-host target without executing it, including the fourth
     * advertised target darwin-amd64 previously excluded from matrix parsing. */
    const char *target = !strcmp(host(), "darwin-amd64") ? "linux-amd64" : "darwin-amd64";
    Strs extra = {0};
    strs_addf(&extra, "--target=%s", target);
    strs_add(&extra, "--llvm-prefix=llvm");
    strs_add(&extra, "--bridge-args=recipes/bridge.args");
    strs_add(&extra, "--link-args=recipes/link.args");
    strs_add(&extra, "--native-dir=native");
    strs_add(&extra, "--output=dist/cross");
    char *out = NULL;
    CHECK(invoke(root, "release", "", &extra, &out) == 0);
    char *receipt = read_file(PJ(root, "dist", "cross.build-manifest.txt"), NULL);
    CHECK(contains(receipt, "cross same-input repeatability using stage1; target not executed; NOT native convergence"));
    CHECK(exists(PJ(root, "dist", "cross.zip")));
    CHECK(exists(PJ(root, "dist", "cross.zip.sha256")));
    Opts common = default_options(), targets[4];
    common.root = root;
    char *matrix = PJ(root, "matrix.json");
    put(matrix, "{\"targets\":[{\"target\":\"linux-amd64\"},{\"target\":\"darwin-arm64\"},{\"target\":\"darwin-amd64\"},{\"target\":\"windows-amd64\"}]}\n");
    CHECK(matrix_targets(&common, matrix, targets) == 4);
    CHECK(!strcmp(targets[2].target, "darwin-amd64"));
    put(matrix, "{\"targets\":[{\"target\":\"darwin-amd64\"}]}\n");
    CHECK(matrix_targets(&common, matrix, targets) == 1);
    put(matrix, sfmt("{\"targets\":[{\"target\":\"%s\",\"llvm-prefix\":\"llvm\",\"native-dir\":\"native\",\"bridge-args\":\"recipes/bridge.args\",\"link-args\":\"recipes/link.args\",\"output\":\"dist/matrix\"}]}\n", target));
    Strs matrix_args = {0};
    strs_addf(&matrix_args, "--matrix=%s", matrix);
    CHECK(invoke(root, "release", "", &matrix_args, &out) == 0);
    CHECK(exists(PJ(root, "dist", "matrix.zip")));
    /* Exercise the Windows Path orchestration on every host, not just Windows.
     * Debug Path deliberately retains wp_flags' NDEBUG/static-CRT policy. */
    put(matrix, "{\"targets\":[{\"target\":\"windows-amd64\",\"llvm-prefix\":\"llvm\",\"native-dir\":\"native\",\"bridge-args\":\"recipes/bridge.args\",\"link-args\":\"recipes/windows-link.args\",\"output\":\"dist/windows\"}]}\n");
    int code = invoke(root, "debug", "", &matrix_args, &out);
    CHECK(code == 0);
    if (code) { fprintf(stderr, "%s\n", out); return; }
    receipt = read_file(PJ(root, "dist", "windows.build-manifest.txt"), NULL);
    CHECK(contains(receipt, "windows-canonical-paths true"));
    CHECK(contains(receipt, "recipe mock-llvm-windows-forward-path-v1"));
    CHECK(contains(receipt, "artifact ") && contains(receipt, "llvm-WindowsPath.cpp.o"));
    CHECK(contains(receipt, "windows-path-input "));
    CHECK(exists(PJ(root, "dist", "windows.zip")));
    CHECK(exists(PJ(root, "dist", "windows.zip.sha256")));
    CHECK(is_file(PJ(root, "dist", "windows", "cross_cc.exe")));
    char *path_arg = strstr(receipt, sfmt("link-arg %s", PJ(root, "build", "release", "windows-amd64.stage", "llvm-WindowsPath.cpp.o")));
    char *bridge_arg = strstr(receipt, sfmt("link-arg %s", PJ(root, "build", "release", "windows-amd64.stage", "libcross_cc_backend.a")));
    CHECK(path_arg && bridge_arg && path_arg < bridge_arg);
    Strs commands = split_lines(read_file(PJ(root, "commands.log"), NULL));
    int path_compiles = 0;
    for (int i = 0; i < commands.n; i++) if (!strcmp(commands.v[i], "current windows-path-profile -O0")) path_compiles++;
    CHECK(path_compiles == 4); /* twice in each of stage2 and stage3 */
    char *bad_windows = replace_all(read_file(matrix, NULL), "dist/windows", "dist/windows-bad");
    put(matrix, "{\"targets\":[{\"target\":\"linux-amd64\"},{\"target\":\"linux-amd64\"}]}\n");
    CHECK(invoke(root, "release", "", &matrix_args, &out) != 0);
    put(matrix, bad_windows);
    CHECK(invoke(root, "debug", "1", &matrix_args, &out) != 0);
    CHECK(contains(read_file(PJ(root, "build", "release", "windows-amd64.stage", "bridge.cpp.o"), NULL), "nonrepeatable C++ object"));
    CHECK(!exists(PJ(root, "dist", "windows-bad.build-manifest.txt")));
    CHECK(!exists(PJ(root, "dist", "windows-bad.zip")));
    /* A retained failed stage must still be rejected, never silently reset. */
    CHECK(invoke(root, "debug", "", &matrix_args, &out) != 0);
}

int main(int argc, char **argv) {
    if (*env_get("SELFHOST_MOCK_ROOT")) {
        const char *name = path_base(argv[0]);
        if (!strcmp(name, "release" EXE_SUFFIX)) return cross_cc_release_main(argc, argv);
        if (!strcmp(name, "build" EXE_SUFFIX)) return mock_package(argc, argv);
        return mock_compiler(argc, argv);
    }
    tool_name = "selfhost_pipeline_test";
    /* Windows spelling rules (checked on every host) and exact POSIX rules. */
    CHECK(same_path_rule("C:\\Work\\.cache\\downloads\\mock-llvm.tar", "C:/Work/.cache/downloads/mock-llvm.tar", 1));
    CHECK(!same_path_rule("C:\\Work\\.cache\\downloads\\other.tar", "C:/Work/.cache/downloads/mock-llvm.tar", 1));
    CHECK(!same_path_rule("C:\\Work\\.cache\\downloads\\mock-llvm.tar", "C:/work/.cache/downloads/mock-llvm.tar", 1));
    CHECK(!same_path_rule("C:/Work/.cache//downloads/mock-llvm.tar", "C:/Work/.cache/downloads/mock-llvm.tar", 1));
    CHECK(!same_path_rule("C:/Work/a", "C:/Work/a/", 1));
    CHECK(!same_path_rule("/a/b", "/A/b", 0));
    CHECK(!same_path_rule("a\\b", "a/b", 0));
    char *self = abs_path(argv[0]);
    char *base = abs_path(sfmt("build/tests/selfhost-fixture-%lu", (unsigned long)(size_t)&failures));
    CHECK(!strcmp(default_options().linux_glibc, "2.34"));
    Opts windows = default_options();
    windows.target = "windows-amd64"; windows.root = base;
    windows.sdk = PJ(base, "accepted-sdk"); windows.stage = PJ(base, "windows-stage");
    Strs windows_env = environment(&windows);
    int system = 0, bound = 0;
    for (int i = 0; i < windows_env.n; i++) {
        if (!strcmp(windows_env.v[i], "CROSS_CC_MSVC=system")) system++;
        if (!strcmp(windows_env.v[i], sfmt("ZIG_LIBC=%s", PJ(windows.stage, "msvc-libc.txt")))) bound++;
    }
    CHECK(system == 1 && bound == 1);
    CHECK(is_file(PJ(windows.stage, "msvc-libc.txt")));
    Opts defaults = default_options(), notices = default_options();
    defaults.extra_licenses = "host-notices"; defaults.msvc_helper = "host-xwin";
    notices = native_defaults(notices, &defaults);
    CHECK(!strcmp(notices.extra_licenses, "host-notices") && !strcmp(notices.msvc_helper, "host-xwin"));
    native_test(base, self, "debug", 0);
    native_test(base, self, "release", 0);
    native_test(base, self, "release", 1);
    cross_and_matrix_test(base, self);
    remove_all(base);
    if (failures) fprintf(stderr, "%d selfhost pipeline failures\n", failures);
    else puts("selfhost pipeline tests passed");
    return failures ? 1 : 0;
}
