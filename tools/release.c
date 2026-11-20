/* Shared self-host build and release pipeline. An explicitly selected cross_cc
 * seed builds a fresh current host compiler, which builds each final compiler.
 * Production compilation and linking never invoke a system compiler or Make. */
#include "common.h"
#include "windows_path/windows_path.c"

static const char USAGE[] =
    "usage: build/tools/release [options]\n"
    "\n"
    "  --compiler=FILE       REQUIRED absolute selected cross_cc seed (never system CC)\n"
    "  --host-config=FILE    REQUIRED JSON host recipe; paths relative to checkout root\n"
    "  --profile=release|debug  affects BOTH C driver and C++ bridge (default release)\n"
    "  --build-only=FILE     publish native executable + FILE.build-manifest.txt; no packaging\n"
    "  --matrix=FILE         JSON {\"targets\":[{...}, ...]}: share one fresh host stage1\n"
    "  --target=linux-amd64|darwin-arm64|darwin-amd64|windows-amd64\n"
    "  --llvm-prefix=PATH    target LLVM 22.1.8 static SDK; native default from host config\n"
    "  --link-args=FILE      final link recipe, one literal argument per line\n"
    "  --link-mode=cc|lld    cross_cc driver (default) or direct embedded LLD\n"
    "  --linux-glibc=VERSION driver ABI baseline (default 2.34); SDK must also meet it\n"
    "  --cpu=NAME           target CPU (default x86-64-v2 on amd64, generic on arm64)\n"
    "  --native-dir=PATH     prepared authenticated C++ sources (default .cache/native)\n"
    "  --bridge=FILE        advanced explicit prebuilt TARGET bridge, never a host default;\n"
    "                       limits bridge verification to same-input identity, not convergence\n"
    "  --bridge-args=FILE   compile fresh bridge, one argument per line, target C++/CRT flags\n"
    "  --version-object=FILE  plain clang Version.cpp object for a prebuilt --bridge= (REQUIRED\n"
    "                        there); built from the unmodified pinned source with an empty\n"
    "                        VCSVersion.inc. Freshly compiled with --bridge-args.\n"
    "  --lld-version-object=FILE  plain lld Common/Version.cpp object for a prebuilt --bridge=\n"
    "                        (REQUIRED there, like --version-object); built from the unmodified pinned\n"
    "                        source with empty VCSVersion.inc and llvm/Support/VCSRevision.h so\n"
    "                        getLLDVersion() and the ELF .comment carry no repository/commit.\n"
    "                        Freshly compiled with --bridge-args.\n"
    "  --path-args=FILE      Windows Path.cpp compile recipe (target C++/CRT headers/flags),\n"
    "                        defaults to --bridge-args; REQUIRED for a cross release with\n"
    "                        a prebuilt bridge. Always compiles authenticated LLVM sources,\n"
    "                        never accepts an opaque prebuilt Path object. No network;\n"
    "                        compiler-rt.lock.json's LLVM archive must be in .cache/downloads.\n"
    "  --sdk=PATH            target platform SDK/sysroot; available to recipe as {sdk}\n"
    "  --output=DIR          new bundle directory and DIR.zip (default dist/cross_cc-TARGET)\n"
    "  --package-only=BIN    package an existing verified TARGET executable without rebuilding\n"
    "  --extra-licenses=DIR  notices for the selected target support SDK (not host defaults)\n"
    "  --msvc-helper=DIR     pinned xwin binary and notices; required for darwin-* or a bundled-only host pin\n"
    "  --help\n"
    "\n"
    "Matrix entries accept target, llvm-prefix, link-args, link-mode, linux-glibc, cpu,\n"
    "bridge OR bridge-args, version-object, lld-version-object, sdk, output, extra-licenses,\n"
    "msvc-helper, path-args, native-dir. Matrix allows one to four distinct advertised targets.\n"
    "Global --compiler, --host-config, --profile and --native-dir may accompany --matrix.\n"
    "All relative JSON paths resolve from checkout root, NOT the JSON file directory.\n"
    "Recipes also run from checkout root; use placeholders for relocatable inputs.\n"
    "All target recipes are validated before compiling the shared host stage1.\n"
    "A linker remains the authority on ABI validity.\n"
    "\n"
    "Prerequisites: tools/build prepare for authenticated host sources/recipes, and tools/build\n"
    "next to this tool for packaging. No downloads are performed here. Each stage rebuilds\n"
    "C/C++ sources with the preceding compiler. Cross targets need their own LLVM, C++ ABI,\n"
    "CRT, compression/XML and platform SDK libraries; host archives are never substituted.\n"
    "\n"
    "Recipes: blank lines and lines beginning # are ignored. Do not shell-quote paths;\n"
    "each line is already one argv element. Placeholders: {root}, {native}, {llvm},\n"
    "{sdk}, {object}, {version}, {lld_version}, {windows_path}, {bridge}, {output}, {stage}.\n"
    "The link recipe MUST list {version} AND {lld_version} (and {windows_path} for Windows) as\n"
    "separate plain objects BEFORE {bridge} and every LLVM/Clang archive (or linker group) so\n"
    "the pinned upstream Version.cpp.o, with no repository/hash strings, wins over\n"
    "libclangBasic's. --bridge-args compiles it from {native}/version/clang/lib/Basic/Version.cpp\n"
    "and {native}/version/lld/Common/Version.cpp (+ {native}/version/lld-gen; made by\n"
    "tools/build native) using the same flags/target as the bridge. Include all target archives,\n"
    "CRT objects, library search paths and (where applicable) loader/minimum OS flags.\n"
    "The tool appends deterministic linker flags and the output path; recipes must not\n"
    "supply their own output path. cc mode uses normal compiler-driver arguments\n"
    "(-Wl, for linker options); lld mode uses raw linker arguments. For cc mode with\n"
    "fully explicit SDK CRT/runtime paths, include -nostdlib. Otherwise the cross_cc\n"
    "driver prepares its target runtimes as usual. Do not pass -lstdc++ for an LLVM SDK\n"
    "built against GNU C++: cross_cc interprets it as bundled libc++;\n"
    "pass the matching target libstdc++ archive/shared-library path explicitly.\n"
    "No shell, host compiler or host linker runs in the final target link.\n"
    "\n"
    "Seed -> fresh current host stage1 -> final stage2. Native targets rebuild with a copied\n"
    "stage2 at the SAME paths and compare stage2/stage3 binary, bridge and all object hashes.\n"
    "Cross targets cannot run here: rebuild using stage1 proves same-input repeatability,\n"
    "NOT native convergence or cross-host equality. Mismatches fail before publishing.\n"
    "Detailed FILE.build-manifest.txt records hashes/recipes. Bundles contain one executable,\n"
    "lib/, vendor/, licenses/ and lock files. ZIP entries are UTF-8 byte-sorted with fixed\n"
    "1980 timestamps and Unix modes, compressed by raw DEFLATE (LZ77 with fixed Huffman\n"
    "codes, implemented in this tool). The ZIP is generated twice and compared before\n"
    "publishing DIR.zip plus its SHA256. Binary and ZIP hashes are separate. Host-path build\n"
    "provenance stays outside the ZIP. --package-only trusts the caller to supply the correct\n"
    "verified target executable.\n";

typedef struct {
    char *root, *target, *llvm, *native, *bridge, *bridge_args, *link_args, *sdk, *output;
    char *stage, *object, *binary, *link_mode, *linux_glibc, *cpu, *package_binary, *extra_licenses, *version, *lld_version, *msvc_helper;
    char *windows_path, *path_args;
    char *build_tool; /* tools/build next to this executable */
    char *compiler, *host_config, *build_only, *profile, *resource_root;
    int host_stage; /* internal: stage1 needs no packaging inputs */
} Opts;

static char *host(void) {
    if (!strcmp(HOST_OS, "unknown") || !strcmp(HOST_ARCH, "unknown")) fail("unsupported host");
    return sfmt("%s-%s", HOST_OS, HOST_ARCH);
}
static int is_windows_target(const Opts *o) { return strcmp(o->target, "windows-amd64") == 0; }
static int is_darwin_target(const Opts *o) { return starts_with(o->target, "darwin-"); }

/* Resource/libc overrides and Clang search-path injections must not silently
 * replace the checkout and explicit recipe inputs. Host SDK discovery (SDKROOT,
 * INCLUDE, LIB, toolset variables) stays available: the bootstrap and MSVC
 * recipes may intentionally require it. */
