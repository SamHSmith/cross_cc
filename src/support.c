#include "cross.h"
#include <limits.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

const char *nz(const char *s)
{
    return s ? s : "";
}
bool streq(const char *a, const char *b)
{
    return strcmp(nz(a), nz(b)) == 0;
}
bool starts_with(const char *s, const char *p)
{
    return strncmp(nz(s), nz(p), strlen(nz(p))) == 0;
}
bool ends_with(const char *s, const char *p)
{
    size_t n = strlen(nz(s)), m = strlen(nz(p));
    return n >= m && !strcmp(nz(s) + n - m, nz(p));
}
bool contains(const char *s, const char *p)
{
    return strstr(nz(s), nz(p)) != NULL;
}
void *xmalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p)
        fatal("out of memory");
    return p;
}
void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        fatal("out of memory");
    return p;
}
char *str_ncopy(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    return p;
}
char *str_copy(const char *s)
{
    return str_ncopy(nz(s), strlen(nz(s)));
}
char *str_fmt(const char *fmt, ...)
{
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    int n = vsnprintf(NULL, 0, fmt, a);
    va_end(a);
    if (n < 0)
        fatal("cannot format string");
    char *s = xmalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, b);
    va_end(b);
    return s;
}
char *str_lower(const char *s)
{
    char *r = str_copy(s);
    for (char *p = r; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    return r;
}
char *str_trim(const char *s)
{
    s = nz(s);
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        n--;
    return str_ncopy(s, n);
}
char *str_replace(const char *s, const char *old, const char *replacement)
{
    s = nz(s);
    old = nz(old);
    replacement = nz(replacement);
    if (!*old)
        return str_copy(s);
    size_t count = 0, n = strlen(old), m = strlen(replacement);
    for (const char *p = s; (p = strstr(p, old)); p += n)
        count++;
    char *out = xmalloc(strlen(s) + count * m - count * n + 1), *w = out;
    const char *p = s, *q;
    while ((q = strstr(p, old)))
    {
        memcpy(w, p, (size_t)(q - p));
        w += q - p;
        memcpy(w, replacement, m);
        w += m;
        p = q + n;
    }
    strcpy(w, p);
    return out;
}
void sv_push(StrVec *v, const char *s)
{
    if (v->len == v->cap)
    {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->data = xrealloc(v->data, (v->cap + 1) * sizeof(char *));
    }
    v->data[v->len++] = (char *)nz(s);
    v->data[v->len] = NULL;
}
void sv_extend(StrVec *v, StrVec b)
{
    for (size_t i = 0; i < b.len; i++)
        sv_push(v, b.data[i]);
}
StrVec sv_copy(StrVec v)
{
    StrVec r = {0};
    sv_extend(&r, v);
    return r;
}
StrVec sv_slice(StrVec v, size_t b, size_t e)
{
    StrVec r = {0};
    if (e > v.len)
        e = v.len;
    for (size_t i = b; i < e; i++)
        sv_push(&r, v.data[i]);
    return r;
}
StrVec sv_make(size_t count, ...)
{
    StrVec v = {0};
    va_list a;
    va_start(a, count);
    for (size_t i = 0; i < count; i++)
        sv_push(&v, va_arg(a, const char *));
    va_end(a);
    return v;
}
StrVec str_split(const char *s, const char *d)
{
    StrVec v = {0};
    s = nz(s);
    size_t n = strlen(d);
    if (!n)
    {
        sv_push(&v, s);
        return v;
    }
    const char *p;
    while ((p = strstr(s, d)))
    {
        sv_push(&v, str_ncopy(s, (size_t)(p - s)));
        s = p + n;
    }
    sv_push(&v, s);
    return v;
}
_Noreturn void fatal(const char *s)
{
    fprintf(stderr, "cross_cc: error: %s\n", nz(s));
    exit(1);
}
#ifdef _WIN32
wchar_t *windows_wide(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, nz(utf8), -1, NULL, 0);
    if (!n)
        fatal("invalid UTF-8 string");
    wchar_t *wide = xmalloc((size_t)n * sizeof(wchar_t));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, nz(utf8), -1, wide, n))
        fatal("cannot convert UTF-8 string");
    return wide;
}
char *windows_utf8(const wchar_t *wide)
{
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    if (!n)
        fatal("invalid UTF-16 string");
    char *utf8 = xmalloc((size_t)n);
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, utf8, n, NULL, NULL))
        fatal("cannot convert UTF-16 string");
    return utf8;
}
#endif
char *env(const char *s)
{
#ifdef _WIN32
    wchar_t *name = windows_wide(s);
    DWORD n = GetEnvironmentVariableW(name, NULL, 0);
    if (!n)
        return "";
    wchar_t *value = xmalloc((size_t)n * sizeof(wchar_t));
    if (!GetEnvironmentVariableW(name, value, n))
        return "";
    return windows_utf8(value);
#else
    return (char *)nz(getenv(s));
#endif
}
bool set_env(const char *name, const char *value)
{
#ifdef _WIN32
    return _wputenv_s(windows_wide(name), windows_wide(value)) == 0;
#else
    return setenv(name, nz(value), 1) == 0;
#endif
}
static bool path_sep(char c)
{
    return c == '/'
#ifdef _WIN32
           || c == '\\'
#endif
        ;
}
#ifndef _WIN32
static bool path_absolute(const char *p)
{
    return path_sep(*p);
}
#endif
char *path_join_n(size_t count, ...)
{
    size_t cap = 1, len = 0;
    char *r = xmalloc(cap);
    va_list a;
    va_start(a, count);
    for (size_t i = 0; i < count; i++)
    {
        const char *p = nz(va_arg(a, const char *));
        if (!*p)
            continue;
        size_t n = strlen(p);
        cap = len + n + 3;
        r = xrealloc(r, cap);
        if (len && !path_sep(r[len - 1]) && !path_sep(*p))
            r[len++] = '/';
        while (len && path_sep(r[len - 1]) && path_sep(*p))
            p++, n--;
        memcpy(r + len, p, n + 1);
        len += n;
    }
    va_end(a);
    return r;
}
char *path_base(const char *p)
{
    p = nz(p);
    size_t end = strlen(p);
    if (!end)
        return str_copy(".");
    while (end > 1 && path_sep(p[end - 1]))
        end--;
    size_t start = end;
    while (start > 0 && !path_sep(p[start - 1]))
        start--;
    if (start == end)
        return str_ncopy(p, 1);
    return str_ncopy(p + start, end - start);
}
char *path_dir(const char *p)
{
    p = nz(p);
    const char *s = NULL;
    for (const char *q = p; *q; q++)
        if (path_sep(*q))
            s = q;
    if (!s)
        return str_copy(".");
    if (s == p)
        return str_ncopy(p, 1);
    return str_ncopy(p, (size_t)(s - p));
}
char *path_ext(const char *p)
{
    char *b = path_base(p), *dot = strrchr(b, '.');
    return dot ? dot : (char *)"";
}
char *absolute_path(const char *p)
{
#ifdef _WIN32
    wchar_t *input = windows_wide(p);
    DWORD n = GetFullPathNameW(input, 0, NULL, NULL);
    if (!n)
        fatal(str_fmt("cannot resolve path %s", nz(p)));
    wchar_t *wide = xmalloc(((size_t)n + 1) * sizeof(wchar_t));
    DWORD written = GetFullPathNameW(input, n + 1, wide, NULL);
    if (!written || written > n)
        fatal(str_fmt("cannot resolve path %s", nz(p)));
    return windows_utf8(wide);
#else
    /* Existing paths use their physical spelling, including symlinks before '..'.
       Output paths and cache directories may not exist yet; keep their lexical
       fallback rather than requiring the caller to create them first. */
    char *resolved = realpath(nz(p), NULL);
    if (resolved)
        return resolved;
    if (path_absolute(nz(p)))
        return str_copy(p);
    size_t cap = 256;
    char *cwd;
    for (;;)
    {
        cwd = xmalloc(cap);
        if (getcwd(cwd, cap))
            break;
        free(cwd);
        if (errno != ERANGE)
            fatal("cannot get current directory");
        cap *= 2;
    }
    return path_join(cwd, p);
#endif
}
char *absolute_directory_path(const char *p)
{
    char *a = absolute_path(p);
    StrVec parts = {0};
    size_t prefix = 0;
    bool unc = false;
#ifdef _WIN32
    for (char *q = a; *q; q++)
        if (*q == '\\')
            *q = '/';
    if (a[1] == ':')
    {
        a[0] = (char)toupper((unsigned char)a[0]);
        prefix = 2;
    }
    else if (a[0] == '/' && a[1] == '/')
    {
        unc = true;
        prefix = 2;
    }
#endif
    bool root = path_sep(a[prefix]);
    char *s = a + prefix;
    while (*s)
    {
        while (path_sep(*s))
            s++;
        char *e = s;
        while (*e && !path_sep(*e))
            e++;
        if (e == s)
            break;
        char *part = str_ncopy(s, (size_t)(e - s));
        if (streq(part, ".."))
        {
            if (parts.len > (unc ? 2u : 0u))
                parts.len--;
        }
        else if (!streq(part, "."))
            sv_push(&parts, part);
        s = e;
    }
    char *r = prefix ? str_ncopy(a, prefix) : str_copy("");
    if (root)
        r = str_fmt("%s/", r);
    for (size_t i = 0; i < parts.len; i++)
        r = path_join(r, parts.data[i]);
    return r;
}
bool file_exists(const char *p)
{
    if (!p || !*p)
        return false;
#ifdef _WIN32
    return GetFileAttributesW(windows_wide(p)) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p, &st) == 0;
