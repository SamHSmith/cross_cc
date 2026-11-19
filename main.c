/* One translation unit keeps the driver build explicit and inexpensive.
   LLVM's C++ interface is isolated in deps/llvm-bridge/. */
#include "src/cross.h"
#include "src/runtime.h"
#include "src/resources.h"
#include "src/platforms.h"
#include "src/windows_runtime.h"

#include "src/support.c"
#include "src/arguments.c"
#include "src/target.c"
#include "src/libc_config.c"
#include "src/library_names.c"
#include "src/linker_flags.c"
#include "src/objcopy.c"
#include "src/resource_vfs.c"
#include "src/determinism.c"
#include "src/rc.c"
#include "src/ar.c"

#include "src/runtime_recipes.c"
#include "src/runtime_musl_supplement.c"
#include "src/builtins_recipes.c"
#include "src/builtins_memory.c"
#include "src/builtins.c"
#include "src/wasi_recipes.c"
#include "src/wasi_supplement.c"
#include "src/wasi.c"
#include "src/runtime.c"
#include "src/freestanding.c"
#include "src/sparc_tls.c"
#include "src/sparc_link.c"

#include "src/glibc.c"
#include "src/bsd.c"
#include "src/tsan.c"
#include "src/ubsan.c"
#include "src/profile.c"
#include "src/macos.c"
#include "src/msvc.c"
#include "src/msvc_other.c"
#include "src/msvc_windows.c"
#include "src/msvc_managed.c"
#include "src/msvc_libcxx.c"
#include "src/msvc_link.c"
#include "src/mingw_recipes.c"
#include "src/mingw.c"

#include "src/toolchain.c"
#include "src/backend.c"
#include "src/main.c"

#ifdef _WIN32
#include <shellapi.h>
#endif

int main(int argc, char **argv)
{
#ifdef _WIN32
    /* The Windows CRT's narrow argv uses the active code page. LLVM and the
       driver use UTF-8, so read the Unicode command line instead. */
    int wide_argc = 0;
    wchar_t **wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (!wide_argv)
        fatal("cannot decode command line");
    char **utf8_argv = xmalloc(((size_t)wide_argc + 1) * sizeof(char *));
    for (int i = 0; i < wide_argc; ++i)
        utf8_argv[i] = windows_utf8(wide_argv[i]);
    LocalFree(wide_argv);
    (void)argc;
    (void)argv;
    return cross_main(wide_argc, utf8_argv);
#else
    return cross_main(argc, argv);
#endif
}