static Strs environment(const Opts *o) {
    static const char *const drop[] = {"TMPDIR", "TMP", "TEMP", "SOURCE_DATE_EPOCH", "ZERO_AR_DATE", "LC_ALL", "CROSS_CC_ROOT", "CROSS_CC_LIB_DIR", "CROSS_CC_MSVC", "ZIG_LIB_DIR", "CROSS_CC_CACHE_DIR", "ZIG_GLOBAL_CACHE_DIR", "ZIG_LOCAL_CACHE_DIR", "ZIG_LIBC", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH", "LIBRARY_PATH", "CCC_OVERRIDE_OPTIONS", NULL};
    Strs extra = {0};
    char *tmp = PJ(o->root, ".cache", "tmp");
    strs_addf(&extra, "TMPDIR=%s", tmp);
    strs_addf(&extra, "TMP=%s", tmp);
    strs_addf(&extra, "TEMP=%s", tmp);
    strs_add(&extra, "SOURCE_DATE_EPOCH=0");
    strs_add(&extra, "ZERO_AR_DATE=1");
    strs_add(&extra, "LC_ALL=C");
    /* Aliases live in staging directories, not next to the seed's lib/ tree.
     * Pin the seed to its own resources; only current-stage compilers use ours. */
    strs_addf(&extra, "CROSS_CC_ROOT=%s", *o->resource_root ? o->resource_root : o->root);
    strs_addf(&extra, "CROSS_CC_CACHE_DIR=%s", PJ(o->root, ".cache", o->host_stage ? "seed-cc" : "cc"));
    return make_env(drop, &extra);
}

static void run(const Opts *o, Strs *args, const char *cwd) {
    printf("release:");
    for (int i = 0; i < args->n; i++) printf(" \"%s\"", args->v[i]);
    printf("\n");
    Strs env = environment(o);
    int code = run_cmd(args->v, cwd && *cwd ? cwd : o->root, env.v);
    if (code != 0) fail("%s failed: exit %d", args->v[0], code);
}

static char *sha(const char *file) { return sha256_file(file); }

static char *expand(const Opts *o, const char *s) {
    const struct {
        const char *name, *value;
    } pairs[] = {{"root", o->root}, {"native", o->native}, {"llvm", o->llvm}, {"sdk", o->sdk}, {"object", o->object}, {"version", o->version}, {"lld_version", o->lld_version}, {"windows_path", o->windows_path}, {"bridge", o->bridge}, {"output", o->binary}, {"stage", o->stage}};
    char *result = xstrdup(s);
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        char *key = sfmt("{%s}", pairs[i].name);
        if (!*pairs[i].value && contains(result, key)) fail("recipe needs --%s=", pairs[i].name);
        result = replace_all(result, key, pairs[i].value);
    }
    return result;
}
static Strs recipe(const Opts *o, const char *file) {
    Strs out = {0};
    Strs lines = split_lines(read_file(file, NULL));
    for (int i = 0; i < lines.n; i++) {
        char *l = trim(lines.v[i]);
        if (!*l || l[0] == '#') continue;
        strs_add(&out, expand(o, l));
    }
    if (out.n == 0) fail("empty recipe %s", file);
    return out;
}
static void copy_new(const char *dst, const char *src) {
    if (exists(dst)) fail("refusing to overwrite %s", dst);
    copy_file(dst, src);
}
static void claim_stage(const char *stage) {
    /* Atomic mkdir, not an exists-then-create claim. Retain stages on failure. */
    mkdirs(path_dir(stage));
    if (make_dir(stage) != 0) fail("stage %s exists or cannot be claimed: %s; inspect it before removing", stage, strerror(errno));
}

/* ---- ZIP writer: no host archiver, timestamps, paths, ownership or modes leak ---- */
static void zip_files(const char *root, const char *relative, Strs *names) {
    Strs entries = {0};
    if (!list_dir(*relative ? PJ(root, relative) : root, &entries)) fail("ZIP list %s failed", relative);
    for (int i = 0; i < entries.n; i++) {
        char *name = *relative ? sfmt("%s/%s", relative, entries.v[i]) : xstrdup(entries.v[i]);
        char *full = PJ(root, name);
        if (is_symlink(full)) fail("ZIP refuses non-regular input %s", name);
        if (is_dir(full)) zip_files(root, name, names);
        else if (is_file(full)) strs_add(names, name);
        else fail("ZIP refuses non-regular input %s", name);
    }
}

static uint32_t crc32_table[256];
static uint32_t crc32_of(const unsigned char *p, size_t n) {
    if (!crc32_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc32_table[i] = c;
        }
    }
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = crc32_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

/* Raw DEFLATE (RFC 1951): one final block with the fixed Huffman code, LZ77
 * matches found by a bounded hash-chain search. Deterministic by construction. */
typedef struct {
    Buf out;
    uint32_t acc;
    int nbits;
} BitWriter;
static void bw_bits(BitWriter *w, uint32_t value, int count) {
    w->acc |= value << w->nbits;
    w->nbits += count;
    while (w->nbits >= 8) {
        buf_char(&w->out, (char)(w->acc & 0xff));
        w->acc >>= 8;
        w->nbits -= 8;
    }
}
static void bw_huff(BitWriter *w, uint32_t code, int len) {
    uint32_t rev = 0;
    for (int i = 0; i < len; i++) rev |= ((code >> i) & 1u) << (len - 1 - i);
    bw_bits(w, rev, len);
}
static void put_literal_length(BitWriter *w, int sym) {
    if (sym < 144) bw_huff(w, 0x30u + (uint32_t)sym, 8);
    else if (sym < 256) bw_huff(w, 0x190u + (uint32_t)(sym - 144), 9);
    else if (sym < 280) bw_huff(w, (uint32_t)(sym - 256), 7);
    else bw_huff(w, 0xc0u + (uint32_t)(sym - 280), 8);
}
static const int len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const int len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const int dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const int dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void put_match(BitWriter *w, int length, int dist) {
    int li = 28;
    while (len_base[li] > length) li--;
    put_literal_length(w, 257 + li);
    if (len_extra[li]) bw_bits(w, (uint32_t)(length - len_base[li]), len_extra[li]);
    int di = 29;
    while (dist_base[di] > dist) di--;
    bw_huff(w, (uint32_t)di, 5);
    if (dist_extra[di]) bw_bits(w, (uint32_t)(dist - dist_base[di]), dist_extra[di]);
}

#define DEFLATE_HASH_BITS 15
#define DEFLATE_WINDOW 32768
#define DEFLATE_MAX_CHAIN 64
static Buf deflate_raw(const unsigned char *data, size_t n) {
    BitWriter w;
    memset(&w, 0, sizeof w);
    bw_bits(&w, 1, 1); /* BFINAL */
    bw_bits(&w, 1, 2); /* BTYPE = fixed Huffman */
    int32_t *head = (int32_t *)xmalloc(sizeof(int32_t) << DEFLATE_HASH_BITS);
    int32_t *prev = (int32_t *)xmalloc(sizeof(int32_t) * (n ? n : 1));
    for (int i = 0; i < (1 << DEFLATE_HASH_BITS); i++) head[i] = -1;
    size_t i = 0;
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 3 <= n) {
            uint32_t h = (((uint32_t)data[i] << 10) ^ ((uint32_t)data[i + 1] << 5) ^ data[i + 2]) & ((1u << DEFLATE_HASH_BITS) - 1);
            int32_t cand = head[h];
            int chain = 0;
            while (cand >= 0 && i - (size_t)cand <= DEFLATE_WINDOW && chain++ < DEFLATE_MAX_CHAIN) {
                int len = 0;
                size_t limit = n - i < 258 ? n - i : 258;
                while ((size_t)len < limit && data[(size_t)cand + (size_t)len] == data[i + (size_t)len]) len++;
                if (len > best_len) {
                    best_len = len;
                    best_dist = (int)(i - (size_t)cand);
                    if (len == 258) break;
                }
                cand = prev[cand];
            }
        }
        size_t step = 1;
        if (best_len >= 3) {
            put_match(&w, best_len, best_dist);
            step = (size_t)best_len;
        } else {
            put_literal_length(&w, data[i]);
        }
        for (size_t k = 0; k < step; k++, i++) {
            if (i + 3 <= n) {
                uint32_t h = (((uint32_t)data[i] << 10) ^ ((uint32_t)data[i + 1] << 5) ^ data[i + 2]) & ((1u << DEFLATE_HASH_BITS) - 1);
                prev[i] = head[h];
                head[h] = (int32_t)i;
            }
        }
    }
    put_literal_length(&w, 256);
    if (w.nbits > 0) bw_bits(&w, 0, 8 - w.nbits);
    free(head);
    free(prev);
    return w.out;
}

