#include "cross.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

char *embedded_tool_dir(Toolchain tc)
{
    char *exe = executable_path();
    unsigned long long size, seconds, nanoseconds = 0;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA st;
    if (!GetFileAttributesExW(windows_wide(exe), GetFileExInfoStandard, &st))
        fatal("cannot inspect embedded executable");
    size = ((unsigned long long)st.nFileSizeHigh << 32) | st.nFileSizeLow;
    seconds = ((unsigned long long)st.ftLastWriteTime.dwHighDateTime << 32) |
              st.ftLastWriteTime.dwLowDateTime;
#else
    struct stat st;
    if (stat(exe, &st))
        fatal("cannot inspect embedded executable");
    size = (unsigned long long)st.st_size;
    seconds = (unsigned long long)st.st_mtime;
#ifdef __APPLE__
    nanoseconds = (unsigned long long)st.st_mtimespec.tv_nsec;
#else
    nanoseconds = (unsigned long long)st.st_mtim.tv_nsec;
#endif
#endif
    char *identity = str_fmt("%s|%llu|%llu|%llu|dsymutil-v1", exe, size, seconds, nanoseconds);
    char *dest = path_join(tc.cache_dir,
                           str_fmt("tools-%016llx", (unsigned long long)runtime_hash(identity)));
    if (file_exists(path_join(dest, "complete")))
        return dest;
#ifndef _WIN32
    /* Only cold provisioning is serialized. Keep this sibling inode permanently:
       unlinking it could let a new opener bypass waiters on the old lock. The
       kernel releases the lock on exit (including fatal errors or signals). */
    char *lock_path = str_fmt("%s.lock", dest);
    int lock = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0)
        fatal(str_fmt("cannot open embedded tools lock %s: %s", lock_path, strerror(errno)));
    while (flock(lock, LOCK_EX))
    {
        if (errno == EINTR)
            continue;
        int e = errno;
        close(lock);
        fatal(str_fmt("cannot acquire embedded tools lock %s: %s", lock_path, strerror(e)));
    }
    /* A preceding publisher may have completed while we were waiting. Do not
       even create a stage or hardlink the executable in that case. */
    if (file_exists(path_join(dest, "complete")))
    {
        close(lock);
        return dest;
    }
#endif
    char *error = NULL, *work = make_temp_dir(tc.cache_dir, "tools-stage-", &error);
    if (error)
        fatal(error);
    char *first = path_join(work, "cross_clang" NATIVE_EXE_SUFFIX);
    if (!link_file(exe, first) && !copy_file(first, exe))
        fatal("cannot provision embedded Clang");
    StrVec names = SV("cross_c++", "ld.lld", "ld64.lld", "lld-link", "wasm-ld", "llvm-ar",
                      "llvm-ranlib", EMBEDDED_DSYMUTIL_NAME);
    for (size_t i = 0; i < names.len; i++)
    {
        char *alias = path_join(work, str_fmt("%s%s", names.data[i], NATIVE_EXE_SUFFIX));
        if (!link_file(first, alias) && !copy_file(alias, first))
            fatal("cannot provision embedded tool alias");
    }
    error = runtime_publish(work, dest);
    remove_tree(work);
#ifndef _WIN32
    close(lock);
