#include "cross.h"

static Options prepare_resource_inputs(Options o, Target t, Toolchain tc, char **error)
{
    Options result = o;
    *error = NULL;
    if (!streq(t.os, "windows") || !needs_link(o) || o.dry_run)
        return result;
    size_t *resources = xmalloc((o.forwarded.len + 1) * sizeof(size_t)), count = 0;
    bool operands = false, skip = false;
    char *language = "";
    for (size_t i = 0; i < o.forwarded.len; i++)
    {
        char *arg = o.forwarded.data[i];
        if (skip)
        {
            skip = false;
            continue;
        }
        if (!operands)
        {
            if (streq(arg, "--"))
            {
                operands = true;
                continue;
            }
            if (streq(arg, "-x") || streq(arg, "--language"))
            {
                if (i + 1 < o.forwarded.len)
                    language = o.forwarded.data[i + 1];
                skip = true;
                continue;
            }
            if (starts_with(arg, "-x") && strlen(arg) > 2)
            {
                language = arg + 2;
                continue;
            }
            if (starts_with(arg, "--language="))
            {
                language = arg + 11;
                continue;
            }
            if (takes_value(arg))
            {
                skip = true;
                continue;
            }
            if (starts_with(arg, "-"))
                continue;
        }
        if ((!*language || streq(language, "none")) && streq(str_lower(path_ext(arg)), ".rc"))
            resources[count++] = i;
    }
    if (!count)
        return result;
    char *work = path_join(tc.cache_dir, "tmp", str_fmt("resources-%d", process_id()));
    ensure_dir(work);
    StrVec flags = {0};
    for (size_t i = 0; i < o.forwarded.len; i++)
    {
        char *arg = o.forwarded.data[i];
        if (streq(arg, "--"))
            break;
        if (in_target_inventory("-D -U -I -isystem -iquote -idirafter", arg))
        {
            if (i + 1 < o.forwarded.len)
            {
                char *option = streq(arg, "-D") ? "/d" : streq(arg, "-U") ? "/u" : "/i";
                sv_push(&flags, option);
                sv_push(&flags, o.forwarded.data[++i]);
            }
        }
        else if (strlen(arg) > 2 &&
                 (starts_with(arg, "-D") || starts_with(arg, "-U") || starts_with(arg, "-I")))
            sv_push(&flags, str_fmt("/%s", arg + 1));
        else if (takes_value(arg))
            i++;
    }
    result.forwarded = sv_copy(o.forwarded);
    for (size_t i = 0; i < count; i++)
    {
        char *output = path_join(work, str_fmt("input-%zu.res", i));
        StrVec args = sv_copy(flags);
        sv_extend(&args, SV("/fo", output, "--", o.forwarded.data[resources[i]]));
        if (run_rc(args))
        {
            *error = str_fmt("resource compilation failed: %s", o.forwarded.data[resources[i]]);
            return result;
        }
        result.forwarded.data[resources[i]] = output;
    }
    return result;
}
int cross_main(int argc, char **argv)
{
    dispatch_backend(argc, argv);
    StrVec args = {0};
    for (int i = 1; i < argc; i++)
        sv_push(&args, argv[i]);
    bool cxx = contains(path_base(argv[0]), "++");
    if (args.len)
    {
        char *cmd = args.data[0];
        if (streq(cmd, "cc"))
            args = sv_slice(args, 1, args.len);
        else if (streq(cmd, "c++"))
        {
            cxx = true;
            args = sv_slice(args, 1, args.len);
        }
        else if (in_target_inventory("ar ranlib lib dlltool", cmd))
            return run_llvm_ar(args);
        else if (streq(cmd, "objcopy"))
            return run_objcopy(sv_slice(args, 1, args.len));
        else if (streq(cmd, "rc"))
            return run_rc(sv_slice(args, 1, args.len));
        else if (streq(cmd, "build"))
            fatal("the build subcommand is explicitly excluded; use Make, CMake, Meson or direct "
                  "compiler arguments");
    }
    char *error = NULL;
    Options o = parse_arguments(args, cxx, &error);
    if (error)
        fatal(error);
    if (*nz(o.msvc_libc_file) && !set_env("ZIG_LIBC", o.msvc_libc_file))
        fatal("cannot select libc configuration");
    Target target = resolve_target(o.target_name, &error);
    if (error)
        fatal(error);
    o = normalize_linker_flags(o, target, &error);
    if (error)
        fatal(error);
    if (streq(target.os, "linux") && starts_with(target.abi, "musl") && !o.dynamic_link &&
        !o.shared && ((!o.no_stdlib && !o.no_libc) || o.explicit_libc || o.explicit_libcxx))
        o.static_link = true;
    bool libunwind = false;
    o.forwarded = drop_bundled_runtime_libraries(o, target, &libunwind);
    Toolchain tc = discover_toolchain(o, target);
    int code = 0;
    if (handle_query(o, target, tc, &code))
        return code;
    o = prepare_resource_inputs(o, target, tc, &error);
    if (error)
        fatal(error);
    Options runtime_options = o;
    if (o.no_stdlibxx)
        runtime_options.cxx = false;
    if (o.explicit_libcxx)
    {
        runtime_options.cxx = true;
        runtime_options.no_stdincxx = false;
    }
    bool static_library = needs_link(o) && o.shared && o.static_link;
    if (static_library)
        runtime_options.compile_only = true;
    if (o.dry_run || !o.has_input)
        runtime_options.query = true;
    Runtime rt = {0};
    if (in_target_inventory("freebsd netbsd openbsd", target.os))
        rt = prepare_bsd(runtime_options, target, tc, &error);
    else if (streq(target.os, "wasi"))
        rt = prepare_wasi(runtime_options, target, tc, &error);
    else if (is_apple_os(target.os))
        rt = prepare_macos(runtime_options, target, tc, &error);
    else if (streq(target.os, "windows") && streq(target.abi, "gnu"))
        rt = prepare_mingw(runtime_options, target, tc, &error);
    else if (streq(target.os, "windows") && streq(target.abi, "msvc"))
        rt = prepare_msvc(runtime_options, target, tc, &error);
    else
        rt = prepare_runtime(runtime_options, target, tc, &error);
    if (error)
        fatal(error);
    if (static_library && o.has_input)
        return run_static_library(o, target, tc, rt);
    if (needs_link(o) && o.has_input && !o.dry_run && !o.no_stdlib && !o.no_libc &&
        ubsan_full_supported(target) && ubsan_full_enabled(o))
    {
        char *ubsan = prepare_ubsan(runtime_options, target, tc, &error);
        if (error)
            fatal(error);
        sv_push(&rt.link_flags, ubsan);
        if (streq(target.os, "windows"))
        {
            if (streq(target.abi, "msvc"))
                sv_push(&rt.link_flags, str_fmt("-Wl,-libpath:%s", path_dir(ubsan)));
            else
                sv_extend(&rt.link_flags, SV("-L", path_dir(ubsan)));
        }
    }
    if (libunwind && needs_link(o) && o.has_input && !o.dry_run &&
        !(runtime_options.cxx && !o.no_defaultlibs))
    {
        char *dir = runtime_build_cxx(runtime_options, target, tc, true, &error);
        if (error)
            fatal(error);
        sv_push(&rt.link_flags, path_join(dir, "libunwind.a"));
    }
    Runtime tsan = prepare_tsan(runtime_options, target, tc, &error);
    if (error)
        fatal(error);
    sv_extend(&rt.compile_flags, tsan.compile_flags);
    sv_extend(&rt.link_flags, tsan.link_flags);
    if ((in_target_inventory("linux freebsd netbsd openbsd", target.os) ||
         builtins_freestanding_supported(target) || builtins_msvc_supported(target) ||
         (streq(target.os, "windows") && streq(target.abi, "gnu") &&
          in_target_inventory("x86 x86_64 aarch64 thumb", target.arch))) &&
        !o.relocatable)
    {
        char *builtins = prepare_builtins(runtime_options, target, tc, &error);
        if (error)
            fatal(error);
        if (*nz(builtins))
            sv_push(&rt.link_flags, builtins);
    }
    if (streq(target.os, "windows") && streq(target.abi, "gnu") && needs_link(o) && o.has_input)
        return run_mingw_link(o, target, tc, rt);
    if (is_apple_os(target.os) && needs_link(o) && o.has_input)
        return run_macos_link(o, target, tc, rt);
    if (streq(target.os, "freestanding") && needs_link(o) && o.has_input)
        return run_freestanding_link(o, target, tc, rt);
    if (wants_sparc_static_link(o, target))
        return run_sparc_static_link(o, target, tc, rt);
    if (streq(target.os, "windows") && streq(target.abi, "msvc") && needs_link(o) && o.has_input &&
        !o.dry_run)
        return run_profile_link(o, target, tc, rt);
    if (wants_profile_split(o))
        return run_profile_link(o, target, tc, rt);
    return run_resource_compiler(o, target, tc, rt);
}
