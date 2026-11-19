#include "windows_runtime.h"

/* Match the upstream findDef: architecture-specific .def, common .def, then
   common .def.in. Do not interpret a path or an exact-filename -l: operand
   as a bundled library name. */
const char *mingw_arch_directory(Target t) {
    if (streq(t.arch, "x86")) return "lib32";
    if (streq(t.arch, "thumb")) return "libarm32";
    if (streq(t.arch, "aarch64")) return "libarm64";
    return "lib64";
}

const char *mingw_coff_machine(Target t) {
    if (streq(t.arch, "x86")) return "x86";
    if (streq(t.arch, "thumb")) return "arm";
    if (streq(t.arch, "aarch64")) return "arm64";
    return "x64";
}

char *mingw_import_definition(const char *library, Target t, Toolchain tc) {
    library = nz(library);
    if (!library[0]) return str_copy("");
    for (const char *b = library; *b; b++) {
        if (!((*b >= 'a' && *b <= 'z') || (*b >= 'A' && *b <= 'Z') || (*b >= '0' && *b <= '9') || *b == '_' || *b == '-' || *b == '.')) return str_copy("");
    }
    char *relatives[3] = {
        path_join(mingw_arch_directory(t), str_fmt("%s.def", library)),
        path_join("lib-common", str_fmt("%s.def", library)),
        path_join("lib-common", str_fmt("%s.def.in", library)),
    };
    for (int i = 0; i < 3; i++) {
        char *p = path_join(tc.lib_dir, "libc", "mingw", relatives[i]);
        if (file_exists(p)) return p;
    }
    return str_copy("");
}

typedef struct { char *data; size_t len, cap; } Text;
static void text_add(Text *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) { b->cap = (b->len + n + 1) * 2; b->data = xrealloc(b->data, b->cap); }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}
static void text_str(Text *b, const char *s) { text_add(b, s, strlen(s)); }

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
static char *trim_ws(const char *s, size_t n) {
    size_t a = 0;
    while (a < n && is_ws(s[a])) a++;
    while (n > a && is_ws(s[n - 1])) n--;
    return str_ncopy(s + a, n - a);
}
static char *trim_quotes(const char *s, size_t n) {
    size_t a = 0;
    while (a < n && s[a] == '"') a++;
    while (n > a && s[n - 1] == '"') n--;
    return str_ncopy(s + a, n - a);
}

/* MinGW DEFs omit the leading underscore on x86 stdcall names, unlike
   lld-link's MSVC DEF parser. Restore that prefix and use EXPORTAS to
   import the undecorated DLL export while retaining the decorated COFF
   symbol. Other architectures strip the suffix, matching the upstream fixup.
   Ordinal tokens and C++ names are not stdcall decorations. */
static char *mingw_normalize_def(const char *data, Target t) {
    Text out = {0};
    const char *pos = data;
    for (;;) {
        const char *nl = strchr(pos, '\n');
        size_t ll = nl ? (size_t)(nl - pos) : strlen(pos);
        const char *line = pos;
        size_t start = 0;
        while (start < ll && (line[start] == ' ' || line[start] == '\t')) start++;
        size_t end = start;
        while (end < ll && line[end] != ' ' && line[end] != '\t' && line[end] != '\r' && line[end] != '=' && line[end] != ';') end++;
        const char *name = line + start;
        size_t nlen = end - start;
        const char *atp = memchr(name, '@', nlen);
        size_t at = atp ? (size_t)(atp - name) : 0;
        bool handled = false;
        if (atp && at > 0) {
            const char *after_quote = name[0] == '"' ? name + 1 : name;
            if (after_quote[0] != '?' && name[0] != ';') {
                const char *suffix = name + at + 1;
                size_t slen = nlen - at - 1;
                if (slen > 0 && suffix[slen - 1] == '"') slen--;
                bool digits = slen > 0;
                for (size_t k = 0; k < slen; k++) if (suffix[k] < '0' || suffix[k] > '9') digits = false;
                if (digits) {
                    handled = true;
                    if (streq(t.arch, "x86")) {
                        char *plain = trim_quotes(name, nlen);
                        text_add(&out, line, start);
                        text_str(&out, str_fmt("\"_%s\"", plain));
                        /* EXPORTAS must follow DATA/NONAME/ordinal attributes, but
                           precede a comment. Internal aliases are irrelevant when
                           generating import libraries, as in the upstream DEF fixup. */
                        char *rest = trim_ws(line + end, ll - end);
                        if (rest[0] == '=') {
                            char *tail = trim_ws(rest + 1, strlen(rest + 1));
                            size_t alias_end = 0;
                            while (tail[alias_end] && tail[alias_end] != ' ' && tail[alias_end] != '\t' && tail[alias_end] != ';') alias_end++;
                            rest = tail + alias_end;
                        }
                        const char *comment = strchr(rest, ';');
                        char *attrs = trim_ws(rest, comment ? (size_t)(comment - rest) : strlen(rest));
                        text_str(&out, str_fmt(" %s EXPORTAS \"%s\"", attrs, trim_quotes(name, at)));
                        if (comment) { text_str(&out, " "); text_str(&out, comment); }
                    } else {
                        text_add(&out, line, start + at);
                        if (name[nlen - 1] == '"') text_str(&out, "\"");
                        text_add(&out, line + end, ll - end);
                    }
                }
            }
        }
        if (!handled) text_add(&out, line, ll);
        text_str(&out, "\n");
        if (!nl) break;
        pos = nl + 1;
    }
    return out.data;
}

