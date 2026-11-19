#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#endif
#include "windows_runtime.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/types.h>
#endif

const char *msvc_arch_dir(const char *arch) {
    arch = nz(arch);
    if (streq(arch, "x86_64")) return "x64";
    if (streq(arch, "x86")) return "x86";
    if (streq(arch, "aarch64")) return "arm64";
    if (streq(arch, "arm") || streq(arch, "thumb")) return "arm";
    return arch;
}

/* Parses the libc file format (key=value lines), as the ZIG_LIBC variable names it. */
Msvc_Libc msvc_libc_from_file(const char *path, char **error) {
    Msvc_Libc l = {0};
    Target t = {0};
    t.os = "windows";
    t.abi = "msvc";
    char *err = NULL;
    Libc_Config config = libc_config_from_file(path, t, &err);
    if (err) { *error = err; return l; }
    l.include_dir = config.include_dir;
    l.sys_include_dir = config.sys_include_dir;
    l.crt_dir = config.crt_dir;
    l.msvc_lib_dir = config.msvc_lib_dir;
    l.kernel32_lib_dir = config.kernel32_lib_dir;
    *error = NULL;
    return l;
}

/* Match the original signed component parser, whose success flag was ignored:
   prefixes and underscores are accepted, and malformed/overflowing tails leave
   the numeric prefix parsed so far rather than resetting the component to 0. */
static int64_t msvc_component(const char *s, size_t n) {
    size_t i = 0;
    bool neg = false;
    if (n > 1 && (s[0] == '-' || s[0] == '+')) { neg = s[0] == '-'; i++; }
    uint64_t base = 10;
    if (n - i > 2 && s[i] == '0') {
        switch (s[i + 1]) {
        case 'b': base = 2; break;
        case 'o': base = 8; break;
        case 'd': base = 10; break;
        case 'z': base = 12; break;
        case 'x': base = 16; break;
        default: break;
        }
        if (strchr("bodzx", s[i + 1])) i += 2;
    }
    uint64_t limit = (uint64_t)INT64_MAX + (neg ? 1 : 0), v = 0;
    for (; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '_') continue;
        uint64_t digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
        else break;
        if (digit >= base || v > (limit - digit) / base) break;
        v = v * base + digit;
    }
    return neg && v ? -(int64_t)(v - 1) - 1 : (int64_t)v;
}

bool msvc_version_less(const char *a, const char *b) {
    a = nz(a);
    b = nz(b);
    bool ea = false, eb = false;
    while (!ea || !eb) {
        long long x = 0, y = 0;
        if (!ea) {
            const char *dot = strchr(a, '.');
            size_t n = dot ? (size_t)(dot - a) : strlen(a);
            x = msvc_component(a, n);
            if (dot) a = dot + 1; else ea = true;
        }
        if (!eb) {
            const char *dot = strchr(b, '.');
            size_t n = dot ? (size_t)(dot - b) : strlen(b);
            y = msvc_component(b, n);
            if (dot) b = dot + 1; else eb = true;
        }
        if (x != y) return x < y;
    }
    return false;
}

static void msvc_entries_push(Msvc_Dir_Entries *out, const char *name, int type) {
    if (out->len == out->cap) {
        out->cap = out->cap ? out->cap * 2 : 16;
        out->data = xrealloc(out->data, out->cap * sizeof(Msvc_Dir_Entry));
    }
    out->data[out->len].name = str_copy(name);
    out->data[out->len].type = type;
    out->len++;
}

#ifdef _WIN32
static char *msvc_wide_to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return str_copy("");
    char *s = xmalloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static wchar_t *msvc_utf8_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = xmalloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
#endif

bool msvc_read_dir(const char *dir, Msvc_Dir_Entries *out) {
    out->data = NULL;
    out->len = out->cap = 0;
#ifdef _WIN32
    char *pattern = str_fmt("%s\\*", dir);
    wchar_t *wide = msvc_utf8_to_wide(pattern);
    if (!wide) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wide, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        int type = 0;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            (fd.dwReserved0 == 0xa000000c || fd.dwReserved0 == 0xa0000003)) type = 2;
        else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) type = 1;
        msvc_entries_push(out, msvc_wide_to_utf8(fd.cFileName), type);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
#else
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (streq(e->d_name, ".") || streq(e->d_name, "..")) continue;
        struct stat st;
        char *full = path_join(dir, e->d_name);
        int type = 3;
        if (lstat(full, &st) == 0) {
            if (S_ISLNK(st.st_mode)) type = 2;
            else if (S_ISDIR(st.st_mode)) type = 1;
            else if (S_ISREG(st.st_mode)) type = 0;
        }
        msvc_entries_push(out, e->d_name, type);
    }
    closedir(d);
    return true;
