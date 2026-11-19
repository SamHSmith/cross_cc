#include "cross.h"
static const char *const libc_config_fields[] = {"include_dir",   "sys_include_dir",  "crt_dir",
                                                 "msvc_lib_dir",  "kernel32_lib_dir", "cc_dir",
                                                 "darwin_sdk_dir"};
Libc_Config libc_config_parse(const char *text, Target t, char **error)
{
    Libc_Config l = {0};
    bool found[7] = {0};
    char *values[7] = {0};
    *error = NULL;
    const char *start = text;
    for (const char *p = text;; p++)
    {
        if (*p && *p != '\r' && *p != '\n')
            continue;
        char *line = str_ncopy(start, (size_t)(p - start));
        start = p + 1;
        if (*line && *line != '#')
        {
            char *eq = strchr(line, '='), *value = "";
            if (eq)
            {
                *eq = 0;
                value = eq + 1;
            }
            char *name = streq(line, "gcc_dir") ? "cc_dir" : line;
            for (size_t i = 0; i < 7; i++)
                if (streq(name, libc_config_fields[i]))
                {
                    found[i] = true;
                    values[i] = value;
                    break;
                }
        }
        if (!*p)
            break;
    }
    for (size_t i = 0; i < 7; i++)
        if (!found[i])
        {
            *error = str_fmt("missing field: %s", libc_config_fields[i]);
            return l;
        }
    l = (Libc_Config){values[0], values[1], values[2], values[3], values[4], values[5], values[6]};
    for (size_t i = 0; i < 3; i++)
        if (!*values[i])
        {
            *error = str_fmt("%s may not be empty", libc_config_fields[i]);
            return l;
        }
    if (!*l.cc_dir && in_target_inventory("linux haiku serenity", t.os))
    {
        *error = str_fmt("cc_dir may not be empty for %s", t.os);
        return l;
    }
    if (streq(t.os, "windows") && (streq(t.abi, "msvc") || streq(t.abi, "itanium")))
    {
        if (!*l.msvc_lib_dir)
        {
            *error = str_fmt("msvc_lib_dir may not be empty for %s-%s", t.os, t.abi);
            return l;
        }
        if (!*l.kernel32_lib_dir)
        {
            *error = str_fmt("kernel32_lib_dir may not be empty for %s-%s", t.os, t.abi);
            return l;
        }
    }
    if (streq(t.os, "macos") && !*l.darwin_sdk_dir)
        *error = "darwin_sdk_dir may not be empty for macos";
    return l;
}
Libc_Config libc_config_from_file(const char *path, Target t, char **error)
{
    char *data = read_file(path, NULL);
    if (!data)
    {
        *error = str_fmt("unable to read libc paths file %s: %s", path, strerror(errno));
        return (Libc_Config){0};
    }
    Libc_Config l = libc_config_parse(data, t, error);
    if (*error)
        *error = str_fmt("libc paths file %s: %s", path, *error);
    return l;
}
StrVec libc_config_include_flags(Libc_Config l, Toolchain tc)
{
    StrVec flags =
        SV("-nostdlibinc", "-isystem", path_join(tc.lib_dir, "include"), "-isystem", l.include_dir);
    if (!streq(l.sys_include_dir, l.include_dir))
        sv_extend(&flags, SV("-isystem", l.sys_include_dir));
    return flags;
}
char *libc_config_cache_identity(Target t)
{
    if (!streq(t.os, "linux"))
        return "";
    char *path = env("ZIG_LIBC");
    if (!*path)
        return "";
    char *data = read_file(path, NULL);
    if (!data)
        fatal(str_fmt("unable to read libc paths file %s: %s", path, strerror(errno)));
    return str_fmt("|external-libc-v1|%s|%s", path, data);
}
Runtime prepare_linux_external_libc(Options options, Target t, Toolchain tools, Libc_Config l,
                                    char **error)
{
    Options o = linux_libc_runtime_options(options);
    Toolchain tc = tools;
    Runtime r = {0};
    *error = NULL;
    if (options.no_stdlib)
        tc.builtins = "";
    if (o.cxx && !o.no_stdinc && !o.no_stdincxx)
        sv_extend(&r.compile_flags, runtime_cxx_flags(t, tc.lib_dir));
    if (!o.no_stdinc)
    {
        if ((o.no_libc || o.no_stdlib) && !o.explicit_libc)
            sv_extend(&r.compile_flags,
                      SV("-nostdlibinc", "-isystem", path_join(tc.lib_dir, "include")));
        else
            sv_extend(&r.compile_flags, libc_config_include_flags(l, tc));
    }
    if (!needs_link(o) || o.no_stdlib || o.relocatable)
        return r;
    if (o.no_libc && !o.explicit_libc)
    {
        sv_push(&r.link_flags, "-nostdlib");
        return r;
    }
    sv_extend(&r.link_flags, SV("-nostdlib", "-Wl,-z,now", "-L", l.crt_dir, "-L", l.cc_dir));
    bool pie = o.pie == 1;
    if (!o.shared)
    {
        if (pie)
        {
            sv_push(&r.link_flags, "-Wl,-pie");
            if (o.static_link)
                sv_extend(&r.link_flags, SV("-Wl,--no-dynamic-linker", "-Wl,-z,text"));
        }
        else
            sv_push(&r.link_flags, "-Wl,-no-pie");
        if (!o.static_link && *nz(t.dynamic_linker))
            sv_push(&r.link_flags, str_fmt("-Wl,--dynamic-linker=%s", t.dynamic_linker));
    }
    const char *begin = pie || o.shared ? "crtbeginS.o" : "crtbegin.o",
               *end = pie || o.shared ? "crtendS.o" : "crtend.o";
    if (!o.no_startfiles)
    {
        if (!o.shared)
            sv_push(&r.link_flags,
                    path_join(l.crt_dir, pie ? (o.static_link ? "rcrt1.o" : "Scrt1.o") : "crt1.o"));
        sv_extend(&r.link_flags, SV(path_join(l.crt_dir, "crti.o"), path_join(l.cc_dir, begin)));
    }
    if (!o.no_defaultlibs)
    {
        sv_push(&r.link_flags, "-Wl,--push-state,--as-needed,--start-group");
        if (o.cxx || (o.static_link && starts_with(t.abi, "gnu")))
        {
            char *dir = runtime_build_cxx(o, t, tc, !o.cxx, error);
            if (*error)
                return r;
            if (o.cxx)
                sv_extend(&r.link_flags,
                          SV(path_join(dir, "libc++.a"), path_join(dir, "libc++abi.a")));
            sv_push(&r.link_flags, path_join(dir, "libunwind.a"));
        }
        sv_extend(&r.link_flags, SV("-lm", "-lpthread", "-lc", "-ldl", "-lrt", "-lutil"));
        if (*nz(tc.builtins))
            sv_push(&r.link_flags, tc.builtins);
        sv_push(&r.link_flags, "-Wl,--end-group,--pop-state");
    }
    if (!o.no_startfiles)
        sv_extend(&r.link_flags, SV(path_join(l.cc_dir, end), path_join(l.crt_dir, "crtn.o")));
    return r;
}
