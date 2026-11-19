/* Offline differential links against the pinned Zig. Run once per target:
 *
 *   cc -std=c11 -Isrc -Itests -o build/linker_flags tests/linker_flags_integration.c
 *   build/linker_flags CC REFERENCE OUTDIR TARGET
 *
 * CC and REFERENCE are both driven as `TOOL cc ...`. Each case's command,
 * stdout and stderr are kept in OUTDIR; only success/failure must agree. */
#include "integration_process.h"

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: linker_flags CC REFERENCE OUTDIR TARGET\n"); return 2; }
    const char *cc = argv[1], *reference = argv[2], *dir = argv[3], *target = argv[4];
    (void)mkdir(dir, 0777);
    char *source = ip_join(dir, "main.c");
    char *mapfile = ip_join(dir, "version.map");
    ip_write_or_exit(source, "int main(void) { return 0; }\n");
    ip_write_or_exit(mapfile, "V1 { global: main; local: *; };\n");
    char *version_script = ip_format("-Wl,--version-script=%s", mapfile);
    char *dynamic_list = ip_format("-Wl,--dynamic-list=%s", mapfile);
    char *wl_mapfile = ip_format("-Wl,%s", mapfile);
    /* Each case is a NULL-terminated flag list; an empty list is the baseline. */
    const char *cases[][4] = {
        {NULL}, {"-Wl,--gc-sections"}, {"-Wl,--no-gc-sections"}, {version_script},
        {"-Wl,--as-needed"}, {"-Wl,--no-as-needed"}, {"-Wl,-z,relro"}, {"-Wl,-z,now"}, {"-Wl,-z,noexecstack"},
        {"-Wl,--no-undefined"}, {"-Wl,-O1"}, {"-Wl,--build-id"}, {"-Wl,--build-id=none"}, {"-Wl,--strip-all"}, {"-Wl,-s"},
        {"-Wl,--export-dynamic"}, {"-Wl,-E"}, {"-Wl,--whole-archive,--no-whole-archive"}, {"-Wl,--start-group,--end-group"},
        {"-Wl,-soname,libx.so"}, {"-Wl,--sort-common"}, {"-Wl,--hash-style=gnu"}, {"-Wl,-dead_strip"}, {"-Wl,-rpath,/opt/lib"},
        {"-Wl,--subsystem,console"}, {"-Wl,--enable-auto-import"}, {"-Wl,--stack,1048576"}, {"-Wl,-z,stack-size=1048576"},
        {"-Wl,-headerpad_max_install_names"}, {"-Wl,--icf=all"}, {"-Wl,--no-entry"}, {"-Wl,--eh-frame-hdr"}, {"-Wl,-Bsymbolic"},
        {"-Wl,--compress-debug-sections=zlib"}, {"-Wl,--threads=1"}, {"-Wl,-x"}, {"-Wl,-S"}, {"-Wl,--allow-shlib-undefined"},
        {dynamic_list}, {"-Wl,--exclude-libs,ALL"}, {"-Wl,--disable-new-dtags"}, {"-Wl,--print-gc-sections"},
        {"-Xlinker", "--version-script", wl_mapfile}, {"-Wl,--version-script", "-Xlinker", mapfile},
        {"-Xlinker", "-rpath=/opt/lib"}, {"-Wl,--version-script"}, {"-Wl,-z"}, {"-Wl,-z,unknown"},
        {"-c", "-Wl,--version-script"}, {"-c", "-Wl,--version-script=missing.map,--gc-sections"},
        {"-Wl,-zrelro"}, {"-z", "relro"}, {"-Wl,--gc-sections,--no-gc-sections"},
    };
    size_t count = sizeof cases / sizeof cases[0];
    int mismatches = 0;
    for (size_t i = 0; i < count; i++) {
        int codes[2];
        const char *tools[2] = {reference, cc};
        for (int j = 0; j < 2; j++) {
            Argv args = {0};
            char *output = ip_format("%s/%02zu-%d.out", dir, i, j);
            ip_push(&args, tools[j]);
            ip_push(&args, "cc"); ip_push(&args, "-target"); ip_push(&args, target);
            ip_push(&args, "-O2"); ip_push(&args, "-g0"); ip_push(&args, source); ip_push(&args, "-o"); ip_push(&args, output);
            for (int k = 0; k < 4 && cases[i][k]; k++) ip_push(&args, cases[i][k]);
            char *stem = ip_format("%s/%02zu-%d", dir, i, j);
            char *text = ip_argv_text(&args, 0, "\n");
            char *command_text = ip_format("%s\n", text);
            ip_write_or_exit(ip_format("%s.command", stem), command_text);
            codes[j] = ip_run(&args, ip_format("%s.stdout", stem), ip_format("%s.stderr", stem));
            free(text); free(command_text); free(args.data);
        }
        int same = (codes[0] == 0) == (codes[1] == 0);
        if (!same) mismatches++;
        Argv flags = {0};
        for (int k = 0; k < 4 && cases[i][k]; k++) ip_push(&flags, cases[i][k]);
        char *flag_text = flags.len ? ip_argv_text(&flags, 0, " ") : ip_format("%s", "");
        printf("%s\t%02zu\tref=%d\town=%d\t%s\t%s\n", target, i, codes[0], codes[1], same ? "MATCH" : "DIFF", flag_text);
        free(flag_text); free(flags.data);
    }
    printf("%s: %zu cases, %d status mismatches; commands/stdout/stderr in %s\n", target, count, mismatches, dir);
    return mismatches != 0;
}