#endif
}
bool is_dir(const char *p)
{
    if (!p || !*p)
        return false;
#ifdef _WIN32
    DWORD attr = GetFileAttributesW(windows_wide(p));
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}
FILE *open_file(const char *p, const char *mode)
{
#ifdef _WIN32
    return _wfopen(windows_wide(p), windows_wide(mode));
#else
    return fopen(p, mode);
#endif
}
char *read_file(const char *p, size_t *size)
{
    FILE *f = open_file(p, "rb");
    if (!f)
        return NULL;
    size_t n = 0, cap = 8192;
    char *data = xmalloc(cap + 1);
    for (;;)
    {
        if (n == cap)
        {
            cap *= 2;
            data = xrealloc(data, cap + 1);
        }
        size_t got = fread(data + n, 1, cap - n, f);
        n += got;
        if (!got)
            break;
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok)
    {
        free(data);
        return NULL;
    }
    data[n] = 0;
    if (size)
        *size = n;
    return data;
}
bool write_file(const char *p, const void *data, size_t n)
{
    FILE *f = open_file(p, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
bool copy_file(const char *dest, const char *src)
{
    size_t n;
    char *b = read_file(src, &n);
    if (!b)
        return false;
    bool ok = write_file(dest, b, n);
    free(b);
#ifndef _WIN32
    struct stat st;
    if (ok && stat(src, &st) == 0)
        chmod(dest, st.st_mode & 0777);
#endif
    return ok;
}
bool link_file(const char *src, const char *dest)
{
#ifdef _WIN32
    return CreateHardLinkW(windows_wide(dest), windows_wide(src), NULL) != 0;
#else
    return link(src, dest) == 0;
#endif
}
bool is_file(const char *path)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesW(windows_wide(path));
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}
bool change_directory(const char *path)
{
#ifdef _WIN32
    return _wchdir(windows_wide(path)) == 0;
#else
    return chdir(path) == 0;
#endif
}
bool remove_file(const char *path)
{
#ifdef _WIN32
    return DeleteFileW(windows_wide(path)) != 0;
#else
    return unlink(path) == 0;
#endif
}
bool move_path(const char *source, const char *dest)
{
    return rename_file(source, dest);
}
bool rename_file(const char *source, const char *dest)
{
#ifdef _WIN32
    return MoveFileExW(windows_wide(source), windows_wide(dest), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(source, dest) == 0;
#endif
}
void ensure_dir(const char *p)
{
    if (is_dir(p))
        return;
    char *parent = path_dir(p);
    if (!streq(parent, p) && !streq(parent, "."))
        ensure_dir(parent);
#ifdef _WIN32
    int result = _wmkdir(windows_wide(p));
#else
    int result = mkdir(p, 0777);
#endif
    if (result && errno != EEXIST)
        fatal(str_fmt("cannot create directory %s: %s", p, strerror(errno)));
}
bool remove_tree(const char *p)
{
#ifdef _WIN32
    wchar_t *wide = windows_wide(p);
    DWORD attr = GetFileAttributesW(wide);
    if (attr == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY))
        return DeleteFileW(wide) != 0;
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT)
        return RemoveDirectoryW(wide) != 0;
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(windows_wide(path_join(p, "*")), &d);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            char *name = windows_utf8(d.cFileName);
            if (!streq(name, ".") && !streq(name, ".."))
                if (!remove_tree(path_join(p, name)))
                {
                    FindClose(h);
                    return false;
                }
        } while (FindNextFileW(h, &d));
        FindClose(h);
    }
    return RemoveDirectoryW(wide) != 0;