typedef struct {
    char *name;
    uint32_t crc, size, compressed, offset;
    uint16_t method;
} ZipEntry;

static void le(Buf *b, uint64_t value, int bytes) {
    for (int i = 0; i < bytes; i++) buf_char(b, (char)((value >> (8 * i)) & 0xff));
}
static void zip_write(FILE *f, const void *data, size_t n) {
    if (n && fwrite(data, 1, n, f) != n) fail("ZIP write failed");
}
static ZipEntry zip_entry(FILE *f, const char *root, const char *relative, const char *archive_name, uint32_t offset) {
    size_t len;
    char *data = read_file(PJ(root, relative), &len);
    if ((uint64_t)len >= 0xffffffffu) fail("ZIP64 required for %s", relative);
    Buf compressed = deflate_raw((const unsigned char *)data, len);
    const char *payload = data;
    size_t payload_len = len;
    uint16_t method = 0;
    if (compressed.n < len) {
        payload = compressed.s;
        payload_len = compressed.n;
        method = 8;
    }
    ZipEntry e;
    e.name = xstrdup(archive_name);
    e.crc = crc32_of((const unsigned char *)data, len);
    e.size = (uint32_t)len;
    e.compressed = (uint32_t)payload_len;
    e.offset = offset;
    e.method = method;
    Buf h = {0};
    le(&h, 0x04034b50, 4);
    le(&h, 20, 2);
    le(&h, 0x800, 2);
    le(&h, method, 2);
    le(&h, 0, 2);
    le(&h, 0x21, 2); /* 1980-01-01 00:00:00 */
    le(&h, e.crc, 4);
    le(&h, e.compressed, 4);
    le(&h, e.size, 4);
    le(&h, strlen(archive_name), 2);
    le(&h, 0, 2);
    zip_write(f, h.s, h.n);
    zip_write(f, archive_name, strlen(archive_name));
    zip_write(f, payload, payload_len);
    return e;
}
static void make_zip(const char *root, const char *destination, const char *top, const char *executable) {
    if (exists(destination)) fail("refusing to overwrite ZIP %s", destination);
    Strs names = {0};
    zip_files(root, "", &names);
    strs_sort(&names);
    if (names.n >= 65535) fail("ZIP64 required for %d entries", names.n);
    FILE *f = fopen(destination, "wb");
    if (!f) fail("ZIP create %s: %s", destination, strerror(errno));
    ZipEntry *entries = (ZipEntry *)xmalloc(sizeof(ZipEntry) * (size_t)(names.n ? names.n : 1));
    uint64_t offset = 0;
    for (int i = 0; i < names.n; i++) {
        char *archive_name = sfmt("%s/%s", top, names.v[i]);
        if (strlen(archive_name) > 65535 || offset >= 0xffffffffu) fail("ZIP32 limit exceeded");
        entries[i] = zip_entry(f, root, names.v[i], archive_name, (uint32_t)offset);
        offset += 30 + strlen(archive_name) + entries[i].compressed;
    }
    uint64_t start = offset;
    for (int i = 0; i < names.n; i++) {
        ZipEntry *e = &entries[i];
        Buf h = {0};
        le(&h, 0x02014b50, 4);
        le(&h, 0x0314, 2);
        le(&h, 20, 2);
        le(&h, 0x800, 2);
        le(&h, e->method, 2);
        le(&h, 0, 2);
        le(&h, 0x21, 2);
        le(&h, e->crc, 4);
        le(&h, e->compressed, 4);
        le(&h, e->size, 4);
        le(&h, strlen(e->name), 2);
        le(&h, 0, 2);
        le(&h, 0, 2);
        le(&h, 0, 2);
        le(&h, 0, 2);
        uint64_t mode = 0100644;
        if (!strcmp(e->name, sfmt("%s/%s", top, executable)) || !strcmp(e->name, sfmt("%s/libexec/xwin", top)) || !strcmp(e->name, sfmt("%s/libexec/xwin.exe", top))) mode = 0100755;
        le(&h, mode << 16, 4);
        le(&h, e->offset, 4);
        zip_write(f, h.s, h.n);
        zip_write(f, e->name, strlen(e->name));
        offset += h.n + strlen(e->name);
    }
    if (offset >= 0xffffffffu) fail("ZIP64 required for archive");
    Buf end = {0};
    le(&end, 0x06054b50, 4);
    le(&end, 0, 2);
    le(&end, 0, 2);
    le(&end, (uint64_t)names.n, 2);
    le(&end, (uint64_t)names.n, 2);
    le(&end, offset - start, 4);
    le(&end, start, 4);
    le(&end, 0, 2);
    zip_write(f, end.s, end.n);
    if (fclose(f) != 0) fail("ZIP write failed");
}

/* ---- release steps ---- */
static void llvm_version(const Opts *o) {
    char *h = read_file(PJ(o->llvm, "include", "llvm", "Config", "llvm-config.h"), NULL);
    static const char *const lines[] = {"#define LLVM_VERSION_MAJOR 22", "#define LLVM_VERSION_MINOR 1", "#define LLVM_VERSION_PATCH 8", NULL};
    for (int i = 0; lines[i]; i++) {
        if (!contains(h, lines[i])) fail("target LLVM_PREFIX must contain pinned LLVM 22.1.8 (%s missing)", lines[i]);
    }
    /* The linker is the authoritative format/ABI check. Never substitute host
     * archives when target libraries are absent or fail to link. */
}

static const char *target_triple(const Opts *o) {
    if (is_windows_target(o)) return "x86_64-pc-windows-msvc";
    if (!strcmp(o->target, "darwin-arm64")) return "arm64-apple-macosx11.0.0";
    if (!strcmp(o->target, "darwin-amd64")) return "x86_64-apple-macosx11.0.0";
    return "x86_64-unknown-linux-gnu";
}
/* Target spelling of the bootstrap's own `cc` driver. */
static char *cc_target(const Opts *o) {
    if (is_windows_target(o)) return xstrdup("x86_64-windows-msvc");
    if (!strcmp(o->target, "darwin-arm64")) return xstrdup("aarch64-macos");
    if (!strcmp(o->target, "darwin-amd64")) return xstrdup("x86_64-macos");
    return sfmt("x86_64-linux-gnu.%s", o->linux_glibc);
}

/* The C driver root translation unit (main.c) as one repeatable target object,
 * compiled by the bootstrap's cc mode with the same target as the final link. */
static void emit(const Opts *o, const char *boot) {
    Strs a = {0};
    strs_add(&a, boot);
    strs_add(&a, "cc");
    strs_add(&a, "-target");
    strs_add(&a, cc_target(o));
    strs_add(&a, "-std=c11");
    strs_add(&a, !strcmp(o->profile, "debug") ? "-O0" : "-O2");
    if (!strcmp(o->profile, "debug")) strs_add(&a, "-g");
    strs_add(&a, "-c");
    strs_add(&a, "-fPIC");
    if (!strcmp(o->target + strlen(o->target) - 5, "amd64")) strs_addf(&a, "-march=%s", o->cpu);
    else strs_addf(&a, "-mcpu=%s", o->cpu);
    if (strcmp(o->profile, "debug")) strs_add(&a, "-DNDEBUG");
    strs_add(&a, "-DCROSS_CC_RELEASE_OBJECT=1");
    strs_addf(&a, "-I%s", PJ(o->root, "src"));
    if (is_windows_target(o)) strs_add(&a, "-DCROSS_CC_WINDOWS_CANONICAL_PATHS=1");
    strs_addf(&a, "-ffile-prefix-map=%s=/cross_cc", o->root);
    strs_add(&a, "-o");
    strs_add(&a, o->object);
    strs_add(&a, PJ(o->root, "main.c"));
    run(o, &a, NULL);
}

