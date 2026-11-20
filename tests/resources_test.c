#ifndef _WIN32
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#endif
/* Unit tests for the resource compiler, resource VFS, dependency rewriting,
   linker-flag normalization and deterministic staging (src/rc.c,
   src/resource_vfs.c, src/linker_flags.c, src/determinism.c). Unity build of
   the driver sources; only the LLVM backend entry points are test doubles.
   Directory spelling and resource prefix-map ordering are covered by
   tests/core_test.c. */
#include "../src/ar.c"
#include "../src/arguments.c"
#include "../src/backend.c"
#include "../src/bsd.c"
#include "../src/builtins.c"
#include "../src/builtins_memory.c"
#include "../src/builtins_recipes.c"
#include "../src/determinism.c"
#include "../src/freestanding.c"
#include "../src/glibc.c"
#include "../src/libc_config.c"
#include "../src/library_names.c"
#include "../src/linker_flags.c"
#include "../src/macos.c"
#include "../src/main.c"
#include "../src/mingw.c"
#include "../src/mingw_recipes.c"
#include "../src/msvc.c"
#include "../src/msvc_libcxx.c"
#include "../src/msvc_link.c"
#include "../src/msvc_managed.c"
#include "../src/msvc_other.c"
#include "../src/msvc_windows.c"
#include "../src/objcopy.c"
#include "../src/profile.c"
#include "../src/rc.c"
#include "../src/resource_vfs.c"
#include "../src/runtime.c"
#include "../src/runtime_musl_supplement.c"
#include "../src/runtime_recipes.c"
#include "../src/sparc_link.c"
#include "../src/sparc_tls.c"
#include "../src/support.c"
#include "../src/target.c"
#include "../src/toolchain.c"
#include "../src/tsan.c"
#include "../src/ubsan.c"
#include "../src/wasi.c"
#include "../src/wasi_recipes.c"
#include "../src/wasi_supplement.c"
#include <assert.h>

/* Backend entry points (LLVM is not linked into unit tests). */
int BridgeClang_main(int argc, char **argv) { (void)argc; (void)argv; return 1; }
int BridgeLlvmAr_main(int argc, char **argv) { (void)argc; (void)argv; return 1; }
int cross_cc_dsymutil_main(int argc, char **argv) { (void)argc; (void)argv; return 1; }
bool BridgeLLDLinkMinGW(int argc, const char **argv, bool a, bool b) { (void)argc; (void)argv; (void)a; (void)b; return false; }
bool BridgeLLDLinkELF(int argc, const char **argv, bool a, bool b) { (void)argc; (void)argv; (void)a; (void)b; return false; }
bool BridgeLLDLinkCOFF(int argc, const char **argv, bool a, bool b) { (void)argc; (void)argv; (void)a; (void)b; return false; }
bool BridgeLLDLinkWasm(int argc, const char **argv, bool a, bool b) { (void)argc; (void)argv; (void)a; (void)b; return false; }
bool BridgeLLDLinkMachO(int argc, const char **argv, bool a, bool b) { (void)argc; (void)argv; (void)a; (void)b; return false; }

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "%s:%d: CHECK failed: %s ", __FILE__, __LINE__, #cond); fprintf(stderr, " " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define CHECK_STR(got, want) do { const char *g_ = (got), *w_ = (want); if (!g_ || !w_ || strcmp(g_, w_) != 0) { failures++; fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, g_ ? g_ : "(null)", w_ ? w_ : "(null)"); } } while (0)

static const char *err_of(char *e) { return e ? e : ""; }
static long index_of(StrVec v, const char *want) { for (size_t i = 0; i < v.len; i++) if (streq(v.data[i], want)) return (long)i; return -1; }
static char *joined(StrVec v) { Bytes b = {0}; for (size_t i = 0; i < v.len; i++) { if (i) bytes_push(&b, ' '); bytes_append(&b, v.data[i], strlen(v.data[i])); } bytes_push(&b, 0); return (char *)b.data; }
static Options args_of(StrVec v, bool cxx) { char *e = NULL; Options o = parse_arguments(v, cxx, &e); return o; }

/* ---- rc ---- */
static void test_rc_msvc_env_override(void) {
    char *e = NULL;
    RC_Options o = rc_parse_options_msvc_env(SV("a.rc"), "", &e); CHECK_STR(err_of(e), ""); CHECK(!o.msvc_system);
    e = NULL; o = rc_parse_options_msvc_env(SV("a.rc"), "managed", &e); CHECK_STR(err_of(e), ""); CHECK(!o.msvc_system);
    e = NULL; o = rc_parse_options_msvc_env(SV("a.rc"), "system", &e); CHECK_STR(err_of(e), ""); CHECK(o.msvc_system);
    e = NULL; o = rc_parse_options_msvc_env(SV("--cross-cc-msvc=managed", "a.rc"), "system", &e); CHECK_STR(err_of(e), ""); CHECK(!o.msvc_system);
    e = NULL; o = rc_parse_options_msvc_env(SV("--cross-cc-msvc=system", "a.rc"), "managed", &e); CHECK_STR(err_of(e), ""); CHECK(o.msvc_system);
    e = NULL; o = rc_parse_options_msvc_env(SV("--cross-cc-msvc=system", "a.rc"), "bogus", &e); CHECK_STR(err_of(e), ""); CHECK(o.msvc_system);
    e = NULL; rc_parse_options_msvc_env(SV("a.rc"), "bogus", &e); CHECK(*err_of(e));
    e = NULL; rc_parse_options_msvc_env(SV("a.rc"), "System", &e); CHECK(*err_of(e));
    /* The environment never accepts the license. */
    e = NULL; o = rc_parse_options_msvc_env(SV("a.rc"), "system", &e); CHECK(!o.msvc_accept_license);
}

/* Fuzz: rc_key_less orders .res keys the way the COFF resource directory needs,
   checked against a separate comparator. Named keys sort before ordinals,
   ordinals by value, names by UTF-16 code unit with a shorter prefix first. */
