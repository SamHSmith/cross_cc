#include "cross.h"

bool takes_value(const char *a)
{
    static const char *const names[] = {"-o",
                                        "-x",
                                        "-I",
                                        "-D",
                                        "-U",
                                        "-include",
                                        "-imacros",
                                        "-isystem",
                                        "-idirafter",
                                        "-iquote",
                                        "-isysroot",
                                        "-iwithprefix",
                                        "-iwithprefixbefore",
                                        "-iprefix",
                                        "-iframework",
                                        "-isystem-after",
                                        "-cxx-isystem",
                                        "-imultilib",
                                        "-include-pch",
                                        "-L",
                                        "-T",
                                        "-Xlinker",
                                        "-Xclang",
                                        "-Xassembler",
                                        "-Xpreprocessor",
                                        "-MF",
                                        "-MT",
                                        "-MQ",
                                        "-MJ",
                                        "-z",
                                        "-u",
                                        "-e",
                                        "-arch",
                                        "-framework",
                                        "-F",
                                        "-B",
                                        "-mllvm",
                                        "--param",
                                        "-working-directory",
                                        "-dependency-file",
                                        "-rpath",
                                        "-undefined",
                                        "-install_name",
                                        "-current_version",
                                        "-compatibility_version",
                                        "-add-plugin",
                                        "-Xopenmp-target",
                                        "--sysroot",
                                        "--target",
                                        "-target",
                                        "--zig-lib-dir",
                                        "--global-cache-dir",
                                        "--cache-dir",
                                        "--serialize-diagnostics",
                                        "-serialize-diagnostics",
                                        "-l",
                                        "-Xcuda-ptxas",
                                        "-Xcuda-fatbinary",
                                        "-Xanalyzer",
                                        "-Xclangas",
                                        "-Xarch_device",
                                        "-Xarch_host",
                                        "-Xflang",
                                        "-resource-dir",
                                        "-ivfsoverlay",
                                        "-imsvc",
                                        "-iwithsysroot",
                                        "-iframeworkwithsysroot",
                                        "-iapinotes-modules",
                                        "-stdlib++-isystem",
                                        "-mfloat-abi",
                                        "-mfpmath",
                                        "-meabi",
                                        "-mtp",
                                        "-mthread-model",
                                        "-mregparm",
                                        "-fdebug-compilation-dir",
                                        "-ferror-limit",
                                        "-fdiagnostics-format",
                                        "-ftabstop",
                                        "-fconstant-string-class",
                                        "-ftrapv-handler",
                                        "-fmodules-user-build-path",
                                        "-force_load",
                                        "-reexport_library",
                                        "-reexport_framework",
                                        "-filelist",
                                        "-weak_library",
                                        "-weak_framework",
                                        "--output",
                                        "--language",
                                        "--include",
                                        "--include-directory",
                                        "--include-directory-after",
                                        "--include-prefix",
                                        "--include-with-prefix",
                                        "--include-with-prefix-before",
                                        "--include-with-prefix-after",
                                        "--define-macro",
                                        "--undefine-macro",
                                        "--library-directory",
                                        "--for-linker",
                                        "--force-link",
                                        "--std",
                                        "--stdlib",
                                        "--rtlib"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
        if (streq(a, names[i]))
            return true;
    return starts_with(a, "-Xarch_") || starts_with(a, "-Xopenmp-target=");
}

typedef struct
{
    char *value;
    size_t consumed;
    bool matched;
    char *error;
} LongOpt;
static LongOpt long_opt(StrVec args, size_t i, const char *name)
{
    const char *a = args.data[i];
    if (streq(a, name))
    {
        if (i + 1 >= args.len)
            return (LongOpt){.matched = true,
                             .error =
                                 str_fmt("argument to '%s' is missing (expected 1 value)", name)};
        return (LongOpt){args.data[i + 1], 2, true, NULL};
    }
    size_t n = strlen(name);
    if (strlen(a) > n && starts_with(a, name) && a[n] == '=')
    {
        if (!a[n + 1])
            return (LongOpt){.matched = true,
                             .error = str_fmt("argument to '%s' is missing (empty value)", name)};
        return (LongOpt){(char *)a + n + 1, 1, true, NULL};
    }
    return (LongOpt){0};
}

bool is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
bool has_prefix_any_n(const char *s, size_t count, ...)
{
    va_list args;
    va_start(args, count);
    bool found = false;
    for (size_t i = 0; i < count; i++)
        if (starts_with(s, va_arg(args, const char *)))
            found = true;
    va_end(args);
    return found;
}

/* Response file quoting follows the pinned reference argument iterator. */
char *tokenize_response(const char *text, StrVec *out)
{
    size_t len = strlen(text), i = 0;
    char *word = xmalloc(len + 1);
    for (;;)
    {
        while (i < len)
        {
            if (strchr(" \t\r\n", text[i]))
            {
                i++;
                continue;
            }
            if (text[i] == '#')
            {
                while (i < len && text[i] != '\n')
                    i++;
                continue;
            }
            break;
        }
        if (i >= len)
            break;
        size_t used = 0, bs = 0;
        bool quote = false;
        for (;;)
        {
            if (i >= len)
            {
                while (bs)
                {
                    word[used++] = '\\';
                    bs--;
                }
                break;
            }
            char c = text[i];
            if (c == '"' || c == '\'')
            {
                bool real = bs % 2 == 0;
                size_t count = bs / 2;
                while (count--)
                    word[used++] = '\\';
                bs = 0;
                if (real)
                    quote = !quote;
                else
                    word[used++] = '"';
            }
            else if (c == '\\')
                bs++;
            else
            {
                while (bs)
                {
                    word[used++] = '\\';
                    bs--;
                }
                if (strchr(" \t\r\n", c) && !quote)
                    break;
                word[used++] = c;
            }
            i++;
        }
        sv_push(out, str_ncopy(word, used));
    }
    free(word);
    return NULL;
}

static char *response_take(StrVec *argv, size_t *i, StrVec root, size_t saved, bool *in_response)
{
    char *arg = argv->data[(*i)++];
    if (*i >= argv->len && *in_response)
    {
        *argv = root;
        *i = saved;
        *in_response = false;
    }
    return arg;
}
StrVec expand_response_files(StrVec root, char **error)
{
    StrVec out = {0}, argv = root;
    size_t i = 0, saved = 0;
    bool in_response = false;
    *error = NULL;
    while (i < argv.len)
    {
        char *a = response_take(&argv, &i, root, saved, &in_response);
        if (starts_with(a, "@"))
        {
            if (in_response)
            {
                *error = "unable to parse command line parameters: NestedResponseFile";
                return out;
            }
            size_t n = 0;
            char *data = read_file(a + 1, &n);
            if (!data || n > 10 * 1024 * 1024)
            {
                *error = str_fmt("unable to read response file '%s'", a + 1);
                return out;
            }
            StrVec sub = {0};
            tokenize_response(data, &sub);
            saved = i;
            argv = sub;
            i = 0;
            in_response = true;
            if (!sub.len)
            {
                argv = root;
                i = saved;
                in_response = false;
                continue;
            }
            a = response_take(&argv, &i, root, saved, &in_response);
            if (starts_with(a, "@"))
            {
                *error = str_fmt("%s: unrecognized file extension", a);
                return out;
            }
        }
        sv_push(&out, a);
        if (takes_value(a) && i < argv.len)
            sv_push(&out, response_take(&argv, &i, root, saved, &in_response));
        if (out.len > (1u << 20))
        {
            *error = "too many arguments after response file expansion";
            return out;
        }
    }
    return out;
}
static bool is_header_language(const char *s)
{
    return streq(s, "c-header") || streq(s, "c++-header") || streq(s, "objective-c-header") ||
           streq(s, "objective-c++-header");
}
static bool nonlink_transport_consumed(StrVec args)
{
    for (size_t i = 0; i < args.len; i++)
    {
        char *a = args.data[i], *z = "";
        if (streq(a, "--gc-sections") || streq(a, "--no-gc-sections"))
            continue;
        if (streq(a, "-rpath") || streq(a, "--rpath") || streq(a, "-R"))
        {
            if (++i >= args.len)
                return false;
            continue;
        }
        if (starts_with(a, "-rpath=") || starts_with(a, "--rpath="))
            continue;
        if (streq(a, "-z"))
        {
            if (++i >= args.len)
                return false;
            z = args.data[i];
        }
        else if (starts_with(a, "-z"))
            z = a + 2;
        if (in_target_inventory(
                "nodelete notext defs undefs origin nocopyreloc noexecstack now lazy relro norelro",
                z))
            continue;
        return false;
    }
    return true;
}
StrVec stage_arguments(Options o)
{
    bool linking = needs_link(o);
    StrVec transport = {0}, kept = {0};
    if (!linking)
        for (size_t i = 0; i < o.forwarded.len; i++)
        {
            char *a = o.forwarded.data[i];
            if (streq(a, "--"))
                break;
            if (starts_with(a, "-Wl,"))
            {
                sv_extend(&transport, str_split(a + 4, ","));
                continue;
            }
            if ((streq(a, "-Xlinker") || streq(a, "--for-linker")) && i + 1 < o.forwarded.len)
            {
                sv_push(&transport, o.forwarded.data[++i]);
                continue;
            }
            if (takes_value(a))
                i++;
        }
    bool consume_transport = !linking && nonlink_transport_consumed(transport);
    for (size_t i = 0; i < o.forwarded.len; i++)
    {
        char *a = o.forwarded.data[i];
        if (streq(a, "--"))
        {
            sv_extend(&kept, sv_slice(o.forwarded, i, o.forwarded.len));
            break;
        }
        if (linking && streq(a, "-static-pie"))
            continue;
        if (!linking)
        {
            if (in_target_inventory("-shared -pie -no-pie -s -nolibc", a))
                continue;
            if (streq(a, "-l") || streq(a, "-L") || streq(a, "--library-directory"))
            {
                i++;
                continue;
            }
            if (strlen(a) > 2 && (starts_with(a, "-l") || starts_with(a, "-L")))
                continue;
            if (starts_with(a, "--library-directory="))
                continue;
            if (consume_transport)
            {
                if (starts_with(a, "-Wl,"))
                    continue;
                if (streq(a, "-Xlinker") || streq(a, "--for-linker"))
                {
                    i++;
                    continue;
                }
            }
        }
        sv_push(&kept, a);
        if (takes_value(a) && i + 1 < o.forwarded.len)
            sv_push(&kept, o.forwarded.data[++i]);
    }
    return kept;
}
bool debug_info_flag(const char *a)
{
    return in_target_inventory("-g -g1 -gline-tables-only -gdwarf -gdwarf32 -gdwarf64 -gdwarf-2 "
                               "-gdwarf-3 -gdwarf-4 -gdwarf-5",
                               a);
}
Options parse_arguments(StrVec args, bool cxx, char **error)
{
    return parse_arguments_with_env(args, cxx, env("CROSS_CC_MSVC"), error);
}
Options parse_arguments_with_env(StrVec args, bool cxx, const char *msvc_env, char **error)
{
    Options o = {0};
    o.target_name = o.cpu = o.output = o.sysroot = o.lib_dir = o.cache_dir = o.msvc_libc_file = "";
    o.cxx = cxx;
    StrVec list = expand_response_files(args, error);
    if (*error)
        return o;
    bool want_m32 = false, want_m64 = false, after = false, cli_msvc = false;
    char *trailing_language = "", *language = "";
    size_t file_inputs = 0, header_inputs = 0;
    for (size_t i = 0; i < list.len;)
    {
        char *a = list.data[i];
        if (after || !*a || a[0] != '-' || streq(a, "-"))
        {
            o.has_input = true;
            file_inputs++;
            if (is_header_language(language))
                header_inputs++;
            sv_push(&o.forwarded, a);
            i++;
            continue;
        }
        if (streq(a, "--"))
        {
            after = true;
            sv_push(&o.forwarded, a);
            i++;
            continue;
        }
        if (streq(a, "--cross-cc-accept-msvc-license"))
        {
            o.msvc_accept_license = true;
            i++;
            continue;
        }
        LongOpt lo = long_opt(list, i, "--cross-cc-msvc");
        if (lo.matched)
        {
            if (lo.error)
            {
                *error = lo.error;
                return o;
            }
            if (!streq(lo.value, "managed") && !streq(lo.value, "system"))
            {
                *error = "--cross-cc-msvc expects 'managed' or 'system'";
                return o;
            }
            o.msvc_system = streq(lo.value, "system");
            cli_msvc = true;
            i += lo.consumed;
            continue;
        }
        lo = long_opt(list, i, "--libc");
        if (lo.matched)
        {
            if (lo.error)
            {
                *error = lo.error;
                return o;
            }
            o.msvc_libc_file = lo.value;
            i += lo.consumed;
            continue;
        }
        if (streq(a, "-target") || streq(a, "--target") || starts_with(a, "--target="))
        {
            lo = long_opt(list, i, "--target");
            if (!lo.matched)
                lo = long_opt(list, i, "-target");
            if (lo.error)
            {
                *error = lo.error;
                return o;
            }
            o.target_name = lo.value;
            i += lo.consumed;
            continue;
        }
        if (starts_with(a, "-mcpu="))
        {
            o.cpu = a + 6;
            i++;
            continue;
        }
        if (streq(a, "-mcpu"))
        {
            if (i + 1 >= list.len)
            {
                *error = "argument to '-mcpu' is missing (expected 1 value)";
                return o;
            }
            o.cpu = list.data[i + 1];
            i += 2;
            continue;
        }
        bool location = false;
        const char *locations[] = {"--zig-lib-dir", "--global-cache-dir", "--cache-dir"};
        for (size_t k = 0; k < 3; k++)
        {
            lo = long_opt(list, i, locations[k]);
            if (!lo.matched)
                continue;
            if (lo.error)
            {
                *error = lo.error;
                return o;
            }
            if (!k)
                o.lib_dir = lo.value;
            else
                o.cache_dir = lo.value;
            i += lo.consumed;
            location = true;
            break;
        }
        if (location)
            continue;
        if (streq(a, "-dynamic"))
        {
            o.dynamic_link = true;
            o.static_link = false;
            i++;
            continue;
        }
        if (streq(a, "-static"))
        {
            o.static_link = true;
            o.dynamic_link = false;
            i++;
            continue;
        }
        if (streq(a, "-static-pie"))
        {
            sv_push(&o.forwarded, a);
            i++;
            continue;
        }
        if (streq(a, "-m32"))
        {
            want_m32 = true;
            want_m64 = false;
            i++;
            continue;
        }
        if (streq(a, "-m64"))
        {
            want_m64 = true;
            want_m32 = false;
            i++;
            continue;
        }
        if (streq(a, "-c") || streq(a, "--compile"))
            o.compile_only = true;
        else if (streq(a, "-E") || streq(a, "--preprocess"))
            o.preprocess_only = true;
        else if (streq(a, "-S") || streq(a, "--assemble"))
            o.assembly_only = true;
        else if (streq(a, "-fsyntax-only"))
            o.syntax_only = true;
        else if (in_target_inventory("-M -MM --dependencies --user-dependencies", a))
            o.dependency_only = true;
        else if (streq(a, "-shared"))
        {
            o.shared = true;
            o.dynamic_link = true;
            o.static_link = false;
        }
        else if (streq(a, "-r"))
            o.relocatable = true;
        else if (streq(a, "-municode"))
            o.municode = true;
        else if (streq(a, "-nostdinc++"))
        {
            o.no_stdincxx = true;
            o.no_stdlibxx = true;
        }
        else if (streq(a, "-nostdlib++"))
        {
            o.no_stdincxx = true;
            o.no_stdlibxx = true;
            sv_push(&o.forwarded, "-nostdinc++");
        }
        else if (streq(a, "-nostdlib"))
        {
            o.no_stdlib = true;
            o.no_defaultlibs = true;
            o.no_startfiles = true;
        }
        else if (streq(a, "-ffreestanding"))
            o.freestanding = true;
        else if (streq(a, "-v"))
            o.verbose = true;
        else if (streq(a, "-###"))
            o.dry_run = true;
        else if (streq(a, "-fpic") || streq(a, "-fPIC"))
            o.pic_mode = 1;
        else if (streq(a, "-fno-pic") || streq(a, "-fno-PIC"))
            o.pic_mode = -1;
        else if (in_target_inventory("-pie -fpie -fPIE", a))
        {
            o.pie_mode = 1;
            if (streq(a, "-pie"))
                o.pie = 1;
        }
        else if (in_target_inventory("-no-pie -nopie -fno-pie -fno-PIE", a))
        {
            o.pie_mode = -1;
            if (streq(a, "-no-pie"))
                o.pie = -1;
        }
        else if (in_target_inventory("--version -version --help -help -dumpmachine -dumpversion "
                                     "-dumpspecs --cross-cc-version",
                                     a))
            o.query = true;
        if (starts_with(a, "-print-"))
            o.query = true;
        if (streq(a, "-o") || streq(a, "--output"))
        {
            if (i + 1 >= list.len)
            {
                *error = str_fmt("argument to '%s' is missing (expected 1 value)", a);
                return o;
            }
            o.output = list.data[i + 1];
            sv_push(&o.forwarded, a);
            sv_push(&o.forwarded, list.data[i + 1]);
            i += 2;
            continue;
        }
        if (starts_with(a, "--output="))
        {
            o.output = a + 9;
            sv_push(&o.forwarded, a);
            i++;
            continue;
        }
        if (starts_with(a, "-o") && !has_prefix_any(a, "-objc", "-openmp", "-obj"))
        {
            o.output = a + 2;
            sv_push(&o.forwarded, a);
            i++;
            continue;
        }
        if (streq(a, "-x"))
        {
            if (i + 1 >= list.len)
            {
                *error = "argument to '-x' is missing (expected 1 value)";
                return o;
            }
            trailing_language = language = list.data[i + 1];
            sv_push(&o.forwarded, a);
            sv_push(&o.forwarded, list.data[i + 1]);
            i += 2;
            continue;
        }
        if (starts_with(a, "-x") && strlen(a) > 2)
            trailing_language = language = a + 2;
        lo = long_opt(list, i, "--sysroot");
        if (lo.matched)
        {
            if (lo.error)
            {
                *error = lo.error;
                return o;
            }
            o.sysroot = lo.value;
            for (size_t k = 0; k < lo.consumed; k++)
                sv_push(&o.forwarded, list.data[i + k]);
            i += lo.consumed;
            continue;
        }
        if (in_target_inventory("-l -L -I -D -U -F -B", a))
        {
            if (i + 1 >= list.len)
            {
                *error = str_fmt("argument to '%s' is missing (expected 1 value)", a);
                return o;
            }
            if (streq(a, "-l"))
            {
                o.has_input = true;
                if (streq(list.data[i + 1], "c"))
                    o.explicit_libc = true;
            }
            sv_push(&o.forwarded, a);
            sv_push(&o.forwarded, list.data[i + 1]);
            i += 2;
            continue;
        }
        if (starts_with(a, "-l") && strlen(a) > 2)
        {
            o.has_input = true;
            if (streq(a, "-lc"))
                o.explicit_libc = true;
        }
        if (streq(a, "-nolibc"))
            o.no_libc = true;
        if (takes_value(a))
        {
            if (i + 1 >= list.len)
            {
                *error = str_fmt("argument to '%s' is missing (expected 1 value)", a);
                return o;
            }
            sv_push(&o.forwarded, a);
            sv_push(&o.forwarded, list.data[i + 1]);
            i += 2;
            continue;
        }
        sv_push(&o.forwarded, a);
        i++;
    }
    char *target_error = NULL;
    Target target = resolve_target(o.target_name, &target_error);
    bool ci = !target_error && (streq(target.os, "windows") || streq(target.os, "macos"));
    StrVec kept = {0};
    bool value = false, operands = false;
    for (size_t k = 0; k < o.forwarded.len; k++)
    {
        char *a = o.forwarded.data[k];
        if (value || operands)
        {
            sv_push(&kept, a);
            value = false;
            continue;
        }
        if (streq(a, "--"))
            operands = true;
        char *name = "";
        bool joined = false;
        if (streq(a, "-l") && k + 1 < o.forwarded.len)
            name = o.forwarded.data[k + 1];
        else if (starts_with(a, "-l") && strlen(a) > 2)
        {
            name = a + 2;
            joined = true;
        }
        if (*name)
        {
            if (in_target_inventory("c++ stdc++ c++abi supc++", ci ? str_lower(name) : name))
            {
                o.explicit_libcxx = true;
                if (!joined)
                    k++;
                continue;
            }
            if (!joined)
            {
                sv_push(&kept, a);
                sv_push(&kept, name);
                k++;
                continue;
            }
        }
        value = takes_value(a);
        sv_push(&kept, a);
    }
    o.forwarded = kept;
    o.header_emit = file_inputs > 0 && file_inputs == header_inputs;
    if (*trailing_language && !streq(trailing_language, "none") && o.has_input && needs_link(o) &&
        !o.no_stdlib && !o.relocatable)
        sv_extend(&o.forwarded, SV("-x", "none"));
    if (*nz(o.target_name) || want_m32 || want_m64)
    {
        Target t = resolve_target(o.target_name, error);
        if (*error)
            return o;
        if (want_m32 || want_m64)
        {
            char *arch = t.arch;
            if (want_m32 && streq(t.arch, "x86_64"))
                arch = "x86";
            else if (want_m64 && streq(t.arch, "x86"))
                arch = "x86_64";
            else if ((want_m32 && streq(t.arch, "x86")) || (want_m64 && streq(t.arch, "x86_64")) ||
                     (want_m64 && streq(t.arch, "aarch64")) || (want_m32 && streq(t.arch, "arm")))
            {
            }
            else if (want_m32 && streq(t.arch, "aarch64"))
                arch = "arm";
            else if (want_m64 && streq(t.arch, "arm"))
                arch = "aarch64";
            else
            {
                *error = str_fmt("%s is not supported for target %s", want_m32 ? "-m32" : "-m64",
                                 t.name);
                return o;
            }
            if (!streq(arch, t.arch))
            {
                char *name = str_fmt("%s%s", arch, t.name + strlen(t.arch));
                if (streq(arch, "arm") != streq(t.arch, "arm") && streq(t.os, "linux"))
                {
                    name = str_fmt("%s-linux", arch);
                    if (*nz(t.glibc_version))
                        name = str_fmt("%s-%s.%s", name, streq(arch, "arm") ? "gnueabihf" : "gnu",
                                       t.glibc_version);
                }
                char *err = NULL;
                resolve_target(name, &err);
                if (err)
                {
                    *error = str_fmt("%s: %s", want_m32 ? "-m32" : "-m64", err);
                    return o;
                }
                o.target_name = name;
            }
            else if (!*nz(o.target_name) && !t.native)
                o.target_name = t.name;
        }
    }
    if (!cli_msvc)
    {
        if (!*nz(msvc_env) || streq(msvc_env, "managed"))
            o.msvc_system = false;
        else if (streq(msvc_env, "system"))
            o.msvc_system = true;
        else
        {
            *error = "CROSS_CC_MSVC expects 'managed' or 'system'";
            return o;
        }
    }
    for (size_t i = 0; i < o.forwarded.len; i++)
    {
        char *a = o.forwarded.data[i];
        if (streq(a, "--"))
            break;
        if (streq(a, "-s"))
            o.strip = true;
        else if (debug_info_flag(a))
            o.strip = false;
        if (takes_value(a))
            i++;
    }
    o.forwarded = stage_arguments(o);
    return o;
}
