#include "windows_runtime.h"
#ifndef _WIN32
Msvc_Libc msvc_libc_detect(Target t, char **error) {
    Msvc_Libc l = {0};
    *error = str_fmt("unable to find libc installation for %s: MSVC headers and libraries only exist on Windows hosts; set ZIG_LIBC to a libc file", nz(t.name));
    return l;
}
#endif