static void compile_bridge_object(const Opts *o, const char *clang, const char *src, const char *extra_include, const char *input, const char *out) {
    Strs a = {0};
    strs_add(&a, clang);
    static const char *const base[] = {"--no-default-config", "-std=c++17", "-w", "-c", "-ffunction-sections", "-fdata-sections", "-fno-exceptions", "-fno-rtti", "-fno-stack-protector", "-fvisibility-inlines-hidden", "-DLLVM_BUILD_STATIC", "-DCLANG_BUILD_STATIC", "-D__STDC_CONSTANT_MACROS", "-D__STDC_FORMAT_MACROS", "-D__STDC_LIMIT_MACROS", NULL};
    strs_addv(&a, base);
    /* Generated headers first: they must shadow the installed VCSRevision.h. */
    if (*extra_include) strs_addf(&a, "-I%s", extra_include);
    char *dgen = PJ(o->native, "dsymutil", "gen");
    char *input_base = path_base(input);
    if (!strcmp(input_base, "CFBundle.cpp") && !strcmp(extra_include, dgen)) strs_add(&a, "-DCROSS_CC_DSYMUTIL_PORTABLE_BUNDLE");
    /* The bridge's extern "C" cross_cc_dsymutil_main wraps this entry point. */
    if (!strcmp(input, PJ(dgen, "driver.cpp"))) strs_add(&a, "-Dmain=cross_cc_dsymutil_driver_main");
    strs_addf(&a, "-I%s", PJ(o->llvm, "include"));
    strs_addf(&a, "-I%s", src);
    int is_path = *o->windows_path && !strcmp(out, o->windows_path);
    const char *flags = is_path ? o->path_args : o->bridge_args;
    if (*flags) {
        Strs r = recipe(o, flags);
        strs_cat(&a, &r);
    }
    if (is_path) strs_addv(&a, wp_flags);
    /* Last wins: an SDK recipe cannot silently turn a debug bridge into O2. */
    strs_add(&a, !strcmp(o->profile, "debug") ? "-O0" : "-O2");
    strs_add(&a, !strcmp(o->profile, "debug") ? "-g" : "-DNDEBUG");
    strs_addf(&a, "--target=%s", target_triple(o));
    /* COFF .debug$S otherwise records the absolute object output path. */
    if (is_windows_target(o)) strs_add(&a, "-mno-incremental-linker-compatible");
    if (ends_with(o->target, "-amd64")) strs_addf(&a, "-march=%s", o->cpu);
    else strs_addf(&a, "-mcpu=%s", o->cpu);
    char *cwd = xstrdup(o->root);
    const char *actual_input = input;
    if (is_path) {
        cwd = path_dir(input);
        actual_input = "Path.cpp";
    }
    strs_addf(&a, "-ffile-prefix-map=%s=/cross_cc", o->root);
    strs_addf(&a, "-ffile-prefix-map=%s=/cross_cc_llvm/include", PJ(o->llvm, "include"));
    strs_add(&a, "-o");
    strs_add(&a, out);
    strs_add(&a, actual_input);
    if (is_path) {
        wp_rooted_args(a.v, a.n, o->root);
        wp_prefix_flags(PJ(o->stage, "windows-path"), &a);
        Buf record = {0};
        buf_addf(&record, "compile-cwd %s\n", cwd);
        for (int i = 0; i < a.n; i++) buf_addf(&record, "compile-arg %s\n", a.v[i]);
        write_file(PJ(o->stage, "windows-path", "compile.txt"), buf_cstr(&record));
    }
    run(o, &a, cwd);
}

static void build_windows_path(const Opts *o, const char *clang) {
    Json *lock = json_load(PJ(o->root, "compiler-rt.lock.json"));
    const char *url = jstr(lock, "url");
    char *archive = PJ(o->root, ".cache", "downloads", last_segment(url));
    char *source, *gen;
    wp_prepare(archive, jstr(lock, "sha256"), jstr(lock, "strip_prefix"), PJ(o->stage, "windows-path"), &source, &gen);
    compile_bridge_object(o, clang, path_dir(source), gen, source, o->windows_path);
    char *first = sha(o->windows_path);
    compile_bridge_object(o, clang, path_dir(source), gen, source, o->windows_path);
    if (strcmp(sha(o->windows_path), first) != 0) fail("configured LLVM Path object is not repeatable");
}

static const char *const DSYM_FILES[] = {"dsymutil.cpp", "BinaryHolder.cpp", "CFBundle.cpp", "DebugMap.cpp", "DwarfLinkerForBinary.cpp", "MachODebugMapParser.cpp", "MachOUtils.cpp", "Reproducer.cpp", "RelocationMap.cpp", "SwiftModule.cpp", "driver.cpp", NULL};

static void build_bridge(const Opts *o, const char *clang) {
    /* Only the unchanged pinned upstream C++ bridge is compiled. */
    Json *nat = jobj(json_load(PJ(o->root, "toolchain.lock.json")), "native");
    Json *sources = jarr_opt(nat, "sources");
    char *src = PJ(o->root, "deps", "llvm-bridge");
    if (!is_dir(src)) fail("bridge sources missing %s", src);
    Strs objects = {0};
    for (int i = 0; sources && i < sources->n; i++) {
        const char *name = sources->items[i]->str;
        char *out = PJ(o->stage, sfmt("%s.o", name));
        strs_add(&objects, out);
        compile_bridge_object(o, clang, src, "", PJ(src, name), out);
    }
    /* Reuse the native step's authenticated LLVM sources, generated option
     * table and configured portable CFBundle fallback on every release host. */
    char *ddir = PJ(o->native, "dsymutil");
    char *dsrc = PJ(ddir, "llvm", "tools", "dsymutil");
    for (int i = 0; DSYM_FILES[i]; i++) {
        const char *name = DSYM_FILES[i];
        char *input = (!strcmp(name, "CFBundle.cpp") || !strcmp(name, "driver.cpp")) ? PJ(ddir, "gen", name) : PJ(dsrc, name);
        char *out = PJ(o->stage, sfmt("dsymutil-%s.o", name));
        strs_add(&objects, out);
        compile_bridge_object(o, clang, dsrc, PJ(ddir, "gen"), input, out);
    }
    /* Unmodified upstream clang Version.cpp from the host native step, empty
     * VCSVersion.inc: a plain object (never an archive member) that the link
     * recipe places before libclangBasic. */
    char *vsrc = PJ(o->native, "version", "clang", "lib", "Basic", "Version.cpp");
    char *gen = PJ(o->stage, "version-gen");
    mkdirs(gen);
    write_file(PJ(gen, "VCSVersion.inc"), "");
    compile_bridge_object(o, clang, src, gen, vsrc, o->version);
    /* Same for lld: the generated dir (Version.inc from Version.inc.in, empty
     * VCSVersion.inc and VCSRevision.h) comes from the host native step. */
    char *lsrc = PJ(o->native, "version", "lld", "Common", "Version.cpp");
    compile_bridge_object(o, clang, src, PJ(o->native, "version", "lld-gen"), lsrc, o->lld_version);
    Strs ar = {0};
    strs_add(&ar, PJ(o->stage, sfmt("llvm-ar%s", EXE_SUFFIX)));
    strs_add(&ar, "rcsD");
    strs_add(&ar, o->bridge);
    strs_cat(&ar, &objects);
    run(o, &ar, NULL);
}

static void link_target(const Opts *o, const char *linker, const Strs *inputs) {
    Strs a = {0};
    strs_add(&a, linker);
    if (!strcmp(o->link_mode, "cc")) {
        strs_add(&a, "cc");
        strs_add(&a, "-target");
        strs_add(&a, cc_target(o));
        strs_cat(&a, inputs);
        if (!strcmp(o->target, "linux-amd64")) {
            /* Zig-compatible cc rejects --threads; direct LLD mode below keeps its explicit thread limit. */
            strs_add(&a, "-Wl,--build-id=none");
            strs_add(&a, "-Wl,--gc-sections");
        } else if (is_windows_target(o)) {
            /* -s drops the PDB, whose GUID and path would carry host and bootstrap identity. */
            strs_add(&a, "-s");
            strs_add(&a, "-Wl,/Brepro");
        } else {
            strs_add(&a, "-Wl,-no_uuid");
        }
        strs_add(&a, "-o");
        strs_add(&a, o->binary);
        run(o, &a, NULL);
        return;
    }
    strs_cat(&a, inputs);
    if (!strcmp(o->target, "linux-amd64")) {
        strs_add(&a, "--build-id=none");
        strs_add(&a, "--threads=2");
        strs_add(&a, "--gc-sections");
        strs_add(&a, "-o");
        strs_add(&a, o->binary);
    } else if (is_windows_target(o)) {
        strs_add(&a, "/Brepro");
        strs_add(&a, "/timestamp:0");
        strs_add(&a, "/threads:2");
        strs_add(&a, "/incremental:no");
        strs_addf(&a, "/out:%s", o->binary);
    } else {
        strs_add(&a, "-no_uuid");
        strs_add(&a, "-o");
        strs_add(&a, o->binary);
    }
    run(o, &a, NULL);
}

