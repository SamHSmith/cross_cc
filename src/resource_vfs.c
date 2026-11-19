/* LLVM's UBSan type printer embeds presumed header filenames before prefix maps
   run. Give only driver-owned bundled headers stable FileEntry names instead.
   The main input and every user operand remain ordinary physical paths. */
#include "cross.h"
#include "resources.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

char *resource_vfs_root(void) { return RESOURCE_VFS_ROOT; }

/* Supported Windows builds link the authenticated upstream Path.cpp configured
   with LLVM_WINDOWS_PREFER_FORWARD_SLASH=1. Unknown/custom backends stay physical;
   defining CROSS_CC_WINDOWS_CANONICAL_PATHS asserts that same backend contract. */
bool resource_vfs_eligible(Options o, Toolchain tc, bool dependencies) {
#if defined(_WIN32) && !defined(CROSS_CC_WINDOWS_CANONICAL_PATHS)
    (void)o; (void)tc; (void)dependencies;
    return false;
#else
    if (!o.has_input || o.query || o.preprocess_only || o.dependency_only || compiler_plain_assembly(o.forwarded) || !*nz(tc.lib_dir) || !*nz(tc.cache_dir)) return false;
    /* External SDKs and libc configurations are deliberately outside this policy. */
    if (*nz(o.sysroot) || *nz(o.msvc_libc_file) || *nz(env("ZIG_LIBC"))) return false;
    if (*nz(env("DEPENDENCIES_OUTPUT")) || *nz(env("SUNPRO_DEPENDENCIES"))) return false;
    static const char *refused[] = {"-M", "-MM", "-MG", "-dependency-file",
        "--write-dependencies", "--write-user-dependencies", "--dependencies", "--user-dependencies",
        "-ivfsoverlay", "-resource-dir", "-isysroot", "--sysroot", "-Xclang", "-Xpreprocessor"};
    static const char *prefixes[] = {"-dependency-file=", "--dependency-file", "-ivfsoverlay", "-resource-dir=", "--sysroot=", "-isysroot",
        "-Wp,", "-Xclang=", "-Xpreprocessor=", "-Xarch_", "-print-", "--print-"};
    for (size_t i = 0; i < o.forwarded.len; i++) {
        const char *a = o.forwarded.data[i];
        if (streq(a, "--")) break;
        /* Only the central execution helper may enable dependency remapping. */
        if (streq(a, "-MD") || streq(a, "-MMD") || streq(a, "-MF") || streq(a, "-MP") || streq(a, "-MT") || streq(a, "-MQ")) {
            if (!dependencies) return false;
        }
        for (size_t k = 0; k < sizeof refused / sizeof refused[0]; k++) if (streq(a, refused[k])) return false;
        if (!dependencies && has_prefix_any(a, "-MF", "-MT", "-MQ")) return false;
        for (size_t k = 0; k < sizeof prefixes / sizeof prefixes[0]; k++) if (starts_with(a, prefixes[k])) return false;
        if (takes_value(a)) i++;
    }
    return true;
#endif
}

static char *resource_trim_slashes(const char *s) {
    size_t n = strlen(nz(s));
    while (n > 0 && s[n-1] == '/') n--;
    return str_ncopy(nz(s), n);
}

/* Return a virtual path only for a complete path component beneath the owned
   lib root. Normalizing separators is exclusively for these driver operands. */
char *resource_vfs_path(const char *path, const char *lib, bool *mapped) {
    char *p = builtins_driver_path(path), *root = resource_trim_slashes(builtins_driver_path(lib));
    if (mapped) *mapped = true;
    if (streq(p, root)) return resource_vfs_root();
    if (starts_with(p, str_fmt("%s/", root))) return str_fmt("%s%s", resource_vfs_root(), p + strlen(root));
    if (mapped) *mapped = false;
    return (char *)path;
}

