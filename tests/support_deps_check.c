/* Cold-cache priming check (no real compiler, archives or network):
 *   support_deps_check --prime-mock /abs/NEW-workdir   (must not exist; it is created,
 *                                                        removed on success, kept on failure)
 * Drives sd_static_lib against a mock compiler and verifies the single no-target
 * `cc -dumpmachine` query (issued the same way on every supported host), its env/cwd,
 * ordering before every compile, preserved concurrency, and that a failing query aborts
 * before any compile starts. The normal mode below also runs it in an owned subdirectory
 * of its work path.
 *
 * Support-library test. Build with any C compiler, then:
 *   support_deps_test /abs/cross_cc /abs/linux-release-deps-prefix [/abs/workdir]
 * Compiles a program with the seed (`cross_cc cc`, linux gnu 2.34 x86-64-v2) against the
 * prefix's zlib, zstd and libxml2, runs it: zlib roundtrip, multithreaded zstd roundtrip,
 * libxml2 parse and schema validation of a valid and an invalid document. */
#include "../tools/common.h"
#include "../tools/support_deps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

static const char PROGRAM[] =
    "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
    "#include <zlib.h>\n#include <zstd.h>\n#include <libxml/parser.h>\n#include <libxml/xmlschemas.h>\n"
    "#define CHECK(c) do { if (!(c)) { fprintf(stderr, \"FAIL line %d: %s\\n\", __LINE__, #c); return 1; } } while (0)\n"
    "static int validate(xmlSchemaPtr schema, const char *xml) {\n"
    "  xmlDocPtr d = xmlReadMemory(xml, (int)strlen(xml), \"d.xml\", NULL, 0);\n"
    "  if (!d) return -1;\n"
    "  xmlSchemaValidCtxtPtr v = xmlSchemaNewValidCtxt(schema);\n"
    "  int r = xmlSchemaValidateDoc(v, d);\n"
    "  xmlSchemaFreeValidCtxt(v); xmlFreeDoc(d); return r;\n"
    "}\n"
    "int main(void) {\n"
    "  size_t n = 1 << 20; unsigned char *src = malloc(n), *z = malloc(n * 2), *back = malloc(n);\n"
    "  for (size_t i = 0; i < n; i++) src[i] = (unsigned char)((i * 7) ^ (i >> 5));\n"
    "  uLongf zl = (uLongf)(n * 2); CHECK(compress(z, &zl, src, (uLong)n) == Z_OK);\n"
    "  uLongf bl = (uLongf)n; CHECK(uncompress(back, &bl, z, zl) == Z_OK); CHECK(bl == n && !memcmp(src, back, n));\n"
    "  size_t cap = ZSTD_compressBound(n); unsigned char *c = malloc(cap);\n"
    "  ZSTD_CCtx *cx = ZSTD_createCCtx();\n"
    "  CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(cx, ZSTD_c_nbWorkers, 4)));\n"
    "  CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(cx, ZSTD_c_jobSize, 1 << 18)));\n"
    "  size_t cl = ZSTD_compress2(cx, c, cap, src, n); CHECK(!ZSTD_isError(cl));\n"
    "  size_t dl = ZSTD_decompress(back, n, c, cl); CHECK(dl == n && !memcmp(src, back, n));\n"
    "  ZSTD_freeCCtx(cx);\n"
    "  const char *xsd = \"<xs:schema xmlns:xs=\\\"http://www.w3.org/2001/XMLSchema\\\"><xs:element name=\\\"a\\\"><xs:complexType><xs:sequence>\"\n"
    "    \"<xs:element name=\\\"b\\\" type=\\\"xs:int\\\" maxOccurs=\\\"unbounded\\\"/></xs:sequence></xs:complexType></xs:element></xs:schema>\";\n"
    "  xmlDocPtr sd = xmlReadMemory(xsd, (int)strlen(xsd), \"s.xsd\", NULL, 0); CHECK(sd);\n"
    "  xmlSchemaParserCtxtPtr pc = xmlSchemaNewDocParserCtxt(sd); xmlSchemaPtr schema = xmlSchemaParse(pc); CHECK(schema);\n"
    "  CHECK(validate(schema, \"<a><b>1</b><b>2</b></a>\") == 0);\n"
    "  CHECK(validate(schema, \"<a><b>x</b></a>\") > 0);\n"
    "  CHECK(validate(schema, \"<a><c/></a>\") > 0);\n"
    "  CHECK(validate(schema, \"<a><b>1</a>\") == -1);\n"
    "  xmlSchemaFree(schema); xmlSchemaFreeParserCtxt(pc); xmlFreeDoc(sd);\n"
    "  puts(\"support libs ok\"); return 0;\n}\n";