static void package_release(const Opts *o, const char *binary_sha) {
    char *bundle = sfmt("%s.bundle-stage", o->output);
    char *zip = sfmt("%s.zip", o->output);
    char *first = sfmt("%s.stage", zip), *second = sfmt("%s.repeat", zip);
    char *checks[] = {o->output, bundle, zip, first, second, sfmt("%s.sha256", zip)};
    for (int i = 0; i < 6; i++) {
        if (exists(checks[i])) fail("refusing to replace release artifact %s", checks[i]);
    }
    Strs a = {0};
    strs_add(&a, o->build_tool);
    strs_add(&a, "package");
    strs_addf(&a, "--target=%s", o->target);
    strs_add(&a, "--single-executable");
    strs_addf(&a, "--binary=%s", o->binary);
    strs_addf(&a, "--output=%s", bundle);
    if (*o->extra_licenses) strs_addf(&a, "--extra-licenses=%s", o->extra_licenses);
    if (*o->msvc_helper) strs_addf(&a, "--msvc-helper=%s", o->msvc_helper);
    run(o, &a, NULL);
    const char *name = is_windows_target(o) ? "cross_cc.exe" : "cross_cc";
    /* Portable manifest is intentionally distinct from the detailed build receipt
     * (whose absolute input paths are useful locally but not archive identity). */
    write_file(PJ(bundle, "manifest.txt"), sfmt("target %s\nbinary-sha256 %s\n", o->target, binary_sha));
    char *top = sfmt("cross_cc-%s", o->target);
    make_zip(bundle, first, top, name);
    char *zip_sha = sha(first);
    make_zip(bundle, second, top, name);
    if (strcmp(sha(second), zip_sha) != 0) fail("ZIP is not repeatable; stages retained");
    remove_file(second);
    rename_path(bundle, o->output);
    rename_path(first, zip);
    write_file(sfmt("%s.sha256", zip), sfmt("%s  %s\n", zip_sha, path_base(zip)));
    printf("release: bundle %s\nbinary-sha256 %s\nZIP %s\nzip-sha256 %s\n", o->output, binary_sha, zip, zip_sha);
}

/* ---- recipe validation ---- */
static int check_object_order_one(const Opts *o, const Strs *inputs, const char *object, const char *name) {
    int vi = -1;
    for (int i = 0; i < inputs->n; i++) {
        if (!strcmp(inputs->v[i], object)) {
            vi = i;
            break;
        }
    }
    if (vi < 0) fail("link recipe must include %s as a separate plain object before %s and all LLVM libraries", name, "{bridge}");
    for (int i = 0; i <= vi && i < inputs->n; i++) {
        const char *arg = inputs->v[i];
        if (i > vi) continue;
        if (!strcmp(arg, o->bridge) || ends_with(arg, ".a") || ends_with(arg, ".lib") || starts_with(arg, "-l") || ends_with(arg, "start-group")) fail("%s must precede %s in the link recipe", name, arg);
    }
    return 1;
}
static void check_version_order(const Opts *o, const Strs *inputs) {
    check_object_order_one(o, inputs, o->version, "{version}");
    check_object_order_one(o, inputs, o->lld_version, "{lld_version}");
    if (is_windows_target(o)) check_object_order_one(o, inputs, o->windows_path, "{windows_path}");
}

static char *config_path(const char *root, const char *value) {
    if (!*value || is_abs(value)) return xstrdup(value);
    return PJ(root, value);
}

static void validate_recipe(const Opts *o, const char *file, int final_link) {
    int has_object = 0, has_bridge = 0;
    Strs inputs = recipe(o, file);
    if (final_link) check_version_order(o, &inputs);
    for (int n = 0; n < inputs.n; n++) {
        const char *arg = inputs.v[n];
        if (!strcmp(arg, o->object)) {
            has_object = 1;
            continue;
        }
        if ((!strcmp(arg, o->version) || !strcmp(arg, o->lld_version) || !strcmp(arg, o->windows_path)) && final_link) continue;
        if (!strcmp(arg, o->bridge)) {
            has_bridge = 1;
            continue;
        }
        if (!strcmp(arg, "-o") || !strcmp(arg, "--output") || starts_with(arg, "/out:") || starts_with(arg, "--output=")) fail("recipe must not supply an output path: %s", arg);
        /* Plain path arguments include archives, CRT objects and linker scripts.
         * Flag-embedded paths are checked below for the common search/SDK forms. */
        const char *candidate = arg;
        static const char *const prefixes[] = {"-L", "-I", "--sysroot=", "-isysroot=", "/libpath:", "/LIBPATH:", "/wholearchive:", "/WHOLEARCHIVE:", NULL};
        for (int p = 0; prefixes[p]; p++) {
            if (starts_with(arg, prefixes[p]) && strlen(arg) > strlen(prefixes[p])) {
                candidate = arg + strlen(prefixes[p]);
                break;
            }
        }
        /* Raw COFF linker options are not Unix absolute input paths. */
        if (is_windows_target(o) && candidate == arg && arg[0] == '/') {
            const char *tail = arg + 1;
            if (!strchr(tail, '/') && !strchr(tail, '\\') && (strchr(tail, ':') || !strcmp(arg, "/nologo") || !strcmp(arg, "/dll") || !strcmp(arg, "/debug") || !strcmp(arg, "/Brepro"))) continue;
        }
        if (is_abs(candidate) || (candidate[0] != '-' && (strchr(candidate, '/') || strchr(candidate, '\\') || ends_with(candidate, ".a") || ends_with(candidate, ".lib") || ends_with(candidate, ".o") || ends_with(candidate, ".obj")))) {
            if (!exists(config_path(o->root, candidate))) fail("recipe input missing: %s", candidate);
        }
    }
    if (final_link && (!has_object || !has_bridge)) fail("link recipe must include {object} and {bridge} as separate inputs");
}

static void require_file(const char *p) {
    if (!is_file(p)) fail("required input file missing: %s", p);
}
static void require_nonempty_file(const char *p) {
    if (file_size(p) <= 0) fail("required helper notice missing, empty or not a regular file: %s", p);
}