/* JSON string literal: quotes, backslashes and control characters escaped. */
static void resource_json_string(Bytes *out, const char *s) {
    bytes_push(out, '"');
    for (const unsigned char *p = (const unsigned char *)nz(s); *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"': bytes_append(out, "\\\"", 2); break;
        case '\\': bytes_append(out, "\\\\", 2); break;
        case '\b': bytes_append(out, "\\b", 2); break;
        case '\f': bytes_append(out, "\\f", 2); break;
        case '\n': bytes_append(out, "\\n", 2); break;
        case '\r': bytes_append(out, "\\r", 2); break;
        case '\t': bytes_append(out, "\\t", 2); break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                bytes_append(out, buf, 6);
            } else bytes_push(out, c);
        }
    }
    bytes_push(out, '"');
}

/* Stage a complete private directory, then publish atomically; concurrent
   readers never see a partial overlay and writers never share a file. */
char *resource_vfs_overlay(Toolchain tc, char **error) {
    return resource_vfs_overlay_root(tc, resource_vfs_root(), tc.lib_dir, error);
}

/* Independent owned roots share the same atomic, content-checked cache policy.
   Keep overlays separate so managed SDKs cannot change bundled file identities. */
char *resource_vfs_overlay_root(Toolchain tc, const char *virtual_root, const char *physical, char **error) {
    if (error) *error = NULL;
    Bytes json = {0};
    const char *head = "{\"version\":0,\"use-external-names\":false,\"roots\":[{\"type\":\"directory-remap\",\"name\":";
    bytes_append(&json, head, strlen(head));
    resource_json_string(&json, virtual_root);
    bytes_append(&json, ",\"external-contents\":", strlen(",\"external-contents\":"));
    resource_json_string(&json, builtins_driver_path(physical));
    bytes_append(&json, "}]}", 3);
    bytes_push(&json, 0);
    const char *data = (const char *)json.data;
    size_t len = json.len - 1;
    char *dest = path_join(tc.cache_dir, str_fmt("resource-vfs-v1-%016llx", (unsigned long long)runtime_hash(data)));
    char *path = path_join(dest, "overlay.json");
    if (!file_exists(path_join(dest, "complete"))) {
        char *temp_error = NULL;
        char *work = make_temp_dir(tc.cache_dir, "resource-vfs-stage-", &temp_error);
        if (!work || temp_error) { if (error) *error = "cannot stage bundled resource overlay"; return NULL; }
        if (!write_file(path_join(work, "overlay.json"), data, len)) {
            remove_tree(work);
            if (error) *error = "cannot write bundled resource overlay";
            return NULL;
        }
        char *publish = runtime_publish(work, dest);
        remove_tree(work);
        if (publish && *publish) { if (error) *error = publish; return NULL; }
    }
    /* Refuse a corrupt cache or a hash collision rather than using another root. */
    size_t cached_len = 0;
    char *cached = read_file(path, &cached_len);
    if (!cached) { if (error) *error = "cannot read cached bundled resource overlay"; return NULL; }
    if (cached_len != len || memcmp(cached, data, len) != 0) { if (error) *error = "cached bundled resource overlay does not match this installation"; return NULL; }
    return path;
}

/* Preserve user map ordering and scope. Keep the original map (which may also
   affect user files), and add a matching virtual-header map immediately after it.
   An ancestor prefix retains its suffix: /real=/mine maps /real/lib to /mine/lib. */
char *resource_vfs_prefix_map(const char *arg, const char *lib) {
    static const char *prefixes[] = {"-ffile-prefix-map=", "-fmacro-prefix-map=", "-fdebug-prefix-map=", "-fcoverage-prefix-map="};
    for (size_t k = 0; k < sizeof prefixes / sizeof prefixes[0]; k++) {
        const char *prefix = prefixes[k];
        if (!starts_with(arg, prefix)) continue;
        const char *pair = arg + strlen(prefix);
        const char *equal = strchr(pair, '=');
        if (!equal) return "";
        char *from = str_ncopy(pair, (size_t)(equal - pair));
        const char *to = equal + 1;
        char *root = builtins_driver_path(lib);
        /* Do not normalize user map text: Clang itself matches it textually. */
        if (streq(from, root) || starts_with(from, str_fmt("%s/", root))) {
            return str_fmt("%s%s%s=%s", prefix, resource_vfs_root(), from + strlen(root), to);
        }
        if (*from && starts_with(root, from)) {
            return str_fmt("%s%s=%s%s", prefix, resource_vfs_root(), to, root + strlen(from));
        }
    }
    return "";
}