static int run(char *const argv[], int quiet) {
    pid_t pid = fork();
    if (pid < 0) return 127;
    if (pid == 0) { (void)quiet; execv(argv[0], argv); perror(argv[0]); _exit(127); }
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}


#define PCHECK(c) do { if (!(c)) { fprintf(stderr, "prime-mock FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static const char MOCK_SH[] =
    "#!/bin/sh\n"
    "d=\"$(dirname \"$0\")\"\n"
    "echo \"$$ $PWD|CACHE=$CROSS_CC_CACHE_DIR|TMP=$TMPDIR|ROOT=$CROSS_CC_ROOT|ARGS=$*\" >> \"$d/log\"\n"
    "case \"$2\" in\n"
    "-dumpmachine) [ -f \"$d/fail\" ] && exit 7; echo mock-machine; exit 0;;\n"
    "esac\n"
    "if [ \"$1\" = ar ]; then : > \"$3\"; exit 0; fi\n"
    "mkdir \"$d/live.$$\"; sleep 1; n=$(ls -d \"$d\"/live.* | wc -l); echo $n >> \"$d/conc\"; rmdir \"$d/live.$$\"\n"
    "while [ $# -gt 0 ]; do [ \"$1\" = -o ] && : > \"$2\"; shift; done\n"
    "exit 0\n";

static int count_lines(const char *path, const char *needle, int *first) {
    char *t = exists(path) ? read_file(path, NULL) : NULL;
    int n = 0, i = 0;
    if (first) *first = -1;
    if (!t) return 0;
    for (char *save = NULL, *l = strtok_r(t, "\n", &save); l; l = strtok_r(NULL, "\n", &save), i++)
        if (strstr(l, needle)) { if (!n && first) *first = i; n++; }
    return n;
}