static uint64_t rng_state = 0x6f64696eULL;
static unsigned rng(unsigned n) { rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL; return (unsigned)((rng_state >> 33) % n); }
static RC_Coff_Key random_key(void) {
    RC_Coff_Key key; memset(&key, 0, sizeof key);
    key.named = rng(2) == 0;
    if (!key.named) { key.ordinal = (uint16_t)rng(4); return key; }
    static const uint16_t units[] = {'A', 'B', 0x00e9, 0xff00};
    size_t n = 2 * rng(4);
    unsigned char *name = xmalloc(n + 1);
    for (size_t i = 0; i < n; i += 2) { uint16_t u = units[rng(4)]; name[i] = (unsigned char)u; name[i + 1] = (unsigned char)(u >> 8); }
    key.name = name; key.name_len = n;
    return key;
}
static bool reference_less(RC_Coff_Key a, RC_Coff_Key b) {
    if (a.named && !b.named) return true;
    if (!a.named && b.named) return false;
    if (!a.named) return a.ordinal < b.ordinal;
    size_t n = a.name_len < b.name_len ? a.name_len : b.name_len;
    for (size_t i = 0; i < n; i += 2) {
        uint16_t x = (uint16_t)(a.name[i] | a.name[i + 1] << 8), y = (uint16_t)(b.name[i] | b.name[i + 1] << 8);
        if (x != y) return x < y;
    }
    return a.name_len < b.name_len;
}
static void test_rc_key_order_fuzz(void) {
    for (int i = 0; i < 20000; i++) {
        RC_Coff_Key a = random_key(), b = random_key();
        CHECK(rc_key_less(a, b) == reference_less(a, b));
        CHECK(!(rc_key_less(a, b) && rc_key_less(b, a)), "rc_key_less is not asymmetric");
        if (rc_key_equal(a, b)) CHECK(!rc_key_less(a, b), "rc_key_less holds for equal keys");
    }
}

/* ---- linker flags ---- */
static void lf_case(const char *triple, StrVec args, StrVec want) {
    char *pe = NULL, *te = NULL, *err = NULL;
    Options o = parse_arguments(args, false, &pe);
    CHECK_STR(err_of(pe), "");
    Target target = resolve_target(triple, &te);
    CHECK_STR(err_of(te), "");
    Options got = normalize_linker_flags(o, target, &err);
    CHECK_STR(err_of(err), "");
    CHECK(got.forwarded.len == want.len, "%s: %s -> %s", triple, joined(args), joined(got.forwarded));
    for (size_t i = 0; i < want.len && i < got.forwarded.len; i++) CHECK_STR(got.forwarded.data[i], want.data[i]);
}
static void test_linker_flags_cross_format(void) {
    StrVec common = SV("main.c", "-Wl,--version-script=missing.map,--as-needed,--no-as-needed,-z,relro,--no-undefined,--start-group,--end-group,--hash-style=gnu,--disable-new-dtags,--eh-frame-hdr");
    { const char *triples[] = {"aarch64-macos", "x86_64-windows-gnu", "wasm32-wasi"}; for (size_t k = 0; k < 3; k++) { const char *triple = triples[k];
        lf_case(triple, common, SV("main.c"));
    } }
    lf_case("aarch64-macos", SV("main.c", "-Wl,--gc-sections,--export-dynamic"),
        SV("main.c", "-Xlinker", "-export_dynamic", "-Xlinker", "-dead_strip"));
    lf_case("aarch64-macos", SV("main.c", "-Wl,--gc-sections,--no-gc-sections"), SV("main.c"));
    lf_case("x86_64-linux-musl", SV("main.c", "-Wl,-dead_strip,--version-script=some.map,-z,now"),
        SV("main.c", "-Xlinker", "--version-script", "-Xlinker", "some.map", "-Xlinker", "-z", "-Xlinker", "now", "-Xlinker", "--gc-sections"));
    lf_case("x86_64-windows-gnu", SV("main.c", "-Wl,--export-dynamic,-z,stack-size=1048576"),
        SV("main.c", "-Xlinker", "--export-all-symbols", "-Xlinker", "--stack", "-Xlinker", "1048576"));
    lf_case("wasm32-wasi", SV("main.c", "-Wl,--stack,1048576,--export-dynamic"),
        SV("main.c", "-Xlinker", "-z", "-Xlinker", "stack-size=1048576", "-Xlinker", "--export-dynamic"));
}
static void test_linker_flags_operands_and_transports(void) {
    lf_case("aarch64-macos", SV("main.c", "-Wl,--no-as-needed", "-lfoo", "-Xlinker", "--as-needed", "-lbar"),
        SV("main.c", "-Xlinker", "-needed-lfoo", "-l", "bar"));
    lf_case("aarch64-macos", SV("-Wl,-zrelro", "main.c"), SV("main.c"));
    lf_case("aarch64-macos", SV("-Wl,--version-script", "main.c", "-Xlinker", "map with,comma.map", "-Xlinker", "-rpath=/lib,space"),
        SV("main.c", "-Xlinker", "-rpath", "-Xlinker", "/lib,space"));
    lf_case("aarch64-macos", SV("-include", "-Wl,--gc-sections", "main.c", "--", "-Wl,--bad"),
        SV("-include", "-Wl,--gc-sections", "main.c", "--", "-Wl,--bad"));
    lf_case("aarch64-macos", SV("main.c", "-Wl,--gc-sections", "--", "input.o"),
        SV("main.c", "-Xlinker", "-dead_strip", "--", "input.o"));
    lf_case("aarch64-macos", SV("-c", "main.c", "-Wl,--gc-sections,--version-script=missing"), SV("-c", "main.c"));
    lf_case("aarch64-macos", SV("-c", "main.c", "-Wl,-s"), SV("-c", "main.c"));
    lf_case("x86_64-linux-musl", SV("main.c", "-Wl,--no-as-needed", "-lfoo"),
        SV("main.c", "-Xlinker", "--no-as-needed", "-lfoo", "-Xlinker", "--as-needed"));
    lf_case("x86_64-linux-musl", SV("main.c", "-Wl,--whole-archive", "a.a", "-Wl,--no-whole-archive", "b.a"),
        SV("main.c", "-Xlinker", "--whole-archive", "a.a", "-Xlinker", "--no-whole-archive", "b.a"));
    lf_case("aarch64-macos", SV("main.c", "-Wl,--whole-archive", "a.a", "-Wl,--no-whole-archive", "b.a"),
        SV("main.c", "-Xlinker", "-force_load", "-Xlinker", "a.a", "b.a"));
    // Real format-specific flags and joined path operands are not discarded.
    lf_case("aarch64-macos", SV("main.c", "-Wl,-headerpad_max_install_names,-install_name,/usr/lib/libx.dylib"),
        SV("main.c", "-Xlinker", "-headerpad_max_install_names", "-Xlinker", "-install_name", "-Xlinker", "/usr/lib/libx.dylib"));
    lf_case("x86_64-windows-msvc", SV("main.c", "-Wl,--stack,4096"), SV("main.c", "-Wl,--stack,4096"));
}
static void test_linker_flags_errors(void) {
    const char *triples[] = {"aarch64-macos", "x86_64-linux-musl", "x86_64-windows-gnu", "wasm32-wasi"};
    const char *missing[] = {"--version-script", "-z", "--subsystem", "--stack", "--compress-debug-sections"};
    const char *bad[] = {"-z,unknown", "--hash-style=bad", "--subsystem,bad", "--stack,not-a-number", "--icf=all", "--threads=1", "--enable-auto-import", "--dynamic-list=some.map", "--exclude-libs,ALL", "--push-state", "--pop-state", "-lm", "libfake.a"};
    for (size_t k = 0; k < 4; k++) {
        char *te = NULL;
        Target target = resolve_target(triples[k], &te);
        for (size_t i = 0; i < sizeof missing / sizeof *missing; i++) {
            char *err = NULL;
            normalize_linker_flags(args_of(SV("-c", "main.c", str_fmt("-Wl,%s", missing[i])), false), target, &err);
            CHECK(starts_with(err_of(err), "expected parameter after "), "%s %s: %s", triples[k], missing[i], err_of(err));
        }
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
            char *err = NULL;
            normalize_linker_flags(args_of(SV("main.c", str_fmt("-Wl,%s", bad[i])), false), target, &err);
            CHECK(*err_of(err), "%s %s", triples[k], bad[i]);
        }
    }
}

