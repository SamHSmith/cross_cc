/* Reproducible link staging: stable private stages, MSVC runtime aliases and canonical MinGW links. */
#include "cross.h"
#include "resources.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define det_mkdir(p) _wmkdir(windows_wide(p))
#else
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#define det_mkdir(p) mkdir(p, 0755)
#endif

static char *det_getcwd(void) {
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

static bool det_sep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static bool det_is_abs(const char *p) {
    p = nz(p);
#ifdef _WIN32
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && det_sep(p[2])) return true;
    return det_sep(p[0]) && det_sep(p[1]);
#else
    return p[0] == '/';
#endif
}

static bool det_exists(const char *p) { return p && *p && file_exists(p); }

static bool det_is_file(const char *p) { return p && *p && is_file(p); }

/* Volume prefix length ("C:" or "\\server\share" on Windows; none elsewhere). */
static size_t det_volume(const char *p) {
#ifdef _WIN32
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') return 2;
    if (det_sep(p[0]) && det_sep(p[1]) && p[2] && !det_sep(p[2])) {
        size_t i = 2;
        while (p[i] && !det_sep(p[i])) i++;
        if (p[i]) { i++; while (p[i] && !det_sep(p[i])) i++; }
        return i;
    }
#else
    (void)p;
#endif
    return 0;
}

char *path_clean(const char *path) {
    const char *p = nz(path);
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    size_t vol = det_volume(p);
    const char *rest = p + vol;
    bool rooted = det_sep(rest[0]);
    if (!*p) return str_copy(".");
    StrVec parts = {0};
    size_t i = 0;
    while (rest[i]) {
        while (rest[i] && det_sep(rest[i])) i++;
        size_t start = i;
        while (rest[i] && !det_sep(rest[i])) i++;
        size_t n = i - start;
        if (n == 0) continue;
        if (n == 1 && rest[start] == '.') continue;
        if (n == 2 && rest[start] == '.' && rest[start+1] == '.') {
            if (parts.len > 0 && !streq(parts.data[parts.len-1], "..")) { parts.len--; continue; }
            if (rooted) continue;
            sv_push(&parts, "..");
            continue;
        }
        sv_push(&parts, str_ncopy(rest + start, n));
    }
    Bytes out = {0};
    for (size_t k = 0; k < vol; k++) bytes_push(&out, (unsigned char)(det_sep(p[k]) ? sep : p[k]));
    if (rooted) bytes_push(&out, (unsigned char)sep);
    for (size_t k = 0; k < parts.len; k++) {
        if (k > 0) bytes_push(&out, (unsigned char)sep);
        bytes_append(&out, parts.data[k], strlen(parts.data[k]));
    }
    if (out.len == 0) bytes_push(&out, '.');
    bytes_push(&out, 0);
    return (char *)out.data;
}

