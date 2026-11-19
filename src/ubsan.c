#include "../runtime/ubsan/runtime_source.h"
#include "platforms.h"

static const char LIBBACKTRACE_COMMIT[] =
    "0b9b49cf4a2c9229fc052d6716e1528b2f23e91a";
static const char LIBBACKTRACE_CONFIG[] =
    "\n#define BACKTRACE_ELF_SIZE 64\n#define HAVE_ATOMIC_FUNCTIONS 1\n#define "
    "HAVE_SYNC_FUNCTIONS 1\n#define HAVE_DECL_GETPAGESIZE 1\n#define "
    "HAVE_DECL_STRNLEN 1\n#define HAVE_DL_ITERATE_PHDR 1\n#define HAVE_LINK_H "
    "1\n#define HAVE_FCNTL 1\n#define HAVE_GETIPINFO 1\n#define HAVE_LSTAT "
    "1\n#define HAVE_READLINK 1\n";
static const char LIBBACKTRACE_SUPPORTED[] =
    "\n#define BACKTRACE_SUPPORTED 1\n#define BACKTRACE_USES_MALLOC 0\n#define "
    "BACKTRACE_SUPPORTS_THREADS 1\n#define BACKTRACE_SUPPORTS_DATA 1\n#define "
    "BACKTRACE_SUPPORTS_MOREDATA 1\n";
static const char LIBBACKTRACE_DARWIN_CONFIG[] =
    "\n#define HAVE_MACH_O_DYLD_H 1\n#define HAVE_ATOMIC_FUNCTIONS 1\n#define "
    "HAVE_SYNC_FUNCTIONS 1\n#define HAVE_DECL_GETPAGESIZE 1\n#define "
    "HAVE_DECL_STRNLEN 1\n#define HAVE_FCNTL 1\n#define HAVE_GETIPINFO 1\n";