/* ---- resource VFS ---- */
static Toolchain toolchain(const char *lib, const char *cache, const char *clang) {
    Toolchain tc; memset(&tc, 0, sizeof tc);
    tc.lib_dir = (char *)lib; tc.cache_dir = (char *)cache; tc.clang = (char *)clang;
    return tc;
}
static bool vfs_host(void) {
#if defined(_WIN32) && !defined(CROSS_CC_WINDOWS_CANONICAL_PATHS)
    return false;
#else
    return true;
#endif
}

static void test_resource_vfs_scope(void) {
    Toolchain tc = toolchain("/real/lib", "/cache", "");
    Options eligible = args_of(SV("-c", "-O0", "-g0", "x.cpp"), true);
    if (!vfs_host()) { CHECK(!resource_vfs_eligible(eligible, tc, false)); return; }
    CHECK(resource_vfs_eligible(eligible, tc, false));
    StrVec refused[] = {
        SV("-c", "x.s"), SV("-E", "x.cpp"), SV("--version"), SV("-print-resource-dir"),
        SV("-M", "x.cpp"), SV("-MM", "x.cpp"), SV("-MD", "-c", "x.cpp"), SV("-MMD", "-c", "x.cpp"),
        SV("-MFdeps.d", "-c", "x.cpp"), SV("-Wp,-MD,deps.d", "-c", "x.cpp"),
        SV("--write-dependencies", "-c", "x.cpp"), SV("-ivfsoverlay", "user.json", "-c", "x.cpp"),
        SV("-resource-dir=/sdk", "-c", "x.cpp"), SV("--sysroot=/sdk", "-c", "x.cpp"),
        SV("-Xclang", "-resource-dir", "-c", "x.cpp"),
    };
    for (size_t i = 0; i < sizeof refused / sizeof *refused; i++)
        CHECK(!resource_vfs_eligible(args_of(refused[i], true), tc, false), "%s", joined(refused[i]));
    /* Values and names after -- are not interpreted as driver options. */
    CHECK(resource_vfs_eligible(args_of(SV("-c", "-I", "-MD", "x.cpp"), true), tc, false));
    CHECK(resource_vfs_eligible(args_of(SV("-c", "--", "-MD"), true), tc, false));
    bool owned = false, other = true;
    char *p = resource_vfs_path("/real/lib/include/a.h", tc.lib_dir, &owned);
    CHECK(owned);
    CHECK_STR(p, str_fmt("%s/include/a.h", resource_vfs_root()));
    resource_vfs_path("/real/library/a.h", tc.lib_dir, &other);
    CHECK(!other);
}

static void test_resource_vfs_map_precedence(void) {
    const char *root = resource_vfs_root();
    CHECK_STR(resource_vfs_prefix_map("-ffile-prefix-map=/real/lib=/mine", "/real/lib"), str_fmt("-ffile-prefix-map=%s=/mine", root));
    CHECK_STR(resource_vfs_prefix_map("-fdebug-prefix-map=/real=/mine", "/real/lib"), str_fmt("-fdebug-prefix-map=%s=/mine/lib", root));
    CHECK_STR(resource_vfs_prefix_map("-fmacro-prefix-map=/real/lib/include=/headers", "/real/lib"), str_fmt("-fmacro-prefix-map=%s/include=/headers", root));
    CHECK_STR(nz(resource_vfs_prefix_map("-ffile-prefix-map=/user=/elsewhere", "/real/lib")), "");
}