static bool det_part_equal(const char *a, size_t an, const char *b, size_t bn) {
    if (an != bn) return false;
#ifdef _WIN32
    for (size_t i = 0; i < an; i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
#else
    return memcmp(a, b, an) == 0;
#endif
}

char *path_relative(const char *base_path, const char *target_path) {
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    char *base = path_clean(base_path), *target = path_clean(target_path);
    size_t bv = det_volume(base), tv = det_volume(target);
    if (!det_part_equal(base, bv, target, tv)) return NULL;
    const char *b = base + bv, *t = target + tv;
    if (det_part_equal(b, strlen(b), t, strlen(t))) return str_copy(".");
    if (streq(b, ".")) b = "";
    if (streq(t, ".")) t = "";
    if (det_sep(b[0]) != det_sep(t[0])) return NULL;
    size_t bl = strlen(b), tl = strlen(t);
    size_t b0 = 0, bi = 0, t0 = 0, ti = 0;
    for (;;) {
        while (bi < bl && !det_sep(b[bi])) bi++;
        while (ti < tl && !det_sep(t[ti])) ti++;
        if (!det_part_equal(b + b0, bi - b0, t + t0, ti - t0)) break;
        if (bi < bl) bi++;
        if (ti < tl) ti++;
        b0 = bi; t0 = ti;
    }
    if (bi - b0 == 2 && b[b0] == '.' && b[b0+1] == '.') return NULL;
    Bytes out = {0};
    if (b0 != bl) {
        size_t seps = 0;
        for (size_t k = b0; k < bl; k++) if (det_sep(b[k])) seps++;
        for (size_t k = 0; k <= seps; k++) {
            if (k) bytes_push(&out, (unsigned char)sep);
            bytes_append(&out, "..", 2);
        }
        if (t0 != tl) bytes_push(&out, (unsigned char)sep);
    }
    if (t0 < tl) bytes_append(&out, t + t0, tl - t0);
    if (out.len == 0) bytes_push(&out, '.');
    bytes_push(&out, 0);
    return (char *)out.data;
}

/* CodeView/PDB records include the linker input object names. Prefer a stable
   private stage for sequential builds of the same output without ever sharing
   its files with a concurrent invocation. mkdir is the exclusive claim: an
   occupied directory (including one left by a crashed build) is never reused
   or removed, and that invocation gets an ordinary unique stage instead.
   The caller owns and removes only the successfully created returned directory. */
char *deterministic_link_stage(const char *cache_dir, const char *prefix, const char *output, char **error) {
    if (error) *error = NULL;
    char *cwd = det_getcwd();
    if (!cwd) { if (error) *error = str_fmt("cannot get working directory: %s", strerror(errno)); return NULL; }
    /* The output need not exist yet: do not canonicalize it with realpath. */
    char *identity = str_fmt("%zu:%s%s", strlen(cwd), cwd, nz(output));
    char *stage = path_join(cache_dir, str_fmt("%s%016llx", nz(prefix), (unsigned long long)runtime_hash(identity)));
    if (det_mkdir(stage) == 0) return stage;
    return make_temp_dir(cache_dir, prefix, error);
}

typedef struct { StrVec keys, values; } Det_Map;

static long det_find(Det_Map *m, const char *key) {
    for (size_t i = 0; i < m->keys.len; i++) if (streq(m->keys.data[i], key)) return (long)i;
    return -1;
}

static const char *det_get(Det_Map *m, const char *key) {
    long i = det_find(m, key);
    return i < 0 ? NULL : m->values.data[i];
}

static void det_put(Det_Map *m, const char *key, const char *value) {
    long i = det_find(m, key);
    if (i >= 0) { m->values.data[i] = str_copy(value); return; }
    sv_push(&m->keys, key);
    sv_push(&m->values, value);
}

/* Runtime link flags are driver-owned by construction (never user operands).
   Stage their file inputs and matching autolink search directories: the PDB
   records both. Only default managed SDKs may stage search-only directories;
   explicit/system SDKs keep their physical paths and existing lookup behavior.
   Distinct directories retain distinct aliases and their original search order.
   If staging fails, retain the original flags as a unit. */
StrVec msvc_stage_runtime(StrVec flags, const char *work, bool managed_sdk) {
    /* LLD keeps -libpath spellings in S_ENVBLOCK. Relate owned inputs to the
       original cwd, never change the cwd used for user paths and directives. */
    char *cwd = det_getcwd();
    if (!cwd) return sv_copy(flags);
    char *relative_work = path_relative(cwd, absolute_path(work));
    if (!relative_work) return sv_copy(flags);
    StrVec result = {0};
    Det_Map directories = {0}, aliases = {0};
    StrVec sdk_directories = {0};
    for (size_t i = 0; i < flags.len; i++) {
        const char *a = flags.data[i];
        if (starts_with(a, "-")) {
            sv_push(&result, a);
            if (takes_value(a) && i+1 < flags.len) { i++; sv_push(&result, flags.data[i]); }
            continue;
        }
        if (!det_is_file(a)) { sv_push(&result, a); continue; }
        char *dir = path_dir(a);
        const char *alias = det_get(&directories, dir);
        if (!alias) {
            char *name = str_fmt("runtime-%zu", directories.keys.len);
            alias = path_join(work, name);
            if (det_mkdir(alias) != 0) return sv_copy(flags);
            det_put(&directories, dir, alias);
            det_put(&aliases, dir, path_join(relative_work, name));
        }
        char *dest = path_join(alias, path_base(a));
        if (!det_exists(dest) && !link_file(a, dest) && !copy_file(dest, a)) return sv_copy(flags);
        sv_push(&result, path_join(det_get(&aliases, dir), path_base(a)));
    }
    const char *prefix = "-Wl,-libpath:";
    for (size_t i = 0; i < result.len; i++) {
        const char *a = result.data[i];
        if (!starts_with(a, prefix)) {
            if (takes_value(a) && i+1 < result.len) i++;
            continue;
        }
        const char *dir = a + strlen(prefix);
        const char *alias = det_get(&directories, dir);
        if (!alias) {
            if (!managed_sdk) continue;
            char *name = str_fmt("runtime-%zu", directories.keys.len);
            alias = path_join(work, name);
            if (det_mkdir(alias) != 0) return sv_copy(flags);
            det_put(&directories, dir, alias);
            det_put(&aliases, dir, path_join(relative_work, name));
            sv_push(&sdk_directories, dir);
        }
        bool sdk = false;
        for (size_t k = 0; k < sdk_directories.len; k++) if (streq(sdk_directories.data[k], dir)) sdk = true;
        if (!msvc_stage_libraries(dir, alias, sdk, 0)) return sv_copy(flags);
        result.data[i] = str_fmt("%s%s", prefix, det_get(&aliases, dir));
    }
    return result;
}

typedef enum { DET_REGULAR, DET_DIRECTORY, DET_SYMLINK, DET_OTHER } Det_Kind;

static Det_Kind det_kind(const char *path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesW(windows_wide(path));
    if (attr == INVALID_FILE_ATTRIBUTES) return DET_OTHER;
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return DET_SYMLINK;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) return DET_DIRECTORY;
    if (attr & FILE_ATTRIBUTE_DEVICE) return DET_OTHER;
    return DET_REGULAR;
#else
    struct stat st;
    if (lstat(path, &st) != 0) return DET_OTHER;
    if (S_ISLNK(st.st_mode)) return DET_SYMLINK;
    if ((st.st_mode & S_IFMT) == S_IFDIR) return DET_DIRECTORY;
    if ((st.st_mode & S_IFMT) == S_IFREG) return DET_REGULAR;
    return DET_OTHER;
#endif
}