char *mingw_build_import(const char *library, const char *definition, const char *work, Target t, Toolchain tc, bool verbose) {
    const char *def_path = definition;
    if (ends_with(def_path, ".in")) {
        def_path = path_join(work, str_fmt("%s.pp.def", library));
        StrVec pp = SV(tc.clang, "--no-default-config", str_fmt("--target=%s", nz(t.clang)), "-w", "-E", "-P", "-x", "c", "-I",
            path_join(tc.lib_dir, "libc", "mingw", "def-include"), definition, "-o", def_path);
        if (run_command(pp, verbose) != 0) return str_fmt("MinGW DEF preprocessing failed: %s", library);
    }
    size_t size = 0;
    char *data = read_file(def_path, &size);
    if (!data) return str_fmt("cannot read MinGW DEF: %s", library);
    /* read_file output may not be NUL-terminated at size. */
    char *text = str_ncopy(data, size);
    char *normalized = mingw_normalize_def(text, t);
    def_path = path_join(work, str_fmt("%s.def", library));
    if (!write_file(def_path, normalized, strlen(normalized))) return str_fmt("cannot stage MinGW DEF: %s", library);
    StrVec lib = SV(tc.lld, "/lib", str_fmt("/machine:%s", mingw_coff_machine(t)), str_fmt("/def:%s", def_path),
        str_fmt("/out:%s", path_join(work, str_fmt("%s.lib", library))));
    if (run_command(lib, verbose) != 0) return str_fmt("MinGW import library creation failed: %s", library);
    return NULL;
}

static StrVec mingw_crt_flags(Target t, Toolchain tc, bool include_mingw) {
    StrVec flags = SV("--no-default-config", str_fmt("--target=%s", nz(t.clang)), "-nostdlibinc", "-isystem", path_join(tc.lib_dir, "include"),
        "-isystem", path_join(tc.lib_dir, "libc", "include", "any-windows-any"));
    if (include_mingw) { sv_push(&flags, "-I"); sv_push(&flags, path_join(tc.lib_dir, "libc", "mingw", "include")); }
    return flags;
}

/* The upstream folds its UUID source group into libmingw32 and recognizes -luuid as
   a libc library. Our GNU linker needs an actual archive for that operand.
   Reuse the pinned manifest's complete contiguous UUID group (not just uuid.c),
   with the same CRT flags, in a separate requested-only, target-keyed cache. */
char *mingw_build_uuid(Options o, Target t, Toolchain tc, char **error) {
    *error = NULL;
    char *dest = runtime_cache("mingw-uuid-v2-thumb", o, t, tc);
    if (file_exists(path_join(dest, "complete"))) return dest;
    char *terr = NULL;
    char *work = make_temp_dir(tc.cache_dir, "mingw-uuid-", &terr);
    if (terr) { *error = str_fmt("cannot create MinGW UUID stage: %s", terr); return NULL; }
    StrVec flags = mingw_crt_flags(t, tc, true);
    const char *more[] = {"-w", "-O2", "-std=gnu11", "-fno-stack-protector", "-D__USE_MINGW_ANSI_STDIO=0", "-D__MSVCRT_VERSION__=0x700", "-D_CRTBLD", "-D_SYSCRT=1", "-D_WIN32_WINNT=0x0f00", "-DCRTDLL=1", "-DHAVE_CONFIG_H"};
    for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); i++) sv_push(&flags, more[i]);
    if (streq(t.arch, "thumb")) sv_push(&flags, "-mfpu=vfp");
    StrVec objects = {0};
    bool uuid_group = false, complete = false;
    char *err = NULL;
    for (size_t i = 0; i < mingw32_generic_src_len; i++) {
        const char *source = mingw32_generic_src[i];
        if (streq(source, "libsrc/atsmedia-uuid.c")) uuid_group = true;
        if (!uuid_group) continue;
        char *object = path_join(work, str_fmt("uuid-%zu.obj", objects.len));
        err = runtime_compile(path_join(tc.lib_dir, "libc", "mingw", source), object, flags, tc, o.verbose);
        if (err) { *error = err; goto fail; }
        sv_push(&objects, object);
        if (streq(source, "libsrc/windowscodecs.c")) { complete = true; break; }
    }
    if (!complete || objects.len != 46) { *error = str_copy("pinned MinGW UUID source group is incomplete"); goto fail; }
    err = runtime_archive(objects, path_join(work, "libuuid.a"), tc, o.verbose);
    if (err) { *error = err; goto fail; }
    err = runtime_publish(work, dest);
    if (err) { *error = err; goto fail; }
    remove_tree(work);
    return dest;
fail:
    remove_tree(work);
    return NULL;
}

/* Scan the flattened linker arguments, so ordinary -l, -Wl and -Xlinker
   spellings share one implementation. Each requested bundled library has its
   own atomic cache entry. Unknown/external libraries remain the linker's job. */