static void test_resource_dependency_make_tokens(void) {
    const char *target = "/cross_cc/lib/target: unquoted /cross_cc/lib/target2:";
    char *text = str_fmt("%s user\\ file.c /cross_cc/library/other.h \\\n  /cross_cc/lib/include/a\\ b.h user/cross_cc/lib/a.h dollar$$.h /cross_cc/lib/a\\#b.h\n\n/cross_cc/lib/include/a\\ b.h:\n", target);
    char *error = NULL;
    /* "\\" is a literal filename character on POSIX but a separator on Windows,
       where the canonical real path spells it "/". */
#ifdef _WIN32
#define BACK_SLASH_IN "back\\slash"
#define BACK_SLASH_OUT "back/slash"
#else
#define BACK_SLASH_IN "back\\slash"
#define BACK_SLASH_OUT "back\\slash"
#endif
    char *got = resource_dependency_text(text, target, "/real/\xe7\xa9\xba space/#hash/$dollar/" BACK_SLASH_IN "/lib", &error);
    const char *prefix = "/real/\xe7\xa9\xba\\ space/\\#hash/$$dollar/" BACK_SLASH_OUT "/lib";
    char *expected = str_fmt("%s user\\ file.c /cross_cc/library/other.h \\\n  %s/include/a\\ b.h user/cross_cc/lib/a.h dollar$$.h %s/a\\#b.h\n\n%s/include/a\\ b.h:\n", target, prefix, prefix, prefix);
    CHECK_STR(err_of(error), "");
    CHECK_STR(got, expected);
    CHECK_STR(resource_make_quote("a\\\\ b\t#$\xe7\xa9\xba"), "a\\\\\\\\\\ b\\\t\\#$$\xe7\xa9\xba");
    char *bad = NULL;
    resource_dependency_text("other: x.c\n", target, "/real/lib", &bad);
    CHECK(*err_of(bad));
}

static void test_resource_dependency_scope(void) {
    Toolchain tc = toolchain("/real/lib", "/cache", "");
    StrVec enabled[] = {
        SV("-c", "-MD", "x.cpp"), SV("-c", "-MMD", "-MP", "x.c", "-o", "obj/x.o"),
        SV("-c", "-MD", "-MFcustom.d", "-MT", "/cross_cc/lib/target: text", "x.cpp"),
        SV("-c", "-MMD", "-MF", "custom.d", "-MQ", "target #$", "x.cpp"),
    };
    for (size_t i = 0; i < sizeof enabled / sizeof *enabled; i++) {
        Options o = args_of(enabled[i], true);
        Resource_Dependency_Plan p = resource_dependency_plan(o, tc);
        if (!vfs_host()) { CHECK(!p.enabled); continue; }
        CHECK(p.enabled, "%s", joined(enabled[i]));
        /* Every direct compiler_command caller without dependencies still disables the VFS. */
        CHECK(!resource_vfs_eligible(o, tc, false));
        CHECK(resource_vfs_eligible(o, tc, p.enabled));
    }
    StrVec disabled[] = {
        SV("-MD", "x.cpp"), SV("-c", "-MD", "x.cpp", "y.cpp"),
        SV("-c", "-MD", "-MF", "-", "x.cpp"), SV("-c", "-MMD", "-o-", "x.cpp"),
        SV("-c", "-MD", "-MF", "", "x.cpp"),
        SV("-c", "-MD", "-working-directory", "/tmp", "x.cpp"),
        SV("-c", "-MD", "-Xclang", "-MP", "x.cpp"), SV("-c", "-MD", "-Wp,-MP", "x.cpp"),
        SV("-c", "-MD", "-MT", "bad\nrule", "x.cpp"), SV("-c", "-MD", "-fmodules", "x.cpp"),
        SV("-c", "-MD", "-x", "c++-header", "x.cpp"), SV("-c", "-MD", "-MFx.cpp", "x.cpp"),
        SV("-c", "-MD", "-MFx.o", "-ox.o", "x.cpp"), SV("-c", "-MD", "-###", "x.cpp"),
        SV("-c", "-MD", "-MT", "a-long-target-which-combines-with-the-second-target-to-wrap", "-MQ", "second-target", "x.cpp"),
    };
    for (size_t i = 0; i < sizeof disabled / sizeof *disabled; i++)
        CHECK(!resource_dependency_plan(args_of(disabled[i], true), tc).enabled, "%s", joined(disabled[i]));
    if (vfs_host()) {
        Resource_Dependency_Plan p = resource_dependency_plan(args_of(SV("-c", "-MD", "dir/x.cpp", "-o", "obj/name.weird"), true), tc);
        CHECK_STR(p.path, "obj/name.d");
        CHECK_STR(p.targets, "obj/name.weird:");
    }
}

static char *test_temp_dir(const char *prefix) {
    char *e = NULL;
    char *dir = make_temp_dir(".", prefix, &e);
    if (!dir || e) { fprintf(stderr, "cannot create test directory: %s\n", err_of(e)); exit(1); }
    return absolute_path(dir);
}