#else
    struct stat st;
    if (lstat(p, &st))
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(p) == 0;
    DIR *d = opendir(p);
    if (!d)
        return false;
    struct dirent *e;
    bool ok = true;
    while ((e = readdir(d)))
        if (!streq(e->d_name, ".") && !streq(e->d_name, ".."))
            if (!remove_tree(path_join(p, e->d_name)))
                ok = false;
    closedir(d);
    return rmdir(p) == 0 && ok;
#endif
}
int process_id(void)
{
#ifdef _WIN32
    return _getpid();
#else
    return (int)getpid();
#endif
}
char *make_temp_dir(const char *parent, const char *prefix, char **error)
{
    if (error)
        *error = NULL;
    ensure_dir(parent);
#ifdef _WIN32
    static unsigned counter;
    for (unsigned i = 0; i < 10000; i++)
    {
        char *p = path_join(
            parent, str_fmt("%s%lu-%u", prefix, (unsigned long)GetCurrentProcessId(), counter++));
        if (!_wmkdir(windows_wide(p)))
            return p;
        if (errno != EEXIST)
            break;
    }
#else
    char *p = path_join(parent, str_fmt("%sXXXXXX", prefix));
    if (mkdtemp(p))
        return p;
#endif
    if (error)
        *error = str_fmt("cannot create temporary directory: %s", strerror(errno));
    return NULL;
}
char *executable_path(void)
{
#ifdef _WIN32
    DWORD n = 256;
    for (;;)
    {
        wchar_t *b = xmalloc(((size_t)n + 1) * sizeof(wchar_t));
        DWORD len = GetModuleFileNameW(NULL, b, n);
        if (len && len < n)
        {
            b[len] = 0;
            char *utf8 = windows_utf8(b);
            free(b);
            return utf8;
        }
        free(b);
        n *= 2;
    }
#elif defined(__APPLE__)
    uint32_t n = 0;
    _NSGetExecutablePath(NULL, &n);
    char *b = xmalloc(n);
    if (_NSGetExecutablePath(b, &n))
        fatal("cannot locate executable");
    return absolute_path(b);
#else
    size_t n = 256;
    for (;;)
    {
        char *b = xmalloc(n + 1);
        ssize_t len = readlink("/proc/self/exe", b, n);
        if (len < 0)
            fatal("cannot locate executable");
        if ((size_t)len < n)
        {
            b[len] = 0;
            return b;
        }
        free(b);
        n *= 2;
    }
#endif
}
void print_command(StrVec args)
{
    for (size_t i = 0; i < args.len; i++)
    {
        if (i)
            fputc(' ', stderr);
        fputc('"', stderr);
        for (const char *p = args.data[i]; *p; p++)
        {
            if (*p == '"' || *p == '\\')
                fputc('\\', stderr);
            fputc(*p, stderr);
        }
        fputc('"', stderr);
    }
    fputc('\n', stderr);
}
#ifdef _WIN32
char *windows_command_line(StrVec v)
{
    char *r = str_copy("");
    for (size_t i = 0; i < v.len; i++)
    {
        r = str_fmt("%s%s\"", r, i ? " " : "");
        const char *p = v.data[i];
        size_t bs = 0;
        for (;;)
        {
            if (*p == '\\')
            {
                bs++;
                p++;
                continue;
            }
            size_t count = *p == '"' || !*p ? bs * 2 : bs;
            while (count--)
                r = str_fmt("%s\\", r);
            bs = 0;
            if (!*p)
                break;
            if (*p == '"')
                r = str_fmt("%s\\", r);
            r = str_fmt("%s%c", r, *p++);
        }
        r = str_fmt("%s\"", r);
    }
    return r;
}
#endif
static int process_run(StrVec args, FILE *out, FILE *err)
{
    if (!args.len)
        return 127;
#ifdef _WIN32
    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out ? (HANDLE)_get_osfhandle(_fileno(out)) : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = err ? (HANDLE)_get_osfhandle(_fileno(err)) : GetStdHandle(STD_ERROR_HANDLE);
    if (out)
        SetHandleInformation(si.hStdOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (err)
        SetHandleInformation(si.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    wchar_t *cmd = windows_wide(windows_command_line(args));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return 127;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
#else
    pid_t pid = fork();
    if (pid < 0)
        return 127;
    if (!pid)
    {
        if (out)
            dup2(fileno(out), STDOUT_FILENO);
        if (err)
            dup2(fileno(err), STDERR_FILENO);
        char **argv = xmalloc((args.len + 1) * sizeof(char *));
        memcpy(argv, args.data, args.len * sizeof(char *));
        execvp(argv[0], argv);
        fprintf(stderr, "cross_cc: cannot execute %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0)
    {
        if (errno != EINTR)
            return 1;
    }
    return WIFEXITED(status)     ? WEXITSTATUS(status)
           : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                 : 1;
#endif
}
int run_command(StrVec args, bool verbose)
{
    if (verbose)
        print_command(args);
    return process_run(args, NULL, NULL);
}
static FILE *temporary_stream(void)
{
#ifdef _WIN32
    wchar_t *dir;
    char *configured = env("TMPDIR");
    if (*configured)
        dir = windows_wide(configured);
    else
    {
        DWORD n = GetTempPathW(0, NULL);
        if (!n)
            return NULL;
        dir = xmalloc(((size_t)n + 1) * sizeof(wchar_t));
        if (!GetTempPathW(n + 1, dir))
            return NULL;
    }
    wchar_t path[MAX_PATH + 1];
    if (!GetTempFileNameW(dir, L"xcc", 0, path))
        return NULL;
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    HANDLE handle = CreateFileW(
        path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (handle == INVALID_HANDLE_VALUE)
    {
        DeleteFileW(path);
        return NULL;
    }
    int fd = _open_osfhandle((intptr_t)handle, _O_RDWR | _O_BINARY);
    if (fd < 0)
    {
        CloseHandle(handle);
        return NULL;
    }
    FILE *stream = _fdopen(fd, "w+b");
    if (!stream)
        _close(fd);
    return stream;
#else
    return tmpfile();
#endif
}
char *capture_command(StrVec args, int *code)
{
    FILE *out = temporary_stream(), *err = temporary_stream();
    if (!out || !err)
    {
        if (out)
            fclose(out);
        if (err)
            fclose(err);
        *code = 127;
        return str_copy("");
    }
    *code = process_run(args, out, err);
    FILE *f = *code ? err : out;
    fflush(f);
    long n = ftell(f);
    if (n < 0)
        n = 0;
    rewind(f);
    char *s = xmalloc((size_t)n + 1);
    size_t got = fread(s, 1, (size_t)n, f);
    s[got] = 0;
    fclose(out);
    fclose(err);
    return *code ? s : str_trim(s);
}
char *find_program(const char *name)
{
    StrVec ext = SV("");
#ifdef _WIN32
    if (!ends_with(str_lower(name), ".exe"))
        ext = SV("", ".exe", ".cmd", ".bat", ".com");
#endif
    bool sep = contains(name, "/");
#ifdef _WIN32
    sep = sep || contains(name, "\\") || contains(name, ":");
#endif
    if (sep)
    {
        for (size_t i = 0; i < ext.len; i++)
        {
            char *p = str_fmt("%s%s", name, ext.data[i]);
            if (file_exists(p) && !is_dir(p))
                return absolute_path(p);
        }
        return str_copy("");
    }
    StrVec dirs = str_split(env("PATH"), PATH_LIST_SEPARATOR);
    for (size_t i = 0; i < dirs.len; i++)
        if (*dirs.data[i])
            for (size_t j = 0; j < ext.len; j++)
            {
                char *p = path_join(dirs.data[i], str_fmt("%s%s", name, ext.data[j]));
                if (file_exists(p) && !is_dir(p))
                    return p;
            }
    return str_copy("");
}
char *first_existing_n(size_t n, ...)
{
    va_list a;
    va_start(a, n);
    char *result = "";
    for (size_t i = 0; i < n; i++)
    {
        char *p = va_arg(a, char *);
        if (!*result && file_exists(p))
            result = p;
    }
    va_end(a);
    return result;
}
bool needs_link(Options o)
{
    return !o.header_emit && !o.compile_only && !o.preprocess_only && !o.assembly_only &&
           !o.syntax_only && !o.dependency_only && !o.query;
}