static int prime_mock(const char *work) {
    char *mdir = PJ(work, "mock"), *cc = PJ(mdir, "cross_cc");
    char *log = PJ(mdir, "log"), *conc = PJ(mdir, "conc");
    char *src = PJ(work, "src"), *cache = PJ(work, "cache"), *tmp = PJ(work, "tmp");
    if (exists(work)) { fprintf(stderr, "prime-mock: refusing existing path %s\n", work); return 2; }
    mkdirs(mdir); mkdirs(src);
    write_file(PJ(mdir, "upstream.lock.json"), "{}\n");
    write_file(cc, MOCK_SH);
    chmod(cc, 0755);
    Strs env = sd_seed_env(cc, cache, tmp, NULL);
    SdCtx c = {0};
    c.compiler = cc;
    strs_add(&c.target, "-target");
    strs_add(&c.target, "x86_64-linux-gnu.2.34");
    c.env = env.v;
    c.jobs = 4;
    static const char *const srcs[] = {"a.c", "b.c", "c.c", "d.c", "e.c", "f.c", "g.c", "h.c", NULL};
    Strs flags = {0};
    strs_add(&flags, "-O2");

    /* Success: exactly one query, first, no -target, same cwd/env as the compiles. */
    sd_static_lib(&c, src, srcs, &flags, PJ(work, "obj"), PJ(work, "libt.a"));
    PCHECK(is_file(PJ(work, "libt.a")));
    int first;
    PCHECK(count_lines(log, "ARGS=cc -dumpmachine", &first) == 1);
    PCHECK(first == 0);
    PCHECK(count_lines(log, "-dumpmachine", NULL) == 1);
    PCHECK(count_lines(log, "-target", NULL) == 8);
    PCHECK(count_lines(log, "ARGS=cc -target x86_64-linux-gnu.2.34 -O2 -c ", NULL) == 8);
    PCHECK(count_lines(log, "ARGS=ar rcs ", NULL) == 1);
    PCHECK(count_lines(log, sfmt("%s|CACHE=%s|TMP=%s|ROOT=%s|ARGS=cc -dumpmachine", src, cache, tmp, mdir), NULL) == 1);
    PCHECK(count_lines(log, sfmt("%s|CACHE=%s|TMP=%s|ROOT=%s|ARGS=cc -target", src, cache, tmp, mdir), NULL) == 8);
    /* Concurrency survives the initializer: some compile saw >1 live peers. */
    char *ct = read_file(conc, NULL);
    int peak = 0;
    for (char *save = NULL, *l = strtok_r(ct, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) if (atoi(l) > peak) peak = atoi(l);
    PCHECK(peak > 1 && peak <= 4);

    /* jobs == 1: no pool concurrency to protect, so no query. */
    remove_file(log);
    c.jobs = 1;
    sd_static_lib(&c, src, srcs, &flags, PJ(work, "obj"), PJ(work, "libt.a"));
    PCHECK(count_lines(log, "-dumpmachine", NULL) == 0);
    c.jobs = 4;

    /* Failing query: abort (nonzero exit) before any compile or archive. */
    remove_file(log);
    write_file(PJ(mdir, "fail"), "x\n");
    remove_file(PJ(work, "libt.a"));
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    PCHECK(pid >= 0);
    if (pid == 0) {
        sd_static_lib(&c, src, srcs, &flags, PJ(work, "obj"), PJ(work, "libt.a"));
        _exit(0);
    }
    int st = 0, wr;
    while ((wr = (int)waitpid(pid, &st, 0)) < 0 && errno == EINTR) {}
    PCHECK(wr == (int)pid);
    PCHECK(WIFEXITED(st) && WEXITSTATUS(st) != 0);
    PCHECK(count_lines(log, "-dumpmachine", NULL) == 1);
    PCHECK(count_lines(log, "", NULL) == 1);
    PCHECK(!is_file(PJ(work, "libt.a")));
    remove_all(work); /* created by this function above, so owned */
    puts("support_deps_check --prime-mock: PASS");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--prime-mock")) {
        if (argv[2][0] != '/') { fputs("usage: support_deps_check --prime-mock /abs/workdir\n", stderr); return 2; }
        return prime_mock(argv[2]);
    }
    if (argc < 3 || argv[1][0] != '/' || argv[2][0] != '/') { fputs("usage: support_deps_test /abs/cross_cc /abs/prefix [/abs/workdir]\n", stderr); return 2; }
    const char *cc = argv[1], *prefix = argv[2];
    char work[4096];
    if (argc > 3) snprintf(work, sizeof work, "%s", argv[3]);
    else snprintf(work, sizeof work, "%s/tmp/support_deps_test", prefix);
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "%s/tmp", prefix); mkdir(cmd, 0777);
    mkdir(work, 0777);
    {
        char owned[4400];
        snprintf(owned, sizeof owned, "%s/prime-mock-%ld", work, (long)getpid());
        if (prime_mock(owned) != 0) { fputs("support_deps_test: prime-mock failed\n", stderr); return 1; }
    }
    /* Isolation: the seed uses its own resources, not a checkout's. */
    unsetenv("CROSS_CC_LIB_DIR"); unsetenv("ZIG_LIB_DIR"); unsetenv("ZIG_LIBC"); unsetenv("CC"); unsetenv("CFLAGS");
    unsetenv("CPATH"); unsetenv("LIBRARY_PATH"); unsetenv("C_INCLUDE_PATH");
    const char *forced = getenv("CROSS_CC_SEED_ROOT");
    if (forced && *forced) setenv("CROSS_CC_ROOT", forced, 1); else unsetenv("CROSS_CC_ROOT");
    snprintf(cmd, sizeof cmd, "%s/cache", work); mkdir(cmd, 0777);
    setenv("CROSS_CC_CACHE_DIR", cmd, 1);
    char src[4400], exe[4400];
    snprintf(src, sizeof src, "%s/t.c", work);
    snprintf(exe, sizeof exe, "%s/t", work);
    FILE *f = fopen(src, "w");
    if (!f) { perror(src); return 1; }
    fputs(PROGRAM, f);
    fclose(f);
    char inc[4400], inc2[4400], l1[4400], l2[4400], l3[4400];
    snprintf(inc, sizeof inc, "-I%s/include", prefix);
    snprintf(inc2, sizeof inc2, "-I%s/include/libxml2", prefix);
    snprintf(l1, sizeof l1, "%s/lib/libxml2.a", prefix);
    snprintf(l2, sizeof l2, "%s/lib/libzstd.a", prefix);
    snprintf(l3, sizeof l3, "%s/lib/libz.a", prefix);
    char *build[] = {(char *)cc, "cc", "-target", "x86_64-linux-gnu.2.34", "-march=x86-64-v2", "-O1", "-DLIBXML_STATIC", inc, inc2, src, l1, l2, l3, "-lpthread", "-o", exe, NULL};
    if (run(build, 0) != 0) { fputs("support_deps_test: compile/link failed\n", stderr); return 1; }
    char *go[] = {exe, NULL};
    int code = run(go, 0);
    if (code != 0) { fprintf(stderr, "support_deps_test: program failed (%d)\n", code); return 1; }
    puts("support_deps_test: PASS");
    return 0;
}