bool ubsan_darwin_symbols(Target t) {
  return streq(t.os, "macos") &&
         (streq(t.arch, "x86_64") || streq(t.arch, "aarch64"));
}
bool ubsan_linux_symbols(Target t) {
  return streq(t.os, "linux") && streq(t.arch, "x86_64") &&
         (streq(t.abi, "gnu") || streq(t.abi, "musl"));
}
const char *libbacktrace_config(Target t) {
  return streq(t.os, "macos") ? LIBBACKTRACE_DARWIN_CONFIG
                              : LIBBACKTRACE_CONFIG;
}
StrVec libbacktrace_sources(Target t) {
  return SV("atomic.c", "dwarf.c", "fileline.c", "posix.c", "sort.c", "state.c",
            "backtrace.c", "simple.c",
            streq(t.os, "macos") ? "macho.c" : "elf.c", "mmapio.c", "mmap.c");
}
char *libbacktrace_source(Toolchain tc) {
  return first_existing(
      path_join(tc.root, "lib", "libbacktrace"),
      path_join(tc.root, ".cache", "upstream",
                str_fmt("libbacktrace-%s", LIBBACKTRACE_COMMIT)));
}
uint64_t libbacktrace_identity(Target t, Toolchain tc, char **error) {
  *error = NULL;
  char *root = libbacktrace_source(tc);
  if (streq(root, "")) {
    *error = "libbacktrace sources missing: run cross_cc setup, or use a "
             "complete runtime bundle";
    return 0;
  }
  Platform_Text identity = {0};
  pt_append(&identity, LIBBACKTRACE_COMMIT);
  pt_append(&identity, libbacktrace_config(t));
  pt_append(&identity, LIBBACKTRACE_SUPPORTED);
  StrVec sources = libbacktrace_sources(t);
  sv_extend(&sources, SV("backtrace.h", "internal.h", "filenames.h"));
  for (size_t i = 0; i < sources.len; i++) {
    size_t size = 0;
    char *bytes = read_file(path_join(root, sources.data[i]), &size);
    if (!bytes) {
      *error = str_fmt("cannot read libbacktrace %s: %s",
                       i < 11 ? "source" : "header", sources.data[i]);
      free(identity.data);
      return 0;
    }
    pt_append(&identity, bytes);
    free(bytes);
  }
  uint64_t hash = runtime_hash(nz(identity.data));
  free(identity.data);
  return hash;
}
StrVec libbacktrace_objects(const char *work, Options o, Target t, Toolchain tc,
                            char **error) {
  StrVec objects = {0};
  *error = NULL;
  const char *config = libbacktrace_config(t);
  if (!write_file(path_join(work, "config.h"), config, strlen(config)) ||
      !write_file(path_join(work, "backtrace-supported.h"),
                  LIBBACKTRACE_SUPPORTED, strlen(LIBBACKTRACE_SUPPORTED))) {
    *error = str_fmt("cannot write libbacktrace config: %s", strerror(errno));
    return objects;
  }
  char *root = libbacktrace_source(tc);
  StrVec flags = runtime_base_flags(o, t, tc);
  if (streq(t.os, "macos"))
    sv_extend(&flags,
              SV("-nostdlibinc", "-isystem", path_join(tc.lib_dir, "include"),
                 "-isystem",
                 path_join(tc.lib_dir, "libc", "include", "any-darwin-any")));
  sv_extend(&flags,
            SV("-O2", "-g0", "-std=gnu11", "-fno-sanitize=all",
               "-fvisibility=hidden", "-I", work, "-I", root, "-I",
               path_join(tc.lib_dir, "libunwind", "include"),
               str_fmt("-ffile-prefix-map=%s=/cross_cc/libbacktrace", root),
               str_fmt("-ffile-prefix-map=%s=/cross_cc/ubsan", work)));
  if (!streq(t.os, "macos"))
    sv_push(&flags, "-D_GNU_SOURCE");
  StrVec sources = libbacktrace_sources(t);
  for (size_t i = 0; i < sources.len; i++) {
    char *object = path_join(work, str_fmt("backtrace-%zu.o", i));
    *error = runtime_compile(path_join(root, sources.data[i]), object, flags,
                             tc, o.verbose);
    if (*error)
      return objects;
    sv_push(&objects, object);
  }
  return objects;
}
bool ubsan_full_supported(Target t) {
  if (streq(t.abi, "gnux32") || streq(t.abi, "muslx32") ||
      streq(t.abi, "ilp32"))
    return false;
  if (streq(t.os, "linux")) {
    if (streq(t.arch, "mips") || streq(t.arch, "mipsel"))
      return streq(t.abi, "musleabi");
    if (streq(t.arch, "mips64") || streq(t.arch, "mips64el"))
      return streq(t.abi, "muslabi64");
    return in_target_inventory(
        "x86 x86_64 arm thumb aarch64 riscv32 riscv64 loongarch64 powerpc64le "
        "powerpc64 s390x armeb aarch64_be",
        t.arch);
  }
  if (streq(t.os, "freebsd"))
    return in_target_inventory("x86 x86_64 aarch64 riscv64", t.arch);
  if (streq(t.os, "netbsd"))
    return in_target_inventory("x86 x86_64 aarch64", t.arch);
  if (streq(t.os, "openbsd"))
    return in_target_inventory("x86_64 aarch64", t.arch);
  if (streq(t.os, "windows"))
    return in_target_inventory("x86 x86_64 aarch64", t.arch);
  return streq(t.os, "macos") &&
         (streq(t.arch, "x86_64") || streq(t.arch, "aarch64"));
}
bool compiler_sanitizer_consumed(const char *arg) {
  const char *prefixes[] = {"-fsanitize=", "-fno-sanitize="},
             *names[] = {"undefined", "thread", "fuzzer", "fuzzer-no-link"};
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 4; j++)
      if (compiler_sanitizer_contains(arg, prefixes[i], names[j]))
        return true;
  return compiler_sanitizer_contains(arg, "-fsanitize-trap=", "undefined") ||
         compiler_sanitizer_contains(arg, "-fno-sanitize-trap=", "undefined");
}
bool compiler_sanitizer_enabled(Options o, StrVec names) {
  bool enabled = false, skip = false, operands = false;
  for (size_t i = 0; i < o.forwarded.len; i++) {
    const char *arg = o.forwarded.data[i];
    if (skip) {
      skip = false;
      continue;
    }
    if (operands)
      continue;
    if (streq(arg, "--")) {
      operands = true;
      continue;
    }
    skip = takes_value(arg);
    const char *prefixes[] = {"-fsanitize=", "-fno-sanitize="};
    for (int p = 0; p < 2; p++)
      for (size_t j = 0; j < names.len; j++)
        if (compiler_sanitizer_contains(arg, prefixes[p], names.data[j]))
          enabled = p == 0;
  }
  return enabled;
}
Ubsan_Mode compiler_ubsan_mode(Options o) {
  Ubsan_Mode mode = UBSAN_OFF;
  bool explicit_mode = false, skip = false, operands = false;
  const char *optimization = "-O0";
  for (size_t i = 0; i < o.forwarded.len; i++) {
    const char *arg = o.forwarded.data[i];
    if (skip) {
      skip = false;
      continue;
    }
    if (operands)
      continue;
    if (streq(arg, "--")) {
      operands = true;
      continue;
    }
    skip = takes_value(arg);
    bool known = false;
    char *opt = compiler_optimization(arg, &known);
    if (known)
      optimization = opt;
    const char *prefixes[] = {"-fsanitize=", "-fno-sanitize=",
                              "-fsanitize-trap=", "-fno-sanitize-trap="};
    for (int p = 0; p < 4; p++) {
      if (!compiler_sanitizer_contains(arg, prefixes[p], "undefined"))
        continue;
      if (p == 0)
        mode = UBSAN_FULL;
      else if (p == 1)
        mode = UBSAN_OFF;
      else if (p == 2)
        mode = UBSAN_TRAP;
      else if (mode == UBSAN_TRAP)
        mode = UBSAN_FULL;
      explicit_mode = true;
    }
  }
  if (!explicit_mode && streq(optimization, "-O0"))
    mode = UBSAN_FULL;
  return mode;
}
bool ubsan_full_enabled(Options o) {
  const char *checks[] = {"alignment",
                          "array-bounds",
                          "bool",
                          "builtin",
                          "enum",
                          "float-cast-overflow",
                          "integer-divide-by-zero",
                          "nonnull-attribute",
                          "null",
                          "object-size",
                          "pointer-overflow",
                          "return",
                          "returns-nonnull-attribute",
                          "shift-base",
                          "shift-exponent",
                          "signed-integer-overflow",
                          "unreachable",
                          "vla-bound",
                          "unsigned-integer-overflow"};
  enum { N = sizeof(checks) / sizeof(*checks) };
  bool enabled[N], trapped[N];
  Ubsan_Mode mode = compiler_ubsan_mode(o);
  for (int i = 0; i < N; i++) {
    enabled[i] =
        mode != UBSAN_OFF && !streq(checks[i], "unsigned-integer-overflow");
    trapped[i] =
        mode == UBSAN_TRAP && !streq(checks[i], "unsigned-integer-overflow");
  }
  bool skip = false, operands = false;
  for (size_t i = 0; i < o.forwarded.len; i++) {
    const char *arg = o.forwarded.data[i];
    if (skip) {
      skip = false;
      continue;
    }
    if (operands)
      continue;
    if (streq(arg, "--")) {
      operands = true;
      continue;
    }
    skip = takes_value(arg);
    if (streq(arg, "-fsanitize-trap") || streq(arg, "-fno-sanitize-trap"))
      for (int j = 0; j < N; j++)
        trapped[j] = streq(arg, "-fsanitize-trap");
    const char *prefixes[] = {"-fsanitize=", "-fno-sanitize=",
                              "-fsanitize-trap=", "-fno-sanitize-trap="};
    for (int p = 0; p < 4; p++) {
      if (!starts_with(arg, prefixes[p]) || compiler_sanitizer_consumed(arg))
        continue;
      bool on = p == 0 || p == 2, trap = p >= 2;
      StrVec parts = str_split(arg + strlen(prefixes[p]), ",");
      for (size_t k = 0; k < parts.len; k++)
        for (int j = 0; j < N; j++) {
          const char *part = parts.data[k], *check = checks[j];
          bool match =
              streq(part, check) || streq(part, "all") ||
              (streq(part, "undefined") &&
               !streq(check, "unsigned-integer-overflow")) ||
              (streq(part, "shift") && starts_with(check, "shift-")) ||
              (streq(part, "bounds") && streq(check, "array-bounds")) ||
              (streq(part, "integer") &&
               (contains(check, "overflow") || starts_with(check, "shift-") ||
                streq(check, "integer-divide-by-zero")));
          if (match) {
            if (trap)
              trapped[j] = on;
            else
              enabled[j] = on;
          }
        }
    }
  }
  for (int i = 0; i < N; i++)
    if (enabled[i] && !trapped[i])
      return true;
  return false;
}
char *prepare_ubsan(Options o, Target t, Toolchain tc, char **error) {
  *error = NULL;
  if (!ubsan_full_supported(t))
    return rt_error(error, "full UBSan runtime is unavailable for this target; "
                           "use trap instrumentation");
  bool linux_symbols = ubsan_linux_symbols(t),
       darwin_symbols = ubsan_darwin_symbols(t),
       windows = streq(t.os, "windows");
  StrVec flags =
      SV("--no-default-config", str_fmt("--target=%s", t.clang), "-x", "c",
         "-std=c11", "-O2", "-g0", "-nostdinc", "-ffreestanding",
         "-fno-sanitize=all", "-fno-stack-protector", "-fno-builtin", "-w");
  if (!windows)
    sv_push(&flags, "-fPIC");
  if (windows && streq(t.abi, "msvc"))
    sv_push(&flags, "-mno-incremental-linker-compatible");
  if (windows && streq(t.arch, "x86"))
    sv_extend(&flags,
              SV("-fno-omit-frame-pointer", "-fno-asynchronous-unwind-tables",
                 "-fno-unwind-tables"));
  if (linux_symbols)
    sv_push(&flags, "-DCROSS_CC_UBSAN_LINUX_SYMBOLS=1");
  if (darwin_symbols)
    sv_push(&flags, "-DCROSS_CC_UBSAN_DARWIN_SYMBOLS=1");
  if (windows)
    sv_push(&flags, "-DCROSS_CC_UBSAN_WINDOWS_SYMBOLS=1");
  StrVec cpu = cpu_flags(t, o.cpu, o.forwarded, error);
  if (*error)
    return NULL;
  sv_extend(&flags, cpu);
  char *recipe = str_fmt("ubsan-c-v1-lazy-archive-%" PRIx64 "-%" PRIx64,
                         runtime_hash(cross_cc_ubsan_runtime_source),
                         runtime_hash(rt_join(flags, "\n"))),
       *unwind = "";
  if (linux_symbols || darwin_symbols) {
    uint64_t source_hash = libbacktrace_identity(t, tc, error);
    if (*error)
      return NULL;
    recipe = str_fmt("%s-%" PRIx64, recipe, source_hash);
    if (linux_symbols) {
      char *dir = runtime_build_cxx(o, t, tc, true, error);
      if (*error)
        return NULL;
      unwind = path_join(dir, "libunwind.a");
      size_t size = 0;
      unsigned char *bytes = (unsigned char *)read_file(unwind, &size);
      if (!bytes)
        return rt_error(error, "cannot fingerprint bundled libunwind");
      uint64_t hash = UINT64_C(14695981039346656037);
      for (size_t i = 0; i < size; i++)
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
      free(bytes);
      recipe = str_fmt("%s-%" PRIx64, recipe, hash);
    }
  }
  char *dest = runtime_cache(recipe, o, t, tc),
       *artifact = path_join(dest, "libubsan.a");
  if (file_exists(artifact))
    return artifact;
  ensure_dir(tc.cache_dir);
  char *work = make_temp_dir(tc.cache_dir, "ubsan-build-", error);
  if (*error)
    return NULL;
  char *source = path_join(work, "ubsan.c");
  if (!write_file(source, cross_cc_ubsan_runtime_source,
                  CROSS_CC_UBSAN_RUNTIME_SOURCE_LEN)) {
    *error = str_fmt("cannot write UBSan source: %s", strerror(errno));
    goto fail;
  }
  sv_push(&flags, str_fmt("-ffile-prefix-map=%s=/cross_cc/ubsan", work));
  *error =
      runtime_compile(source, path_join(work, "ubsan.o"), flags, tc, o.verbose);
  if (*error)
    goto fail;
  if (windows) {
    StrVec aliases =
        SV("libubsan.a", "libclang_rt.ubsan_standalone.a",
           "liblibclang_rt.ubsan_standalone.a", "clang_rt.ubsan_standalone.lib",
           "libclang_rt.ubsan_standalone_cxx.a",
           "liblibclang_rt.ubsan_standalone_cxx.a",
           "clang_rt.ubsan_standalone_cxx.lib");
    for (size_t i = 0; i < aliases.len; i++) {
      *error = runtime_archive(SV(path_join(work, "ubsan.o")),
                               path_join(work, aliases.data[i]), tc, o.verbose);
      if (*error)
        goto fail;
    }
  } else {
    StrVec objects = SV(path_join(work, "ubsan.o"));
    if (linux_symbols || darwin_symbols) {
      StrVec dependency_objects = libbacktrace_objects(work, o, t, tc, error);
      if (*error)
        goto fail;
      sv_extend(&objects, dependency_objects);
    }
    char *archive = path_join(work, "libubsan.a");
    *error = runtime_archive(objects, archive, tc, o.verbose);
    if (*error)
      goto fail;
    if (!streq(unwind, "") &&
        run_command(SV(tc.ar, "qLsD", archive, unwind), o.verbose)) {
      *error = "cannot bundle libunwind into UBSan archive";
      goto fail;
    }
  }
  *error = runtime_publish(work, dest);
  if (*error && !file_exists(artifact))
    goto fail;
  *error = NULL;
  remove_tree(work);
  return artifact;
fail:
  remove_tree(work);
  return NULL;
}