static Opts preflight(Opts o) {
    if (strcmp(o.target, "linux-amd64") && strcmp(o.target, "darwin-arm64") && strcmp(o.target, "darwin-amd64") && strcmp(o.target, "windows-amd64")) fail("unsupported target %s", o.target);
    if (!*o.cpu) o.cpu = ends_with(o.target, "-amd64") ? "x86-64-v2" : "generic";
    if (strcmp(o.link_mode, "cc") && strcmp(o.link_mode, "lld")) fail("--link-mode must be cc or lld");
    if (!*o.output) o.output = PJ(o.root, "dist", sfmt("cross_cc-%s", o.target));
    if (!o.host_stage && !*o.build_only) {
    static const char *const suffixes[] = {"", ".bundle-stage", ".zip", ".zip.stage", ".zip.repeat", ".zip.sha256", ".build-manifest.txt", NULL};
    for (int i = 0; suffixes[i]; i++) {
        char *p = sfmt("%s%s", o.output, suffixes[i]);
        if (exists(p)) fail("release artifact exists: %s", p);
    }
    char *msvc_lock = PJ(o.root, "msvc.lock.json");
    if (exists(msvc_lock)) {
        Json *lock = json_load(msvc_lock);
        const char *expected = "";
        int bundled_only = 0;
        Json *pin = jget(jget(lock, "hosts"), o.target);
        if (pin && pin->type == J_OBJ) {
            expected = jstr_opt(pin, "binary_sha256");
            bundled_only = *expected && !*jstr_opt(pin, "url");
        }
        if ((is_darwin_target(&o) || bundled_only) && !*o.msvc_helper) fail("%s uses a bundled managed-MSVC helper and requires --msvc-helper=DIR containing pinned xwin and notices", o.target);
        if (*o.msvc_helper) {
            if (strlen(expected) != 64) fail("no bundled helper binary pin for %s", o.target);
            char *name = PJ(o.msvc_helper, is_windows_target(&o) ? "xwin.exe" : "xwin");
            require_file(name);
            if (strcmp(sha(name), expected) != 0) fail("managed Microsoft helper SHA-256 mismatch");
            static const char *const notices[] = {"LICENSE-APACHE", "LICENSE-MIT", "Cargo.lock", "dependency-notices.json", "rust-std-notices/LICENSE-APACHE", "rust-std-notices/LICENSE-MIT", "rust-std-notices/COPYRIGHT", NULL};
            for (int i = 0; notices[i]; i++) require_nonempty_file(PJ(o.msvc_helper, notices[i]));
            Strs entries = {0};
            if (!list_dir(PJ(o.msvc_helper, "dependency-notices"), &entries) || entries.n == 0) fail("helper dependency-notices directory missing or empty");
            if (file_size(PJ(o.msvc_helper, "sbom.spdx.json")) <= 0 && file_size(PJ(o.msvc_helper, "build-provenance.txt")) <= 0) fail("helper needs sbom.spdx.json or build-provenance.txt");
        }
    } else if (*o.msvc_helper) {
        fail("msvc.lock.json required for --msvc-helper=");
    }
    }
    if (*o.package_binary) {
        require_file(o.package_binary);
        return o;
    }
    if (!*o.llvm || !*o.link_args) fail("--llvm-prefix= and --link-args= are required; no cross-host SDK is guessed");
    if (!*o.native) o.native = PJ(o.root, ".cache", "native");
    if (*o.bridge_args && (*o.version || *o.lld_version)) fail("--version-object= and --lld-version-object= are for prebuilt --bridge=; --bridge-args= builds its own");
    if (*o.bridge_args && *o.bridge) fail("choose --bridge= OR --bridge-args=, not both");
    if (!*o.bridge && !*o.bridge_args) fail("--bridge-args= is required to build a fresh bridge (advanced prebuilt --bridge= requires both version objects)");
    if (o.host_stage && *o.bridge) fail("host stage1 must build a fresh bridge, not --bridge=");
    if (!*o.stage) o.stage = PJ(o.root, "build", "release", sfmt("%s.stage", o.target));
    if (exists(o.stage)) fail("stage %s exists; inspect it before removing", o.stage);
    o.object = PJ(o.stage, "cross_cc.o");
    o.binary = PJ(o.stage, is_windows_target(&o) ? "cross_cc.exe" : "cross_cc");
    if (is_windows_target(&o)) {
        o.windows_path = PJ(o.stage, "llvm-WindowsPath.cpp.o");
        if (!*o.path_args) o.path_args = o.bridge_args;
        if (!*o.path_args && strcmp(o.target, host()) != 0) fail("Windows cross release requires --path-args= (or --bridge-args=) to compile authenticated Path.cpp; prebuilt Path objects are not accepted");
        Json *lock = json_load(PJ(o.root, "compiler-rt.lock.json"));
        require_file(PJ(o.root, ".cache", "downloads", last_segment(jstr(lock, "url"))));
    } else if (*o.path_args) {
        fail("--path-args= is Windows-only");
    }
    if (*o.bridge_args) {
        o.bridge = PJ(o.stage, "libcross_cc_backend.a");
        o.version = PJ(o.stage, "clang-Version.cpp.o");
        o.lld_version = PJ(o.stage, "lld-Version.cpp.o");
        require_file(PJ(o.native, "version", "clang", "lib", "Basic", "Version.cpp"));
        static const char *const dfiles[] = {"inputs.txt", "gen/Options.inc", "gen/driver.cpp", "gen/CFBundle.cpp", "gen/llvm/Config/config.h", "llvm/tools/dsymutil/dsymutil.cpp", NULL};
        for (int i = 0; dfiles[i]; i++) require_file(PJ(o.native, "dsymutil", dfiles[i]));
        char *lf[] = {PJ(o.native, "version", "lld", "Common", "Version.cpp"), PJ(o.native, "version", "lld-gen", "lld", "Common", "Version.inc"), PJ(o.native, "version", "lld-gen", "llvm", "Support", "VCSRevision.h"), PJ(o.native, "version", "lld-gen", "VCSVersion.inc")};
        for (int i = 0; i < 4; i++) {
            if (!exists(lf[i])) fail("lld version source %s missing; re-run tools/build native", lf[i]);
        }
    } else {
        require_file(o.bridge);
        if (!*o.version || !*o.lld_version) fail("advanced prebuilt --bridge= requires both version objects; no host object is substituted");

        require_file(o.version);
        require_file(o.lld_version);
    }
    if (*o.sdk && !is_dir(o.sdk)) fail("input directory missing: %s", o.sdk);
    if (*o.extra_licenses && !is_dir(o.extra_licenses)) fail("input directory missing: %s", o.extra_licenses);
    llvm_version(&o);
    if (!is_dir(PJ(o.llvm, "lib"))) fail("target LLVM library directory missing: %s", o.llvm);
    validate_recipe(&o, o.link_args, 1);
    if (*o.path_args) validate_recipe(&o, o.path_args, 0);
    if (*o.bridge_args) {
        validate_recipe(&o, o.bridge_args, 0);
        if (!is_dir(PJ(o.root, "deps", "llvm-bridge"))) fail("bridge sources missing %s", PJ(o.root, "deps", "llvm-bridge"));
    }
    return o;
}


static void hash_tree(Buf *manifest, const char *label, const char *root) {
    Strs names = {0};
    if (!is_dir(root)) return;
    zip_files(root, "", &names);
    strs_sort(&names);
    for (int i = 0; i < names.n; i++) {
        char *f = PJ(root, names.v[i]);
        buf_addf(manifest, "%s %s %s\n", label, sha(f), f);
    }
}

static void build_stage(const Opts *o, const char *boot) {
    static const char *const aliases[] = {"cross_clang", "llvm-ar", "ld.lld", "ld64.lld", "lld-link", NULL};
    for (int i = 0; aliases[i]; i++) {
        char *dst = PJ(o->stage, sfmt("%s%s", aliases[i], EXE_SUFFIX));
        remove_file(dst);
        link_or_copy(dst, boot);
    }
    char *clang = PJ(o->stage, sfmt("cross_clang%s", EXE_SUFFIX));
    if (*o->bridge_args) build_bridge(o, clang);
    if (!exists(o->bridge)) fail("target bridge missing %s", o->bridge);
    if (is_windows_target(o)) build_windows_path(o, clang);
    emit(o, boot);
    const char *tool = "ld.lld";
    if (is_windows_target(o)) tool = "lld-link";
    if (is_darwin_target(o)) tool = "ld64.lld";
    char *linker = PJ(o->stage, sfmt("%s%s", tool, EXE_SUFFIX));
    if (!strcmp(o->link_mode, "cc")) linker = xstrdup(boot);
    Strs inputs = recipe(o, o->link_args);
    int has_object = 0, has_bridge = 0;
    check_version_order(o, &inputs);
    for (int i = 0; i < inputs.n; i++) {
        const char *arg = inputs.v[i];
        if (!strcmp(arg, o->object)) has_object = 1;
        if (!strcmp(arg, o->bridge)) has_bridge = 1;
        if (!strcmp(arg, "-o") || !strcmp(arg, "--output") || starts_with(arg, "/out:") || starts_with(arg, "--output=")) fail("recipe must not supply an output path: %s", arg);
    }
    if (!has_object || !has_bridge) fail("link recipe must include {object} and {bridge} as separate inputs");
    link_target(o, linker, &inputs);
}

