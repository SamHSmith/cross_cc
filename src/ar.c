/* Static archives: `-shared -static` archive builds and the llvm-ar family of subcommands. */
#include "cross.h"
#include "resources.h"

/* An argument the option needs is absent: stop like an out-of-range operand read. */
static const char *ar_value(StrVec args, size_t i) {
    if (i >= args.len) fatal("missing value for archive option");
    return args.data[i];
}

static int ar_finish(const char *work, int status) {
    remove_tree(work);
    return status;
}

/* A shared-library request followed by -static selects a static archive in
   Zig's driver, rather than a shared ELF with statically linked dependencies. */
int run_static_library(Options o, Target t, Toolchain tc, Runtime rt) {
    char *err = NULL;
    char *work = make_temp_dir(tc.cache_dir, "archive-stage-", &err);
    if (!work || err) fatal("cannot create static-library work directory");
    StrVec common = {0}, inputs = {0}, languages = {0}, objects = {0};
    const char *language = "";
    bool operands = false;
    for (size_t i = 0; i < o.forwarded.len; i++) {
        const char *a = o.forwarded.data[i];
        if (!operands) {
            if (streq(a, "--")) { operands = true; continue; }
            if (streq(a, "-shared")) continue;
            if (streq(a, "-o") || streq(a, "--output")) { i++; continue; }
            if ((starts_with(a, "-o") && !has_prefix_any(a, "-objc", "-openmp", "-obj")) || starts_with(a, "--output=")) continue;
            if (streq(a, "-x") || streq(a, "--language")) { i++; language = ar_value(o.forwarded, i); continue; }
            if (starts_with(a, "-x") && strlen(a) > 2) { language = a + 2; continue; }
            if (starts_with(a, "--language=")) { language = a + 11; continue; }
            bool link_value = streq(a, "-L") || streq(a, "-l") || streq(a, "-Xlinker") || streq(a, "-framework") || streq(a, "-F") || streq(a, "-u") || streq(a, "-T") || streq(a, "-z");
            if (link_value) { i++; continue; }
            if (has_prefix_any(a, "-Wl,", "-L", "-l", "-fuse-ld=")) continue;
            if (takes_value(a)) { sv_push(&common, a); sv_push(&common, ar_value(o.forwarded, i+1)); i++; continue; }
            if (starts_with(a, "-") && !streq(a, "-")) { sv_push(&common, a); continue; }
        }
        if (!*language || streq(language, "none")) {
            char *ext = path_ext(a);
            if (streq(ext, ".o") || streq(ext, ".obj") || streq(ext, ".bc")) { sv_push(&objects, a); continue; }
            /* Zig does not incorporate library inputs into a new archive. */
            if (streq(ext, ".a") || streq(ext, ".lib") || streq(ext, ".so") || streq(ext, ".dylib")) continue;
        }
        sv_push(&inputs, a);
        sv_push(&languages, language);
    }
    for (size_t i = 0; i < inputs.len; i++) {
        const char *source = inputs.data[i];
        char *dir = path_join(work, str_fmt("%zu", i));
        ensure_dir(dir);
        char *base = path_base(source);
        char *ext = path_ext(base);
        char *stem = str_ncopy(base, strlen(base) - strlen(ext));
        char *object = path_join(dir, str_fmt("%s.o", stem));
        Options compile = o;
        compile.shared = false;
        compile.compile_only = true;
        StrVec forwarded = sv_copy(common);
        if (*languages.data[i] && !streq(languages.data[i], "none")) { sv_push(&forwarded, "-x"); sv_push(&forwarded, languages.data[i]); }
        sv_push(&forwarded, "-c"); sv_push(&forwarded, "-o"); sv_push(&forwarded, object);
        sv_push(&forwarded, "--"); sv_push(&forwarded, source);
        compile.forwarded = forwarded;
        char *message = NULL;
        StrVec cmd = compiler_command(compile, t, tc, rt, false, &message);
        if (message && *message) fatal(message);
        int status = run_command(cmd, o.verbose);
        if (status != 0) return ar_finish(work, status);
        sv_push(&objects, object);
    }
    const char *output = o.output;
    if (!output || !*output) output = streq(t.os, "windows") ? "a.exe" : "a.out";
    char *archive = path_join(work, "result.a");
    StrVec cmd = {0};
    sv_push(&cmd, tc.ar); sv_push(&cmd, "rcs"); sv_push(&cmd, archive);
    if (streq(t.os, "macos")) sv_push(&cmd, "--format=darwin");
    sv_extend(&cmd, objects);
    if (o.dry_run) {
        for (size_t i = 0; i < cmd.len; i++) fprintf(stderr, "%s%s", i ? " " : "", cmd.data[i]);
        fputc('\n', stderr);
        return ar_finish(work, 0);
    }
    int status = run_command(cmd, o.verbose);
    if (status != 0) return ar_finish(work, status);
    errno = 0;
    if (!copy_file(output, archive)) fatal(str_fmt("cannot write static library %s: %s", output, strerror(errno ? errno : EIO)));
    return ar_finish(work, 0);
}

/* run_llvm_ar is `zig ar`, `zig ranlib`, `zig lib` and `zig dlltool`: llvm-ar picks its mode from argv[0]'s
   stem, so the subcommand itself is passed as argv[0], as Zig does. Never returns. */
int run_llvm_ar(StrVec args) {
    char **argv = xmalloc(sizeof(char *) * (args.len + 1));
    for (size_t i = 0; i < args.len; i++) argv[i] = str_copy(args.data[i]);
    argv[args.len] = NULL;
    /* exit flushes LLVM's buffered output. */
    exit(BridgeLlvmAr_main((int)args.len, argv));
}