StrVec mingw_user_imports(StrVec args, Options o, Target t, Toolchain tc, char **error) {
    StrVec paths = {0}, seen = {0};
    bool skip = false;
    *error = NULL;
    for (size_t i = 0; i < args.len; i++) {
        const char *arg = args.data[i];
        if (skip) { skip = false; continue; }
        const char *library = "";
        if (streq(arg, "-l") || streq(arg, "--library")) {
            if (i + 1 < args.len) { library = args.data[i + 1]; skip = true; }
        } else if (starts_with(arg, "-l")) {
            library = arg + 2;
        } else if (starts_with(arg, "--library=")) {
            library = arg + strlen("--library=");
        }
        bool already = false;
        for (size_t k = 0; k < seen.len; k++) if (streq(seen.data[k], library)) already = true;
        if (already) continue;
        char *err = NULL;
        if (streq(library, "uuid")) {
            char *dest = mingw_build_uuid(o, t, tc, &err);
            if (err) { *error = err; return (StrVec){0}; }
            sv_push(&seen, library);
            sv_push(&paths, "-L");
            sv_push(&paths, dest);
            continue;
        }
        char *definition = mingw_import_definition(library, t, tc);
        if (!definition[0]) continue;
        sv_push(&seen, library);
        char *dest = runtime_cache(str_fmt("mingw-import-v2-%s", library), o, t, tc);
        if (!file_exists(path_join(dest, "complete"))) {
            char *terr = NULL;
            char *work = make_temp_dir(tc.cache_dir, "mingw-import-", &terr);
            if (terr) { *error = str_fmt("cannot create MinGW import stage: %s", terr); return (StrVec){0}; }
            err = mingw_build_import(library, definition, work, t, tc, o.verbose);
            if (!err) err = runtime_publish(work, dest);
            remove_tree(work);
            if (err) { *error = err; return (StrVec){0}; }
        }
        sv_push(&paths, "-L");
        sv_push(&paths, dest);
    }
    return paths;
}

/* The pinned original spinlock dependency (deps/winpthreads/spinlock.c).
   tests/windows_test.c verifies this text equals that file. */
const char MINGW_SPINLOCK_SOURCE[] =
    "/*\n"
    "   Copyright (c) 2013 mingw-w64 project\n"
    "   Copyright (c) 2015 Intel Corporation\n"
    "\n"
    "   Permission is hereby granted, free of charge, to any person obtaining a\n"
    "   copy of this software and associated documentation files (the \"Software\"),\n"
    "   to deal in the Software without restriction, including without limitation\n"
    "   the rights to use, copy, modify, merge, publish, distribute, sublicense,\n"
    "   and/or sell copies of the Software, and to permit persons to whom the\n"
    "   Software is furnished to do so, subject to the following conditions:\n"
    "\n"
    "   The above copyright notice and this permission notice shall be included in\n"
    "   all copies or substantial portions of the Software.\n"
    "\n"
    "   THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
    "   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
    "   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
    "   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
    "   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING\n"
    "   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER\n"
    "   DEALINGS IN THE SOFTWARE.\n"
    "*/\n"
    "\n"
    "#ifdef HAVE_CONFIG_H\n"
    "#include \"config.h\"\n"
    "#endif\n"
    "\n"
    "#define WIN32_LEAN_AND_MEAN\n"
    "#include <windows.h>\n"
    "\n"
    "/* public header files */\n"
    "#include \"pthread.h\"\n"
    "/* internal header files */\n"
    "#include \"misc.h\"\n"
    "\n"
    "/* We use the pthread_spinlock_t itself as a lock:\n"
    "   -1 is free, 0 is locked.\n"
    "   (This is dictated by PTHREAD_SPINLOCK_INITIALIZER, which we can't change\n"
    "   without breaking binary compatibility.) */\n"
    "typedef intptr_t spinlock_word_t;\n"
    "\n"
    "int\n"
    "pthread_spin_init (pthread_spinlock_t *lock, int pshared)\n"
    "{\n"
    "  spinlock_word_t *lk = (spinlock_word_t *)lock;\n"
    "  *lk = -1;\n"
    "  return 0;\n"
    "}\n"
    "\n"
    "\n"
    "int\n"
    "pthread_spin_destroy (pthread_spinlock_t *lock)\n"
    "{\n"
    "  return 0;\n"
    "}\n"
    "\n"
    "int\n"
    "pthread_spin_lock (pthread_spinlock_t *lock)\n"
    "{\n"
    "  volatile spinlock_word_t *lk = (volatile spinlock_word_t *)lock;\n"
    "  while (unlikely(InterlockedExchangePointer((PVOID volatile *)lk, 0) == 0))\n"
    "    do {\n"
    "      YieldProcessor();\n"
    "    } while (*lk == 0);\n"
    "  return 0;\n"
    "}\n"
    "\n"
    "int\n"
    "pthread_spin_trylock (pthread_spinlock_t *lock)\n"
    "{\n"
    "  spinlock_word_t *lk = (spinlock_word_t *)lock;\n"
    "  return InterlockedExchangePointer((PVOID volatile *)lk, 0) == 0 ? EBUSY : 0;\n"
    "}\n"
    "\n"
    "\n"
    "int\n"
    "pthread_spin_unlock (pthread_spinlock_t *lock)\n"
    "{\n"
    "  volatile spinlock_word_t *lk = (volatile spinlock_word_t *)lock;\n"
    "  *lk = -1;\n"
    "  return 0;\n"
    "}\n";

/* Windows GNU runtime: x86, x86_64, thumb and aarch64, UCRT, executables and DLLs.
   Runtime sources and DEF files are the pinned MinGW distribution. */
