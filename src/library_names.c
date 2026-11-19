#include "runtime.h"

bool libunwind_library_name(const char *name) {
  return in_target_inventory("unwind gcc_eh gcc_s", name);
}
bool links_bundled_libunwind(Target t) {
  return in_target_inventory("linux freebsd netbsd openbsd", t.os) ||
         (streq(t.os, "windows") && streq(t.abi, "gnu"));
}
bool bundled_runtime_library_name(Target t, const char *name) {
  if (in_target_inventory("compiler_rt gcc atomic ssp", name))
    return true;
  if (libunwind_library_name(name))
    return links_bundled_libunwind(t) || is_apple_os(t.os) ||
           streq(t.os, "wasi");
  const char *n =
      (streq(t.os, "windows") || is_apple_os(t.os)) ? str_lower(name) : name;
  if (streq(t.os, "windows") && streq(t.abi, "gnu"))
    return in_target_inventory(
        "c adsiid amstrmid bits delayimp dloadhelper dmoguids dxerr8 dxerr9 "
        "dxguid ksguid largeint m mfuuid mingw32 mingwex mingwthrd moldname "
        "msvcrt-os portabledeviceguids pthread strmiids uuid wbemuuid wiaguid "
        "winpthread wmcodecdspuuid",
        n);
  if (streq(t.os, "windows"))
    return streq(n, "c");
  if (is_apple_os(t.os))
    return in_target_inventory(
               "c system dbm dl info m poll proc pthread rpcsvc", n) ||
           (streq(n, "mx") && streq(t.os, "macos"));
  if (streq(t.os, "wasi"))
    return in_target_inventory(
        "c m rt pthread util resolv dl crypt xnet setjmp wasi-emulated-getpid "
        "wasi-emulated-mman wasi-emulated-process-clocks wasi-emulated-signal",
        n);
  return false;
}
static bool absorb(Target t, const char *name, bool *unwind) {
  if (!bundled_runtime_library_name(t, name))
    return false;
  if (libunwind_library_name(name) && links_bundled_libunwind(t))
    *unwind = true;
  return true;
}
StrVec drop_bundled_runtime_libraries(Options o, Target t, bool *libunwind) {
  bool unused = false;
  if (!libunwind)
    libunwind = &unused;
  *libunwind = false;
  bool links_libc = o.explicit_libc || o.explicit_libcxx ||
                    (!o.no_stdlib && !o.no_defaultlibs && !o.no_libc);
  if (!links_libc)
    return sv_copy(o.forwarded);
  StrVec args = o.forwarded, result = {0};
  for (size_t i = 0; i < args.len; i++) {
    char *a = args.data[i];
    if (streq(a, "--")) {
      sv_extend(&result, sv_slice(args, i, args.len));
      break;
    }
    char *value;
    bool separate;
    char kind = linux_library_arg(a, &value, &separate);
    if (kind == 'l') {
      if (separate) {
        if (i + 1 >= args.len) {
          sv_push(&result, a);
          continue;
        }
        value = args.data[++i];
        if (!absorb(t, value, libunwind))
          RT_ADD(&result, a, value);
      } else if (!absorb(t, value, libunwind))
        sv_push(&result, a);
      continue;
    }
    sv_push(&result, a);
    if (takes_value(a) && i + 1 < args.len)
      sv_push(&result, args.data[++i]);
  }
  return result;
}