/* Directory entries (without . and ..), NULL data with ok=false on failure. */
static StrVec det_read_dir(const char *dir, bool *ok) {
    StrVec names = {0};
    *ok = false;
#ifdef _WIN32
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(windows_wide(path_join(dir, "*")), &data);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) *ok = true;
        return names;
    }
    do {
        char *name = windows_utf8(data.cFileName);
        if (streq(name, ".") || streq(name, "..")) continue;
        sv_push(&names, name);
    } while (FindNextFileW(h, &data));
    FindClose(h);
    *ok = true;
    return names;
#else
    DIR *d = opendir(dir);
    if (!d) return names;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (streq(e->d_name, ".") || streq(e->d_name, "..")) continue;
        sv_push(&names, str_copy(e->d_name));
    }
    closedir(d);
    *ok = true;
    return names;
#endif
}

/* Autolink may name libcpmt, alternate CRTs, or a nested SDK archive. Only the
   three validated managed library trees may recurse; runtime directories keep
   their direct-file policy. No headers, PDBs or arbitrary SDK files are copied.
   Hardlinks never modify source bytes; copying handles different filesystems. */
bool msvc_stage_libraries(const char *dir, const char *alias, bool managed_sdk, int depth) {
    if (depth > 32) return false;
    bool ok;
    StrVec files = det_read_dir(dir, &ok);
    if (!ok) return false;
    for (size_t f = 0; f < files.len; f++) {
        const char *file = files.data[f];
        char *source = path_join(dir, file), *dest = path_join(alias, file);
        Det_Kind kind = det_kind(source);
        if (kind == DET_DIRECTORY) {
            if (!managed_sdk) continue;
            if (!det_exists(dest) && det_mkdir(dest) != 0) return false;
            if (!msvc_stage_libraries(source, dest, true, depth+1)) return false;
            continue;
        }
#ifndef _WIN32
        if (kind == DET_SYMLINK && managed_sdk) {
            /* xwin's file case aliases point to a regular sibling. Resolve only
               this bounded form: no directory links, cycles, or root escapes. */
            char target[4096];
            ssize_t n = readlink(source, target, sizeof target - 1);
            if (n < 0) return false;
            target[n] = 0;
            if (!msvc_safe_relative(target) || !streq(path_base(target), target)) return false;
            source = path_join(dir, target);
            if (det_kind(source) != DET_REGULAR) return false;
            kind = DET_REGULAR;
        }
#endif
        if (kind != DET_REGULAR) {
            if (managed_sdk) return false;
            continue;
        }
        char *ext = path_ext(file);
        if (managed_sdk) ext = str_lower(ext);
        if (!streq(ext, ".a") && !streq(ext, ".lib") && !streq(ext, ".dll") && !(managed_sdk && (streq(ext, ".obj") || streq(ext, ".o")))) continue;
        if (!det_exists(dest) && !link_file(source, dest) && !copy_file(dest, source)) return false;
    }
    return true;
}