#endif
}

/* Returns the highest-versioned entry of dir for which ok holds ("" if none). */
char *msvc_newest_subdir(const char *dir, bool (*ok)(const char *path)) {
    Msvc_Dir_Entries entries;
    if (!msvc_read_dir(dir, &entries)) return str_copy("");
    char *best = str_copy("");
    for (size_t i = 0; i < entries.len; i++) {
        const char *name = entries.data[i].name;
        if (entries.data[i].type != 1 || name[0] < '0' || name[0] > '9') continue;
        if (!ok(path_join(dir, name))) continue;
        if (best[0] == 0 || msvc_version_less(best, name)) best = str_copy(name);
    }
    return best;
}

static bool is_sep(char c) { return c == '/' || c == '\\'; }

/* The dirname rules for the paths a libc file holds, Windows-style. */
char *msvc_dirname(const char *path) {
    path = nz(path);
    size_t n = strlen(path);
    while (n > 0 && is_sep(path[n - 1])) n--;
    ptrdiff_t i = -1;
    for (size_t k = n; k > 0; k--) if (is_sep(path[k - 1])) { i = (ptrdiff_t)k - 1; break; }
    if (i < 0) return str_copy("");
    if (i == 0) return str_ncopy(path, 1);
    size_t m = (size_t)i;
    while (m > 0 && is_sep(path[m - 1])) m--;
    return str_ncopy(path, m);
}

/* Search order for a libc installation: ucrt, the MSVC toolset, its atlmfc,
   then the SDK's um and shared beside ucrt. */
StrVec msvc_include_dirs(Msvc_Libc l) {
    StrVec dirs = {0};
    sv_push(&dirs, nz(l.include_dir));
    if (!streq(l.sys_include_dir, l.include_dir)) sv_push(&dirs, nz(l.sys_include_dir));
    /* Joined as text: path_join would also resolve a libc file's "..". */
    char *p = msvc_dirname(l.sys_include_dir);
    if (p[0]) {
        const char *sep = contains(l.sys_include_dir, "\\") ? "\\" : "/";
        sv_push(&dirs, str_fmt("%s%satlmfc%sinclude", p, sep, sep));
    }
    p = msvc_dirname(l.include_dir);
    if (p[0]) {
        const char *sep = contains(l.include_dir, "\\") ? "\\" : "/";
        sv_push(&dirs, str_fmt("%s%sum", p, sep));
        sv_push(&dirs, str_fmt("%s%sshared", p, sep));
    }
    return dirs;
}

/* Clang keeps the first equal-prefix macro map, so ordering a default before
   user flags alone does not establish user precedence. Respect explicit raw
   prefix overlap without canonicalizing paths. */
bool msvc_user_maps_resources(StrVec args, const char *root) {
    static const char *const flags[] = {"-ffile-prefix-map=", "-fmacro-prefix-map=", "-fdebug-prefix-map=", "-fcoverage-prefix-map="};
    for (size_t i = 0; i < args.len; i++) {
        const char *a = args.data[i];
        if (streq(a, "--")) break;
        for (size_t f = 0; f < 4; f++) {
            if (!starts_with(a, flags[f])) continue;
            const char *value = a + strlen(flags[f]);
            const char *sep = strchr(value, '=');
            if (!sep) continue;
            char *old = str_ncopy(value, (size_t)(sep - value));
            if (starts_with(root, old) || starts_with(old, root)) return true;
        }
        if (takes_value(a)) i++;
    }
    return false;
}