/* Clang's Make quoting: dollars double, hashes escape, and backslash runs
   preceding whitespace double before the whitespace's own escape is added. */
char *resource_make_quote(const char *text) {
    Bytes out = {0};
    text = nz(text);
    size_t n = strlen(text);
    for (size_t i = 0; i < n; i++) {
        char c = text[i];
        if (c == ' ' || c == '\t') {
            size_t j = i;
            while (j > 0 && text[j-1] == '\\') { bytes_push(&out, '\\'); j--; }
            bytes_push(&out, '\\');
        } else if (c == '#') bytes_push(&out, '\\');
        else if (c == '$') bytes_push(&out, '$');
        bytes_push(&out, (unsigned char)c);
    }
    bytes_push(&out, 0);
    return (char *)out.data;
}

#ifdef _WIN32
/* Volume serial and file index identify a file; follow selects whether a
   final reparse point is resolved. */
static bool resource_file_identity(const char *path, bool follow, BY_HANDLE_FILE_INFORMATION *info) {
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
    HANDLE h = CreateFileW(windows_wide(path), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, flags, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = GetFileInformationByHandle(h, info) != 0;
    CloseHandle(h);
    return ok;
}
#endif

static char *resource_getcwd(void) {
    size_t cap = 4096;
    for (;;) {
#ifdef _WIN32
        wchar_t *wbuf = xmalloc(cap * sizeof(wchar_t));
        if (_wgetcwd(wbuf, (int)cap)) return windows_utf8(wbuf);
#else
        char *buf = xmalloc(cap);
        if (getcwd(buf, cap)) return buf;
#endif
        if (errno != ERANGE) return NULL;
        cap *= 2;
    }
}

static bool resource_is_abs(const char *p) {
#ifdef _WIN32
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && (p[2] == '/' || p[2] == '\\')) return true;
    return (p[0] == '/' || p[0] == '\\') && (p[1] == '/' || p[1] == '\\');
#else
    return p[0] == '/';
#endif
}

static char *resource_join(StrVec v, const char *sep) {
    Bytes out = {0};
    for (size_t i = 0; i < v.len; i++) {
        if (i) bytes_append(&out, sep, strlen(sep));
        bytes_append(&out, v.data[i], strlen(v.data[i]));
    }
    bytes_push(&out, 0);
    return (char *)out.data;
}

static char *resource_strip_ext(const char *name) {
    char *ext = path_ext(name);
    return str_ncopy(name, strlen(name) - strlen(ext));
}

/* Deliberately bounded: one ordinary C-family source, a real object action,
   no frontend escapes, offloading/modules, alternate working directory, or
   dependency dialects. Unknown non-dependency compiler flags still pass through. */