/* Normalize driver-managed PDB links, including user source debug information.
   Compilation stays in the caller's cwd; only the linker sees staged paths.
   Custom linker arguments retain their cwd, output and side-file semantics. */
bool canonical_mingw_link(Options o) {
    if (o.relocatable || !emits_debug_info(o, true)) return false;
    for (size_t i = 0; i < o.forwarded.len; i++) {
        const char *a = o.forwarded.data[i];
        if (streq(a, "--")) break;
        if (streq(a, "-Xlinker") || streq(a, "-Wl") || starts_with(a, "-Wl,")) return false;
        /* Only the driver's implicit object name may be canonicalized. Preserve
           explicit cc1 object-file-name requests, joined or separate, verbatim. */
        if (streq(a, "-Xclang") && i+1 < o.forwarded.len && (streq(o.forwarded.data[i+1], "-object-file-name") || starts_with(o.forwarded.data[i+1], "-object-file-name="))) return false;
        if (starts_with(a, "-Xclang=-object-file-name")) return false;
        if (takes_value(a)) i++;
    }
    return true;
}

uint32_t coff_u32(const unsigned char *b, size_t at) {
    return (uint32_t)b[at] | (uint32_t)b[at+1] << 8 | (uint32_t)b[at+2] << 16 | (uint32_t)b[at+3] << 24;
}

/* LLD normally resolves relative CodeView source names against the caller cwd.
   The canonical child's -pdbsourcepath:. must not change that meaning. Inspect
   C13 file-checksum references, never rewrite user debug paths. Unknown or
   malformed metadata conservatively keeps the ordinary linker invocation. */
bool canonical_mingw_debug_paths(const unsigned char *data, size_t len) {
    if (len < 4 || coff_u32(data, 0) != 4) return false;
    const unsigned char *table = NULL;
    size_t table_len = 0;
    for (size_t at = 4; at < len;) {
        if (len - at < 8) return false;
        uint32_t kind = coff_u32(data, at);
        size_t size = coff_u32(data, at+4);
        if (size > len - at - 8) return false;
        if (kind == 0xf3) {
            if (table) return false;
            table = data + at + 8; table_len = size;
        }
        at += 8 + ((size + 3) & ~(size_t)3);
        if (at > len) return false;
    }
    for (size_t at = 4; at < len;) {
        uint32_t kind = coff_u32(data, at);
        size_t size = coff_u32(data, at+4);
        if (kind == 0xf4) {
            const unsigned char *checksums = data + at + 8;
            for (size_t pos = 0; pos < size;) {
                if (size - pos < 6) return false;
                size_t name = coff_u32(checksums, pos);
                if (name >= table_len) return false;
                size_t end = name;
                while (end < table_len && table[end] != 0) end++;
                if (end == table_len || end == name) return false;
                const unsigned char *path = table + name;
                size_t plen = end - name;
                bool drive = plen >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
                bool unc = plen >= 3 && path[0] == '\\' && path[1] == '\\';
                if (path[0] != '/' && !drive && !unc) return false;
                pos += (6 + (size_t)checksums[pos+4] + 3) & ~(size_t)3;
                if (pos > size) return false;
            }
        }
        at += 8 + ((size + 3) & ~(size_t)3);
    }
    return true;
}

static bool det_read_at(FILE *f, void *buf, size_t n, long long offset) {
    if (offset < 0) return false;
#ifdef _WIN32
    if (_fseeki64(f, offset, SEEK_SET) != 0) return false;
#else
    if (fseeko(f, (off_t)offset, SEEK_SET) != 0) return false;
#endif
    return fread(buf, 1, n, f) == n;
}

