/* Shared support-library recipes for tools/linux-release-deps.c and
 * tools/macos-x64-llvm.c: zlib, zstd and libxml2 from fixed source lists with
 * generated config headers. No Make, configure, Python or CMake. Every compile
 * and archive step is a literal argv run through the selected seed compiler:
 *   COMPILER cc ... -c SRC -o OBJ        COMPILER ar rcs LIB OBJ...
 * Requires "common.h" to be included first. */
#ifndef SUPPORT_DEPS_H
#define SUPPORT_DEPS_H

typedef struct {
    const char *compiler; /* absolute path of cross_cc */
    Strs target;          /* -target/-march/-isysroot flags */
    char *const *env;
    int jobs;
} SdCtx;

/* --compiler=/abs/cross_cc, else the first cross_cc on PATH; never gcc/cc. */
static char *sd_find_compiler(const char *arg) {
    if (arg && *arg) {
        if (!is_abs(arg)) fail("--compiler must be an absolute path, got \"%s\"", arg);
        if (access(arg, X_OK) != 0) fail("--compiler %s is not executable", arg);
        return xstrdup(arg);
    }
    const char *path = getenv("PATH");
    if (path) {
        char *copy = xstrdup(path), *save = NULL;
        for (char *d = strtok_r(copy, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
            if (!is_abs(d)) continue;
            char *cand = PJ(d, "cross_cc");
            if (is_file(cand) && access(cand, X_OK) == 0) return cand;
        }
    }
    fail("no cross_cc on PATH; pass --compiler=/abs/cross_cc");
    return NULL;
}

static void sd_echo(const Strs *cmd) {
    for (int i = 0; i < cmd->n; i++) printf("%s%s", i ? " " : "", cmd->v[i]);
    printf("\n");
}

/* Archive tool: `cross_cc ar`, e.g. sd_ar(c, cwd, "x", archive) or "rcs" LIB OBJ... */
static void sd_ar(const SdCtx *c, const char *cwd, const Strs *args) {
    Strs cmd = {0};
    strs_add(&cmd, c->compiler);
    strs_add(&cmd, "ar");
    strs_cat(&cmd, args);
    sd_echo(&cmd);
    int code = run_cmd(cmd.v, cwd, c->env);
    if (code != 0) fail("ar failed (exit %d)", code);
}

/* Compile srcs (relative to srcdir) with flags into objdir, then archive into lib. */
static void sd_static_lib(const SdCtx *c, const char *srcdir, const char *const *srcs, const Strs *flags, const char *objdir, const char *lib) {
    remove_all(objdir);
    mkdirs(objdir);
    Pool pool = {0};
    pool.limit = c->jobs;
    Strs objs = {0};
    for (int i = 0; srcs[i]; i++) {
        char *oname = xstrdup(srcs[i]);
        for (char *p = oname; *p; p++) if (*p == '/') *p = '_';
        char *obj = PJ(objdir, sfmt("%s.o", oname));
        strs_add(&objs, obj);
        Strs cmd = {0};
        strs_add(&cmd, c->compiler);
        strs_add(&cmd, "cc");
        strs_cat(&cmd, &c->target);
        strs_cat(&cmd, flags);
        strs_add(&cmd, "-c");
        strs_add(&cmd, PJ(srcdir, srcs[i]));
        strs_add(&cmd, "-o");
        strs_add(&cmd, obj);
        sd_echo(&cmd);
        pool_add(&pool, cmd.v, srcdir, c->env, srcs[i]);
    }
    pool_drain(&pool);
    char *part = sfmt("%s.tmp", lib);
    remove_file(part);
    remove_file(lib);
    Strs a = {0};
    strs_add(&a, "rcs");
    strs_add(&a, part);
    strs_cat(&a, &objs);
    sd_ar(c, srcdir, &a);
    rename_path(part, lib);
}

static void sd_install_headers(const char *srcdir, const char *incdir, const char *const *headers) {
    mkdirs(incdir);
    for (int i = 0; headers[i]; i++) copy_file(PJ(incdir, headers[i]), PJ(srcdir, headers[i]));
}

/* ---- zlib 1.3.1: configure --static. zconf.h gains Z_HAVE_UNISTD_H like configure's. ---- */
static const char *const SD_ZLIB_SOURCES[] = {"adler32.c", "crc32.c", "deflate.c", "infback.c", "inffast.c", "inflate.c", "inftrees.c", "trees.c", "zutil.c", "compress.c", "uncompr.c", "gzclose.c", "gzlib.c", "gzread.c", "gzwrite.c", NULL};

static void sd_build_zlib(const SdCtx *c, const char *src, const char *objdir, const char *libdir, const char *incdir, int posix) {
    Strs f = {0};
    static const char *const base[] = {"-O2", "-std=gnu11", "-DHAVE_VSNPRINTF", "-DHAVE_STDARG_H", NULL};
    strs_addv(&f, base);
    if (posix) {
        strs_add(&f, "-D_LARGEFILE64_SOURCE=1");
        strs_add(&f, "-DHAVE_HIDDEN");
        strs_add(&f, "-DZ_HAVE_UNISTD_H");
    }
    strs_add(&f, "-fno-stack-protector");
    strs_addf(&f, "-I%s", src);
    mkdirs(libdir);
    sd_static_lib(c, src, SD_ZLIB_SOURCES, &f, objdir, PJ(libdir, "libz.a"));
    mkdirs(incdir);
    copy_file(PJ(incdir, "zlib.h"), PJ(src, "zlib.h"));
    char *zconf = read_file(PJ(src, "zconf.h"), NULL);
    write_file(PJ(incdir, "zconf.h"), posix ? sfmt("#define Z_HAVE_UNISTD_H 1\n%s", zconf) : zconf);
}

/* ---- zstd 1.5.7 lib/: `make -C lib libzstd.a-mt` (multithread, no legacy, amd64 asm). ---- */
static const char *const SD_ZSTD_SOURCES[] = {
    "common/debug.c", "common/entropy_common.c", "common/error_private.c", "common/fse_decompress.c", "common/pool.c", "common/threading.c", "common/xxhash.c", "common/zstd_common.c",
    "compress/fse_compress.c", "compress/hist.c", "compress/huf_compress.c", "compress/zstd_compress.c", "compress/zstd_compress_literals.c", "compress/zstd_compress_sequences.c", "compress/zstd_compress_superblock.c", "compress/zstd_double_fast.c", "compress/zstd_fast.c", "compress/zstd_lazy.c", "compress/zstd_ldm.c", "compress/zstd_opt.c", "compress/zstd_preSplit.c", "compress/zstdmt_compress.c",
    "decompress/huf_decompress.c", "decompress/zstd_ddict.c", "decompress/zstd_decompress.c", "decompress/zstd_decompress_block.c", "decompress/huf_decompress_amd64.S",
    "dictBuilder/cover.c", "dictBuilder/divsufsort.c", "dictBuilder/fastcover.c", "dictBuilder/zdict.c", NULL};

static void sd_build_zstd(const SdCtx *c, const char *src, const char *objdir, const char *libdir, const char *incdir, int noexecstack) {
    char *lib = PJ(src, "lib");
    Strs f = {0};
    static const char *const base[] = {"-O3", "-fno-stack-protector", "-fomit-frame-pointer", "-fno-ident", "-DXXH_NAMESPACE=ZSTD_", "-DDEBUGLEVEL=0", "-DZSTD_MULTITHREAD", "-pthread", "-DZSTD_DISABLE_DEPRECATE_WARNINGS", NULL};
    strs_addv(&f, base);
    if (noexecstack) strs_add(&f, "-Wa,--noexecstack");
    strs_addf(&f, "-I%s", lib);
    strs_addf(&f, "-I%s", PJ(lib, "common"));
    strs_addf(&f, "-I%s", PJ(lib, "compress"));
    strs_addf(&f, "-I%s", PJ(lib, "decompress"));
    strs_addf(&f, "-I%s", PJ(lib, "dictBuilder"));
    mkdirs(libdir);
    sd_static_lib(c, lib, SD_ZSTD_SOURCES, &f, objdir, PJ(libdir, "libzstd.a"));
    static const char *const hdr[] = {"zstd.h", "zdict.h", "zstd_errors.h", NULL};
    sd_install_headers(lib, incdir, hdr);
}

/* ---- libxml2: full parser, schemas, RelaxNG, XPath, HTML... No http/iconv/icu/lzma/zlib/modules. ---- */
static const char *const SD_XML2_SOURCES[] = {"buf.c", "chvalid.c", "dict.c", "encoding.c", "entities.c", "error.c", "globals.c", "hash.c", "list.c", "parser.c", "parserInternals.c", "SAX2.c", "threads.c", "tree.c", "uri.c", "valid.c", "xmlIO.c", "xmlmemory.c", "xmlstring.c", "c14n.c", "catalog.c", "debugXML.c", "HTMLparser.c", "HTMLtree.c", "xmlmodule.c", "xmlsave.c", "pattern.c", "xmlreader.c", "xmlregexp.c", "xmlunicode.c", "relaxng.c", "xmlschemas.c", "xmlschemastypes.c", "schematron.c", "xmlwriter.c", "xinclude.c", "xpath.c", "xlink.c", "xpointer.c", NULL};
static const char *const SD_XML2_ON[] = {"C14N", "CATALOG", "DEBUG", "HTML", "ISO8859X", "OUTPUT", "PATTERN", "PUSH", "READER", "REGEXPS", "RELAXNG", "SAX1", "SCHEMAS", "SCHEMATRON", "THREADS", "VALID", "WRITER", "XINCLUDE", "XPATH", "XPTR", NULL};
static const char *const SD_XML2_OFF[] = {"HTTP", "ICONV", "ICU", "LZMA", "MODULES", "THREAD_ALLOC", "ZLIB", NULL};
static const char SD_XML2_CONFIG_POSIX[] = "#define HAVE_DECL_GETENTROPY 1\n#define HAVE_DECL_GLOB 1\n#define HAVE_DECL_MMAP 1\n#define HAVE_POSIX_THREADS 1\n#define HAVE_ATTRIBUTE_DESTRUCTOR 1\n#define HAVE_STDINT_H 1\n#define XML_SYSCONFDIR \"/etc\"\n";

static void sd_build_libxml2(const SdCtx *c, const char *version, const char *src, const char *genroot, const char *objdir, const char *libdir, const char *incdir) {
    char *gen = PJ(genroot, "gen");
    mkdirs(PJ(gen, "libxml"));
    char *text = read_file(PJ(src, "include", "libxml", "xmlversion.h.in"), NULL);
    int n[3];
    if (sscanf(version, "%d.%d.%d", &n[0], &n[1], &n[2]) != 3) fail("libxml2 version %s is not MAJOR.MINOR.PATCH", version);
    text = replace_all(text, "@VERSION@", version);
    text = replace_all(text, "@LIBXML_VERSION_NUMBER@", sfmt("%d", (n[0] * 100 + n[1]) * 100 + n[2]));
    text = replace_all(text, "@LIBXML_VERSION_EXTRA@", "");
    text = replace_all(text, "@MODULE_EXTENSION@", ".so");
    for (int i = 0; SD_XML2_ON[i]; i++) text = replace_all(text, sfmt("@WITH_%s@", SD_XML2_ON[i]), "1");
    for (int i = 0; SD_XML2_OFF[i]; i++) text = replace_all(text, sfmt("@WITH_%s@", SD_XML2_OFF[i]), "0");
    char *at = strchr(text, '@');
    if (at) fail("libxml2 xmlversion.h.in has an unhandled placeholder near: %.40s", at);
    write_file(PJ(gen, "libxml", "xmlversion.h"), text);
    write_file(PJ(gen, "config.h"), SD_XML2_CONFIG_POSIX);
    Strs f = {0};
    static const char *const base[] = {"-O2", "-std=gnu11", "-fno-stack-protector", "-DLIBXML_STATIC", "-DNDEBUG", "-DHAVE_CONFIG_H", NULL};
    strs_addv(&f, base);
    strs_addf(&f, "-I%s", gen);
    strs_addf(&f, "-I%s", PJ(src, "include"));
    mkdirs(libdir);
    sd_static_lib(c, src, SD_XML2_SOURCES, &f, objdir, PJ(libdir, "libxml2.a"));
    mkdirs(PJ(incdir, "libxml2", "libxml"));
    Strs names = {0};
    list_dir(PJ(src, "include", "libxml"), &names);
    for (int i = 0; i < names.n; i++) {
        size_t len = strlen(names.v[i]);
        if (len > 2 && !strcmp(names.v[i] + len - 2, ".h")) copy_file(PJ(incdir, "libxml2", "libxml", names.v[i]), PJ(src, "include", "libxml", names.v[i]));
    }
    copy_file(PJ(incdir, "libxml2", "libxml", "xmlversion.h"), PJ(gen, "libxml", "xmlversion.h"));
}

#endif