Resource_Dependency_Plan resource_dependency_plan(Options o, Toolchain tc) {
    Resource_Dependency_Plan p = {"", "", false}, none = {"", "", false};
    if (!o.compile_only || o.assembly_only || o.syntax_only || o.header_emit || o.dry_run || o.shared || !resource_vfs_eligible(o, tc, true)) return p;
    int inputs = 0;
    const char *source = "", *language = "";
    bool md = false, operands = false;
    StrVec targets = {0};
    for (size_t i = 0; i < o.forwarded.len; i++) {
        const char *a = o.forwarded.data[i];
        if (!operands) {
            if (streq(a, "--")) { operands = true; continue; }
            if (streq(a, "-MD") || streq(a, "-MMD")) { md = true; continue; }
            if (streq(a, "-MF") || streq(a, "-MT") || streq(a, "-MQ")) {
                if (i+1 >= o.forwarded.len) return none;
                i++;
                const char *value = o.forwarded.data[i];
                /* Clang accepts an empty -MF but writes no dependency file. */
                if (streq(a, "-MF") && !*value) return none;
                if (streq(a, "-MF")) p.path = (char *)value;
                else if (streq(a, "-MT")) sv_push(&targets, value);
                else sv_push(&targets, resource_make_quote(value));
                continue;
            }
            if (starts_with(a, "-MF")) { p.path = (char *)a + 3; continue; }
            if (starts_with(a, "-MT")) { sv_push(&targets, a + 3); continue; }
            if (starts_with(a, "-MQ")) { sv_push(&targets, resource_make_quote(a + 3)); continue; }
            if (streq(a, "-x") || streq(a, "--language")) { i++; if (i >= o.forwarded.len) return none; language = o.forwarded.data[i]; continue; }
            if (starts_with(a, "-x")) { language = a + 2; continue; }
            if (starts_with(a, "--language=")) { language = a + strlen("--language="); continue; }
            if (streq(a, "-working-directory") || streq(a, "-arch") || streq(a, "-mllvm") || streq(a, "-MJ") ||
                has_prefix_any(a, "-working-directory=", "-fmodule", "-fcxx-module", "-fopenmp", "-fsycl", "--offload", "--cuda", "--hip", "-MJ", "-fdeps", "-fdep", "-save-temps", "-via-file-asm")) return none;
            if (takes_value(a)) { i++; continue; }
            if (starts_with(a, "-") && !streq(a, "-")) continue;
        }
        inputs++;
        source = a;
        if (*language && !streq(language, "none") && !in_target_inventory("c c++ cpp-output c++-cpp-output", language)) return none;
        if ((!*language || streq(language, "none")) && !in_target_inventory(".c .cc .cpp .cxx .C .i .ii", path_ext(a))) return none;
    }
    if (!md || inputs != 1 || streq(source, "-") || streq(nz(o.output), "-")) return none;
    const char *output = nz(o.output);
    if (!*output) output = str_fmt("%s.o", resource_strip_ext(path_base(source)));
    if (!*p.path) p.path = str_fmt("%s.d", resource_strip_ext(output));
    if (streq(p.path, "-") || strpbrk(p.path, "\n\r")) return none;
    char *cwd = resource_getcwd();
    if (!cwd) return none;
    char *dep_name = resource_is_abs(p.path) ? p.path : path_join(cwd, p.path);
    dep_name = path_clean(dep_name);
    const char *names[2] = {source, output};
    for (int k = 0; k < 2; k++) {
        char *full = resource_is_abs(names[k]) ? (char *)names[k] : path_join(cwd, names[k]);
        if (streq(dep_name, path_clean(full))) return none;
    }
#ifdef _WIN32
    DWORD attr = GetFileAttributesW(windows_wide(p.path));
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) return none;
        BY_HANDLE_FILE_INFORMATION info;
        if (resource_file_identity(p.path, false, &info)) {
            for (int k = 0; k < 2; k++) {
                BY_HANDLE_FILE_INFORMATION other;
                if (resource_file_identity(names[k], true, &other) && other.dwVolumeSerialNumber == info.dwVolumeSerialNumber &&
                    other.nFileIndexHigh == info.nFileIndexHigh && other.nFileIndexLow == info.nFileIndexLow)
                    return none;
            }
        }
    }
#else
    struct stat info;
    if (lstat(p.path, &info) == 0) {
        if ((info.st_mode & S_IFMT) != S_IFREG) return none;
        for (int k = 0; k < 2; k++) {
            struct stat other;
            if (stat(names[k], &other) == 0 && other.st_dev == info.st_dev && other.st_ino == info.st_ino) return none;
        }
    }
#endif
    /* Keep the main target region byte-identical, even -MT text that is not
       valid Make syntax. Clang may wrap between multiple long targets; keep
       those physical rather than duplicate its column-layout policy here. */
    if (targets.len == 0) sv_push(&targets, resource_make_quote(output));
    p.targets = str_fmt("%s:", resource_join(targets, " "));
    if (strpbrk(p.targets, "\n\r") || (targets.len > 1 && strlen(p.targets) > 60)) return none;
    p.enabled = true;
    return p;
}

/* Rewrite token prefixes, not substrings. The initial target region is opaque;
   later -MP targets are header names and must be restored along with prerequisites.
   All original separators, continuations, suffix escaping and user text survive. */