/* Byte width of a Unicode white-space rune at text[i] (field splitting), 0 otherwise. */
static size_t det_unicode_space(const char *text, size_t i, size_t n) {
    const unsigned char *s = (const unsigned char *)text + i;
    size_t left = n - i;
    if (s[0] == ' ' || (s[0] >= '\t' && s[0] <= '\r')) return 1;
    if (left >= 2 && s[0] == 0xC2 && (s[1] == 0x85 || s[1] == 0xA0)) return 2;
    if (left >= 3 && s[0] == 0xE1 && s[1] == 0x9A && s[2] == 0x80) return 3;
    if (left >= 3 && s[0] == 0xE2 && s[1] == 0x80 && ((s[2] >= 0x80 && s[2] <= 0x8A) || s[2] == 0xA8 || s[2] == 0xA9 || s[2] == 0xAF)) return 3;
    if (left >= 3 && s[0] == 0xE2 && s[1] == 0x81 && s[2] == 0x9F) return 3;
    if (left >= 3 && s[0] == 0xE3 && s[1] == 0x80 && s[2] == 0x80) return 3;
    return 0;
}

/* Linker directives that cannot name or search for a file. Anything else
   (/defaultlib, /manifestdependency, quoted values, unknown options) keeps the
   original cwd. C++ objects routinely carry -exclude-symbols for the clang
   terminate helper, so a blanket .drectve refusal made them unreproducible. */
static bool canonical_mingw_directives(FILE *file, const unsigned char *sections, size_t index) {
    int64_t size = (int64_t)coff_u32(sections, index*40+16);
    long long offset = coff_u32(sections, index*40+20);
    if (size <= 0 || size > 64*1024) return false;
    unsigned char *data = xmalloc((size_t)size + 1);
    bool ok = det_read_at(file, data, (size_t)size, offset);
    if (!ok) { free(data); return false; }
    for (int64_t i = 0; i < size; i++) if (data[i] == '"' || data[i] == 0) { free(data); return false; }
    data[size] = 0;
    bool seen = false;
    const char *text = (const char *)data;
    size_t i = 0, n = (size_t)size;
    while (i < n) {
        size_t width;
        while (i < n && (width = det_unicode_space(text, i, n)) > 0) i += width;
        if (i >= n) break;
        size_t start = i;
        while (i < n && det_unicode_space(text, i, n) == 0) i++;
        const char *token = text + start;
        size_t tlen = i - start;
        if (tlen < 2 || (token[0] != '-' && token[0] != '/')) { free(data); return false; }
        size_t colon = 0;
        while (colon < tlen && token[colon] != ':') colon++;
        if (colon == tlen) { free(data); return false; }
        char *key = str_lower(str_ncopy(token + 1, colon - 1));
        if (streq(key, "exclude-symbols") || streq(key, "include") || streq(key, "alternatename") || streq(key, "export") ||
            streq(key, "merge") || streq(key, "failifmismatch") || streq(key, "entry")) seen = true;
        else { free(data); return false; }
    }
    free(data);
    return seen;
}

/* Moving LLD changes caller-cwd lookup of COFF directives and relative source
   filenames. Only ordinary objects without directives and with absolute C13
   source paths are proven safe. Read headers and bounded debug metadata only;
   archives, bitcode, bigobj and unknown inputs retain their original cwd. */
bool canonical_mingw_object(const char *path) {
    FILE *file = open_file(path, "rb");
    if (!file) return false;
    bool result = false;
    unsigned char header[20];
    unsigned char *sections = NULL;
    if (!det_read_at(file, header, sizeof header, 0)) goto done;
    unsigned machine = (unsigned)header[0] | (unsigned)header[1] << 8;
    if (machine != 0x014c && machine != 0x8664 && machine != 0x01c4 && machine != 0xaa64) goto done;
    size_t count = (size_t)header[2] | (size_t)header[3] << 8;
    size_t optional_size = (size_t)header[16] | (size_t)header[17] << 8;
    sections = xmalloc(count * 40 + 1);
    if (count > 0 && !det_read_at(file, sections, count * 40, (long long)(20 + optional_size))) goto done;
    for (size_t i = 0; i < count; i++) {
        unsigned char *name = sections + i*40;
        if (memcmp(name, ".drectve", 8) == 0 && !canonical_mingw_directives(file, sections, i)) goto done;
        if (name[0] == '/') {
            /* A producer may use a string-table name even for .drectve.
               Read only its first eight bytes. Unsupported name forms fail
               closed rather than accidentally accepting an unseen directive. */
            long long offset = 0;
            for (size_t k = 1; k < 8; k++) {
                unsigned char digit = name[k];
                if (digit == 0) break;
                if (digit < '0' || digit > '9') goto done;
                offset = offset*10 + (long long)(digit - '0');
            }
            if (offset < 4) goto done;
            long long symbols = 0, symbol_count = 0;
            for (int j = 0; j < 4; j++) {
                symbols |= (long long)header[8+j] << (j*8);
                symbol_count |= (long long)header[12+j] << (j*8);
            }
            unsigned char resolved[8];
            if (!det_read_at(file, resolved, sizeof resolved, symbols + symbol_count*18 + offset)) goto done;
            memcpy(name, resolved, 8);
            if (memcmp(name, ".drectve", 8) == 0 && !canonical_mingw_directives(file, sections, i)) goto done;
        }
        if (memcmp(name, ".debug$S", 8) == 0) {
            int64_t size = (int64_t)coff_u32(sections, i*40+16);
            long long offset = coff_u32(sections, i*40+20);
            /* A larger or non-C13 producer still links normally, without moving
               its cwd. Eligibility never requires reading unbounded input data. */
            if (size > 16*1024*1024) goto done;
            if (size < 0) goto done;
            unsigned char *debug = xmalloc((size_t)size + 1);
            bool valid = (size == 0 || det_read_at(file, debug, (size_t)size, offset)) && canonical_mingw_debug_paths(debug, (size_t)size);
            free(debug);
            if (!valid) goto done;
        }
    }
    result = true;
done:
    free(sections);
    fclose(file);
    return result;
}