char *runtime_build_mingw_threads(Options o, Target t, Toolchain tc, char **error) {
    *error = NULL;
    char *dest = runtime_cache("mingw-winpthreads-v2-spinlock", o, t, tc);
    if (file_exists(path_join(dest, "complete"))) return dest;
    char *terr = NULL;
    char *work = make_temp_dir(tc.cache_dir, "winpthreads-build-", &terr);
    if (terr) { *error = str_fmt("cannot create winpthreads stage: %s", terr); return NULL; }
    char *err = NULL;
    /* The upstream builds these into libmingw32 for every C/C++ link.
       Keep a separate cache so C pthreads never require building libc++. */
    StrVec flags = mingw_crt_flags(t, tc, false);
    const char *more[] = {"-w", "-O2", "-std=gnu11", "-fno-stack-protector", "-ffunction-sections", "-fdata-sections", "-D__USE_MINGW_ANSI_STDIO=0", "-DIN_WINPTHREAD"};
    for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); i++) sv_push(&flags, more[i]);
    StrVec objects = {0};
    for (size_t i = 0; i < mingw32_winpthreads_src_len; i++) {
        char *object = path_join(work, str_fmt("pthread-%zu.obj", i));
        err = runtime_compile(path_join(tc.lib_dir, "libc", "mingw", mingw32_winpthreads_src[i]), object, flags, tc, o.verbose);
        if (err) { *error = err; goto fail; }
        sv_push(&objects, object);
    }
    /* Spinlocks moved out of the upstream CRT archive. Use the original unmodified C
       dependency instead; it preserves the same intptr_t/-1-unlocked ABI. */
    char *spin_source = path_join(work, "spinlock.c");
    if (!write_file(spin_source, MINGW_SPINLOCK_SOURCE, strlen(MINGW_SPINLOCK_SOURCE))) {
        *error = str_fmt("cannot stage MinGW spinlock dependency: %s", strerror(errno));
        goto fail;
    }
    sv_push(&flags, "-I");
    sv_push(&flags, path_join(tc.lib_dir, "libc", "mingw", "winpthreads"));
    char *spin_object = path_join(work, "spinlock.obj");
    err = runtime_compile(spin_source, spin_object, flags, tc, o.verbose);
    if (err) { *error = err; goto fail; }
    sv_push(&objects, spin_object);
    err = runtime_archive(objects, path_join(work, "libpthread.a"), tc, o.verbose);
    if (err) { *error = err; goto fail; }
    err = runtime_publish(work, dest);
    if (err) { *error = err; goto fail; }
    remove_tree(work);
    return dest;
fail:
    remove_tree(work);
    return NULL;
}

/* Pinned mingw.always_link_libs, in its order (API-set UCRT imports first). */
const char *const mingw_always_link_libs[] = {
    "api-ms-win-crt-conio-l1-1-0", "api-ms-win-crt-convert-l1-1-0", "api-ms-win-crt-environment-l1-1-0",
    "api-ms-win-crt-filesystem-l1-1-0", "api-ms-win-crt-heap-l1-1-0", "api-ms-win-crt-locale-l1-1-0",
    "api-ms-win-crt-math-l1-1-0", "api-ms-win-crt-multibyte-l1-1-0", "api-ms-win-crt-private-l1-1-0",
    "api-ms-win-crt-process-l1-1-0", "api-ms-win-crt-runtime-l1-1-0", "api-ms-win-crt-stdio-l1-1-0",
    "api-ms-win-crt-string-l1-1-0", "api-ms-win-crt-time-l1-1-0", "api-ms-win-crt-utility-l1-1-0",
    "advapi32", "kernel32", "ntdll", "shell32", "user32",
};
const size_t mingw_always_link_libs_len = sizeof(mingw_always_link_libs) / sizeof(mingw_always_link_libs[0]);


