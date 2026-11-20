/* Per-host self-hosting recipes for tools/build.c `prepare`. Included by
 * build.c after its Linux recipe helpers (host_bridge_args/host_runtime_args),
 * which stay the linux-amd64 recipe unchanged.
 *
 * Every host compiles with the seed only: raw `cross_clang` (argv0 alias of
 * the seed) for C++ against the host LLVM SDK's C++ ABI, the seed's `cc`
 * driver for host executables. Hosts:
 *   linux-amd64   pinned official LLVM, GNU ABI via tools/linux-release-deps
 *   darwin-arm64  pinned official LLVM + the system Apple SDK (libc++)
 *   darwin-amd64  LLVM_PREFIX from tools/macos-x64-llvm (no official pin)
 *   windows-amd64 pinned official LLVM + a user-supplied MSVC/Windows SDK
 *                 (xwin splat layout; license acceptance is the user's act)
 * Anything else: --host-config=FILE with a complete recipe. */

/* Select a supplied MSVC splat explicitly. INCLUDE/LIB also serve raw Clang,
 * but cross_cc's system detector uses registry discovery on Windows and rejects
 * discovery elsewhere: its documented ZIG_LIBC file is needed to bind this SDK.
 * No managed cache, downloader, or license-consent flag is involved. */
static void sp_msvc_env(Strs *env, const char *sdk, const char *libc_file) {
    static const char *const keys[] = {"INCLUDE", "LIB", "CROSS_CC_MSVC", "ZIG_LIBC", NULL};
    Strs clean = {0};
    for (int i = 0; i < env->n; i++) {
        int skip = 0;
        for (int k = 0; keys[k]; k++) {
            size_t n = strlen(keys[k]);
            if (strlen(env->v[i]) <= n || env->v[i][n] != '=') continue;
            int match = 1;
            for (size_t j = 0; j < n; j++) if (toupper((unsigned char)env->v[i][j]) != keys[k][j]) match = 0;
            if (match) skip = 1;
        }
        if (!skip) strs_add(&clean, env->v[i]);
    }
    mkdirs(path_dir(libc_file));
    write_file(libc_file, sfmt("include_dir=%s\nsys_include_dir=%s\ncrt_dir=%s\nmsvc_lib_dir=%s\nkernel32_lib_dir=%s\ncc_dir=\ndarwin_sdk_dir=\n", PJ(sdk, "sdk", "include", "ucrt"), PJ(sdk, "crt", "include"), PJ(sdk, "sdk", "lib", "ucrt", "x86_64"), PJ(sdk, "crt", "lib", "x86_64"), PJ(sdk, "sdk", "lib", "um", "x86_64")));
    strs_addf(&clean, "INCLUDE=%s;%s;%s;%s", PJ(sdk, "crt", "include"), PJ(sdk, "sdk", "include", "ucrt"), PJ(sdk, "sdk", "include", "um"), PJ(sdk, "sdk", "include", "shared"));
    strs_addf(&clean, "LIB=%s;%s;%s", PJ(sdk, "crt", "lib", "x86_64"), PJ(sdk, "sdk", "lib", "ucrt", "x86_64"), PJ(sdk, "sdk", "lib", "um", "x86_64"));
    strs_add(&clean, "CROSS_CC_MSVC=system");
    strs_addf(&clean, "ZIG_LIBC=%s", libc_file);
    *env = clean;
}

#ifndef CROSS_CC_MSVC_ENV_ONLY
typedef struct {
    const char *host;         /* host_key() */
    const char *clang_target; /* raw cross_clang --target */
    const char *cc_target;    /* seed `cc -target` (linux appends .glibc) */
    const char *cpu_flag;     /* -march= or -mcpu= */
    const char *cpu;          /* release.c cpu default for the host */
    const char *kind;         /* linux | apple | msvc */
} SelfhostHost;