static void test_resource_vfs_overlay_and_command(void) {
    char *work = test_temp_dir(".cross-cc-vfs-test-");
    Toolchain tc = toolchain("/real/quote\"back\\slash/lib", work, "/clang");
    char *error = NULL;
    char *overlay = resource_vfs_overlay(tc, &error);
    CHECK_STR(err_of(error), "");
    if (*err_of(error)) { remove_tree(work); return; }
    char *e2 = NULL;
    char *again = resource_vfs_overlay(tc, &e2);
    CHECK_STR(err_of(e2), "");
    CHECK_STR(again, overlay);
    size_t size = 0;
    char *text = read_file(overlay, &size);
    CHECK(text != NULL);
    if (text) {
        Bytes external = {0};
        const char *ext = builtins_driver_path(tc.lib_dir);
        for (const char *c = ext; *c; c++) { if (*c == '"' || *c == '\\') bytes_push(&external, '\\'); bytes_push(&external, (unsigned char)*c); }
        bytes_push(&external, 0);
        CHECK(strstr(text, "\"use-external-names\": false") || strstr(text, "\"use-external-names\":false"), "%s", text);
        CHECK(strstr(text, (char *)external.data) != NULL, "%s", text);
        CHECK(strstr(text, str_fmt("\"%s\"", resource_vfs_root())) != NULL, "%s", text);
        const char *first = strstr(text, "external-contents");
        CHECK(first && !strstr(first + 1, "external-contents"), "exactly one root: %s", text);
    }
    char *user_map = str_fmt("-ffile-prefix-map=%s=/mine", tc.lib_dir);
    Options o = args_of(SV("-c", "-O0", "-I", "/user/inc", "-include", "/user/a.h", user_map, "/absolute/user.cpp"), true);
    char *te = NULL;
    Target target = resolve_target("x86_64-linux-musl", &te);
    Runtime rt; memset(&rt, 0, sizeof rt);
    rt.compile_flags = SV("-isystem", str_fmt("%s/libcxx/include", tc.lib_dir), "-D_KEEP=1", "-isystem", "/external/sdk", "-isystem", str_fmt("%s/include", tc.lib_dir));
    char *ce = NULL;
    StrVec cmd = compiler_command(o, target, tc, rt, false, &ce);
    CHECK_STR(err_of(ce), "");
    if (!vfs_host()) {
        CHECK(index_of(cmd, "-ivfsoverlay") == -1);
    } else {
        CHECK(index_of(cmd, "-ivfsoverlay") >= 0, "%s", joined(cmd));
        CHECK(index_of(cmd, "/absolute/user.cpp") >= 0 && index_of(cmd, "/user/inc") >= 0 && index_of(cmd, "/user/a.h") >= 0);
        long first = index_of(cmd, str_fmt("%s/libcxx/include", resource_vfs_root()));
        long external = index_of(cmd, "/external/sdk");
        long last = index_of(cmd, str_fmt("%s/include", resource_vfs_root()));
        CHECK(first >= 0 && external > first && last > external, "%s", joined(cmd));
        CHECK(index_of(cmd, "-D_KEEP=1") > first);
        CHECK(index_of(cmd, "-fsanitize=undefined") >= 0);
        char *virtual_map = str_fmt("-ffile-prefix-map=%s=/mine", resource_vfs_root());
        CHECK(index_of(cmd, virtual_map) == index_of(cmd, user_map) + 1, "%s", joined(cmd));
        CHECK(write_file(overlay, "{}", 2));
        char *corrupt = NULL;
        resource_vfs_overlay(tc, &corrupt);
        CHECK(*err_of(corrupt), "do not silently reuse a wrong root");
    }
    remove_tree(work);
}

/* ---- deterministic staging and canonical MinGW links ---- */
static void tmkdir(const char *path) {
#ifdef _WIN32
    bool ok = _wmkdir(windows_wide(path)) == 0;
#else
    bool ok = mkdir(path, 0777) == 0;
#endif
    CHECK(ok, "mkdir %s", path);
}
static void twrite(const char *path, const char *text) { CHECK(write_file(path, text, strlen(text)), "write %s", path); }
static void twrite_n(const char *path, const void *data, size_t n) { CHECK(write_file(path, data, n), "write %s", path); }
static char *tcwd(void) { return absolute_path("."); }

static void test_mingw_source_debug_and_runtime_pdb(void) {
    CHECK_STR(canonical_mingw_relative(path_join(tcwd(), "cache", "stage"), "does-not-exist"), "../../does-not-exist");
    StrVec no_debug[] = {SV("-g0"), SV("-O2", "-g0"), SV("-ggdb0"), SV("-s", "-g", "-g0")};
    for (size_t i = 0; i < sizeof no_debug / sizeof *no_debug; i++) {
        char *e = NULL;
        Options o = parse_arguments(no_debug[i], false, &e);
        CHECK_STR(err_of(e), "");
        CHECK(!emits_debug_info(o, false), "%s", joined(no_debug[i]));
        CHECK(emits_debug_info(o, true));
        CHECK(canonical_mingw_link(o), "%s", joined(no_debug[i]));
    }
    const char *debug[] = {"-g", "-gline-tables-only"};
    for (size_t i = 0; i < 2; i++) {
        char *e = NULL;
        Options o = parse_arguments(SV(debug[i]), false, &e);
        CHECK_STR(err_of(e), "");
        CHECK(emits_debug_info(o, false));
        CHECK(canonical_mingw_link(o), "%s", debug[i]);
    }
    StrVec refused[] = {SV("-s"), SV("-Os", "-g0"), SV("-r", "-g0"), SV("-g0", "-Wl,--pdb=custom.pdb"), SV("-g0", "-Xlinker", "--out-implib=custom.lib"),
        SV("-g", "-Xclang", "-object-file-name=custom.obj"), SV("-g", "-Xclang", "-object-file-name", "-Xclang", "custom.obj"), SV("-g", "-Xclang=-object-file-name=custom.obj")};
    for (size_t i = 0; i < sizeof refused / sizeof *refused; i++) {
        char *e = NULL;
        Options o = parse_arguments(refused[i], false, &e);
        CHECK_STR(err_of(e), "");
        CHECK(!canonical_mingw_link(o), "%s", joined(refused[i]));
    }
}

static void test_deterministic_stage_exclusive_ownership(void) {
    char *root = test_temp_dir(".cross-cc-stage-test-");
    char *e1 = NULL;
    char *first = deterministic_link_stage(root, "link-", "app.exe", &e1);
    CHECK_STR(err_of(e1), "");
    if (!*err_of(e1)) {
        char *marker = path_join(first, "owned");
        twrite(marker, "owner");
        char *e2 = NULL;
        char *second = deterministic_link_stage(root, "link-", "app.exe", &e2);
        CHECK_STR(err_of(e2), "");
        CHECK(!streq(first, second));
        if (!*err_of(e2)) CHECK(remove_tree(second));
        CHECK(file_exists(marker));
        CHECK(remove_tree(first));
        char *e3 = NULL;
        char *third = deterministic_link_stage(root, "link-", "app.exe", &e3);
        CHECK_STR(err_of(e3), "");
        CHECK_STR(third, first);
    }
    remove_tree(root);
}