Runtime prepare_mingw(Options o, Target t, Toolchain tc, char **error) {
    Runtime rt = {0};
    *error = NULL;
    if ((needs_link(o) && !in_target_inventory("x86 x86_64 thumb aarch64", t.arch)) || !streq(t.abi, "gnu")) {
        *error = str_copy("MinGW linking supports x86, x86_64, thumb and aarch64 Windows GNU targets");
        return rt;
    }
    if (o.cxx && !o.no_stdinc && !o.no_stdincxx) sv_extend(&rt.compile_flags, runtime_cxx_flags(t, tc.lib_dir));
    if (!o.no_stdinc) {
        sv_extend(&rt.compile_flags, SV("-nostdlibinc", "-isystem", path_join(tc.lib_dir, "include"), "-isystem", path_join(tc.lib_dir, "libc", "include", "any-windows-any")));
    }
    /* The upstream adds these for user C-family compiles when libc is linked
       (-nostdlib/-nolibc drop them unless -lc is repeated). The ANSI stdio
       macro is left to the headers: 0xE00 selects 0, an explicit older
       __MSVCRT_VERSION__ selects 1.
       An explicit -lc anywhere wins over -nolibc/-nostdlib, in any order. */
    bool no_libc = o.no_stdlib || o.no_libc;
    if (!no_libc || o.explicit_libc) {
        sv_extend(&rt.compile_flags, SV("-D__MSVCRT_VERSION__=0xE00", "-D_WIN32_WINNT=0x0a00", "-D__CRT__NO_INLINE"));
    }
    sv_push(&rt.compile_flags, "-Wno-pragma-pack");
    if (o.query || o.dry_run || !o.has_input || !needs_link(o) || o.no_stdlib || o.no_defaultlibs || o.relocatable) return rt;
    char *dest = runtime_cache("mingw-ucrt-v11-codeview-paths", o, t, tc);
    char *err = NULL;
    if (!file_exists(path_join(dest, "complete"))) {
        char *terr = NULL;
        char *work = make_temp_dir(tc.cache_dir, "mingw-build-", &terr);
        if (terr) { *error = str_fmt("cannot create MinGW stage: %s", terr); return rt; }
        StrVec base = SV(tc.clang, "--no-default-config", str_fmt("--target=%s", nz(t.clang)), "-nostdlibinc", "-isystem", path_join(tc.lib_dir, "include"),
            "-isystem", path_join(tc.lib_dir, "libc", "include", "any-windows-any"), "-I", path_join(tc.lib_dir, "libc", "mingw", "include"),
            "-w", "-O2", "-std=gnu11", "-fno-stack-protector", "-D__USE_MINGW_ANSI_STDIO=0", "-D__MSVCRT_VERSION__=0x700", "-D_CRTBLD",
            "-D_SYSCRT=1", "-D_WIN32_WINNT=0x0f00", "-DCRTDLL=1", "-DHAVE_CONFIG_H");
        /* The CRT retains real CodeView types, symbols and source lines even when
           the user's compilation has -g0. Keep staging/output paths out of it.
           Use forward-slash source operands as well as mapping the install
           prefix: CodeView BuildInfo retains the source operand's spelling. */
        sv_extend(&base, SV("-g", "-gcodeview", "-gno-codeview-command-line", "-fdebug-compilation-dir=.",
            str_fmt("-fdebug-prefix-map=%s=lib", tc.lib_dir), str_fmt("-fdebug-prefix-map=%s=lib", canonical_mingw_path(tc.lib_dir))));
        /* The upstream addCrtCcArgs uses VFP for Thumb MinGW CRT sources. */
        if (streq(t.arch, "thumb")) sv_push(&base, "-mfpu=vfp");
        StrVec sources = {0};
        for (size_t i = 0; i < mingw32_generic_src_len; i++) sv_push(&sources, mingw32_generic_src[i]);
        if (streq(t.arch, "x86") || streq(t.arch, "x86_64")) {
            for (size_t i = 0; i < mingw32_x86_src_len; i++) sv_push(&sources, mingw32_x86_src[i]);
            if (streq(t.arch, "x86")) for (size_t i = 0; i < mingw32_x86_32_src_len; i++) sv_push(&sources, mingw32_x86_32_src[i]);
        } else {
            for (size_t i = 0; i < mingw32_arm_src_len; i++) sv_push(&sources, mingw32_arm_src[i]);
        }
        StrVec objects = {0};
        for (size_t i = 0; i < sources.len; i++) {
            const char *source = sources.data[i];
            char *object = path_join(work, str_fmt("crt-%zu.obj", i));
            StrVec cmd = sv_copy(base);
            sv_extend(&cmd, SV("-Xclang", str_fmt("-object-file-name=%s", path_base(object)), "-c",
                canonical_mingw_path(path_join(tc.lib_dir, "libc", "mingw", source)), "-o", object));
            if (run_command(cmd, o.verbose) != 0) { *error = str_fmt("MinGW runtime compilation failed: %s", source); goto fail; }
            sv_push(&objects, object);
        }
        const char *probe_arch = streq(t.arch, "x86") ? "i386" : streq(t.arch, "thumb") ? "arm" : nz(t.arch);
        char *probe = first_existing(
            path_join(tc.lib_dir, "compiler-rt", "lib", "builtins", probe_arch, "chkstk.S"),
            path_join(tc.root, ".cache", "upstream", "compiler-rt-22.1.8", "compiler-rt", "lib", "builtins", probe_arch, "chkstk.S"));
        if (!nz(probe)[0] || !file_exists(probe)) {
            *error = str_fmt("pinned compiler-rt %s/chkstk.S is required for MinGW; provision compiler-rt sources", probe_arch);
            goto fail;
        }
        char *probe_obj = path_join(work, "chkstk.obj");
        StrVec plain = SV("--no-default-config", str_fmt("--target=%s", nz(t.clang)));
        err = runtime_compile(probe, probe_obj, plain, tc, o.verbose);
        if (err) { *error = err; goto fail; }
        sv_push(&objects, probe_obj);
        if (streq(t.arch, "x86")) {
            /* Clang's dynamic stack allocations use __alloca, not ___chkstk_ms. */
            char *alloca_source = path_join(path_dir(probe), "chkstk2.S");
            char *alloca_obj = path_join(work, "chkstk2.obj");
            err = runtime_compile(alloca_source, alloca_obj, plain, tc, o.verbose);
            if (err) { *error = err; goto fail; }
            sv_push(&objects, alloca_obj);
        }
        /* The pinned ssp runtime uses a pointer-sized terminator canary
           (last bytes '\n', 255), not a randomized constructor. Its production
           panic is a trap, with weak compiler-rt symbols.
           Lower that runtime directly to IR, keeping it independent of user SSP
           options and avoiding a dependency on any other compiler or authored C. */
        char *ssp_ir = path_join(work, "ssp.ll");
        char *guard = (streq(t.arch, "x86") || streq(t.arch, "thumb")) ? str_fmt("i32 %u, align 4", 0xff0a0000u)
            : str_fmt("i64 %llu, align 8", 0xff0a000000000000ull);
        char *text = str_fmt("@__stack_chk_guard = weak global %s\n"
            "\n"
            "declare void @llvm.trap() cold noreturn nounwind\n"
            "\n"
            "define weak void @__stack_chk_fail() cold noreturn nounwind {\n"
            "  call void @llvm.trap()\n"
            "  unreachable\n"
            "}\n"
            "\n"
            "define weak void @__chk_fail() cold noreturn nounwind {\n"
            "  call void @llvm.trap()\n"
            "  unreachable\n"
            "}\n", guard);
        if (!write_file(ssp_ir, text, strlen(text))) { *error = str_fmt("cannot write MinGW SSP runtime: %s", strerror(errno)); goto fail; }
        char *ssp_object = path_join(work, "ssp.obj");
        err = runtime_compile(ssp_ir, ssp_object, SV("--no-default-config", str_fmt("--target=%s", nz(t.clang)), "-O2", "-Wno-override-module", "-fno-stack-protector"), tc, o.verbose);
        if (err) { *error = err; goto fail; }
        sv_push(&objects, ssp_object);
        err = runtime_archive(objects, path_join(work, "mingw32.lib"), tc, o.verbose);
        if (err) { *error = err; goto fail; }
        /* The upstream compiles a separate crt2 with -DUNICODE -D_UNICODE for -municode. */
        static const struct { const char *source, *object; bool unicode; } startups[] = {
            {"crtexe.c", "crt2.obj", false}, {"crtexe.c", "crt2u.obj", true}, {"crtdll.c", "dllcrt2.obj", false}};
        for (size_t k = 0; k < 3; k++) {
            StrVec cmd = sv_copy(base);
            if (startups[k].unicode) { sv_push(&cmd, "-DUNICODE"); sv_push(&cmd, "-D_UNICODE"); }
            sv_extend(&cmd, SV("-Xclang", str_fmt("-object-file-name=%s", startups[k].object), "-c",
                canonical_mingw_path(path_join(tc.lib_dir, "libc", "mingw", "crt", startups[k].source)), "-o", path_join(work, startups[k].object)));
            if (run_command(cmd, o.verbose) != 0) { *error = str_fmt("MinGW startup compilation failed: %s", startups[k].object); goto fail; }
        }
        for (size_t k = 0; k < mingw_always_link_libs_len; k++) {
            const char *library = mingw_always_link_libs[k];
            char *definition = mingw_import_definition(library, t, tc);
            if (!definition[0]) { *error = str_fmt("missing MinGW import definition: %s", library); goto fail; }
            err = mingw_build_import(library, definition, work, t, tc, o.verbose);
            if (err) { *error = err; goto fail; }
        }
        err = runtime_publish(work, dest);
        if (err) { *error = err; goto fail; }
        remove_tree(work);
        goto staged;
    fail:
        remove_tree(work);
        return rt;
    }
staged:
    if (!o.no_startfiles) sv_push(&rt.link_flags, path_join(dest, o.shared ? "dllcrt2.obj" : o.municode ? "crt2u.obj" : "crt2.obj"));
    if (o.cxx) {
        char *dir = runtime_build_cxx(o, t, tc, false, &err);
        if (err) { *error = err; return rt; }
        static const char *const libs[] = {"c++", "c++abi", "unwind"};
        for (int k = 0; k < 3; k++) sv_push(&rt.link_flags, path_join(dir, str_fmt("lib%s.a", libs[k])));
    }
    char *threads = runtime_build_mingw_threads(o, t, tc, &err);
    if (err) { *error = err; return rt; }
    /* The search path also resolves explicit -lpthread and -l pthread operands. */
    sv_extend(&rt.link_flags, SV("-L", threads, path_join(threads, "libpthread.a")));
    sv_push(&rt.link_flags, path_join(dest, "mingw32.lib"));
    for (size_t k = 0; k < mingw_always_link_libs_len; k++) sv_push(&rt.link_flags, path_join(dest, str_fmt("%s.lib", mingw_always_link_libs[k])));
    return rt;
}

