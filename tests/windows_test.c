#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#endif
/* Unit tests for the Windows runtime and SDK handling (src/msvc*.c, src/mingw*.c).
   They run on any host against real files and the real readers; no SDK is
   downloaded. Native Windows behavior (registry discovery, reparse points,
   GetFinalPathName) is only compiled on Windows; on Windows hosts the inventory
   test creates real symlink/reparse fixtures, and reports SKIP if the
   privilege to create them is unavailable. Registry discovery is not exercised. */
/* Unity build: the Windows modules plus the support code they link against. */
#include "support.c"
#include "libc_config.c"
#include "target.c"
#include "arguments.c"
#include "runtime.c"
#include "runtime_recipes.c"
#include "runtime_musl_supplement.c"
#include "wasi_recipes.c"
#include "wasi_supplement.c"
#include "objcopy.c"
#include "msvc.c"
#include "msvc_other.c"
#include "msvc_windows.c"
#include "msvc_libcxx.c"
#include "msvc_link.c"
/* Only managed acquisition's child-process boundary is fake: tests exercise
   the real pin parser, downloader verification, staging and publication. */
static int managed_test_command(StrVec args, bool verbose);
#define run_command managed_test_command
#include "msvc_managed.c"
#undef run_command
#include "mingw.c"
#include "mingw_recipes.c"

/* Test doubles for collaborators outside the Windows modules (compiler driver,
   backend bridge, canonical-link stage, other platforms' runtimes). None of the
   assertions below depend on their behavior; the Windows code under test is real. */