static void test_canonical_mingw_operands_and_owned_aliases(void) {
    char *root = test_temp_dir(".cross-cc-canonical-test-");
    char *work = path_join(root, "stage"), *owned = path_join(root, "arbitrary-cache-hash"), *user = path_join(root, "user");
    tmkdir(work); tmkdir(owned); tmkdir(user);
    char *object = path_join(owned, "crt2.obj"), *library = path_join(owned, "libpthread.a"), *coincidence = path_join(user, "symbol-or-library");
    twrite(object, "test"); twrite(library, "test"); twrite(coincidence, "test"); twrite(path_join(owned, "complete"), "test");
    tmkdir(path_join(owned, "nested.a"));
    /* All separate non-path options must preserve an operand which exists. */
    StrVec link = {0};
    sv_push(&link, "ld.lld");
    const char *options[] = {"-e", "-u", "-l", "-m", "--stack"};
    for (size_t i = 0; i < 5; i++) { sv_push(&link, (char *)options[i]); sv_push(&link, coincidence); }
    StrVec tail = SV("-L", user, "-T", coincidence, "-o", "app.exe", "--pdb=app.pdb");
    for (size_t i = 0; i < tail.len; i++) sv_push(&link, tail.data[i]);
    size_t boundary = link.len;
    StrVec owned_tail = SV(object, "-L", owned, library);
    for (size_t i = 0; i < owned_tail.len; i++) sv_push(&link, owned_tail.data[i]);
    char *alias_err = NULL;
    StrVec aliased = canonical_mingw_runtime(link, work, boundary, &alias_err);
    CHECK_STR(err_of(alias_err), "");
    if (!*err_of(alias_err)) {
        char *args_err = NULL;
        StrVec outputs = {0}, destinations = {0};
        StrVec args = canonical_mingw_arguments(aliased, work, &outputs, &destinations, &args_err);
        CHECK_STR(err_of(args_err), "");
        if (!*err_of(args_err) && args.len > 22) {
            /* Child dispatch contributes one more argument than the original argv0. */
            for (size_t i = 3; i <= 11; i += 2) CHECK_STR(args.data[i], coincidence);
            CHECK_STR(args.data[13], canonical_mingw_relative(work, user));
            CHECK_STR(args.data[15], canonical_mingw_relative(work, coincidence));
            CHECK_STR(args.data[17], "output-0/app.exe");
            CHECK_STR(args.data[18], "--pdb=output-1/app.pdb");
            CHECK_STR(args.data[19], "runtime-0/crt2.obj");
            CHECK_STR(args.data[21], "runtime-0");
            CHECK_STR(args.data[22], "runtime-0/libpthread.a");
        } else CHECK(!*err_of(args_err) && args.len > 22, "%s", joined(args));
        CHECK(file_exists(path_join(work, "runtime-0", "crt2.obj")));
        CHECK(file_exists(path_join(work, "runtime-0", "libpthread.a")));
        CHECK(!file_exists(path_join(work, "runtime-0", "complete")));
        CHECK(!file_exists(path_join(work, "runtime-0", "nested.a")));
        CHECK(!file_exists(path_join(work, "runtime-1")));
    }
#ifdef _WIN32
    CHECK_STR(canonical_mingw_path("output-0\\app.exe"), "output-0/app.exe");
    CHECK_STR(canonical_mingw_relative("C:\\stage", "Z:\\user\\lib"), "Z:\\user\\lib");
#else
    /* A backslash can be a literal filename character on Unix. */
    CHECK_STR(canonical_mingw_path("user\\name"), "user\\name");
#endif
    remove_tree(root);
}

static void test_canonical_mingw_directive_fallback(void) {
    char *root = test_temp_dir(".cross-cc-directive-test-");
    char *object = path_join(root, "input.obj");
    unsigned char raw[73];
    memset(raw, 0, sizeof raw);
    raw[0] = 0x64; raw[1] = 0x86; raw[2] = 1; /* AMD64, one section. */
    memcpy(raw + 20, ".text", 5);
    twrite_n(object, raw, 60);
    CHECK(canonical_mingw_object(object));
    CHECK(canonical_mingw_inputs(SV("ld.lld", "-e", object, "-u", object, "-m", "i386pep", "--stack", "16777216", object)));
    CHECK(!canonical_mingw_inputs(SV("ld.lld", "-l", "foo", object)));
    CHECK(!canonical_mingw_inputs(SV("ld.lld", "-lfoo", object)));
    CHECK(!canonical_mingw_inputs(SV("ld.lld", "-T", "script", object)));
    memcpy(raw + 20, ".drectve", 8);
    twrite_n(object, raw, 60);
    CHECK(!canonical_mingw_object(object));
    CHECK(!canonical_mingw_inputs(SV("ld.lld", object)));
    /* COFF also permits section names through the string table. */
    raw[8] = 60; /* String table immediately after the section table, no symbols. */
    memset(raw + 20, 0, 8);
    memcpy(raw + 20, "/4", 2);
    raw[60] = 13;
    memcpy(raw + 64, ".drectve", 8);
    twrite_n(object, raw, sizeof raw);
    CHECK(!canonical_mingw_object(object));
    memcpy(raw + 64, ".unknown", 8);
    twrite_n(object, raw, sizeof raw);
    CHECK(canonical_mingw_object(object));
    twrite_n(object, raw, 30);
    CHECK(!canonical_mingw_object(object));
    const char *unsupported[] = {"!<arch>\n", "BC\xc0\xde", "\x00\x00\xff\xff", "missing or unsupported object"};
    const size_t lens[] = {8, 4, 4, 29};
    for (size_t i = 0; i < 4; i++) { twrite_n(object, unsupported[i], lens[i]); CHECK(!canonical_mingw_object(object), "%zu", i); }
    remove_tree(root);
}

