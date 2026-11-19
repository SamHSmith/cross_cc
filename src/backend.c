#include "cross.h"

extern int cross_cc_dsymutil_main(int argc, char **argv);
extern bool BridgeLLDLinkELF(int argc, const char **argv, bool can_exit_early, bool disable_output);
extern bool BridgeLLDLinkCOFF(int argc, const char **argv, bool can_exit_early,
                              bool disable_output);
extern bool BridgeLLDLinkWasm(int argc, const char **argv, bool can_exit_early,
                              bool disable_output);
extern bool BridgeLLDLinkMachO(int argc, const char **argv, bool can_exit_early,
                               bool disable_output);

/* Each embedded tool runs in its own process, isolating LLVM command-line state. */
void dispatch_backend(int argc, char **argv)
{
    StrVec args = {0};
    if (argc > 1 && streq(argv[1], "-cross-cc-mingw-link"))
    {
        sv_push(&args, "/cross_cc/ld.lld");
        for (int i = 2; i < argc; i++)
            sv_push(&args, argv[i]);
        exit(BridgeLLDLinkMinGW((int)args.len, (const char **)args.data, true, false) ? 0 : 1);
    }
    char *name = path_base(argv[0]);
    if (ends_with(name, ".exe"))
        name = str_ncopy(name, strlen(name) - 4);
    bool dsym_alias = streq(name, EMBEDDED_DSYMUTIL_NAME);
    bool cc1 = !dsym_alias && argc > 1 && (streq(argv[1], "-cc1") || streq(argv[1], "-cc1as"));
    bool clang = streq(name, "cross_clang") || streq(name, "clang") || cc1;
    bool ar = streq(name, "llvm-ar") || streq(name, "llvm-ranlib");
    bool elf = streq(name, "ld.lld"), coff = streq(name, "lld-link"), wasm = streq(name, "wasm-ld"),
         macho = streq(name, "ld64.lld");
    bool outer = !clang && !ar && !elf && !coff && !wasm && !macho;
    bool dsym =
        dsym_alias || (outer && argc > 1 &&
                       (streq(argv[1], "--cross-cc-dsymutil") || streq(argv[1], "dsymutil")));
    if (!clang && !ar && !elf && !coff && !wasm && !macho && !dsym)
        return;
    sv_push(&args, argv[0]);
    if (clang && !cc1)
    {
        /* The bridge consumes its outer argv[0]; keep a Clang name after it. */
        sv_push(&args, argv[0]);
        char *lib = env("CROSS_CC_LIB_DIR");
        if (!*lib)
            lib = first_existing(path_join(project_root(), "lib"));
        if (*lib)
            sv_extend(&args, SV("-resource-dir", lib));
    }
    for (int i = dsym && !dsym_alias ? 2 : 1; i < argc; i++)
        sv_push(&args, argv[i]);
    if (coff)
        args.data[0] = "/cross_cc/lld-link";
    if (dsym)
        exit(cross_cc_dsymutil_main((int)args.len, args.data));
    if (clang)
        exit(BridgeClang_main((int)args.len, args.data));
    if (ar)
        exit(BridgeLlvmAr_main((int)args.len, args.data));
    bool ok = false;
    if (elf)
        ok = BridgeLLDLinkELF((int)args.len, (const char **)args.data, true, false);
    if (coff)
        ok = BridgeLLDLinkCOFF((int)args.len, (const char **)args.data, true, false);
    if (wasm)
        ok = BridgeLLDLinkWasm((int)args.len, (const char **)args.data, true, false);
    if (macho)
        ok = BridgeLLDLinkMachO((int)args.len, (const char **)args.data, true, false);
    exit(ok ? 0 : 1);
}