bool BridgeLLDLinkMinGW(int argc, const char **argv, bool can_exit_early, bool disable_output) { (void)argc; (void)argv; (void)can_exit_early; (void)disable_output; return false; }
bool canonical_mingw_link(Options o) { (void)o; return false; }
bool canonical_mingw_inputs(StrVec link) { (void)link; return false; }
char *canonical_mingw_path(const char *path) { return str_copy(path); }
int run_canonical_mingw_link(StrVec link, const char *stage, size_t managed_start, bool verbose) { (void)link; (void)stage; (void)managed_start; (void)verbose; return 1; }
char *deterministic_link_stage(const char *cache_dir, const char *prefix, const char *output, char **error) { (void)output; return make_temp_dir(cache_dir, prefix, error); }
StrVec compiler_command(Options o, Target t, Toolchain tc, Runtime rt, bool deps, char **error) { (void)o; (void)t; (void)tc; (void)rt; (void)deps; *error = NULL; return (StrVec){0}; }
char *compiler_optimization(const char *arg, bool *known) { (void)arg; if (known) *known = false; return str_copy(""); }
bool compiler_plain_assembly(StrVec args) { (void)args; return false; }
bool emits_debug_info(Options o, bool include_runtime) { (void)o; (void)include_runtime; return false; }
bool optimized_build(Options o) { (void)o; return false; }
bool resource_vfs_eligible(Options o, Toolchain tc, bool dependencies) { (void)o; (void)tc; (void)dependencies; return false; }
char *resource_vfs_overlay_root(Toolchain tc, const char *virtual_root, const char *physical, char **error) { (void)tc; (void)virtual_root; (void)physical; *error = NULL; return str_copy(""); }
void bytes_append(Bytes *b, const void *data, size_t n) {
    if (b->len + n > b->cap) { b->cap = (b->len + n) * 2; b->data = xrealloc(b->data, b->cap); }
    memcpy(b->data + b->len, data, n);
    b->len += n;
}
StrVec wasi_include_flags(Toolchain tc) { (void)tc; return (StrVec){0}; }
StrVec wasi_base(Target t, Options o) { (void)t; (void)o; return (StrVec){0}; }
char *glibc_default_version(Target t) { (void)t; return str_copy(""); }
Runtime prepare_glibc(Options o, Target t, Toolchain tc, char **error) { (void)o; (void)t; (void)tc; *error = NULL; return (Runtime){0}; }
StrVec bsd_include_flags(Target t, const char *lib_dir) { (void)t; (void)lib_dir; return (StrVec){0}; }
StrVec bsd_version_flags(Target t) { (void)t; return (StrVec){0}; }
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(got, want) do { checks++; const char *g_ = (got), *w_ = (want); if (!streq(g_, w_)) { failures++; fprintf(stderr, "FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, nz(g_), nz(w_)); } } while (0)

static char *test_dir(const char *prefix) {
    const char *base = getenv("TMPDIR");
    char *tmpl = str_fmt("%s/%sXXXXXX", base && *base ? base : "/tmp", prefix);
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); exit(2); }
    return tmpl;
}

static void put(const char *path, const char *text) { CHECK(write_file(path, text, strlen(text))); }

static char *slurp(const char *path) {
    size_t n = 0;
    char *d = read_file(path, &n);
    return d ? str_ncopy(d, n) : NULL;
}

static void test_include_order(void) {
    Msvc_Libc l = {0};
    l.include_dir = "C:\\Kits\\10\\Include\\10.0.26100.0\\ucrt";
    l.sys_include_dir = "C:\\VS\\MSVC\\14.29.30133\\lib\\x64\\..\\..\\include";
    StrVec dirs = msvc_include_dirs(l);
    const char *want[] = {
        "C:\\Kits\\10\\Include\\10.0.26100.0\\ucrt",
        "C:\\VS\\MSVC\\14.29.30133\\lib\\x64\\..\\..\\include",
        "C:\\VS\\MSVC\\14.29.30133\\lib\\x64\\..\\..\\atlmfc\\include",
        "C:\\Kits\\10\\Include\\10.0.26100.0\\um",
        "C:\\Kits\\10\\Include\\10.0.26100.0\\shared",
    };
    CHECK(dirs.len == 5);
    for (size_t i = 0; i < 5 && i < dirs.len; i++) CHECK_STR(dirs.data[i], want[i]);
    l.sys_include_dir = l.include_dir;
    CHECK(msvc_include_dirs(l).len == 4);
}

static void test_versions(void) {
    CHECK(msvc_version_less("10.0.19041.0", "10.0.26100.0"));
    CHECK(msvc_version_less("14.9.1", "14.29.30133"));
    CHECK(!msvc_version_less("14.44.35207", "14.44.35207"));
    CHECK(msvc_version_less("14.44", "14.44.1"));
    CHECK(msvc_version_less("14.0x10", "14.17"));
    CHECK(!msvc_version_less("14.0x10", "14.16"));
    CHECK(msvc_version_less("14.1_0", "14.11"));
    CHECK(!msvc_version_less("14.12preview", "14.11"));
    const struct { const char *text; int64_t value; } components[] = {
        {"", 0}, {"+", 0}, {"-", 0}, {"_", 0}, {"__12__", 12},
        {"+0b101", 5}, {"-0o17", -15}, {"0d19", 19}, {"0zAB", 131},
        {"0xF_f", 255}, {"0X10", 0}, {"12preview", 12}, {"-12tail", -12},
        {" 12", 0}, {"0x", 0}, {"0x_", 0}, {"0b12", 1},
        {"9223372036854775807", INT64_MAX}, {"-9223372036854775808", INT64_MIN},
        {"9223372036854775808", INT64_C(922337203685477580)},
        {"-9223372036854775809", -INT64_C(922337203685477580)},
        {"0xffffffffffffffff", INT64_C(1152921504606846975)},
    };
    for (size_t i = 0; i < sizeof(components) / sizeof(components[0]); i++)
        CHECK(msvc_component(components[i].text, strlen(components[i].text)) == components[i].value);
    CHECK_STR(msvc_dirname("C:\\a\\include\\"), "C:\\a");
    CHECK_STR(msvc_dirname("include"), "");
    CHECK_STR(msvc_arch_dir("aarch64"), "arm64");
    CHECK_STR(msvc_arch_dir("thumb"), "arm");
}

static void test_libc_file(void) {
    char *dir = test_dir("cross-msvc-");
    char *path = path_join(dir, "libc.txt");
    char *e = NULL;
    put(path, "include_dir=C:\\inc\\ucrt\r\nsys_include_dir=C:\\msvc\\include\ncrt_dir=C:\\sdk\\ucrt\nmsvc_lib_dir=C:\\msvc\\lib\\x64\nkernel32_lib_dir=C:\\sdk\\um\ncc_dir=\ndarwin_sdk_dir=\n");
    Msvc_Libc l = msvc_libc_from_file(path, &e);
    CHECK(e == NULL);
    CHECK_STR(l.include_dir, "C:\\inc\\ucrt");
    CHECK_STR(l.sys_include_dir, "C:\\msvc\\include");
    CHECK_STR(l.crt_dir, "C:\\sdk\\ucrt");
    CHECK_STR(l.msvc_lib_dir, "C:\\msvc\\lib\\x64");
    CHECK_STR(l.kernel32_lib_dir, "C:\\sdk\\um");
    /* Unknown keys are ignored; a missing field or an empty required one is an error. */
    put(path, "include_dir=C:\\inc\nsys_include_dir=C:\\sys\ncrt_dir=C:\\crt\nmsvc_lib_dir=C:\\m\nkernel32_lib_dir=C:\\k\ncc_dir=\ndarwin_sdk_dir=\nbogus=1\n");
    e = NULL;
    l = msvc_libc_from_file(path, &e);
    CHECK(e == NULL);
    CHECK_STR(l.crt_dir, "C:\\crt");
    put(path, "include_dir=x\nbogus=1\n");
    e = NULL;
    msvc_libc_from_file(path, &e);
    CHECK(e && contains(e, "missing field: sys_include_dir"));
    put(path, "include_dir=C:\\inc\nsys_include_dir=C:\\sys\ncrt_dir=\nmsvc_lib_dir=C:\\m\nkernel32_lib_dir=C:\\k\ncc_dir=\ndarwin_sdk_dir=\n");
    e = NULL;
    msvc_libc_from_file(path, &e);
    CHECK(e && contains(e, "crt_dir may not be empty"));
    put(path, "include_dir=C:\\inc\nsys_include_dir=C:\\sys\ncrt_dir=C:\\crt\nmsvc_lib_dir=\nkernel32_lib_dir=C:\\k\ncc_dir=\ndarwin_sdk_dir=\n");
    e = NULL;
    msvc_libc_from_file(path, &e);
    CHECK(e && contains(e, "msvc_lib_dir may not be empty for windows-msvc"));

    /* Fuzz: random libc files and paths must parse or fail with a message, never crash. */
    const char alphabet[] = "=\n\r# \\/.:abcdeinclude_dirsys_crt_msvc_lib_kernel32_";
    srand(12345);
    for (int round = 0; round < 5000; round++) {
        size_t n = (size_t)(rand() % 80);
        char b[81];
        for (size_t i = 0; i < n; i++) b[i] = alphabet[rand() % (sizeof(alphabet) - 1)];
        b[n] = 0;
        CHECK(write_file(path, b, n));
        e = NULL;
        Msvc_Libc fl = msvc_libc_from_file(path, &e);
        if (!e) CHECK(nz(fl.include_dir)[0] && nz(fl.sys_include_dir)[0]);
        Msvc_Libc probe = {0};
        probe.include_dir = b;
        probe.sys_include_dir = b + n / 2;
        msvc_include_dirs(probe);
        msvc_dirname(b);
        msvc_version_less(b, b + n / 3);
    }
    remove_tree(dir);
}

static void test_libcxx_headers(void) {
    char *dir = test_dir("cross-libcxx-");
    Toolchain tc = {0};
    tc.lib_dir = path_join(dir, "lib");
    tc.cache_dir = dir;
    Target target = {0};
    target.arch = "x86_64"; target.os = "windows"; target.abi = "msvc";
    char *e = NULL;
    StrVec flags = msvc_libcxx_flags(target, tc, &e);
    CHECK(e == NULL);
    if (e) return;
    CHECK_STR(flags.data[0], "-isystem");
    char *data = slurp(path_join(flags.data[1], "__new", "new_handler.h"));
    CHECK(data != NULL);
    CHECK_STR(data, MSVC_LIBCXX_NEW_HANDLER_HEADER);
    CHECK(contains(flags.data[1], str_fmt("%016llx", (unsigned long long)runtime_hash(MSVC_LIBCXX_NEW_HANDLER_HEADER))));
    char *again = msvc_libcxx_headers(tc, &e);
    CHECK(e == NULL);
    CHECK_STR(again, flags.data[1]);
    char *joined = str_copy("");
    for (size_t i = 0; i < flags.len; i++) joined = str_fmt("%s %s", joined, flags.data[i]);
    CHECK(contains(joined, "-D_LIBCPP_NO_AUTO_LINK"));
    CHECK(contains(joined, "-D_LIBCPP_HAS_THREADS=1"));
    CHECK(contains(joined, "-D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS"));
    CHECK(!contains(joined, "_LIBCPP_NO_VCRUNTIME"));
    CHECK(!contains(joined, "-fno-exceptions"));
    CHECK(!contains(joined, "-fno-rtti"));
    Target others[2] = {{0}, {0}};
    others[0].os = "windows"; others[0].abi = "gnu";
    others[1].os = "linux"; others[1].abi = "gnu";
    for (int i = 0; i < 2; i++) {
        StrVec f = runtime_cxx_flags(others[i], tc.lib_dir);
        bool found = false;
        for (size_t k = 0; k < f.len; k++) if (contains(f.data[k], "-D_LIBCPP_NO_AUTO_LINK")) found = true;
        CHECK(!found);
    }
    remove_tree(dir);
}

static void test_libcxx_cache_kind(void) {
    Msvc_Libc l = {"sdk/ucrt", "crt/include", "sdk/lib/ucrt/x64", "crt/lib/x64", "sdk/lib/um/x64"};
    Options plain = {0}, shared = {0};
    shared.shared = true;
    char *st = msvc_libcxx_cache_kind(plain, l), *dll = msvc_libcxx_cache_kind(shared, l);
    CHECK(!streq(st, dll));
    CHECK(contains(st, "staticcrt"));
    CHECK(contains(dll, "dllcrt"));
    CHECK(contains(st, str_fmt("%016llx", (unsigned long long)runtime_hash(MSVC_LIBCXX_NEW_HANDLER_HEADER))));
    Msvc_Libc other = l;
    other.sys_include_dir = "other-crt/include";
    CHECK(!streq(st, msvc_libcxx_cache_kind(plain, other)));
    other = l;
    other.msvc_lib_dir = "other-crt/lib/x64";
    CHECK(!streq(st, msvc_libcxx_cache_kind(plain, other)));
}

static void test_libcxx_nonlink_modes(void) {
    char *dir = test_dir("cross-libcxx-modes-");
    char *libc = path_join(dir, "libc.txt");
    put(libc, "include_dir=sdk/ucrt\nsys_include_dir=crt/include\ncrt_dir=sdk/lib/ucrt/x64\nmsvc_lib_dir=crt/lib/x64\nkernel32_lib_dir=sdk/lib/um/x64\ncc_dir=\ndarwin_sdk_dir=\n");
    Toolchain tc = {0};
    tc.lib_dir = path_join(dir, "lib");
    tc.cache_dir = dir;
    Target target = {0};
    target.arch = "x86_64"; target.os = "windows"; target.abi = "msvc";
    Options cases[11];
    memset(cases, 0, sizeof(cases));
    cases[0].cxx = true; cases[0].compile_only = true;
    cases[1].cxx = true; cases[1].query = true;
    cases[2].cxx = true; cases[2].preprocess_only = true;
    cases[3].cxx = true; cases[3].syntax_only = true;
    cases[4].cxx = true; cases[4].no_stdlib = true;
    cases[5].cxx = true; cases[5].relocatable = true;
    cases[6].cxx = true; cases[6].no_defaultlibs = true;
    cases[7].cxx = true; cases[7].no_libc = true;
    cases[8].cxx = true; cases[8].compile_only = true; cases[8].shared = true;
    cases[9].cxx = true; cases[9].compile_only = true; cases[9].no_stdincxx = true;
    cases[10].compile_only = true; cases[10].no_stdlibxx = true; cases[10].no_stdincxx = true;
    for (int i = 0; i < 11; i++) {
        Options o = cases[i];
        o.msvc_libc_file = libc;
        char *err = NULL;
        Runtime r = prepare_msvc(o, target, tc, &err);
        CHECK(err == NULL);
        CHECK(r.link_flags.len == ((cases[i].no_defaultlibs || cases[i].no_libc) ? 3u : 0u));
        char *link = str_copy("");
        for (size_t k = 0; k < r.link_flags.len; k++) link = str_fmt("%s %s", link, r.link_flags.data[k]);
        CHECK(!contains(link, "libc++.lib"));
        char *joined = str_copy("");
        for (size_t k = 0; k < r.compile_flags.len; k++) joined = str_fmt("%s %s", joined, r.compile_flags.data[k]);
        bool wrappers = cases[i].cxx && !cases[i].no_stdincxx;
        CHECK(contains(joined, "headers-msvc-libcxx-") == wrappers);
        CHECK(contains(joined, "-D_LIBCPP_NO_AUTO_LINK") == wrappers);
        if (cases[i].cxx) CHECK(contains(joined, cases[i].shared ? "-fms-runtime-lib=dll" : "-fms-runtime-lib=static"));
    }
    Msvc_Dir_Entries entries;
    CHECK(msvc_read_dir(dir, &entries));
    for (size_t i = 0; i < entries.len; i++) CHECK(!starts_with(entries.data[i].name, "runtime-msvc-libcxx-"));
    remove_tree(dir);
}

static void put_u16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) { put_u16(p, v & 0xffff); put_u16(p + 2, v >> 16); }