Runtime prepare_msvc(Options o, Target t, Toolchain tc, char **error) {
    Runtime r = {0};
    char *err = NULL;
    *error = NULL;
    /* MSVC targets default to Visual C++ 2022 17.3 (19.33), not whatever toolset the host has. */
    sv_push(&r.compile_flags, "-fms-compatibility-version=19.33");
    /* The bundled libc++ and its consumers must agree on CRT import annotations. */
    if (o.cxx) sv_push(&r.compile_flags, o.shared ? "-fms-runtime-lib=dll" : "-fms-runtime-lib=static");
    if (nz(o.sysroot)[0]) return r;
    bool resource_link = needs_link(o) && !o.no_stdlib && !o.no_defaultlibs && !o.no_libc && !o.relocatable;
    /* -nostdinc disables header search, not the CRT libraries of a normal link. */
    if (o.no_stdinc && !resource_link) return r;
    /* Builtin headers and header-free objects do not require a Microsoft SDK.
       Keep libc++ wrappers before libc for include_next semantics. */
    if (!o.no_stdinc) {
        if (o.cxx && !o.no_stdincxx) {
            StrVec flags = msvc_libcxx_flags(t, tc, &err);
            if (err) { *error = err; return r; }
            sv_extend(&r.compile_flags, flags);
        }
        sv_push(&r.compile_flags, "-nostdlibinc");
        sv_push(&r.compile_flags, "-isystem");
        sv_push(&r.compile_flags, path_join(tc.lib_dir, "include"));
    }
    Msvc_Libc l = {0};
    const char *libc_file = nz(o.msvc_libc_file);
    if (!libc_file[0]) libc_file = nz(env("ZIG_LIBC"));
    if (libc_file[0]) {
        l = msvc_libc_from_file(libc_file, &err);
    } else if (o.msvc_system) {
        l = msvc_libc_detect(t, &err);
    } else {
        l = msvc_cached(tc, t, &err);
        if (err && !resource_link && msvc_without_sdk(o, t, tc, r)) return r;
        if (err) { err = NULL; l = managed_msvc(o, t, tc, &err); }
    }
    if (err) {
        /* Only explicit system mode preserves the legacy SDK-less behavior.
           Managed mode never silently discovers or falls back to host resources. */
        if (!libc_file[0] && o.msvc_system && (!needs_link(o) || o.no_stdlib || o.relocatable)) return r;
        *error = err;
        return r;
    }
    char *include_root = NULL;
    if (!o.msvc_system && !libc_file[0]) {
        char *root = msvc_resource_dir(tc, t);
        if (!msvc_user_maps_resources(o.forwarded, root)) {
            Managed_Msvc_Lock lock = msvc_lock(&err);
            if (err) { *error = err; return r; }
            char *virtual_root = str_fmt("/cross_cc/msvc/%s", lock.resource_version);
            /* Prefix maps alone do not cover UBSan's presumed header filenames.
               Give only managed SDK headers canonical file names, before
               metadata is printed. An overlapping user map keeps physical names
               and its original semantics, including macro-map precedence. */
            sv_push(&r.compile_flags, str_fmt("-ffile-prefix-map=%s=%s", root, virtual_root));
            /* Dependency consumers need physical paths. Do not opt into the
               bundled-lib depfile remapper: it intentionally owns no SDK tokens. */
            if (!o.no_stdinc && resource_vfs_eligible(o, tc, false)) {
                char *overlay = resource_vfs_overlay_root(tc, virtual_root, root, &err);
                if (err) { *error = err; return r; }
                sv_push(&r.compile_flags, "-ivfsoverlay");
                sv_push(&r.compile_flags, overlay);
                include_root = virtual_root;
            }
        }
    }
    if (!o.no_stdinc) {
        Msvc_Libc includes = l;
        if (include_root) {
            /* These are VFS names, not host paths. */
            includes.include_dir = str_fmt("%s/sdk/include/ucrt", include_root);
            includes.sys_include_dir = str_fmt("%s/crt/include", include_root);
        }
        StrVec dirs = msvc_include_dirs(includes);
        for (size_t i = 0; i < dirs.len; i++) {
            sv_push(&r.compile_flags, "-isystem");
            sv_push(&r.compile_flags, dirs.data[i]);
        }
    }
    if (!needs_link(o) || o.no_stdlib || o.relocatable) return r;
    const char *libdirs[3] = {l.crt_dir, l.msvc_lib_dir, l.kernel32_lib_dir};
    for (int i = 0; i < 3; i++) {
        if (nz(libdirs[i])[0]) sv_push(&r.link_flags, str_fmt("-Wl,-libpath:%s", libdirs[i]));
    }
    if (o.cxx && !o.no_defaultlibs && !o.no_libc) {
        char *dir = msvc_build_libcxx(o, t, tc, l, &err);
        if (err) { *error = err; return r; }
        sv_push(&r.link_flags, path_join(dir, "libc++.lib"));
        /* The MSVC adapter calls __ExceptionPtr* and new_handler ABI helpers
           supplied by these CRT archives, not by libvcruntime itself. */
        sv_push(&r.link_flags, path_join(l.msvc_lib_dir, o.shared ? "msvcprt.lib" : "libcpmt.lib"));
    }
    return r;
}