static const SelfhostHost SELFHOST_HOSTS[] = {
    {"linux-amd64", "x86_64-unknown-linux-gnu", "x86_64-linux-gnu", "-march", "x86-64-v2", "linux"},
    {"darwin-arm64", "arm64-apple-macosx11.0.0", "aarch64-macos", "-mcpu", "generic", "apple"},
    {"darwin-amd64", "x86_64-apple-macosx11.0.0", "x86_64-macos", "-march", "x86-64-v2", "apple"},
    {"windows-amd64", "x86_64-pc-windows-msvc", "x86_64-windows-msvc", "-march", "x86-64-v2", "msvc"},
    {NULL, NULL, NULL, NULL, NULL, NULL},
};

static const SelfhostHost *selfhost_host(const char *key) {
    for (int i = 0; SELFHOST_HOSTS[i].host; i++) {
        if (!strcmp(SELFHOST_HOSTS[i].host, key)) return &SELFHOST_HOSTS[i];
    }
    return NULL;
}

/* MSVC/Windows SDK layout (xwin splat): headers and x86_64 import libraries. */
static const char *const MSVC_INCLUDES[] = {"crt/include", "sdk/include/ucrt", "sdk/include/um", "sdk/include/shared", NULL};
static const char *const MSVC_LIBDIRS[] = {"crt/lib/x86_64", "sdk/lib/ucrt/x86_64", "sdk/lib/um/x86_64", NULL};

/* Bridge compile flags with {sdk}/{root} placeholders. */
static Strs sp_bridge_args(const SelfhostHost *h) {
    if (!strcmp(h->kind, "linux")) return host_bridge_args("{sdk}", "{root}");
    Strs a = {0};
    if (!strcmp(h->kind, "apple")) {
        /* The SDK's libc++ headers: the official LLVM SDK's C++ ABI. */
        strs_add(&a, "-isysroot");
        strs_add(&a, "{sdk}");
        return a;
    }
    strs_add(&a, "-nostdinc");
    strs_add(&a, "-isystem");
    strs_add(&a, "{root}/lib/include");
    for (int i = 0; MSVC_INCLUDES[i]; i++) {
        strs_add(&a, "-isystem");
        strs_addf(&a, "{sdk}/%s", MSVC_INCLUDES[i]);
    }
    /* Official Windows LLVM libraries use the static CRT (/MT). */
    static const char *const w[] = {"-D_CRT_SECURE_NO_WARNINGS", "-D_SCL_SECURE_NO_WARNINGS", "-D_ITERATOR_DEBUG_LEVEL=0", "-D_MT", "-fms-runtime-lib=static", NULL};
    strs_addv(&a, w);
    return a;
}

/* llvm-config --system-libs as link arguments. Absolute paths from the LLVM
 * builder's machine are refused, except zstd (pinned support prefix) and
 * libxml2 (built by prepare on Windows); either archive may be NULL. */
static void sp_system_libs(Strs *a, const char *system_libs, const char *zstd_archive, const char *xml2_archive, const char *host) {
    Strs t = split_fields(system_libs);
    for (int i = 0; i < t.n; i++) {
        const char *x = t.v[i];
        if (contains(x, "zstd")) {
            if (!zstd_archive) fail("LLVM SDK for %s needs zstd (%s); pass --deps-prefix=DIR holding lib/libzstd.a (tools/macos-x64-llvm builds the pinned one)", host, x);
            strs_add(a, zstd_archive);
        } else if (xml2_archive && contains(x, "xml2")) {
            strs_add(a, xml2_archive);
        } else if (ends_with(x, ".lib")) {
            strs_addf(a, "-l%.*s", (int)(strlen(x) - 4), x);
        } else if (x[0] == '-') {
            strs_add(a, x);
        } else {
            fail("llvm-config --system-libs names a builder-machine file %s; supply --host-config=FILE with an explicit recipe", x);
        }
    }
}

