#define _GNU_SOURCE
#include <string.h>
#include <sys/utsname.h>

int uname(struct utsname *uts) {
    strcpy(uts->sysname, "wasi");
    strcpy(uts->nodename, "(none)");
    strcpy(uts->release, "0.0.0");
    strcpy(uts->version, "0.0.0");
#if defined(__wasm64__)
    strcpy(uts->machine, "wasm64");
#else
    strcpy(uts->machine, "wasm32");
#endif
    strcpy(uts->domainname, "(none)");
    return 0;
}
