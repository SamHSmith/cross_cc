#include "windows_runtime.h"
#ifdef _WIN32
#include <windows.h>

static char *msvc_registry_string(const char *key, const char *value) {
    wchar_t wkey[512], wvalue[128], buf[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 512)) return str_copy("");
    if (!MultiByteToWideChar(CP_UTF8, 0, value, -1, wvalue, 128)) return str_copy("");
    DWORD size = (DWORD)sizeof(buf) - sizeof(wchar_t);
    memset(buf, 0, sizeof(buf));
    /* RRF_RT_REG_SZ | as the original: 0x00010000 */
    LONG status = RegGetValueW(HKEY_LOCAL_MACHINE, wkey, wvalue, 0x00000002 | 0x00010000, NULL, buf, &size);
    if (status != 0) return str_copy("");
    char out[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, (int)sizeof(out), NULL, NULL);
    if (n <= 0) return str_copy("");
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '\\') len--;
    return str_ncopy(out, len);
}

static bool sdk_ok(const char *p) {
    return file_exists(path_join(p, "ucrt", "stdlib.h")) && file_exists(path_join(p, "um", "windows.h"));
}

static bool tools_ok(const char *p) {
    return file_exists(path_join(p, "include", "vcruntime.h"));
}

/* Finds the newest installed Windows 10+ SDK and MSVC toolset. */
Msvc_Libc msvc_libc_detect(Target t, char **error) {
    Msvc_Libc l = {0};
    const char *arch = msvc_arch_dir(t.arch);
    *error = NULL;
    char *kits = msvc_registry_string("SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots", "KitsRoot10");
    if (!kits[0]) kits = path_join(nz(env("ProgramFiles(x86)")), "Windows Kits", "10");
    char *sdk = msvc_newest_subdir(path_join(kits, "Include"), sdk_ok);
    if (!sdk[0]) { *error = str_copy("unable to find a Windows SDK; install one or set ZIG_LIBC to a libc file"); return l; }
    l.include_dir = path_join(kits, "Include", sdk, "ucrt");
    l.crt_dir = path_join(kits, "Lib", sdk, "ucrt", arch);
    l.kernel32_lib_dir = path_join(kits, "Lib", sdk, "um", arch);

    char *best_root = str_copy(""), *best_tools = str_copy("");
    const char *roots[2] = {nz(env("ProgramFiles")), nz(env("ProgramFiles(x86)"))};
    for (int r = 0; r < 2; r++) {
        if (!roots[r][0]) continue;
        char *vs = path_join(roots[r], "Microsoft Visual Studio");
        Msvc_Dir_Entries years;
        if (!msvc_read_dir(vs, &years)) continue;
        for (size_t y = 0; y < years.len; y++) {
            if (years.data[y].type != 1) continue;
            Msvc_Dir_Entries editions;
            if (!msvc_read_dir(path_join(vs, years.data[y].name), &editions)) continue;
            for (size_t e = 0; e < editions.len; e++) {
                if (editions.data[e].type != 1) continue;
                char *tools = path_join(vs, years.data[y].name, editions.data[e].name, "VC", "Tools", "MSVC");
                char *v = msvc_newest_subdir(tools, tools_ok);
                if (!v[0] || !is_dir(path_join(tools, v, "lib", arch))) continue;
                if (!best_tools[0] || msvc_version_less(best_tools, v)) { best_root = tools; best_tools = v; }
            }
        }
    }
    if (!best_tools[0]) { *error = str_copy("unable to find the MSVC toolset; install Visual Studio with the C++ tools or set ZIG_LIBC to a libc file"); return l; }
    l.sys_include_dir = path_join(best_root, best_tools, "include");
    l.msvc_lib_dir = path_join(best_root, best_tools, "lib", arch);
    return l;
}
#endif