bool canonical_mingw_inputs(StrVec link) {
    for (size_t i = 1; i < link.len; i++) {
        const char *a = link.data[i];
        /* A named library may load archive members with further directives.
           Scripts can also refer to files relative to the original cwd. Leave
           their complete lookup semantics to LLD rather than guessing/copying. */
        if (streq(a, "-T") || starts_with(a, "-l")) return false;
        if (streq(a, "-o") || streq(a, "--out-implib") || streq(a, "-L") || streq(a, "-e") || streq(a, "-u") || streq(a, "-m") || streq(a, "--stack")) {
            i++;
        } else if (!starts_with(a, "-") && !canonical_mingw_object(a)) return false;
    }
    return true;
}

char *canonical_mingw_relative(const char *work, const char *path) {
    char *absolute = str_copy(path);
    if (!det_is_abs(path)) {
        char *cwd = det_getcwd();
        if (!cwd) fatal("cannot resolve MinGW input directory");
        absolute = path_join(cwd, path);
    }
    /* Lexical paths also preserve nonexistent -L directories (LLD ignores them). */
    char *relative = path_relative(work, absolute);
    /* Different Windows drives cannot be related. Retaining the absolute path
       remains correct; reproducibility there requires a common cache volume. */
    return relative ? canonical_mingw_path(relative) : absolute;
}

/* Only generated filesystem paths use this conversion; arbitrary user option
   values and cross-drive absolute fallbacks retain their original spelling. */
char *canonical_mingw_path(const char *path) {
#ifdef _WIN32
    return str_replace(path, "\\", "/");
#else
    return str_copy(path);
#endif
}

/* The boundary is recorded before appending rt.link_flags and generated import
   search paths. Never discover ownership from a path prefix: user paths may
   happen to point into the cache too. Each owned directory keeps a distinct,
   ordered alias so search precedence and duplicate library names are unchanged. */