static void test_canonical_mingw_codeview_paths(void) {
    const char *names[] = {"/src/source.c", "C:/src/source.c", "C:\\src\\source.c", "\\\\server\\src\\source.c", "relative/source.c", "source.c", "C:source.c", "\\source.c"};
    for (size_t k = 0; k < 8; k++) {
        const char *name = names[k];
        Bytes data = {0};
        rc_u32(&data, 4); /* C13 signature. */
        rc_u32(&data, 0xf4); rc_u32(&data, 8); /* Checksum before string table. */
        rc_u32(&data, 1); rc_u32(&data, 0); /* Filename offset 1, no checksum. */
        rc_u32(&data, 0xf3); rc_u32(&data, (uint32_t)(strlen(name) + 2));
        bytes_push(&data, 0); bytes_append(&data, name, strlen(name)); bytes_push(&data, 0);
        while (data.len % 4 != 0) bytes_push(&data, 0);
        bool absolute = k < 4;
        CHECK(canonical_mingw_debug_paths(data.data, data.len) == absolute, "%s", name);
        /* Invalid filename offset, subsection size and checksum size fail closed. */
        rc_set32(data.data, 12, UINT32_MAX);
        CHECK(!canonical_mingw_debug_paths(data.data, data.len));
        rc_set32(data.data, 12, 1);
        rc_set32(data.data, 8, UINT32_MAX);
        CHECK(!canonical_mingw_debug_paths(data.data, data.len));
        rc_set32(data.data, 8, 8);
        data.data[16] = 255;
        CHECK(!canonical_mingw_debug_paths(data.data, data.len));
    }
    const unsigned char short1[] = {4, 0, 0}, short2[] = {2, 0, 0, 0};
    CHECK(!canonical_mingw_debug_paths(short1, sizeof short1));
    CHECK(!canonical_mingw_debug_paths(short2, sizeof short2));
}

