/* Build configuration for one unmodified, authenticated LLVM dependency TU
 * (llvm/lib/Support/Path.cpp). This file holds functions only and is included
 * by tools/build.c and by windows_path_test.c; it has no main. */
#ifndef CROSS_CC_WINDOWS_PATH_C
#define CROSS_CC_WINDOWS_PATH_C

#include "../common.h"

#define WP_SOURCE_SHA256 "372ef97699b0019f6ce489e4f71f8c60f63a22ee901ee55c8dd299baa07e70c6"
#define WP_CONFIG                                                                                  \
    "#include \"llvm/Config/llvm-config.h\"\n"                                                     \
    "#define LLVM_WINDOWS_PREFER_FORWARD_SLASH 1\n"                                                \
    "#define HAVE__CHSIZE_S 1\n"

/* Path.cpp's only private configuration tests on Windows are the slash option
 * and HAVE__CHSIZE_S (Windows/Path.inc). Everything else is exported by the SDK.
 * Do not compile this object for Unix or change the dependency's implementation. */
static const char *const wp_flags[] = {"-DNDEBUG", "-DUNICODE", "-D_UNICODE", "-D_CRT_SECURE_NO_WARNINGS", "-D_SCL_SECURE_NO_WARNINGS", "-D_ITERATOR_DEBUG_LEVEL=0", "-D_MT", "-fms-runtime-lib=static", "-fno-sanitize=all", "-fno-lto", "-g0", NULL};

/* Stable file-macro/debug names. Anonymous-namespace discriminators hash the
 * file name before prefix maps: callers use a private cwd plus "Path.cpp". */
static void wp_prefix_flags(const char *dir, Strs *out) {
    strs_addf(out, "-ffile-prefix-map=%s=/llvm-project-22.1.8", dir);
    strs_addf(out, "-ffile-prefix-map=%s=/cross_cc_config", PJ(dir, "gen"));
}

static int wp_include_option(const char *option) {
    static const char *const names[] = {"-I", "-isystem", "-isystem-after", "-cxx-isystem", "-iquote", "-idirafter", "-iframework", "-F", "-imsvc", "/imsvc", NULL};
    for (int i = 0; names[i]; i++) {
        if (strcmp(option, names[i]) == 0) return 1;
    }
    return 0;
}

/* Clang reads these include spellings relative to its sysroot, not its
 * working directory; keep that meaning when the cwd changes. */
static char *wp_rooted(const char *root, const char *value, int include) {
    if (include && (value[0] == '=' || starts_with(value, "$SYSROOT"))) return xstrdup(value);
    if (is_abs(value)) return xstrdup(value);
    return PJ(root, value);
}

/* Preserve checkout-relative include/SDK semantics when the Path-only compile
 * runs beside Path.cpp. Response files and cwd overrides cannot preserve that
 * contract implicitly; reject them rather than silently reinterpret paths.
 * Rewrites args[0..n) in place. */
static void wp_rooted_args(char **args, int n, const char *root) {
    static const char *const separate_options[] = {"-I", "-isystem", "-isystem-after", "-cxx-isystem", "-imsvc", "/imsvc", "-iquote", "-idirafter", "-isysroot", "--sysroot", "-include", "-include-pch", "-imacros", "-F", "-iframework", "-ivfsoverlay", "-resource-dir", "-B", NULL};
    /* Longer spellings first: -isystem-after before -isystem before -I. */
    static const char *const joined_prefixes[] = {"--sysroot=", "-isysroot=", "-resource-dir=", "-ivfsoverlay=", "-include-pch=", "-include=", "-imacros=", "-isystem-after", "-cxx-isystem", "-imsvc", "/imsvc", "-isystem", "-iquote", "-idirafter", "-iframework", "-I", "-F", "-B", NULL};
    if (n > 0 && strpbrk(args[0], "/\\")) args[0] = wp_rooted(root, args[0], 0);
    for (int i = 1; i < n; i++) {
        const char *a = args[i];
        if (a[0] == '@' || strcmp(a, "-working-directory") == 0 || starts_with(a, "-working-directory=")) {
            fail("Path.cpp flags must expand response files and cannot override working directory: %s", a);
        }
        /* These driver/linker switches are not joined -B search paths. */
        if (!strcmp(a, "-Bstatic") || !strcmp(a, "-Bdynamic") || !strcmp(a, "-Bsymbolic")) continue;
        int separate = 0;
        for (int k = 0; separate_options[k]; k++) {
            if (strcmp(a, separate_options[k]) == 0) {
                if (i + 1 >= n) fail("missing Path.cpp flag operand: %s", a);
                i++;
                args[i] = wp_rooted(root, args[i], wp_include_option(a));
                separate = 1;
                break;
            }
        }
        if (separate) continue;
        for (int k = 0; joined_prefixes[k]; k++) {
            size_t pl = strlen(joined_prefixes[k]);
            if (starts_with(a, joined_prefixes[k]) && strlen(a) > pl) {
                args[i] = sfmt("%s%s", joined_prefixes[k], wp_rooted(root, a + pl, wp_include_option(joined_prefixes[k])));
                break;
            }
        }
    }
}