StrVec canonical_mingw_runtime(StrVec link, const char *work, size_t managed_start, char **error) {
    StrVec empty = {0};
    if (error) *error = NULL;
    StrVec result = sv_copy(link);
    Det_Map directories = {0};
    StrVec copied = {0};
    for (size_t i = managed_start; i < result.len; i++) {
        const char *a = result.data[i];
        bool search = streq(a, "-L");
        if (search) {
            i++;
            if (i >= result.len) { if (error) *error = "missing owned MinGW library directory"; return empty; }
            a = result.data[i];
        } else if (starts_with(a, "-")) continue;
        char *dir = absolute_path(search ? a : path_dir(a));
        const char *alias = det_get(&directories, dir);
        if (!alias) {
            alias = path_join(work, str_fmt("runtime-%zu", directories.keys.len));
            if (det_mkdir(alias) != 0) { if (error) *error = str_fmt("cannot create MinGW runtime alias: %s", strerror(errno)); return empty; }
            det_put(&directories, dir, alias);
        }
        StrVec sources = {0};
        if (search) {
            /* Only direct linker-consumable files from an explicitly owned
               runtime search directory, never a recursive or user-dir copy. */
            bool ok;
            StrVec files = det_read_dir(dir, &ok);
            if (!ok) { if (error) *error = str_fmt("cannot read MinGW runtime directory %s: %s", dir, strerror(errno)); return empty; }
            for (size_t f = 0; f < files.len; f++) {
                char *ext = path_ext(files.data[f]);
                char *full = path_join(dir, files.data[f]);
                if (det_kind(full) == DET_REGULAR && (streq(ext, ".a") || streq(ext, ".lib") || streq(ext, ".dll"))) sv_push(&sources, full);
            }
            result.data[i] = str_copy(alias);
        } else {
            sv_push(&sources, a);
            result.data[i] = path_join(alias, path_base(a));
        }
        for (size_t s = 0; s < sources.len; s++) {
            char *dest = path_join(alias, path_base(sources.data[s]));
            bool done = false;
            for (size_t k = 0; k < copied.len; k++) if (streq(copied.data[k], dest)) done = true;
            if (done) continue;
            if (!copy_file(dest, sources.data[s])) { if (error) *error = str_fmt("cannot stage MinGW runtime %s: %s", sources.data[s], strerror(errno)); return empty; }
            sv_push(&copied, dest);
        }
    }
    return result;
}

StrVec canonical_mingw_arguments(StrVec link, const char *work, StrVec *outputs, StrVec *destinations, char **error) {
    StrVec args = {0}, empty = {0};
    if (error) *error = NULL;
    outputs->len = 0; destinations->len = 0;
    char *cwd = det_getcwd();
    if (!cwd) { if (error) *error = "cannot resolve MinGW output directory"; return empty; }
    char *exe = executable_path();
    if (!exe || !*exe) { if (error) *error = "cannot locate MinGW linker child"; return empty; }
    sv_push(&args, exe);
    sv_push(&args, "-cross-cc-mingw-link");
    for (size_t i = 1; i < link.len; i++) {
        const char *a = link.data[i];
        const char *output = "", *prefix = "";
        if (streq(a, "-o") || streq(a, "--out-implib")) {
            if (i+1 >= link.len) { if (error) *error = "missing MinGW output operand"; return empty; }
            sv_push(&args, a);
            i++; output = link.data[i];
        } else if (starts_with(a, "--pdb=")) {
            prefix = "--pdb="; output = a + strlen("--pdb=");
        }
        if (*output) {
            /* Separate directories avoid collisions even when the requested
               image has a .pdb/.lib suffix. Only our own outputs are published. */
            char *subdir = str_fmt("output-%zu", outputs->len);
            if (det_mkdir(path_join(work, subdir)) != 0) { if (error) *error = str_fmt("cannot stage MinGW output: %s", strerror(errno)); return empty; }
            char *staged = canonical_mingw_path(path_join(subdir, path_base(output)));
            sv_push(outputs, path_join(work, staged));
            sv_push(destinations, det_is_abs(output) ? output : path_join(cwd, output));
            sv_push(&args, str_fmt("%s%s", prefix, staged));
            continue;
        }
        /* These are the separate non-path option values emitted by our driver.
           In particular a symbol/library name is never a file input, even if
           there is a file of that name in the caller's directory. */
        if (streq(a, "-e") || streq(a, "-u") || streq(a, "-l") || streq(a, "-m") || streq(a, "--stack")) {
            if (i+1 >= link.len) { if (error) *error = "missing MinGW option operand"; return empty; }
            sv_push(&args, a); sv_push(&args, link.data[i+1]); i++;
        } else if (streq(a, "-L") || streq(a, "-T")) {
            if (i+1 >= link.len) { if (error) *error = "missing MinGW path operand"; return empty; }
            i++;
            sv_push(&args, a);
            sv_push(&args, canonical_mingw_relative(work, link.data[i]));
        } else if (starts_with(a, "-L") && strlen(a) > 2) {
            sv_push(&args, str_fmt("-L%s", canonical_mingw_relative(work, a + 2)));
        } else if (!starts_with(a, "-")) {
            sv_push(&args, canonical_mingw_relative(work, a));
        } else sv_push(&args, a);
    }
    sv_push(&args, "-Xlink=-pdbsourcepath:.");
    return args;
}