char *resource_dependency_text(const char *text, const char *targets, const char *lib, char **error) {
    if (error) *error = NULL;
    if (!starts_with(text, targets)) { if (error) *error = "unexpected Clang dependency target text"; return NULL; }
    Bytes out = {0};
    size_t tlen = strlen(targets), n = strlen(text);
    bytes_append(&out, text, tlen);
    const char *root = resource_vfs_root();
    char *physical = resource_make_quote(resource_trim_slashes(builtins_driver_path(lib)));
    for (size_t i = tlen; i < n;) {
        unsigned char c = (unsigned char)text[i];
        if (is_space(c) || c == ':') { bytes_push(&out, c); i++; continue; }
        size_t start = i;
        while (i < n) {
            if (text[i] == '\\' && i+1 < n) {
                /* Backslash-newline is a separator, not part of a filename. */
                if (text[i+1] == '\n' || text[i+1] == '\r') break;
                i += 2;
                continue;
            }
            if (is_space((unsigned char)text[i]) || text[i] == ':') break;
            i++;
        }
        if (i == start) { bytes_push(&out, (unsigned char)text[i]); i++; continue; }
        size_t len = i - start;
        size_t rlen = strlen(root);
        bool is_root = len == rlen && memcmp(text + start, root, rlen) == 0;
        bool under = len > rlen && memcmp(text + start, RESOURCE_VFS_ROOT "/", rlen + 1) == 0;
        if (is_root || under) {
            bytes_append(&out, physical, strlen(physical));
            bytes_append(&out, text + start + rlen, len - rlen);
        } else bytes_append(&out, text + start, len);
    }
    bytes_push(&out, 0);
    return (char *)out.data;
}

static bool resource_rename(const char *from, const char *to) { return move_path(from, to); }

static int resource_done(const char *work, int status) {
    remove_tree(work);
    return status;
}

/* Only this path opts compiler_command into VFS dependency support. Other
   compile/link staging callers retain physical FileEntry names for dep actions. */
int run_resource_compiler(Options o, Target t, Toolchain tc, Runtime rt) {
    Resource_Dependency_Plan p = resource_dependency_plan(o, tc);
    char *error = NULL;
    StrVec cmd = compiler_command(o, t, tc, rt, p.enabled, &error);
    if (error && *error) { fprintf(stderr, "cross_cc: error: %s\n", error); return 1; }
    if (!p.enabled) return run_command(cmd, o.verbose);
    char *parent = path_dir(p.path);
    char *temp_error = NULL;
    char *work = NULL;
    if (is_dir(parent)) work = make_temp_dir(parent, ".cross-cc-deps-", &temp_error);
    else temp_error = str_copy(strerror(ENOENT));
    if (!work || temp_error) { fprintf(stderr, "cross_cc: error: cannot stage dependency file %s: %s\n", p.path, nz(temp_error)); return 1; }
    char *staged = path_join(work, "clang.d");
    /* Override earlier -MF without changing target derivation or user operands.
       Insert before --, which terminates option parsing. */
    StrVec args = {0};
    bool inserted = false;
    for (size_t i = 0; i < cmd.len; i++) {
        if (!inserted && streq(cmd.data[i], "--")) { sv_push(&args, "-MF"); sv_push(&args, staged); inserted = true; }
        sv_push(&args, cmd.data[i]);
    }
    if (!inserted) { sv_push(&args, "-MF"); sv_push(&args, staged); }
    int status = run_command(args, o.verbose);
    errno = 0;
    size_t len = 0;
    char *data = read_file(staged, &len);
    if (!data) {
        /* Clang need not write a depfile on failure. Never inspect or delete a
           previous user output in that case. */
        if (status != 0) return resource_done(work, status);
        fprintf(stderr, "cross_cc: error: cannot read generated dependency file for %s: %s\n", p.path, strerror(errno ? errno : ENOENT));
        return resource_done(work, 1);
    }
    char *de = NULL;
    char *rewritten = resource_dependency_text(data, p.targets, tc.lib_dir, &de);
    if (de) { fprintf(stderr, "cross_cc: error: cannot restore dependency file %s: %s\n", p.path, de); return resource_done(work, 1); }
    char *ready = path_join(work, "physical.d");
    if (!write_file(ready, rewritten, strlen(rewritten))) { fprintf(stderr, "cross_cc: error: cannot write dependency file %s: %s\n", p.path, strerror(errno)); return resource_done(work, 1); }
    if (!resource_rename(ready, p.path)) { fprintf(stderr, "cross_cc: error: cannot publish dependency file %s: %s\n", p.path, strerror(errno)); return resource_done(work, 1); }
    return resource_done(work, status);
}