static Buf build_receipt(const Opts *o, const char *boot, const char *verification) {
    Strs inputs = recipe(o, o->link_args);
    Buf m = {0};
    buf_addf(&m, "target %s\ncpu %s\nprofile %s\nrequested-linux-glibc %s\nlink-mode %s\nobject-sha256 %s\nbridge-sha256 %s\nbinary-sha256 %s\nverification %s\n", o->target, o->cpu, o->profile, o->linux_glibc, o->link_mode, sha(o->object), sha(o->bridge), sha(o->binary), verification);
    buf_addf(&m, "%s-sha256 %s\nseed-sha256 %s\nhost-config-sha256 %s\nresource-root %s\nprebuilt-bridge %s\n", o->host_stage ? "seed-compiler" : "stage1", sha(boot), sha(o->compiler), sha(o->host_config), *o->resource_root ? o->resource_root : o->root, *o->bridge_args ? "false" : "true (not compiler convergence of bridge)");
    if (o->host_stage) buf_addf(&m, "seed-resource-lock %s\n", sha(PJ(o->resource_root, "upstream.lock.json")));
    char *files[] = {PJ(o->root, "main.c"), PJ(o->root, "tools", "release.c"), PJ(o->root, "tools", "build.c"), PJ(o->root, "tools", "common.h"), PJ(o->root, "tools", "linux-release-deps.c"), PJ(o->root, "tools", "macos-x64-llvm.c"), o->link_args, o->bridge, o->host_config, o->version, o->lld_version, PJ(o->root, "toolchain.lock.json"), PJ(o->root, "upstream.lock.json")};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) buf_addf(&m, "input %s %s\n", sha(files[i]), files[i]);
    hash_tree(&m, "driver-source", PJ(o->root, "src"));
    if (*o->bridge_args) hash_tree(&m, "bridge-source", PJ(o->root, "deps", "llvm-bridge"));
    if (is_windows_target(o)) {
        char *pdir = PJ(o->stage, "windows-path");
        buf_addf(&m, "windows-canonical-paths true\n%s%s", read_file(PJ(pdir, "inputs.txt"), NULL), read_file(PJ(pdir, "compile.txt"), NULL));
        char *wf[] = {PJ(o->root, "tools", "windows_path", "windows_path.c"), PJ(o->root, "compiler-rt.lock.json"), PJ(pdir, "compile.txt"), o->windows_path};
        for (int i = 0; i < 4; i++) buf_addf(&m, "windows-path-input %s %s\n", sha(wf[i]), wf[i]);
        if (*o->path_args) buf_addf(&m, "windows-path-input %s %s\n", sha(o->path_args), o->path_args);
    }
    if (*o->bridge_args) {
        buf_addf(&m, "input %s %s\n", sha(o->bridge_args), o->bridge_args);
        char *ddir = PJ(o->native, "dsymutil");
        char *df[] = {PJ(ddir, "inputs.txt"), PJ(ddir, "gen", "Options.inc"), PJ(ddir, "gen", "driver.cpp"), PJ(ddir, "gen", "CFBundle.cpp"), PJ(ddir, "gen", "llvm", "Config", "config.h")};
        for (int i = 0; i < 5; i++) buf_addf(&m, "dsymutil-input %s %s\n", sha(df[i]), df[i]);
        char *dsrc = PJ(ddir, "llvm", "tools", "dsymutil");
        Strs entries = {0};
        if (!list_dir(dsrc, &entries)) fail("read dsymutil sources failed");
        for (int i = 0; i < entries.n; i++) {
            char *f = PJ(dsrc, entries.v[i]);
            if (is_dir(f)) continue;
            buf_addf(&m, "dsymutil-input %s %s\n", sha(f), f);
        }
        char *vsrc = PJ(o->native, "version", "clang", "lib", "Basic", "Version.cpp");
        buf_addf(&m, "input %s %s\n", sha(vsrc), vsrc);
        char *lf[] = {PJ(o->native, "version", "lld", "Common", "Version.cpp"), PJ(o->native, "version", "lld-gen", "lld", "Common", "Version.inc")};
        for (int i = 0; i < 2; i++) buf_addf(&m, "input %s %s\n", sha(lf[i]), lf[i]);
    }
    /* Hash all explicit file arguments, including LLVM static archives and CRT.
     * Linker-script transitive inputs and -l search resolutions remain part of
     * the supplied SDK identity, not a claimed exhaustive dependency closure. */
    for (int i = 0; i < inputs.n; i++) {
        buf_addf(&m, "link-arg %s\n", inputs.v[i]);
        if (is_file(inputs.v[i])) buf_addf(&m, "input %s %s\n", sha(inputs.v[i]), inputs.v[i]);
    }
    return m;
}

/* Include every freshly built C/C++ object, not just the final link. Names are
 * stage-relative so the two passes compare artifacts, not compiler aliases. */
static char *stage_hashes(const Opts *o) {
    Strs names = {0};
    zip_files(o->stage, "", &names);
    strs_sort(&names);
    Buf hashes = {0};
    for (int i = 0; i < names.n; i++) {
        if (ends_with(names.v[i], ".o") || ends_with(names.v[i], ".a"))
            buf_addf(&hashes, "artifact %s %s\n", sha(PJ(o->stage, names.v[i])), names.v[i]);
    }
    buf_addf(&hashes, "bridge-sha256 %s\nversion-sha256 %s\nlld-version-sha256 %s\nbinary-sha256 %s\n", sha(o->bridge), sha(o->version), sha(o->lld_version), sha(o->binary));
    return (char *)buf_cstr(&hashes);
}

static void release_target(const Opts *o, const char *boot, const char *safe_stage, const char *host_receipt) {
    build_stage(o, boot);
    char *first = stage_hashes(o);
    int native = !strcmp(o->target, host());
    const char *repeat_compiler = boot;
    if (native) {
        /* Never execute a path that the rebuild will truncate. A copy rather
         * than a hard link also isolates aliases from the final output inode. */
        char *safe = PJ(safe_stage, sfmt("stage2-%s%s", o->target, EXE_SUFFIX));
        copy_new(safe, o->binary);
        repeat_compiler = safe;
    }
    remove_file(o->binary);
    if (*o->bridge_args) remove_file(o->bridge);
    build_stage(o, repeat_compiler);
    if (strcmp(first, stage_hashes(o))) fail("%s failed: binary or bridge/object hashes differ; unpublished stages retained at %s", native ? "native stage2/stage3 convergence" : "cross same-input repeatability", o->stage);
    const char *verification = native ? "native stage2/stage3 convergence (binary and all fresh objects)" : "cross same-input repeatability using stage1; target not executed; NOT native convergence";
    Buf m = build_receipt(o, boot, verification);
    buf_str(&m, first);
    Strs host_lines = split_lines(host_receipt);
    for (int i = 0; i < host_lines.n; i++) buf_addf(&m, "host-stage1 %s\n", host_lines.v[i]);
    const char *destination = *o->build_only ? o->build_only : o->output;
    mkdirs(path_dir(destination));
    if (*o->build_only) {
        char *pending = sfmt("%s.selfhost-stage", destination);
        copy_new(pending, o->binary);
        rename_path(pending, destination);
    }
    write_file(sfmt("%s.build-manifest.txt", destination), buf_cstr(&m));
    if (!*o->build_only) package_release(o, sha(o->binary));
    else printf("release: verified native executable %s\n", destination);
}

static void set_target_option(Opts *o, const char *key, const char *value, const char *base) {
    if (!strcmp(key, "target")) o->target = xstrdup(value);
    else if (!strcmp(key, "llvm-prefix")) o->llvm = config_path(base, value);
    else if (!strcmp(key, "native-dir")) o->native = config_path(base, value);
    else if (!strcmp(key, "bridge")) o->bridge = config_path(base, value);
    else if (!strcmp(key, "bridge-args")) o->bridge_args = config_path(base, value);
    else if (!strcmp(key, "version-object")) o->version = config_path(base, value);
    else if (!strcmp(key, "lld-version-object")) o->lld_version = config_path(base, value);
    else if (!strcmp(key, "path-args")) o->path_args = config_path(base, value);
    else if (!strcmp(key, "link-args")) o->link_args = config_path(base, value);
    else if (!strcmp(key, "link-mode")) o->link_mode = xstrdup(value);
    else if (!strcmp(key, "linux-glibc")) o->linux_glibc = xstrdup(value);
    else if (!strcmp(key, "cpu")) o->cpu = xstrdup(value);
    else if (!strcmp(key, "sdk")) o->sdk = config_path(base, value);
    else if (!strcmp(key, "output")) o->output = config_path(base, value);
    else if (!strcmp(key, "extra-licenses")) o->extra_licenses = config_path(base, value);
    else if (!strcmp(key, "msvc-helper")) o->msvc_helper = config_path(base, value);
    else if (!strcmp(key, "package-only")) o->package_binary = config_path(base, value);
    else fail("unknown target option %s", key);
}