/* Use the actual GNU MinGW frontend, not a lossy GNU-to-COFF flag map.
   In particular this preserves archive order, export policy and import libraries.
   The root name: from -soname as the driver derives --name (a "lib" prefix and a
   ".so" or ".so.N" suffix dropped), otherwise the stem of the first C source, else of
   the first file input, else of the output. */
static const char *fp_ext(const char *path) {
    for (size_t i = strlen(path); i > 0; i--) {
        if (path[i - 1] == '/' || path[i - 1] == '\\') break;
        if (path[i - 1] == '.') return path + i - 1;
    }
    return path + strlen(path);
}

static char *trim_ext(const char *path) {
    return str_ncopy(path, (size_t)(fp_ext(path) - path));
}

char *output_root_name(const char *first, const char *soname) {
    first = nz(first);
    soname = nz(soname);
    if (!soname[0]) return trim_ext(path_base(first));
    size_t len = strlen(soname);
    size_t prefix = starts_with(soname, "lib") ? 3 : 0;
    size_t end = len;
    if (ends_with(soname, ".so")) end -= 3;
    else {
        bool found_digit = false;
        while (end > 0 && soname[end - 1] >= '0' && soname[end - 1] <= '9') { found_digit = true; end--; }
        if (found_digit && end > 0 && soname[end - 1] == '.') end--; else end = len;
        if (end >= prefix + 3 && !strncmp(soname + end - 3, ".so", 3)) end -= 3;
    }
    if (end < prefix) end = prefix;
    return str_ncopy(soname + prefix, end - prefix);
}