#endif
    if (error)
        fatal(error);
    return dest;
}
char *project_root(void)
{
    char *p = env("CROSS_CC_ROOT");
    if (*p)
        return absolute_directory_path(p);
    char *dir = path_dir(executable_path());
    StrVec candidates = SV(dir, path_join(dir, ".."), path_join(dir, "..", "share", "cross_cc"));
    for (size_t i = 0; i < candidates.len; i++)
        if (file_exists(path_join(candidates.data[i], "upstream.lock.json")))
            return absolute_directory_path(candidates.data[i]);
    return absolute_directory_path(path_join(dir, ".."));
}
char *default_cache_dir(const char *root)
{
    if (is_dir(path_join(root, ".cache", "native")))
        return path_join(root, ".cache", "cc");
#ifdef _WIN32
    if (*env("LOCALAPPDATA"))
        return path_join(env("LOCALAPPDATA"), "cross_cc");
    if (*env("USERPROFILE"))
        return path_join(env("USERPROFILE"), "AppData", "Local", "cross_cc");
#elif defined(__APPLE__)
    if (*env("HOME"))
        return path_join(env("HOME"), "Library", "Caches", "cross_cc");
#else
    if (*env("XDG_CACHE_HOME"))
        return path_join(env("XDG_CACHE_HOME"), "cross_cc");
    if (*env("HOME"))
        return path_join(env("HOME"), ".cache", "cross_cc");
#endif
    fatal("cannot locate user cache directory; set CROSS_CC_CACHE_DIR");
}
Toolchain discover_toolchain(Options o, Target t)
{
    Toolchain tc = {0};
    tc.builtins = "";
    tc.root = project_root();
    tc.lib_dir = o.lib_dir;
    if (!*nz(tc.lib_dir))
        tc.lib_dir = env("CROSS_CC_LIB_DIR");
    if (!*nz(tc.lib_dir))
        tc.lib_dir = env("ZIG_LIB_DIR");
    if (!*nz(tc.lib_dir)) {
        tc.lib_dir = first_existing(path_join(tc.root, "lib"));
        const char *dirs[] = {"include", "libc", "libcxx", "libcxxabi", "libunwind", "libtsan"};
        for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
            if (!is_dir(path_join(tc.lib_dir, dirs[i])))
                fatal("pinned lib/ resources missing; run `make resources` in the source checkout "
                      "or restore the complete release bundle (or set CROSS_CC_LIB_DIR)");
    }
    if (!*tc.lib_dir || !is_dir(tc.lib_dir))
        fatal("pinned headers and runtime sources not found; run the setup tool or set "
              "CROSS_CC_LIB_DIR");
    tc.lib_dir = absolute_directory_path(tc.lib_dir);
    set_env("CROSS_CC_LIB_DIR", tc.lib_dir);
    tc.cache_dir = o.cache_dir;
    if (!*nz(tc.cache_dir))
        tc.cache_dir = env("CROSS_CC_CACHE_DIR");
    if (!*nz(tc.cache_dir))
        tc.cache_dir = env("ZIG_GLOBAL_CACHE_DIR");
    if (!*nz(tc.cache_dir))
        tc.cache_dir = default_cache_dir(tc.root);
    ensure_dir(tc.cache_dir);
    tc.cache_dir = absolute_directory_path(tc.cache_dir);
    char *tmp = path_join(tc.cache_dir, "tmp");
    ensure_dir(tmp);
    if (!set_env("TMPDIR", tmp))
        fatal("cannot configure disk-backed temporary directory");
#ifdef _WIN32
    set_env("TMP", tmp);
    set_env("TEMP", tmp);
#endif
    char *tools = embedded_tool_dir(tc);
    tc.clang = path_join(tools, "cross_clang" NATIVE_EXE_SUFFIX);
    tc.lld = path_join(tools, "ld.lld" NATIVE_EXE_SUFFIX);
    if (streq(t.os, "windows"))
        tc.lld = path_join(tools, "lld-link" NATIVE_EXE_SUFFIX);
    if (streq(t.os, "macos"))
        tc.lld = path_join(tools, "ld64.lld" NATIVE_EXE_SUFFIX);
    if (starts_with(t.arch, "wasm"))
        tc.lld = path_join(tools, "wasm-ld" NATIVE_EXE_SUFFIX);
    tc.ar = path_join(tools, "llvm-ar" NATIVE_EXE_SUFFIX);
    tc.clang_version = "clang version 22.1.8";
    return tc;
}
static bool is_compiler_query(StrVec args, const char *query)
{
    for (size_t i = 0; i < args.len; i++)
        if (streq(args.data[i], query))
            return true;
    return false;
}
static int query_dumpmachine(Options o, Target t, Toolchain tc)
{
    const char *name = nz(o.target_name);
    if (streq(name, "native"))
    {
        puts("native");
        return 0;
    }
    if (!*name)
    {
        if (streq(t.os, "linux"))
            printf("%s-unknown-linux-%s\n", t.arch, arm_arch(t.arch) ? "musleabihf" : "musl");
        else if (streq(t.os, "macos"))
            printf("%s-unknown-darwin24.6.0\n", t.arch);
        else if (streq(t.os, "windows"))
            printf("%s-unknown-windows-gnu\n", t.arch);
        else
        {
            int code;
            char *out = capture_command(SV(tc.clang, "--no-default-config", "-dumpmachine"), &code);
            puts(code ? t.clang : out);
        }
        return 0;
    }
    return run_command(
        SV(tc.clang, "--no-default-config", str_fmt("--target=%s", name), "-dumpmachine"), false);
}
bool handle_query(Options o, Target t, Toolchain tc, int *code)
{
    if (is_compiler_query(o.forwarded, "--cross-cc-version"))
    {
        printf("cross_cc (compatibility baseline %s, %s)\n", UPSTREAM_VERSION, UPSTREAM_COMMIT);
        *code = 0;
        return true;
    }
    if (is_compiler_query(o.forwarded, "-dumpmachine"))
    {
        *code = query_dumpmachine(o, t, tc);
        return true;
    }
    if (is_compiler_query(o.forwarded, "-print-resource-dir"))
    {
        puts(tc.lib_dir);
        *code = 0;
        return true;
    }
    return false;
}
char *compiler_optimization(const char *arg, bool *known)
{
    *known = true;
    if (streq(arg, "-O0") || streq(arg, "-Og"))
        return "-O0";
    if (in_target_inventory("-O1 -O2 -O3 -O4 -Ofast", arg))
        return "-O2";
    if (streq(arg, "-Os") || streq(arg, "-Oz"))
        return "-Os";
    *known = false;
    return (char *)arg;
}
bool emits_debug_info(Options o, bool include_runtime)
{
    char *optimization = "-O0";
    bool requested = false, level_set = false, level_on = false;
    for (size_t i = 0; i < o.forwarded.len; i++)
    {
        char *a = o.forwarded.data[i];
        if (streq(a, "--"))
            break;
        bool known;
        char *flag = compiler_optimization(a, &known);
        if (known)
            optimization = flag;
        else
        {
            if (debug_info_flag(a))
                requested = true;
            if (streq(a, "-g0") || streq(a, "-ggdb0"))
            {
                level_set = true;
                level_on = false;
            }
            else if (in_target_inventory(
                         "-g1 -g2 -g3 -ggdb -ggdb1 -ggdb2 -ggdb3 -gline-tables-only "
                         "-gline-directives-only -gdwarf -gdwarf-2 -gdwarf-3 -gdwarf-4 -gdwarf-5 "
                         "-gdwarf32 -gdwarf64 -gfull -gused -glldb -gsce -gdbx",
                         a))
            {
                level_set = true;
                level_on = true;
            }
        }
        if (takes_value(a))
            i++;
    }
    return !o.strip && (!streq(optimization, "-Os") || requested) &&
           (include_runtime || !level_set || level_on);
}
bool optimized_build(Options o)
{
    char *optimization = "-O0";
    for (size_t i = 0; i < o.forwarded.len; i++)
    {
        char *a = o.forwarded.data[i];
        if (streq(a, "--"))
            break;
        bool known;
        char *flag = compiler_optimization(a, &known);
        if (known)
            optimization = flag;
        if (takes_value(a))
            i++;
    }
    return !streq(optimization, "-O0");
}
bool compiler_sanitizer_contains(const char *arg, const char *prefix, const char *name)
{
    if (!starts_with(arg, prefix))
        return false;
    StrVec parts = str_split(arg + strlen(prefix), ",");
    for (size_t i = 0; i < parts.len; i++)
        if (streq(parts.data[i], name))
            return true;
    return false;
}
bool compiler_plain_assembly(StrVec args)
{
    char *language = "";
    bool found = false, operands = false;
    for (size_t i = 0; i < args.len; i++)
    {
        char *a = args.data[i];
        if (!operands)
        {
            if (streq(a, "--"))
            {
                operands = true;
                continue;
            }
            if (streq(a, "-x") || streq(a, "--language"))
            {
                if (i + 1 < args.len)
                    language = args.data[++i];
                continue;
            }
            if (starts_with(a, "--language="))
            {
                language = a + 11;
                continue;
            }
            if (starts_with(a, "-x") && strlen(a) > 2)
            {
                language = a + 2;
                continue;
            }
            if (takes_value(a))
            {
                i++;
                continue;
            }
            if (strlen(a) > 1 && a[0] == '-')
                continue;
        }
        if (streq(language, "assembler") ||
            ((!*language || streq(language, "none")) && ends_with(a, ".s")))
            found = true;
        else
            return false;
    }
    return found;
}
StrVec compiler_command(Options o, Target t, Toolchain tc, Runtime rt, bool resource_dependencies,
                        char **error)
{
    *error = NULL;
    bool c_defaults = !compiler_plain_assembly(o.forwarded);
    bool canonical_resources = resource_vfs_eligible(o, tc, resource_dependencies);
    StrVec cmd = SV(tc.clang, "--no-default-config", str_fmt("--target=%s", t.clang));
    StrVec cpu = cpu_flags(t, o.cpu, o.forwarded, error);
    if (*error)
        return (StrVec){0};
    sv_extend(&cmd, cpu);
    if (c_defaults)
    {
        if (streq(t.arch, "x86") || streq(t.arch, "x86_64"))
            sv_push(&cmd, "-mtune=generic");
        sv_extend(&cmd, SV("-fno-spell-checking", "-fno-omit-frame-pointer", "-ferror-limit=19"));
    }
    bool protect_output = false, skip = false;
    for (size_t i = 0; i < o.forwarded.len; i++)
    {
        char *a = o.forwarded.data[i];
        if (skip)
        {
            skip = false;
            continue;
        }
        if (streq(a, "--"))
            break;
        if (streq(a, "-E") || streq(a, "-S"))
            protect_output = true;
        else if (streq(a, "-c") || streq(a, "-r"))
            protect_output = false;
        skip = takes_value(a);
    }
    if (o.dependency_only)
        protect_output = false;
    char *optimization = "-O0";
    bool debug_requested = false;
    int stack_protector = -1, unwind_tables = -1;
    Ubsan_Mode sanitize_mode = compiler_ubsan_mode(o);
    bool sanitize_undefined = sanitize_mode != UBSAN_OFF;
    bool sanitize_thread = tsan_enabled(o),
         fuzz = compiler_sanitizer_enabled(o, SV("fuzzer", "fuzzer-no-link"));
    bool explicit_nostdinc = false, value = false, operands = false, literal_output = false;
    StrVec forwarded = {0};
    for (size_t i = 0; i < o.forwarded.len; i++)
    {
        char *a = o.forwarded.data[i];
        if (value || operands)
        {
            char *arg = a;
            if (literal_output && starts_with(arg, "@"))
                arg = str_fmt("./%s", arg);
            sv_push(&forwarded, arg);
            value = false;
            literal_output = false;
            continue;
        }
        if (streq(a, "--"))
            operands = true;
        value = takes_value(a);
        if (streq(a, "-nostdinc"))
            explicit_nostdinc = true;
        if (streq(t.os, "linux") && streq(a, "-nolibc"))
            continue;
        if (streq(t.os, "linux") && (o.no_libc || o.no_stdlib) && needs_link(o) &&
            (streq(a, "-pie") || streq(a, "-no-pie")))
        {
            sv_push(&forwarded, streq(a, "-pie") ? "-Wl,-pie" : "-Wl,--no-pie");
            continue;
        }
        if (streq(t.os, "openbsd") && streq(a, "-no-pie"))
            continue;
        if (streq(t.os, "linux") && starts_with(t.arch, "sparc") && streq(a, "-no-pie"))
        {
            if (needs_link(o) && !o.shared)
                sv_push(&forwarded, "-Wl,--no-pie");
            continue;
        }
        literal_output = protect_output && streq(a, "-o");
        if (!c_defaults &&
            (streq(a, "-fomit-frame-pointer") || streq(a, "-fno-omit-frame-pointer")))
            continue;
        bool known;
        char *flag = compiler_optimization(a, &known);
        if (protect_output && starts_with(a, "-o@"))
            flag = str_fmt("-o./%s", a + 2);
        if (known)
        {
            optimization = flag;
            continue;
        }
        if (in_target_inventory("-fstack-protector -fstack-protector-strong -fstack-protector-all",
                                a))
        {
            if (stack_protector < 0)
                stack_protector = 1;
            continue;
        }
        if (streq(a, "-fno-stack-protector"))
        {
            stack_protector = 0;
            continue;
        }
        if (streq(a, "-funwind-tables"))
        {
            if (unwind_tables < 1)
                unwind_tables = 1;
            continue;
        }
        if (streq(a, "-fno-unwind-tables"))
        {
            unwind_tables = 0;
            continue;
        }
        if (streq(a, "-fasynchronous-unwind-tables"))
        {
            unwind_tables = 2;
            continue;
        }
        if (streq(a, "-fno-asynchronous-unwind-tables"))
        {
            if (unwind_tables != 0)
                unwind_tables = 1;
            continue;
        }
        if (debug_info_flag(a))
            debug_requested = true;
        if (compiler_sanitizer_consumed(a))
            flag = "";
        if (*flag)
            sv_push(&forwarded, flag);
    }
    if (unwind_tables < 0 && streq(t.arch, "s390x") && streq(t.os, "linux"))
        unwind_tables = 2;
    if (c_defaults)
    {
        if (unwind_tables == 0)
            sv_extend(&cmd, SV("-fno-unwind-tables", "-fno-asynchronous-unwind-tables"));
        else if (unwind_tables == 1)
            sv_extend(&cmd, SV("-fno-asynchronous-unwind-tables", "-funwind-tables"));
        else if (unwind_tables == 2)
            sv_push(&cmd, "-fasynchronous-unwind-tables");
        bool safe_mode =
            streq(optimization, "-O0") || (streq(optimization, "-O2") && sanitize_undefined);
        if (stack_protector == 1 ||
            (stack_protector < 0 && safe_mode && !streq(t.os, "freestanding")))
            sv_extend(&cmd, SV("-fstack-protector-strong", "--param", "ssp-buffer-size=4"));
        else
            sv_push(&cmd, "-fno-stack-protector");
    }
    if (!o.strip && (!streq(optimization, "-Os") || debug_requested))
    {
        if (!streq(t.os, "windows") || emits_debug_info(o, false))
        {
            sv_push(&cmd,
                    streq(t.os, "windows") || streq(t.os, "uefi") ? "-gcodeview" : "-gdwarf-4");
            if (streq(t.os, "windows"))
                sv_push(&cmd, "-gno-codeview-command-line");
        }
    }
    if (streq(t.os, "windows") && streq(t.abi, "msvc"))
        sv_push(&cmd, "-mno-incremental-linker-compatible");
    if (c_defaults)
    {
        if (!streq(optimization, "-O0"))
        {
            sv_push(&cmd, "-Werror=date-time");
            sv_push(&cmd, streq(optimization, "-O2") && sanitize_undefined ? "-D_FORTIFY_SOURCE=2"
                                                                           : "-DNDEBUG");
        }
        if (sanitize_undefined)
            sv_push(&cmd, "-fsanitize=undefined");
        if (sanitize_thread)
            sv_push(&cmd, "-fsanitize=thread");
        if (fuzz)
            sv_push(&cmd, "-fsanitize=fuzzer-no-link");
        if (sanitize_mode == UBSAN_TRAP)
            sv_push(&cmd, "-fsanitize-trap=undefined");
        if (ubsan_full_enabled(o))
        {
            sv_push(&cmd, "-fno-sanitize-link-runtime");
            if (streq(t.os, "windows"))
                sv_push(&cmd, "-fno-rtlib-defaultlib");
        }
        if (needs_link(o) && !ubsan_full_supported(t))
            sv_push(&cmd, "-fsanitize-trap=undefined");
        sv_extend(&cmd, SV("-fno-sanitize=function", "-fno-sanitize=vptr", optimization));
    }
    if (streq(t.os, "linux") && starts_with(t.abi, "musl") && o.pie == 0)
    {
        if (o.shared)
            sv_push(&cmd, "-fPIC");
        else
            sv_extend(&cmd, SV("-fno-pic", "-fno-pie"));
    }
    bool bsd = in_target_inventory("freebsd netbsd openbsd", t.os);
    if (bsd)
        sv_push(&cmd, bsd_pic_enabled(o, t) ? "-fPIC" : "-fno-pic");
    if (streq(t.os, "linux") && starts_with(t.abi, "gnu") && o.pie == 0)
    {
        sv_push(&cmd, "-fPIC");
        if (needs_link(o) && !o.shared)
            sv_push(&cmd, "-Wl,--no-pie");
    }
    if (c_defaults)
    {
        if (*nz(tc.lib_dir))
            sv_push(&cmd, str_fmt("-ffile-prefix-map=%s=/cross_cc/lib",
                                  builtins_driver_path(tc.lib_dir)));
        if (canonical_resources)
        {
            char *overlay = resource_vfs_overlay(tc, error);
            if (*error)
                return (StrVec){0};
            sv_extend(&cmd, SV("-ivfsoverlay", overlay, "-resource-dir", resource_vfs_root()));
        }
        for (size_t i = 0; i < rt.compile_flags.len; i++)
        {
            char *flag = rt.compile_flags.data[i];
            if (explicit_nostdinc && streq(flag, "-nostdlibinc"))
                continue;
            sv_push(&cmd, flag);
            if (takes_value(flag) && i + 1 < rt.compile_flags.len)
            {
                char *operand = rt.compile_flags.data[++i];
                if (canonical_resources &&
                    in_target_inventory("-isystem -I -idirafter -iquote", flag))
                {
                    bool mapped = false;
                    operand = resource_vfs_path(operand, tc.lib_dir, &mapped);
                }
                sv_push(&cmd, operand);
            }
        }
    }
    if (needs_link(o))
    {
        if (streq(t.os, "linux") && !o.relocatable)
            sv_push(&cmd, "-Wl,-z,relro,-z,now");
        else if (bsd && !o.relocatable)
            sv_push(&cmd, "-Wl,-z,now");
        if (streq(t.os, "linux") && !o.relocatable && !o.shared)
            sv_push(&cmd, "-Wl,-z,stack-size=16777216");
        if (!*nz(tc.lld))
        {
            *error = "LLD 22 is required for linking; run setup or set CROSS_CC_LLD";
            return (StrVec){0};
        }
        if (streq(t.os, "windows") && streq(t.abi, "msvc"))
        {
            sv_push(&cmd, "-fuse-ld=lld");
            if (!o.shared && !o.relocatable)
                sv_push(&cmd, "-Wl,/stack:16777216");
            if (!o.relocatable)
                sv_extend(&cmd, SV("-Wl,/pdbaltpath:%_PDB%", "-Wl,/Brepro",
                                   emits_debug_info(o, true) ? "-Wl,/debug" : "-Wl,/debug:none"));
            if (!o.relocatable && *nz(o.output))
            {
                char *first_source = "", *first_file = "";
                bool user_implib = false;
                for (size_t i = 0; i < o.forwarded.len; i++)
                {
                    char *a = o.forwarded.data[i];
                    if (streq(a, "--"))
                        break;
                    if (starts_with(a, "-") && contains(str_lower(a), "implib"))
                        user_implib = true;
                    if (takes_value(a))
                    {
                        i++;
                        continue;
                    }
                    if (starts_with(a, "-"))
                        continue;
                    if (in_target_inventory(".o .obj .a .lib .dll .def .res",
                                            str_lower(path_ext(a))))
                    {
                        if (!*first_file)
                            first_file = a;
                    }
                    else if (!*first_source)
                        first_source = a;
                }
                if (!user_implib)
                {
                    char *first = *first_source ? first_source
                                  : *first_file ? first_file
                                                : o.output;
                    sv_push(&cmd,
                            str_fmt("-Wl,/implib:%s",
                                    path_join(path_dir(o.output),
                                              str_fmt("%s.lib", output_root_name(first, "")))));
                }
            }
            if (o.shared && !o.relocatable && !o.no_stdlib && !o.no_defaultlibs && !o.no_libc)
                sv_extend(&cmd, SV("-Wl,/nodefaultlib:libcmt", "-Wl,/defaultlib:msvcrt",
                                   "-Wl,/defaultlib:vcruntime", "-Wl,/defaultlib:ucrt"));
        }
        else
            sv_push(&cmd, str_fmt("-fuse-ld=%s", tc.lld));
    }
    if (needs_link(o) && o.static_link)
        sv_push(&cmd, "-static");
    bool msvc_link = streq(t.os, "windows") && streq(t.abi, "msvc") && needs_link(o);
    StrVec kept = {0};
    bool after_operands = false, is_value = false;
    for (size_t i = 0; i < forwarded.len; i++)
    {
        char *a = forwarded.data[i];
        if (!is_value && !after_operands)
        {
            if (streq(a, "--"))
                after_operands = true;
            else if (streq(a, "-g"))
                continue;
            else if (streq(a, "-g1"))
            {
                sv_push(&kept, "-gline-tables-only");
                continue;
            }
            else if (streq(a, "-s") && msvc_link)
                continue;
            else if (bsd && in_target_inventory("-fpic -fPIC -fno-pic -fno-PIC -pie -no-pie -nopie "
                                                "-fpie -fPIE -fno-pie -fno-PIE",
                                                a))
                continue;
        }
        char *virtual_map = "";
        if (canonical_resources && !is_value && !after_operands)
            virtual_map = resource_vfs_prefix_map(a, tc.lib_dir);
        is_value = !is_value && !after_operands && takes_value(a);
        sv_push(&kept, a);
        if (*nz(virtual_map))
            sv_push(&kept, virtual_map);
    }
    StrVec link_args = linux_link_arguments(o, t, kept, error);
    if (*error)
        return (StrVec){0};
    StrVec msvc_args = msvc_link_arguments(o, t, link_args, error);
    if (*error)
        return (StrVec){0};
    sv_extend(&cmd, msvc_args);
    if (needs_link(o))
        sv_extend(&cmd, rt.link_flags);
    return cmd;
}