#ifdef _WIN32
/* MSVCRT argument quoting for CreateProcess command lines. */
static void det_quote_arg(Bytes *out, const char *a) {
    if (*a && !strpbrk(a, " \t\n\v\"")) { bytes_append(out, a, strlen(a)); return; }
    bytes_push(out, '"');
    for (const char *p = a;; p++) {
        size_t slashes = 0;
        while (*p == '\\') { slashes++; p++; }
        if (!*p) { for (size_t k = 0; k < slashes * 2; k++) bytes_push(out, '\\'); break; }
        if (*p == '"') { for (size_t k = 0; k < slashes * 2 + 1; k++) bytes_push(out, '\\'); }
        else { for (size_t k = 0; k < slashes; k++) bytes_push(out, '\\'); }
        bytes_push(out, (unsigned char)*p);
    }
    bytes_push(out, '"');
}
#endif

/* Run args with the given working directory, inheriting stdio. *started is false when it cannot start. */
static int det_run_in(StrVec args, const char *dir, bool *started, char **error) {
    *started = false;
    if (error) *error = NULL;
#ifdef _WIN32
    Bytes line = {0};
    for (size_t i = 0; i < args.len; i++) { if (i) bytes_push(&line, ' '); det_quote_arg(&line, args.data[i]); }
    bytes_push(&line, 0);
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(windows_wide(args.data[0]), windows_wide((char *)line.data), NULL, NULL, TRUE, 0, NULL, windows_wide(dir), &si, &pi)) {
        if (error) *error = str_fmt("error %lu", (unsigned long)GetLastError());
        return 127;
    }
    *started = true;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    if (!GetExitCodeProcess(pi.hProcess, &code)) { if (error) *error = str_fmt("error %lu", (unsigned long)GetLastError()); code = 1; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)code;
#else
    char **argv = xmalloc(sizeof(char *) * (args.len + 1));
    for (size_t i = 0; i < args.len; i++) argv[i] = args.data[i];
    argv[args.len] = NULL;
    int pipefd[2];
    if (pipe(pipefd) != 0) { if (error) *error = str_copy(strerror(errno)); return 127; }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { if (error) *error = str_copy(strerror(errno)); close(pipefd[0]); close(pipefd[1]); return 127; }
    if (pid == 0) {
        close(pipefd[0]);
        int err = 0;
        if (chdir(dir) != 0) err = errno;
        else { execv(argv[0], argv); err = errno; }
        ssize_t w = write(pipefd[1], &err, sizeof err);
        (void)w;
        _exit(127);
    }
    close(pipefd[1]);
    int child_err = 0;
    ssize_t got;
    do got = read(pipefd[0], &child_err, sizeof child_err); while (got < 0 && errno == EINTR);
    close(pipefd[0]);
    int status = 0;
    pid_t w;
    do w = waitpid(pid, &status, 0); while (w < 0 && errno == EINTR);
    if (got == (ssize_t)sizeof child_err) { if (error) *error = str_copy(strerror(child_err)); return 127; }
    *started = true;
    if (w < 0) { if (error) *error = str_copy(strerror(errno)); return 1; }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
#endif
}

int run_canonical_mingw_link(StrVec link, const char *stage, size_t managed_start, bool verbose) {
    char *work = absolute_path(stage);
    char *runtime_err = NULL;
    StrVec owned_link = canonical_mingw_runtime(link, work, managed_start, &runtime_err);
    if (runtime_err) { fprintf(stderr, "%s\n", runtime_err); return 1; }
    StrVec outputs = {0}, destinations = {0};
    char *args_err = NULL;
    StrVec args = canonical_mingw_arguments(owned_link, work, &outputs, &destinations, &args_err);
    if (args_err) { fprintf(stderr, "%s\n", args_err); return 1; }
    if (verbose) print_command(args);
    bool started = false;
    char *run_err = NULL;
    int code = det_run_in(args, work, &started, &run_err);
    if (!started) { fprintf(stderr, "cannot execute MinGW linker: %s\n", nz(run_err)); return 127; }
    if (run_err) { fprintf(stderr, "MinGW linker wait failed: %s\n", run_err); return 1; }
    if (code != 0) return code;
    for (size_t i = 0; i < outputs.len; i++) {
        /* LLD deliberately omits an import library when there are no exports. */
        if (!det_exists(outputs.data[i])) continue;
        if (!copy_file(destinations.data[i], outputs.data[i])) {
            fprintf(stderr, "cannot publish MinGW output %s: %s\n", destinations.data[i], strerror(errno));
            return 1;
        }
    }
    return 0;
}