static void test_canonical_mingw_safe_directives(void) {
    char *root = test_temp_dir(".cross-cc-safe-directive-test-");
    char *object = path_join(root, "input.obj");
    /* One .drectve section whose data follows the 60-byte headers. */
    struct { const char *text; bool ok; } cases[] = {
        {" -exclude-symbols:__clang_call_terminate", true},
        {" /include:foo /ALTERNATENAME:a=b", true},
        {" /defaultlib:libfoo", false},
        {" -exclude-symbols:x /defaultlib:\"a b\"", false},
        {" \"-exclude-symbols:x\"", false},
        {"", false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        size_t n = strlen(cases[i].text);
        unsigned char *raw = xmalloc(60 + n);
        memset(raw, 0, 60 + n);
        raw[0] = 0x64; raw[1] = 0x86; raw[2] = 1;
        memcpy(raw + 20, ".drectve", 8);
        raw[36] = (unsigned char)n; raw[40] = 60;
        memcpy(raw + 60, cases[i].text, n);
        twrite_n(object, raw, 60 + n);
        CHECK(canonical_mingw_object(object) == cases[i].ok, "%s", cases[i].text);
    }
    remove_tree(root);
}

static void test_msvc_runtime_stage(void) {
    char *work = test_temp_dir(".cross-cc-msvc-stage-test-");
    char *archive = path_join(work, "src.a"), *second = path_join(work, "other");
    tmkdir(second);
    const char *files[] = {archive, path_join(work, "alias.lib"), path_join(second, "src.a"), path_join(work, "not-a-library.txt")};
    for (size_t i = 0; i < 4; i++) twrite(files[i], "!<arch>\n");
    tmkdir(path_join(work, "nested.a"));
    char *stage = path_join(work, "stage");
    tmkdir(stage);
    char *relative_stage = path_relative(tcwd(), absolute_path(stage));
    char *search = str_fmt("-Wl,-libpath:%s", work);
    StrVec flags = msvc_stage_runtime(SV("-Wl,-libpath:/sdk", "-Xlinker", search, archive, path_join(work, "missing.a"), search, path_join(second, "src.a"), search), stage, false);
    CHECK(flags.len == 8, "%s", joined(flags));
    if (flags.len == 8) {
        CHECK_STR(flags.data[0], "-Wl,-libpath:/sdk");
        CHECK_STR(flags.data[2], search); /* Transport operands are not flags. */
        CHECK_STR(flags.data[3], path_join(relative_stage, "runtime-0", "src.a"));
        CHECK(is_file(flags.data[3]));
        CHECK_STR(flags.data[4], path_join(work, "missing.a"));
        CHECK_STR(flags.data[5], str_fmt("-Wl,-libpath:%s", path_join(relative_stage, "runtime-0")));
        CHECK_STR(flags.data[6], path_join(relative_stage, "runtime-1", "src.a"));
        CHECK_STR(flags.data[7], flags.data[5]);
    }
    CHECK(is_file(path_join(stage, "runtime-0", "alias.lib")));
    CHECK(!file_exists(path_join(stage, "runtime-0", "not-a-library.txt")));
    CHECK(!file_exists(path_join(stage, "runtime-0", "nested.a")));
    /* An occupied alias cannot be reused. All flags fall back together. */
    StrVec original = SV(archive, search);
    StrVec fallback = msvc_stage_runtime(original, stage, false);
    CHECK(fallback.len == original.len);
    for (size_t i = 0; i < original.len && i < fallback.len; i++) CHECK_STR(fallback.data[i], original.data[i]);
    remove_tree(work);
}

static void test_msvc_managed_sdk_stage(void) {
    char *root = test_temp_dir(".cross-cc-sdk-stage-test-");
    char *stage = path_join(root, "stage");
    tmkdir(stage);
    char *relative_stage = path_relative(tcwd(), stage);
    const char *names[] = {"ucrt", "crt", "um"};
    StrVec flags = {0};
    for (size_t i = 0; i < 3; i++) {
        char *dir = path_join(root, names[i]);
        tmkdir(dir);
        /* Equal basenames may contain different contents; never merge aliases. */
        twrite(path_join(dir, "same.lib"), names[i]);
        twrite(path_join(dir, "libcpmt.lib"), "autolink");
        twrite(path_join(dir, "upper.LIB"), "case");
        twrite(path_join(dir, "crt.obj"), "object");
        twrite(path_join(dir, "receipt.txt"), "not a linker input");
        tmkdir(path_join(dir, "nested.lib"));
        twrite(path_join(dir, "nested.lib", "autolink.lib"), "nested pragma input");
        twrite(path_join(dir, "nested.lib", "crt.o"), "object");
        twrite(path_join(dir, "nested.lib", "header.h"), "not a linker input");
#ifndef _WIN32
        CHECK(symlink("same.lib", path_join(dir, "linked.lib")) == 0);
#endif
        sv_push(&flags, str_fmt("-Wl,-libpath:%s", dir));
    }
    /* A transport operand resembling an owned flag is still only an operand. */
    sv_push(&flags, "-Xlinker"); sv_push(&flags, flags.data[0]); sv_push(&flags, flags.data[0]);
    StrVec original = {0};
    for (size_t i = 0; i < flags.len; i++) sv_push(&original, flags.data[i]);
    StrVec unchanged = msvc_stage_runtime(flags, stage, false);
    CHECK(unchanged.len == flags.len);
    for (size_t i = 0; i < flags.len && i < unchanged.len; i++) CHECK_STR(unchanged.data[i], flags.data[i]);
    StrVec staged = msvc_stage_runtime(flags, stage, true);
    CHECK(staged.len == flags.len, "%s", joined(staged));
    if (staged.len == flags.len) {
        for (size_t i = 0; i < 3; i++) {
            char *alias = path_join(stage, str_fmt("runtime-%zu", i));
            CHECK_STR(staged.data[i], str_fmt("-Wl,-libpath:%s", path_join(relative_stage, str_fmt("runtime-%zu", i))));
            size_t size = 0;
            char *contents = read_file(path_join(alias, "same.lib"), &size);
            CHECK(contents && size == strlen(names[i]) && memcmp(contents, names[i], size) == 0, "%s", alias);
            const char *present[] = {"libcpmt.lib", "upper.LIB", "crt.obj"};
            for (size_t k = 0; k < 3; k++) CHECK(is_file(path_join(alias, present[k])), "%s", present[k]);
#ifndef _WIN32
            CHECK(is_file(path_join(alias, "linked.lib")));
#endif
            CHECK(!file_exists(path_join(alias, "receipt.txt")));
            CHECK(is_file(path_join(alias, "nested.lib", "autolink.lib")));
            CHECK(is_file(path_join(alias, "nested.lib", "crt.o")));
            CHECK(!file_exists(path_join(alias, "nested.lib", "header.h")));
        }
        CHECK_STR(staged.data[3], "-Xlinker");
        CHECK_STR(staged.data[4], flags.data[0]);
        CHECK_STR(staged.data[5], staged.data[0]);
    }
    for (size_t i = 0; i < original.len; i++) CHECK_STR(flags.data[i], original.data[i]);
    /* Fail after a successful first rewrite: no partially rewritten flags escape. */
    char *failure_stage = path_join(root, "failure");
    tmkdir(failure_stage);
    sv_push(&flags, str_fmt("-Wl,-libpath:%s", path_join(root, "missing")));
    StrVec fallback = msvc_stage_runtime(flags, failure_stage, true);
    CHECK(fallback.len == flags.len);
    for (size_t i = 0; i < flags.len && i < fallback.len; i++) CHECK_STR(fallback.data[i], flags.data[i]);
    /* Staging does not alter source bytes or remove source aliases. */
    size_t size = 0;
    char *contents = read_file(path_join(root, "crt", "libcpmt.lib"), &size);
    CHECK(contents && size == 8 && memcmp(contents, "autolink", 8) == 0);
    remove_tree(root);
}

static void test_msvc_managed_sdk_unsafe_alias_fallback(void) {
#ifndef _WIN32
    char *root = test_temp_dir(".cross-cc-sdk-alias-test-");
    const char *targets[] = {"../outside.lib", ".", "alias.lib"};
    for (size_t i = 0; i < 3; i++) {
        char *dir = path_join(root, str_fmt("sdk-%zu", i)), *work = path_join(root, str_fmt("stage-%zu", i));
        tmkdir(dir); tmkdir(work);
        twrite(path_join(dir, "regular.lib"), "original");
        CHECK(symlink(targets[i], path_join(dir, "alias.lib")) == 0);
        StrVec flags = SV(str_fmt("-Wl,-libpath:%s", dir));
        StrVec fallback = msvc_stage_runtime(flags, work, true);
        CHECK(fallback.len == flags.len);
        if (fallback.len) CHECK_STR(fallback.data[0], flags.data[0]);
        /* A rejected symlink is never recreated inside the private stage. */
        char buf[64];
        CHECK(readlink(path_join(work, "runtime-0", "alias.lib"), buf, sizeof buf) < 0, "%s", targets[i]);
    }
    remove_tree(root);
#endif
}

int main(void) {
    test_rc_msvc_env_override();
    test_rc_key_order_fuzz();
    test_linker_flags_cross_format();
    test_linker_flags_operands_and_transports();
    test_linker_flags_errors();
    test_resource_vfs_scope();
    test_resource_vfs_map_precedence();
    test_resource_dependency_make_tokens();
    test_resource_dependency_scope();
    test_resource_vfs_overlay_and_command();
    test_mingw_source_debug_and_runtime_pdb();
    test_deterministic_stage_exclusive_ownership();
    test_canonical_mingw_operands_and_owned_aliases();
    test_canonical_mingw_directive_fallback();
    test_canonical_mingw_codeview_paths();
    test_canonical_mingw_safe_directives();
    test_msvc_runtime_stage();
    test_msvc_managed_sdk_stage();
    test_msvc_managed_sdk_unsafe_alias_fallback();
    if (failures) { fprintf(stderr, "resources_test: %d failure(s)\n", failures); return 1; }
    printf("resources_test: ok\n");
    return 0;
}