/* Final link recipe tail after the LLVM archives, with placeholders. */
static void sp_link_tail(const SelfhostHost *h, Strs *a, const char *system_libs, const char *zstd_archive, const char *xml2_archive) {
    if (!strcmp(h->kind, "linux")) {
        host_runtime_args(a, "{sdk}");
        return;
    }
    if (!strcmp(h->kind, "apple")) {
        sp_system_libs(a, system_libs, zstd_archive, xml2_archive, h->host);
        strs_add(a, "-L{sdk}/usr/lib");
        strs_add(a, "-lc++");
        return;
    }
    /* V1 appends compiler-rt before Clang's default CRT libraries. Own the /MT
     * CRT explicitly, before those appended runtimes, so its chkstk.obj supplies
     * both __chkstk and _alloca_probe. -nodefaultlibs reaches Clang; it does NOT
     * suppress cross_cc's automatic runtimes (debug objects still need UBSan).
     * -nostdlib would suppress UBSan too and break the O0 bootstrap. */
    strs_add(a, "-nodefaultlibs");
    strs_add(a, "{sdk}/crt/lib/x86_64/libcmt.lib");
    strs_add(a, "{sdk}/crt/lib/x86_64/libvcruntime.lib");
    strs_add(a, "{sdk}/crt/lib/x86_64/libcpmt.lib");
    strs_add(a, "{sdk}/sdk/lib/ucrt/x86_64/libucrt.lib");
    for (int i = 0; MSVC_LIBDIRS[i]; i++) strs_addf(a, "-L{sdk}/%s", MSVC_LIBDIRS[i]);
    sp_system_libs(a, system_libs, zstd_archive, xml2_archive, h->host);
    /* Clang normally supplies this OS import library with its default CRT. */
    strs_add(a, "-lkernel32");
}

/* Head of the final link recipe (release.c requires these exact inputs). */
static void sp_link_head(const SelfhostHost *h, Strs *a) {
    strs_add(a, "{object}");
    strs_add(a, "{version}");
    strs_add(a, "{lld_version}");
    if (!strcmp(h->kind, "msvc")) strs_add(a, "{windows_path}");
    if (!strcmp(h->kind, "linux")) strs_add(a, "-Wl,--start-group");
    strs_add(a, "{bridge}");
}

/* The host SDK input: support prefix (linux), Apple SDK, or MSVC splat. */
static int sp_sdk_ok(const SelfhostHost *h, const char *sdk, char **why) {
    if (!strcmp(h->kind, "apple")) {
        if (!is_file(PJ(sdk, "SDKSettings.json")) || !is_dir(PJ(sdk, "usr", "include", "c++", "v1"))) {
            *why = sfmt("%s is not a macOS SDK with libc++ headers (SDKSettings.json, usr/include/c++/v1)", sdk);
            return 0;
        }
        return 1;
    }
    if (!strcmp(h->kind, "msvc")) {
        static const char *const *lists[] = {MSVC_INCLUDES, MSVC_LIBDIRS};
        for (int k = 0; k < 2; k++) {
            for (int i = 0; lists[k][i]; i++) {
                if (!is_dir(PJ(sdk, lists[k][i]))) {
                    *why = sfmt("%s lacks %s: pass --sdk=DIR, an MSVC + Windows SDK in xwin splat layout whose Microsoft license you accepted yourself (build never accepts it for you)", sdk, lists[k][i]);
                    return 0;
                }
            }
        }
        return 1;
    }
    return 1;
}

/* Expand a recipe for the seed's own use: {root} is the seed's resources. */
static char *sp_expand(const char *line, const char *root, const char *sdk, const char *llvm, const char *native) {
    char *s = replace_all(line, "{root}", root);
    s = replace_all(s, "{sdk}", sdk ? sdk : "");
    s = replace_all(s, "{llvm}", llvm);
    return replace_all(s, "{native}", native);
}

/* Recipe file lines, comments and blanks dropped (as tools/release.c reads them). */
static Strs sp_recipe_lines(const char *file) {
    Strs out = {0};
    Strs lines = split_lines(read_file(file, NULL));
    for (int i = 0; i < lines.n; i++) {
        char *l = trim(lines.v[i]);
        if (*l && l[0] != '#') strs_add(&out, l);
    }
    return out;
}

/* Link inputs that only the release pipeline produces. */
static int sp_release_only(const char *l) {
    static const char *const ph[] = {"{object}", "{version}", "{lld_version}", "{windows_path}", "{bridge}", "{output}", "{stage}", NULL};
    for (int i = 0; ph[i]; i++) {
        if (strstr(l, ph[i])) return 1;
    }
    return 0;
}
#endif /* CROSS_CC_MSVC_ENV_ONLY */