static int matrix_targets(const Opts *common, const char *file, Opts out[4]) {
    Json *data = json_load(file);
    if (data->type != J_OBJ) fail("matrix must be an object");
    for (int i = 0; i < data->n; i++) {
        if (strcmp(data->keys[i], "targets")) fail("unknown matrix key %s", data->keys[i]);
    }
    Json *entries = jarr_opt(data, "targets");
    if (!entries || entries->n < 1 || entries->n > 4) fail("matrix requires one to four distinct advertised targets");
    for (int e = 0; e < entries->n; e++) {
        Json *item = entries->items[e];
        if (item->type != J_OBJ) fail("matrix target must be an object");
        Opts o = *common;
        o.target = "";
        for (int k = 0; k < item->n; k++) {
            if (item->items[k]->type != J_STR) fail("matrix field %s must be a string", item->keys[k]);
            if (!strcmp(item->keys[k], "package-only")) fail("matrix builds from source; package-only is not allowed");
            set_target_option(&o, item->keys[k], item->items[k]->str, o.root);
        }
        if (strcmp(o.target, "linux-amd64") && strcmp(o.target, "darwin-arm64") && strcmp(o.target, "darwin-amd64") && strcmp(o.target, "windows-amd64")) fail("unsupported matrix target \"%s\"", o.target);
        for (int p = 0; p < e; p++) {
            if (!strcmp(out[p].target, o.target)) fail("duplicate matrix target %s", o.target);
        }
        out[e] = o;
    }
    return entries->n;
}

static Opts default_options(void) {
    Opts o;
    memset(&o, 0, sizeof o);
    char **fields[] = {&o.root, &o.target, &o.llvm, &o.native, &o.bridge, &o.bridge_args, &o.link_args, &o.sdk, &o.output, &o.stage, &o.object, &o.binary, &o.cpu, &o.package_binary, &o.extra_licenses, &o.version, &o.lld_version, &o.msvc_helper, &o.windows_path, &o.path_args, &o.build_tool, &o.compiler, &o.host_config, &o.build_only, &o.resource_root};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) *fields[i] = "";
    o.link_mode = "cc";
    o.linux_glibc = "2.34";
    o.profile = "release";
    o.target = host();
    return o;
}

static char *seed_resource_root(const char *compiler) {
    const char *override = env_get("CROSS_CC_SEED_ROOT");
    char *dir = *override ? abs_path(override) : path_dir(compiler);
    for (;;) {
        if (is_file(PJ(dir, "upstream.lock.json")) && is_dir(PJ(dir, "lib", "include")) && is_dir(PJ(dir, "lib", "libc"))) return dir;
        char *parent = path_dir(dir);
        if (*override || !strcmp(parent, dir) || !*parent) break;
        dir = parent;
    }
    fail("seed resources missing: compiler needs its own upstream.lock.json, lib/include and lib/libc (or CROSS_CC_SEED_ROOT)");
    return NULL;
}

static Opts host_options(const Opts *common) {
    Opts h = default_options();
    h.root = common->root;
    h.build_tool = common->build_tool;
    h.compiler = common->compiler;
    h.host_config = common->host_config;
    h.profile = common->profile;
    h.host_stage = 1;
    Json *data = json_load(common->host_config);
    if (data->type != J_OBJ) fail("host config must be a recipe object");
    for (int i = 0; i < data->n; i++) {
        if (data->items[i]->type != J_STR) fail("host config field %s must be a string", data->keys[i]);
        const char *k = data->keys[i];
        if (!strcmp(k, "package-only") || !strcmp(k, "output") || !strcmp(k, "bridge") || !strcmp(k, "version-object") || !strcmp(k, "lld-version-object")) fail("host config must build fresh objects, not %s", k);
        set_target_option(&h, k, data->items[i]->str, h.root);
    }
    if (strcmp(h.target, host())) fail("host config target %s does not match %s", h.target, host());
    h.stage = PJ(h.root, "build", "release", "host-stage1.stage");
    h.resource_root = seed_resource_root(h.compiler);
    return preflight(h);
}

static Opts native_defaults(Opts o, const Opts *h) {
    if (strcmp(o.target, host())) return o;
    if (!*o.llvm) o.llvm = h->llvm;
    if (!*o.native) o.native = h->native;
    if (!*o.sdk) o.sdk = h->sdk;
    if (!*o.link_args) { o.link_args = h->link_args; o.link_mode = h->link_mode; }
    if (!*o.bridge && !*o.bridge_args) o.bridge_args = h->bridge_args;
    if (!*o.path_args) o.path_args = h->path_args;
    if (!*o.cpu) o.cpu = h->cpu;
    return o;
}

int main(int argc, char **argv) {
    tool_name = "release";
    Opts o = default_options();
    char *cwd = getcwd_dup();
    char *self = abs_path(argv[0]);
    o.root = find_root(argv[0]);
    o.build_tool = PJ(path_dir(self), sfmt("build%s", EXE_SUFFIX));
    char *matrix_file = NULL;
    int single_options = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { fputs(USAGE, stdout); return 0; }
        const char *eq = strchr(a, '=');
        if (!eq || !starts_with(a, "--")) fail("expected --option=value, got %s", a);
        char *key = (char *)xmalloc((size_t)(eq - a) + 1);
        memcpy(key, a, (size_t)(eq - a));
        key[eq - a] = 0;
        const char *value = eq + 1;
        if (!strcmp(key, "--matrix")) {
            if (matrix_file) fail("--matrix specified twice");
            matrix_file = abs_path(value);
        } else if (!strcmp(key, "--native-dir")) o.native = abs_path(value);
        else if (!strcmp(key, "--compiler")) {
            if (!is_abs(value)) fail("--compiler must be an absolute path selected by the launcher/build tool");
            o.compiler = xstrdup(value);
        } else if (!strcmp(key, "--host-config")) o.host_config = abs_path(value);
        else if (!strcmp(key, "--profile")) o.profile = xstrdup(value);
        else if (!strcmp(key, "--build-only")) { o.build_only = abs_path(value); single_options = 1; }
        else { single_options = 1; set_target_option(&o, key + 2, value, cwd); }
    }
    if (strcmp(o.profile, "debug") && strcmp(o.profile, "release")) fail("--profile must be debug or release");
    if (matrix_file && single_options) fail("--matrix may accompany only --native-dir, --compiler, --host-config and --profile");
    if (*o.package_binary) {
        if (*o.build_only) fail("--build-only cannot accompany --package-only");
        o = preflight(o);
        o.binary = o.package_binary;
        mkdirs(PJ(o.root, ".cache", "tmp"));
        package_release(&o, sha(o.binary));
        return 0;
    }
    if (!*o.compiler || !*o.host_config) fail("--compiler=/absolute/cross_cc and --host-config=FILE are required for self-host builds");
    require_file(o.compiler);
    require_file(o.host_config);
    Opts h = host_options(&o);
    Opts targets[4];
    int count = 1;
    targets[0] = o;
    if (matrix_file) count = matrix_targets(&o, matrix_file, targets);
    for (int i = 0; i < count; i++) {
        targets[i] = native_defaults(targets[i], &h);
        if (*targets[i].build_only && strcmp(targets[i].target, host())) fail("--build-only publishes only a native executable");
        targets[i] = preflight(targets[i]);
        for (int j = 0; j < i; j++) {
            const char *a = targets[i].output, *b = targets[j].output;
            if (!strcmp(a, b) || starts_with(a, sfmt("%s.", b)) || starts_with(b, sfmt("%s.", a)) || starts_with(a, sfmt("%s/", b)) || starts_with(b, sfmt("%s/", a))) fail("overlapping matrix outputs: %s and %s", a, b);
        }
    }
    mkdirs(PJ(o.root, ".cache", "tmp"));
    claim_stage(h.stage);
    for (int i = 0; i < count; i++) claim_stage(targets[i].stage);
    build_stage(&h, o.compiler);
    Buf hm = build_receipt(&h, o.compiler, "seed-built fresh host stage1; not a convergence claim");
    buf_str(&hm, stage_hashes(&h));
    for (int i = 0; i < count; i++) release_target(&targets[i], h.binary, h.stage, buf_cstr(&hm));
    /* Failed verification retains all stages. Only success removes owned paths. */
    for (int i = 0; i < count; i++) remove_all(targets[i].stage);
    remove_all(h.stage);
    return 0;
}