static void test_reparse_bounds(void) {
    uint32_t tags[2] = {0xa000000c, 0xa0000003};
    const char *texts[] = {"..\\include\\stdio.h", "\\??\\C:\\cache\\sdk\\stdio.h", "\\??\\UNC\\server\\share\\sdk\\stdio.h", "\\\\?\\C:\\sdk\\stdio.h"};
    for (int ti = 0; ti < 2; ti++) {
        size_t start = tags[ti] == 0xa000000c ? 20 : 16;
        for (int xi = 0; xi < 4; xi++) {
            const char *text = texts[xi];
            size_t tl = strlen(text), size = start + 4 + 2 * tl + 2;
            uint8_t *data = xmalloc(size);
            /* Nonzero byte offset detects accidental UTF-16 indexing. */
            put_u32(data, tags[ti]);
            put_u16(data + 4, (unsigned)(size - 8));
            put_u16(data + 8, 4);
            put_u16(data + 10, (unsigned)(2 * tl));
            for (size_t i = 0; i < tl; i++) put_u16(data + start + 4 + 2 * i, (unsigned char)text[i]);
            char *e = NULL;
            char *target = msvc_reparse_target(data, size, &e);
            CHECK(e == NULL);
            CHECK_STR(target, text);
            for (size_t n = 0; n < size; n++) { e = NULL; msvc_reparse_target(data, n, &e); CHECK(e != NULL); }
            unsigned offsets[2] = {1, 65534};
            for (int k = 0; k < 2; k++) { put_u16(data + 8, offsets[k]); e = NULL; msvc_reparse_target(data, size, &e); CHECK(e != NULL); }
            put_u16(data + 8, 4);
            unsigned lengths[3] = {0, 1, 65534};
            for (int k = 0; k < 3; k++) { put_u16(data + 10, lengths[k]); e = NULL; msvc_reparse_target(data, size, &e); CHECK(e != NULL); }
            put_u16(data + 10, (unsigned)(2 * tl));
            put_u16(data + start + 4, 0);
            e = NULL;
            msvc_reparse_target(data, size, &e);
            CHECK(e != NULL);
            free(data);
        }
    }
    uint8_t unsupported_data[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    char *e = NULL;
    msvc_reparse_target(unsupported_data, 8, &e);
    CHECK(e != NULL);
    CHECK(msvc_path_within("\\\\?\\C:\\cache\\sdk", "\\\\?\\C:\\cache\\sdk\\include\\stdio.h", true));
    CHECK(msvc_path_within("\\\\?\\UNC\\server\\share\\sdk", "\\\\?\\UNC\\server\\share\\sdk/include", true));
    const char *bad[] = {"\\\\?\\C:\\cache\\sdk-other\\file", "\\\\?\\D:\\cache\\sdk\\file", "\\\\?\\C:\\cache\\SDK\\file", "\\\\?\\UNC\\other\\share\\sdk\\file"};
    for (int i = 0; i < 4; i++) CHECK(!msvc_path_within("\\\\?\\C:\\cache\\sdk", bad[i], true));
}

/* Native link fixtures. POSIX uses symlink(2); Windows uses CreateSymbolicLinkW
   (the reparse points msvc_read_link/msvc_final_path inspect in production), with
   backslash targets because the kernel does not normalize '/' in link targets.
   Returns 1 on success, 0 on failure, -1 when Windows denies link creation. */
static int make_link(const char *target, const char *link, bool dir) {
#ifdef _WIN32
    char *t = str_replace(target, "/", "\\");
    wchar_t *wt = w_from(t), *wl = w_from(link);
    if (!wt || !wl) return 0;
    DWORD flags = dir ? 0x1 : 0;
    if (CreateSymbolicLinkW(wl, wt, flags | 0x2 /* ALLOW_UNPRIVILEGED_CREATE */)) return 1;
    DWORD e = GetLastError();
    if (e == ERROR_INVALID_PARAMETER && CreateSymbolicLinkW(wl, wt, flags)) return 1;
    if (e == ERROR_INVALID_PARAMETER) e = GetLastError();
    if (e == ERROR_PRIVILEGE_NOT_HELD) return -1;
    fprintf(stderr, "CreateSymbolicLinkW error %lu\n", (unsigned long)e);
    return 0;
#else
    (void)dir;
    return symlink(target, link) == 0;
#endif
}

static bool remove_link(const char *link, bool dir) {
#ifdef _WIN32
    wchar_t *w = w_from(link);
    return w && (dir ? RemoveDirectoryW(w) : DeleteFileW(w));
#else
    (void)dir;
    return unlink(link) == 0;
#endif
}

static void test_inventory_links(void) {
    char *dir = test_dir("cross-msvc-links-");
    char *root_rel = path_join(dir, "sdk"), *sibling_rel = path_join(dir, "sdk-other");
    ensure_dir(root_rel);
    ensure_dir(sibling_rel);
    char *root = absolute_path(root_rel), *sibling = absolute_path(sibling_rel);
    put(path_join(root, "regular.h"), "fixture\n");
    put(path_join(sibling, "outside.h"), "fixture\n");
    char *link = path_join(root, "alias.h");
    const char *targets[5] = {"regular.h", path_join(root, "regular.h"), "../sdk-other/outside.h", path_join(sibling, "outside.h"), "missing.h"};
    for (int i = 0; i < 5; i++) {
        int made = make_link(targets[i], link, false);
        if (made < 0) {
            printf("windows_test: SKIP test_inventory_links: Windows denied symbolic-link creation (privilege not held)\n");
            remove_tree(dir);
            return;
        }
        CHECK(made == 1);
        if (made != 1) break;
        char *re = NULL;
        char *raw = msvc_read_link(link, &re);
        CHECK(re == NULL);
        CHECK(nz(raw)[0]);
        if (i == 0) CHECK_STR(raw, targets[i]);
        Managed_Msvc_Files files = {0};
        char *error = msvc_inventory(root, "", &files);
        if (i == 0 || i == 1) {
            CHECK(error == NULL);
            CHECK(files.len == 2);
        } else {
            CHECK(error != NULL);
            CHECK(error && contains(error, link));
        }
        CHECK(remove_link(link, false));
    }
    char *bridge = path_join(root, "bridge");
    CHECK(make_link(sibling, bridge, true) == 1);
    CHECK(make_link("bridge/outside.h", link, false) == 1);
    char *e1 = NULL, *e2 = NULL;
    char *resolved = msvc_final_path(link, &e1);
    char *base = msvc_final_path(root, &e2);
    CHECK(e1 == NULL && e2 == NULL);
    CHECK(!msvc_path_within(base, resolved, false));
    Managed_Msvc_Files files = {0};
    CHECK(msvc_inventory(root, "", &files) != NULL);
    CHECK(remove_link(link, false));
    CHECK(remove_link(bridge, true));
    remove_tree(dir);
}

static void test_digest_and_lock(void) {
    CHECK_STR(msvc_digest("", 0), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_STR(msvc_digest("abc", 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    char *million = xmalloc(1000000);
    memset(million, 'a', 1000000);
    CHECK_STR(msvc_digest(million, 1000000), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    char *dir = test_dir("cross-digest-");
    char *p = path_join(dir, "f");
    put(p, "abc");
    char *e = NULL;
    CHECK_STR(msvc_file_digest(p, &e), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(e == NULL);
    /* Exercise UTF-8 filenames through shared filesystem helpers, not fopen or
       rename's ANSI Windows paths. Native Windows execution is still separate. */
    char *unicode = path_join(dir, "sdk-\xc3\xa9-\xe4\xb8\xad");
    CHECK(move_path(p, unicode));
    CHECK(path_exists(unicode));
    CHECK_STR(msvc_file_digest(unicode, &e), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(e == NULL);
    msvc_file_digest(path_join(dir, "missing"), &e);
    CHECK(e != NULL);
    remove_tree(dir);

    /* The embedded lock is exactly the checked-in lock file. */
    char *file = slurp("msvc.lock.json");
    CHECK(file != NULL);
    if (file) CHECK_STR(file, MSVC_LOCK_TEXT);
    char *spin = slurp("deps/winpthreads/spinlock.c");
    CHECK(spin != NULL);
    if (spin) CHECK_STR(spin, MINGW_SPINLOCK_SOURCE);

    Managed_Msvc_Lock lock = msvc_lock(&e);
    CHECK(e == NULL);
    CHECK(nz(lock.resource_version)[0] && nz(lock.crt_version)[0] && nz(lock.sdk_version)[0] && nz(lock.license_version)[0]);
    CHECK(lock.license_urls.len >= 1);
    CHECK(lock.host_count >= 1);
    for (size_t i = 0; i < lock.host_count; i++) {
        CHECK(starts_with(lock.hosts[i].url, "https://") || !nz(lock.hosts[i].url)[0]);
        CHECK(strlen(nz(lock.hosts[i].sha256)) == 64 || !nz(lock.hosts[i].url)[0]);
    }
    CHECK(nz(lock.manifest_json)[0] == '{');
    char *receipt = msvc_license_receipt(lock);
    CHECK(starts_with(receipt, "cross_cc Microsoft license acceptance v1\nlock-sha256="));
    CHECK(contains(receipt, "eligibility-confirmed=yes\n"));
}

static void test_safe_paths_and_layout(void) {
    CHECK(msvc_safe_relative("sdk/include/ucrt/stdio.h"));
    CHECK(!msvc_safe_relative(""));
    CHECK(!msvc_safe_relative("/abs"));
    CHECK(!msvc_safe_relative("a/../b"));
    CHECK(!msvc_safe_relative("a//b"));
    CHECK(!msvc_safe_relative("a\\b"));
    CHECK(!msvc_safe_relative("C:/x"));
    CHECK(!msvc_safe_relative("./a"));
    Target t = {0};
    t.arch = "thumb";
    Msvc_Libc l = msvc_managed_layout("/r", t);
    CHECK_STR(l.crt_dir, "/r/sdk/lib/ucrt/aarch");
    CHECK_STR(l.sys_include_dir, "/r/crt/include");
    CHECK(!msvc_layout_valid(l));
}

static void test_cached_integrity(void) {
    char *dir = test_dir("cross-msvc-cache-");
    Toolchain tc = {0};
    tc.cache_dir = dir;
    Target t = {0};
    t.arch = "x86_64"; t.os = "windows"; t.abi = "msvc";
    char *root = msvc_resource_dir(tc, t);
    char *e = NULL;
    msvc_cached(tc, t, &e);
    CHECK(e && contains(e, "not installed"));
    /* Build a complete fake installation, a receipt for it, and check each tamper fails closed. */
    ensure_dir(root);
    const char *files[] = {"sdk/include/ucrt/stdio.h", "crt/include/vcruntime.h", "sdk/include/um/windows.h", "sdk/include/shared/winerror.h",
        "sdk/lib/ucrt/x86_64/ucrt.lib", "crt/lib/x86_64/libcmt.lib", "sdk/lib/um/x86_64/kernel32.lib"};
    for (int i = 0; i < 7; i++) {
        char *p = path_join(root, files[i]);
        ensure_dir(path_dir(p));
        put(p, files[i]);
    }
    Managed_Msvc_Files inv = {0};
    char *ie = msvc_inventory(root, "", &inv);
    CHECK(ie == NULL);
    CHECK(inv.len == 7);
    char *receipt = msvc_receipt_json("x86_64", &inv);
    put(path_join(root, "receipt.json"), receipt);
    char *marker = msvc_digest(receipt, strlen(receipt));
    put(path_join(root, "complete"), marker);
    e = NULL;
    msvc_cached(tc, t, &e);
    CHECK(e == NULL);
    /* Wrong architecture receipt. */
    Target other = t;
    other.arch = "x86";
    e = NULL;
    msvc_cached(tc, other, &e);
    CHECK(e != NULL);
    /* Changed file contents (size) and a deleted file are both detected. */
    put(path_join(root, "crt/lib/x86_64/libcmt.lib"), "changed contents");
    e = NULL;
    msvc_cached(tc, t, &e);
    CHECK(e && contains(e, "missing or changed"));
    put(path_join(root, "crt/lib/x86_64/libcmt.lib"), "crt/lib/x86_64/libcmt.lib");
    /* A tampered receipt no longer matches the completion marker. */
    put(path_join(root, "receipt.json"), str_fmt("%s ", receipt));
    e = NULL;
    msvc_cached(tc, t, &e);
    CHECK(e && contains(e, "corrupt"));
    remove_tree(dir);
}

static const char manifest_bytes[] = "{\"packages\":[]}";
static const char *manifest_download, *manifest_channel;
static int manifest_fetches, manifest_helpers, manifest_tamper;

static const char *managed_test_arg(StrVec cmd, const char *name) {
    for (size_t i = 0; i + 1 < cmd.len; i++) if (streq(cmd.data[i], name)) return cmd.data[i + 1];
    CHECK(false);
    return "";
}

static int managed_test_command(StrVec cmd, bool verbose) {
    (void)verbose;
    if (cmd.len && streq(cmd.data[0], "curl")) {
        manifest_fetches++;
        CHECK_STR(cmd.data[cmd.len - 1], "https://example.invalid/VisualStudio.vsman");
        CHECK_STR(managed_test_arg(cmd, "--proto"), "=https");
        CHECK_STR(managed_test_arg(cmd, "--proto-redir"), "=https");
        if (manifest_download) put(managed_test_arg(cmd, "-o"), manifest_download);
        return 0;
    }
    manifest_helpers++;
    CHECK(ends_with(cmd.data[0], "libexec/xwin" NATIVE_EXE_SUFFIX));
    CHECK_STR(cmd.data[1], "--accept-license");
    CHECK_STR(slurp(managed_test_arg(cmd, "--manifest")), manifest_channel);
    char *cached = path_join(managed_test_arg(cmd, "--cache-dir"), "dl",
        str_fmt("pkg_manifest_%s.vsman", msvc_digest(manifest_bytes, strlen(manifest_bytes))));
    /* This assertion binds the verified download to xwin's actual input path,
       not to a separate file that the helper never reads. */
    CHECK_STR(slurp(cached), manifest_bytes);
    char *output = (char *)managed_test_arg(cmd, "--output");
    CHECK(!path_exists(path_join(output, "receipt.json")));
    const char *files[] = {"sdk/include/ucrt/stdio.h", "crt/include/vcruntime.h", "sdk/include/um/windows.h", "sdk/include/shared/winerror.h",
        "sdk/lib/ucrt/x86_64/ucrt.lib", "crt/lib/x86_64/libcmt.lib", "sdk/lib/um/x86_64/kernel32.lib"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char *p = path_join(output, files[i]);
        ensure_dir(path_dir(p));
        put(p, files[i]);
    }
    /* Model xwin replacing/refetching or losing the seeded cache. */
    if (manifest_tamper == 1) put(cached, "{\"packages\":[ ]}");
    if (manifest_tamper == 2) put(cached, "{\"packages\":{}}"); /* same size */
    if (manifest_tamper == 3) CHECK(remove_file(cached));
    return 0;
}

static char *managed_test_channel(const char *payload, int items, int payloads) {
    char *item = str_fmt("{\"id\":\"Microsoft.VisualStudio.Manifests.VisualStudio\",\"version\":\"test\",\"type\":\"Manifest\",\"payloads\":[%s%s%s]}",
        payload, payloads == 2 ? "," : "", payloads == 2 ? payload : "");
    return str_fmt("{\"channelItems\":[%s%s%s]}", item, items == 2 ? "," : "", items == 2 ? item : "");
}

static char *managed_test_payload(const char *hash_fields, const char *size_fields) {
    return str_fmt("{\"fileName\":\"VisualStudio.vsman\",\"url\":\"https://example.invalid/VisualStudio.vsman\"%s%s}", hash_fields, size_fields);
}

static void managed_test_install(const char *channel, const char *download, int tamper, const char *version,
    const char *want_error, int want_fetches, int want_helpers) {
    char *dir = test_dir("cross-msvc-manifest-");
    Toolchain tc = {0}; tc.root = dir; tc.cache_dir = path_join(dir, "cache");
    Target t = {0}; t.arch = "x86_64"; t.os = "windows"; t.abi = "msvc";
    char *helper = path_join(dir, "libexec", "xwin" NATIVE_EXE_SUFFIX);
    ensure_dir(path_dir(helper));
    put(helper, "offline fake helper");
    Managed_Msvc_Helper h = {0}; h.binary_sha256 = msvc_digest("offline fake helper", strlen("offline fake helper"));
    char *host = (char *)msvc_host();
    Managed_Msvc_Lock lock = {0};
    lock.hosts = &h; lock.host_names = &host; lock.host_count = 1;
    lock.helper_version = (char *)version; lock.manifest_json = (char *)channel;
    lock.sdk_version = "test-sdk"; lock.crt_version = "test-crt";
    manifest_channel = channel; manifest_download = download; manifest_tamper = tamper;
    manifest_fetches = manifest_helpers = 0;
    char *error = msvc_install((Options){0}, tc, t, lock);
    CHECK(manifest_fetches == want_fetches);
    CHECK(manifest_helpers == want_helpers);
    char *dest = msvc_resource_dir(tc, t);
    if (want_error) {
        CHECK(error && contains(error, want_error));
        CHECK(!path_exists(dest));
        CHECK(!path_exists(path_join(dest, "receipt.json")));
        CHECK(!path_exists(path_join(dest, "complete")));
        char *parent = path_dir(dest);
        if (is_dir(parent)) {
            Msvc_Dir_Entries entries = {0};
            CHECK(msvc_read_dir(parent, &entries));
            CHECK(entries.len == 0); /* no partial stage or hidden receipt */
        }
    } else {
        CHECK(error == NULL);
        CHECK(path_exists(path_join(dest, "receipt.json")));
        CHECK(path_exists(path_join(dest, "complete")));
        msvc_cached(tc, t, &error);
        CHECK(error == NULL);
    }
    remove_tree(dir);
}

static void test_manifest_authentication(void) {
    char *hash = str_fmt(",\"sha256\":\"%s\"", msvc_digest(manifest_bytes, strlen(manifest_bytes)));
    char *size = str_fmt(",\"size\":%zu", strlen(manifest_bytes));
    char *payload = managed_test_payload(hash, size);
    char *channel = managed_test_channel(payload, 1, 1);
#define INSTALL(c, d, t, e, f, h) managed_test_install(c, d, t, "xwin-0.10.0", e, f, h)
    INSTALL(channel, manifest_bytes, 0, NULL, 1, 1);
    INSTALL(channel, "{\"packages\":{}}", 0, "SHA-256 mismatch", 1, 0);
    INSTALL(channel, "wrong size", 0, "SHA-256 mismatch", 1, 0);
    INSTALL(channel, NULL, 0, "cannot open", 1, 0);
    /* Correct SHA but incorrect pinned size must still refuse before helper. */
    INSTALL(managed_test_channel(managed_test_payload(hash, ",\"size\":1"), 1, 1), manifest_bytes, 0, "byte-size mismatch", 1, 0);
    INSTALL(channel, manifest_bytes, 1, "byte-size mismatch", 1, 1);
    INSTALL(channel, manifest_bytes, 2, "SHA-256 mismatch", 1, 1);
    INSTALL(channel, manifest_bytes, 3, "manifest is missing", 1, 1);
    const char *bad_hashes[] = {"", ",\"sha256\":null", ",\"sha256\":\"\"", ",\"sha256\":\"gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg\""};
    for (size_t i = 0; i < sizeof(bad_hashes) / sizeof(bad_hashes[0]); i++)
        INSTALL(managed_test_channel(managed_test_payload(bad_hashes[i], size), 1, 1), manifest_bytes, 0, "download pin", 0, 0);
    const char *bad_sizes[] = {"", ",\"size\":null", ",\"size\":\"15\"", ",\"size\":-1", ",\"size\":0", ",\"size\":1.5", ",\"size\":1e1", ",\"size\":999999999999999999999999"};
    for (size_t i = 0; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); i++)
        INSTALL(managed_test_channel(managed_test_payload(hash, bad_sizes[i]), 1, 1), manifest_bytes, 0, "pin", 0, 0);
    INSTALL(managed_test_channel(payload, 2, 1), manifest_bytes, 0, "channel", 0, 0);
    INSTALL(managed_test_channel(payload, 1, 2), manifest_bytes, 0, "selection", 0, 0);
    INSTALL(managed_test_channel(managed_test_payload(str_fmt("%s%s", hash, hash), size), 1, 1), manifest_bytes, 0, "duplicate", 0, 0);
    INSTALL(managed_test_channel(managed_test_payload(hash, str_fmt("%s%s", size, size)), 1, 1), manifest_bytes, 0, "duplicate", 0, 0);
    INSTALL("{\"channelItems\":[],\"channelItems\":[]}", manifest_bytes, 0, "duplicate", 0, 0);
    managed_test_install(channel, manifest_bytes, 0, "xwin-future", "cache contract", 0, 0);
#undef INSTALL
}

static void test_source_scan(void) {
#define NEEDS(text) msvc_source_needs_sdk((const uint8_t *)(text), strlen(text))
    CHECK(NEEDS("#include <stdio.h>\n"));
    CHECK(NEEDS("  # include \"x.h\"\n"));
    CHECK(NEEDS("%:include <x>\n"));
    CHECK(NEEDS("#import <x>\n"));
    CHECK(NEEDS("#include_next <x>\n"));
    CHECK(NEEDS("/* c */ #include <x>\n"));
    CHECK(NEEDS("#\\\ninclude <x>\n"));
    CHECK(NEEDS("\xef\xbb\xbf#include <x>\n"));
    CHECK(!NEEDS("int main(void){return 0;}\n"));
    CHECK(!NEEDS("// #include <x>\nint x;\n"));
    CHECK(!NEEDS("/* #include <x>\n#include <y> */ int x;\n"));
    CHECK(!NEEDS("const char *s = \"\\n#include <x>\";\n"));
    CHECK(!NEEDS("const char *s = R\"d(\n#include <x>\n)d\";\n"));
    CHECK(!NEEDS("int x = 1; #include <x>\n"));
    CHECK(!NEEDS("#define X 1\n#if X\n#endif\n"));
    CHECK(NEEDS("#define X 1\n#include <x>\n"));
    CHECK(!NEEDS(""));
}

static StrVec v(const char *first, ...) {
    StrVec r = {0};
    va_list a;
    va_start(a, first);
    for (const char *s = first; s; s = va_arg(a, const char *)) sv_push(&r, s);
    va_end(a);
    return r;
}

static bool has(StrVec x, const char *s) { for (size_t i = 0; i < x.len; i++) if (streq(x.data[i], s)) return true; return false; }

static void test_link_arguments(void) {
    Target msvc = {0}, gnu = {0};
    msvc.os = "windows"; msvc.abi = "msvc"; msvc.arch = "x86_64";
    gnu.os = "windows"; gnu.abi = "gnu";
    Options o = {0};
    char *e = NULL;
    StrVec in = v("-o", "a.exe", "x.c", NULL);
    StrVec out = msvc_link_arguments(o, gnu, in, &e);
    CHECK(e == NULL && out.len == in.len); /* non-MSVC targets pass through. */

    in = v("x.c", "-Wl,--stack,1048576,--subsystem,windows,-u,main", "-Wl,--entry=mainCRTStartup", NULL);
    e = NULL;
    out = msvc_link_arguments(o, msvc, in, &e);
    CHECK(e == NULL);
    CHECK(has(out, "x.c"));
    CHECK(has(out, "/stack:1048576"));
    CHECK(has(out, "/subsystem:windows,6.0"));
    CHECK(has(out, "/include:main"));
    CHECK(has(out, "/entry:mainCRTStartup"));
    CHECK(has(out, "-Xlinker"));
    CHECK(!has(out, "--stack"));

    in = v("-Wl,--bogus-option", NULL);
    e = NULL;
    out = msvc_link_arguments(o, msvc, in, &e);
    CHECK(e && contains(e, "unsupported linker arg: --bogus-option"));

    in = v("-Wl,--subsystem,nonsense", NULL);
    e = NULL;
    msvc_link_arguments(o, msvc, in, &e);
    CHECK(e && contains(e, "invalid: --subsystem"));

    in = v("-fno-PIC", "x.c", NULL);
    e = NULL;
    msvc_link_arguments(o, msvc, in, &e);
    CHECK(e && contains(e, "position independent"));

    in = v("-Wl,--major-subsystem-version,70000,--subsystem,console", NULL);
    e = NULL;
    msvc_link_arguments(o, msvc, in, &e);
    CHECK(e != NULL);

    in = v("-Xlinker", NULL);
    e = NULL;
    msvc_link_arguments(o, msvc, in, &e);
    CHECK(e && contains(e, "expected parameter after -Xlinker"));

    in = v("x.c", "exports.def", "--", "-Wl,literal", NULL);
    e = NULL;
    out = msvc_link_arguments(o, msvc, in, &e);
    CHECK(e == NULL);
    CHECK(has(out, "/def:exports.def"));
    CHECK(has(out, "-Wl,literal"));
    CHECK(msvc_link_subsystem("console") && msvc_link_subsystem("efi_rom") && !msvc_link_subsystem("x"));
}

static void test_mingw_names(void) {
    CHECK_STR(output_root_name("dir/hello.c", ""), "hello");
    CHECK_STR(output_root_name("x", "libfoo.so"), "foo");
    CHECK_STR(output_root_name("x", "libfoo.so.1"), "foo");
    CHECK_STR(output_root_name("x", "libfoo.so.1.2"), "foo.so.1");
    CHECK_STR(output_root_name("x", "bar"), "bar");
    CHECK_STR(output_root_name("x", "libfoo"), "foo");
    Target t = {0};
    t.arch = "x86";
    CHECK_STR(mingw_arch_directory(t), "lib32");
    CHECK_STR(mingw_coff_machine(t), "x86");
    t.arch = "thumb";
    CHECK_STR(mingw_arch_directory(t), "libarm32");
    CHECK_STR(mingw_coff_machine(t), "arm");
    t.arch = "aarch64";
    CHECK_STR(mingw_arch_directory(t), "libarm64");
    CHECK_STR(mingw_coff_machine(t), "arm64");
    t.arch = "x86_64";
    CHECK_STR(mingw_arch_directory(t), "lib64");
    CHECK_STR(mingw_coff_machine(t), "x64");
    CHECK(mingw32_generic_src_len == 377 && mingw32_x86_src_len > 0 && mingw32_arm_src_len > 0 && mingw32_x86_32_src_len > 0 && mingw32_winpthreads_src_len > 0);
    CHECK(mingw_always_link_libs_len == 20);

    char *dir = test_dir("cross-mingw-");
    Toolchain tc = {0};
    tc.lib_dir = path_join(dir, "lib");
    char *defs = path_join(tc.lib_dir, "libc", "mingw", "lib-common");
    ensure_dir(defs);
    ensure_dir(path_join(tc.lib_dir, "libc", "mingw", "lib64"));
    put(path_join(defs, "kernel32.def"), "LIBRARY kernel32\n");
    put(path_join(defs, "extra.def.in"), "LIBRARY extra\n");
    put(path_join(tc.lib_dir, "libc", "mingw", "lib64", "kernel32.def"), "LIBRARY arch\n");
    char *found = mingw_import_definition("kernel32", t, tc);
    CHECK(ends_with(found, "lib64/kernel32.def"));
    CHECK(ends_with(mingw_import_definition("extra", t, tc), "lib-common/extra.def.in"));
    CHECK_STR(mingw_import_definition("../kernel32", t, tc), "");
    CHECK_STR(mingw_import_definition("", t, tc), "");
    CHECK_STR(mingw_import_definition("missing", t, tc), "");
    remove_tree(dir);
}

int main(void) {
    test_include_order();
    test_versions();
    test_libc_file();
    test_libcxx_headers();
    test_libcxx_cache_kind();
    test_libcxx_nonlink_modes();
    test_reparse_bounds();
    test_inventory_links();
    test_digest_and_lock();
    test_safe_paths_and_layout();
    test_cached_integrity();
    test_manifest_authentication();
    test_source_scan();
    test_link_arguments();
    test_mingw_names();
    printf("windows_test: %d checks, %d failures (Linux host; Windows-native paths not run)\n", checks, failures);
    return failures ? 1 : 0;
}