/* Authenticate and extract the three inputs from the pinned LLVM source
 * archive into a fresh dir; write the generated config and an inputs receipt.
 * Always re-extracts: a previously extracted source is not trusted. No network,
 * host C++ compiler, prebuilt Path object, or shared SDK mutation. */
static void wp_prepare(const char *archive, const char *expected, const char *strip_prefix, const char *dir, char **source, char **gen) {
    if (strcmp(sha256_file(archive), expected) != 0) fail("LLVM source archive SHA-256 mismatch: %s", archive);
    if (exists(dir)) fail("input stage already exists: %s", dir);
    mkdirs(dir);
    Strs args = {0};
    strs_add(&args, "tar");
    strs_add(&args, "-xf");
    strs_add(&args, archive);
    strs_add(&args, "-C");
    strs_add(&args, dir);
    strs_add(&args, "--strip-components=1");
    static const char *const members[] = {"llvm/lib/Support/Path.cpp", "llvm/lib/Support/Windows/Path.inc", "llvm/LICENSE.TXT", NULL};
    for (int i = 0; members[i]; i++) strs_addf(&args, "%s/%s", strip_prefix, members[i]);
    if (run_cmd(args.v, NULL, NULL) != 0) fail("extract LLVM Path inputs failed");
    *source = PJ(dir, "llvm", "lib", "Support", "Path.cpp");
    if (strcmp(sha256_file(*source), WP_SOURCE_SHA256) != 0) fail("unsupported Path.cpp; expected pinned LLVM 22.1.8 source");
    *gen = PJ(dir, "gen");
    mkdirs(PJ(*gen, "llvm", "Config"));
    char *config = PJ(*gen, "llvm", "Config", "config.h");
    write_file(config, WP_CONFIG);
    write_file(PJ(dir, "inputs.txt"),
               sfmt("recipe llvm-windows-forward-path-v1\narchive-sha256 %s\nsource-sha256 %s\nwindows-path-inc-sha256 %s\nconfig-sha256 %s\noption LLVM_WINDOWS_PREFER_FORWARD_SLASH=1\n",
                    expected, sha256_file(*source), sha256_file(PJ(dir, "llvm", "lib", "Support", "Windows", "Path.inc")), sha256_file(config)));
}

/* A supported native build checks this receipt before enabling the Windows
 * canonical-path gate. The receipt binds the object and the link flags. */
static void wp_receipt(const char *dir, const char *object, const char *flags) {
    write_file(PJ(dir, "backend.sha256"), sfmt("%s\n%s", sha256_file(object), sha256_file(flags)));
}
static int wp_verified(const char *dir, const char *object, const char *flags) {
    char *receipt = PJ(dir, "backend.sha256");
    if (!is_file(receipt) || !is_file(object) || !is_file(flags)) return 0;
    char *data = read_file(receipt, NULL);
    return strcmp(data, sfmt("%s\n%s", sha256_file(object), sha256_file(flags))) == 0;
}

#endif