int run_mingw_link(Options o, Target t, Toolchain tc, Runtime rt) {
    const char *output = nz(o.output);
    if (!output[0]) output = o.relocatable ? "a.obj" : "a.exe";
    char *serr = NULL;
    char *work = deterministic_link_stage(tc.cache_dir, "mingw-link-", output, &serr);
    if (serr) { fprintf(stderr, "cannot create MinGW link stage\n"); return 1; }
    int result = 1;
    /* The COFF writer uses a zero timestamp. LLD's MinGW frontend otherwise
       inserts wall-clock time in the PE header and exported-symbol directory.
       Keep this before user arguments so --insert-timestamp remains an opt-out. */
    const char *emulation = streq(t.arch, "x86") ? "i386pe" : streq(t.arch, "thumb") ? "thumb2pe" : streq(t.arch, "aarch64") ? "arm64pe" : "i386pep";
    /* Also -BUILD-ID:NO: LLD's MinGW frontend would otherwise add a
       CodeView debug record with no PDB to every image. */
    StrVec link = SV("ld.lld", "-m", emulation, "--gc-sections", "--no-insert-timestamp", "--build-id=none");
    sv_push(&link, "-o");
    sv_push(&link, output);
    /* A 16 MiB main-thread stack is reserved for executables; a later user --stack wins. */
    if (!o.shared && !o.relocatable) { sv_push(&link, "--stack"); sv_push(&link, "16777216"); }
    /* Without --pdb, LLD's MinGW frontend drops the CodeView debug info.
       <output>.pdb is written and only its file name recorded. */
    if (!o.relocatable && emits_debug_info(o, true)) {
        sv_push(&link, str_fmt("--pdb=%s.pdb", trim_ext(output)));
        sv_push(&link, "-Xlink=-pdbaltpath:%_PDB%");
    } else if (!o.relocatable) {
        /* The frontend passes -debug:dwarf unless -s, which leaves an empty CodeView
           debug-directory entry even with --build-id=none; stripped images have none. */
        sv_push(&link, "-Xlink=-debug:none");
    }
    /* Optimized images carry a content-hash timestamp and a REPRO debug entry; Debug
       images get the wall clock, which stays zero here. -Brepro overrides any
       timestamp, so a user --insert-timestamp keeps it off. */
    if (!o.relocatable && optimized_build(o)) {
        bool user_timestamp = false;
        for (size_t i = 0; i < o.forwarded.len; i++) if (contains(o.forwarded.data[i], "insert-timestamp")) user_timestamp = true;
        if (!user_timestamp) sv_push(&link, "-Xlink=-Brepro");
    }
    Options compile = o;
    compile.compile_only = true;
    compile.forwarded = (StrVec){0};
    StrVec inputs = {0}, languages = {0};
    size_t *positions = NULL, position_count = 0, position_cap = 0;
    int file_count = 0;
    const char *object_input = "", *first_object = "", *language = "";
    bool skip = false, operands = false;
    for (size_t i = 0; i < o.forwarded.len; i++) {
        const char *arg = o.forwarded.data[i];
        if (skip) { skip = false; continue; }
        if (!operands) {
            if (streq(arg, "--")) { operands = true; continue; }
            if (streq(arg, "-x") || streq(arg, "--language")) {
                if (i + 1 >= o.forwarded.len) { fprintf(stderr, "missing source language\n"); goto done; }
                language = o.forwarded.data[i + 1]; skip = true; continue;
            }
            if (starts_with(arg, "-x") && strlen(arg) > 2) { language = arg + 2; continue; }
            if (starts_with(arg, "--language=")) { language = arg + strlen("--language="); continue; }
            if (streq(arg, "-o") || streq(arg, "--output")) { skip = true; continue; }
            if (starts_with(arg, "--output=") || (starts_with(arg, "-o") && !has_prefix_any(arg, "-objc", "-openmp", "-obj") && strlen(arg) > 2)) continue;
            if (streq(arg, "-Xlinker") || streq(arg, "-Wl")) {
                if (i + 1 >= o.forwarded.len) { fprintf(stderr, "missing linker argument\n"); goto done; }
                if (streq(arg, "-Wl")) sv_extend(&link, str_split(o.forwarded.data[i + 1], ","));
                else sv_push(&link, o.forwarded.data[i + 1]);
                skip = true;
                continue;
            }
            if (starts_with(arg, "-Wl,")) { sv_extend(&link, str_split(arg + 4, ",")); continue; }
            if (streq(arg, "-L") || streq(arg, "-l") || streq(arg, "-u") || streq(arg, "-e") || streq(arg, "-T")) {
                if (i + 1 >= o.forwarded.len) goto done;
                sv_push(&link, arg); sv_push(&link, o.forwarded.data[i + 1]); skip = true; continue;
            }
            if (starts_with(arg, "-L") || starts_with(arg, "-l")) { sv_push(&link, arg); continue; }
            if (streq(arg, "-shared")) { sv_push(&link, "--dll"); continue; }
            if (streq(arg, "-static") || streq(arg, "-s")) { sv_push(&link, arg); continue; }
            if (streq(arg, "-rdynamic")) { sv_push(&link, "--export-all-symbols"); continue; }
            if (streq(arg, "-r") || streq(arg, "-nostdlib") || streq(arg, "-nodefaultlibs") || streq(arg, "-nostartfiles") || streq(arg, "-pie") || streq(arg, "-no-pie")) continue;
            if (takes_value(arg)) {
                sv_push(&compile.forwarded, arg);
                if (i + 1 < o.forwarded.len) { sv_push(&compile.forwarded, o.forwarded.data[i + 1]); skip = true; }
                continue;
            }
        }
        if (operands || !starts_with(arg, "-") || streq(arg, "-")) {
            file_count++;
            const char *ext = fp_ext(arg);
            if ((!language[0] || streq(language, "none")) && (streq(ext, ".o") || streq(ext, ".obj") || streq(ext, ".a") || streq(ext, ".lib") || streq(ext, ".def") || streq(ext, ".res") || streq(ext, ".dll"))) {
                sv_push(&link, arg);
                if (!object_input[0]) first_object = arg;
                object_input = arg;
            } else {
                sv_push(&inputs, arg);
                sv_push(&languages, language);
                if (position_count == position_cap) { position_cap = position_cap ? position_cap * 2 : 8; positions = xrealloc(positions, position_cap * sizeof(size_t)); }
                positions[position_count++] = link.len;
                sv_push(&link, ""); /* Filled after compilation, preserving input order. */
            }
        } else sv_push(&compile.forwarded, arg);
    }
    /* -soname is read only as the output's name (COFF has no soname), and an
       import library <name>.lib is written beside every image unless the user names one; LLD
       creates it only for images with exports. */
    const char *soname = "";
    bool user_implib = false;
    for (size_t i = 0; i < link.len; i++) {
        const char *a = link.data[i];
        if ((streq(a, "-soname") || streq(a, "--soname")) && i + 1 < link.len) { soname = link.data[i + 1]; i++; continue; }
        if (has_prefix_any(a, "--out-implib", "-implib", "--implib")) user_implib = true;
    }
    /* Keep source-placeholder positions intact until compilation has filled them. */
    if (!o.relocatable && !user_implib) {
        const char *first = inputs.len > 0 ? inputs.data[0] : first_object[0] ? first_object : output;
        char *name = output_root_name(first, soname);
        sv_push(&link, "--out-implib");
        sv_push(&link, path_join(path_dir(output), str_fmt("%s.lib", name)));
    }
    /* One COFF input is accepted for -r (compile/copy), but they cannot be combined. */
    if (o.relocatable && file_count != 1) {
        fprintf(stderr, "error: coff does not support linking multiple objects into one\n");
        goto done;
    }
    if (o.relocatable && !nz(o.output)[0]) {
        const char *input = inputs.len > 0 ? inputs.data[0] : object_input;
        char *base = path_base(input);
        output = str_fmt("%s.obj", trim_ext(base));
    }
    for (size_t i = 0; i < inputs.len; i++) {
        Options co = compile;
        co.forwarded = sv_copy(compile.forwarded);
        if (languages.data[i][0] && !streq(languages.data[i], "none")) { sv_push(&co.forwarded, "-x"); sv_push(&co.forwarded, languages.data[i]); }
        const char *object = o.relocatable ? output : path_join(work, str_fmt("input-%zu.obj", i));
        if (canonical_mingw_link(o)) {
            /* S_OBJNAME is emitted during compilation, before LLD sees the
               relative staged object path. Do not record the cache here either. */
            sv_push(&co.forwarded, "-Xclang");
            sv_push(&co.forwarded, str_fmt("-object-file-name=input-%zu.obj", i));
        }
        sv_push(&co.forwarded, "-c");
        sv_push(&co.forwarded, inputs.data[i]);
        sv_push(&co.forwarded, "-o");
        sv_push(&co.forwarded, object);
        char *ce = NULL;
        StrVec cmd = compiler_command(co, t, tc, rt, false, &ce);
        if (ce) { fprintf(stderr, "%s\n", ce); goto done; }
        int code = run_command(cmd, o.verbose);
        if (code != 0) { result = code; goto done; }
        link.data[positions[i]] = (char *)object;
    }
    if (o.relocatable) {
        result = 0;
        if (inputs.len > 0 || o.dry_run) goto done;
        if (file_exists(output) && streq(absolute_path(object_input), absolute_path(output))) goto done;
        if (!copy_file(output, object_input)) { fprintf(stderr, "cannot copy COFF object: %s\n", strerror(errno)); result = 1; }
        goto done;
    }
    /* Remove the COFF-unused soname only after filling source placeholders. */
    StrVec kept = {0};
    for (size_t i = 0; i < link.len; i++) {
        const char *a = link.data[i];
        if ((streq(a, "-soname") || streq(a, "--soname")) && i + 1 < link.len) { i++; continue; }
        sv_push(&kept, a);
    }
    link = kept;
    size_t managed_start = link.len;
    sv_extend(&link, rt.link_flags);
    if (o.dry_run) { print_command(link); result = 0; goto done; }
    char *import_err = NULL;
    StrVec imports = mingw_user_imports(link, o, t, tc, &import_err);
    if (import_err) { fprintf(stderr, "%s\n", import_err); goto done; }
    /* All user -L paths precede these fallback paths, even when supplied after
       -l or through -Wl/-Xlinker. Never replace an explicit user library path. */
    sv_extend(&link, imports);
    if (canonical_mingw_link(o) && canonical_mingw_inputs(sv_slice(link, 0, managed_start))) {
        result = run_canonical_mingw_link(link, work, managed_start, o.verbose);
        goto done;
    }
    if (o.verbose) print_command(link);
    /* No early exit: let the driver clean up the compiled temporary objects. */
    result = BridgeLLDLinkMinGW((int)link.len, (const char **)link.data, false, false) ? 0 : 1;
done:
    remove_tree(work);
    return result;
}
