#include "runtime.h"
#include <inttypes.h>

/* Recipes translated from Zig's libs/{musl,libcxx,libunwind}.zig.
 * Copyright (c) Zig contributors. MIT: licenses/runtime-recipes.txt. */
StrVec runtime_glibc_version_flags(Target t) {
  if (!streq(t.os, "linux") || !starts_with(t.abi, "gnu"))
    return (StrVec){0};
  StrVec parts = str_split(
      *nz(t.glibc_version) ? t.glibc_version : glibc_default_version(t), ".");
  return SV(
      str_fmt("-D__GLIBC_MINOR__=%s", parts.len > 1 ? parts.data[1] : "31"));
}
StrVec libc_include_flags(Target t, const char *lib_dir) {
  if (in_target_inventory("freebsd netbsd openbsd", t.os))
    return bsd_include_flags(t, lib_dir);
  StrVec flags = {0};
  const char *arch = nz(t.arch), *abi = nz(t.abi), *generic = abi;
  if (arm_arch(t.arch))
    arch = "arm";
  if (streq(t.arch, "aarch64_be"))
    arch = "aarch64";
  if (streq(t.arch, "x86_64") && starts_with(t.abi, "gnu"))
    arch = "x86";
  if (in_target_inventory("riscv32 riscv64", t.arch) &&
      starts_with(t.abi, "gnu"))
    arch = "riscv";
  if (in_target_inventory("mips64 mips64el", t.arch) &&
      starts_with(t.abi, "musl"))
    arch = "mips64";
  if (in_target_inventory("mips mipsel", t.arch) && starts_with(t.abi, "musl"))
    arch = "mips";
  if (in_target_inventory("mips64 mips64el", t.arch) &&
      streq(t.abi, "gnuabi64"))
    arch = "mips";
  if (streq(t.arch, "sparc64") && streq(t.abi, "gnu"))
    arch = "sparc";
  if (starts_with(abi, "gnu")) {
    generic = "glibc";
    abi = "gnu";
  }
  if (starts_with(abi, "musl")) {
    generic = "musl";
    abi = "musl";
  }
  const char *family = arch;
  if (streq(t.arch, "x86_64"))
    family = "x86";
  if (in_target_inventory("riscv32 riscv64", t.arch))
    family = "riscv";
  if (in_target_inventory("mips64 mips64el", t.arch))
    family = "mips";
  if (streq(t.arch, "powerpc64le") && streq(abi, "musl"))
    arch = "powerpc64";
  if (streq(t.arch, "powerpc64le") && streq(abi, "gnu"))
    arch = "powerpc";
  if (in_target_inventory("powerpc64 powerpc64le", t.arch))
    family = "powerpc";
  if (streq(t.arch, "loongarch64")) {
    family = "loongarch";
    if (streq(abi, "gnu"))
      arch = "loongarch";
  }
  StrVec suffixes = SV(
      str_fmt("%s-%s-%s", arch, nz(t.os), abi), str_fmt("generic-%s", generic),
      str_fmt("%s-%s-any", family, nz(t.os)), str_fmt("any-%s-any", nz(t.os)));
  for (size_t i = 0; i < suffixes.len; i++) {
    char *p = path_join(lib_dir, "libc", "include", suffixes.data[i]);
    if (is_dir(p))
      RT_ADD(&flags, "-isystem", p);
  }
  return flags;
}
StrVec runtime_cxx_flags(Target t, const char *lib) {
  StrVec flags = SV(
      "-nostdinc++", "-D_LIBCPP_ABI_VERSION=1", "-D_LIBCPP_ABI_NAMESPACE=__1",
      "-D_LIBCPP_HAS_MONOTONIC_CLOCK", "-D_LIBCPP_HAS_TERMINAL",
      "-D_LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS",
      "-D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS",
      "-D_LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS=0",
      "-D_LIBCPP_HAS_RANDOM_DEVICE", "-D_LIBCPP_HAS_LOCALIZATION",
      "-D_LIBCPP_HAS_UNICODE", "-D_LIBCPP_HAS_WIDE_CHARACTERS",
      "-D_LIBCPP_HAS_NO_STD_MODULES", "-D_LIBCPP_PSTL_BACKEND_SERIAL",
      "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_NONE",
      "-D_LIBCPP_ASSERTION_SEMANTIC_DEFAULT=_LIBCPP_ASSERTION_SEMANTIC_IGNORE");
  if (streq(t.os, "wasi"))
    RT_ADD(&flags, "-D_LIBCPP_HAS_THREADS=0", "-D_LIBCPP_HAS_FILESYSTEM=0",
           "-mthread-model", "single");
  else
    RT_ADD(&flags, "-D_LIBCPP_HAS_THREADS=1", "-D_LIBCPP_HAS_FILESYSTEM=1");
  sv_push(&flags, str_fmt("-D_LIBCPP_HAS_MUSL_LIBC=%s",
                          starts_with(t.abi, "musl") ? "1" : "0"));
  if (streq(t.os, "linux"))
    sv_push(&flags, "-D_LIBCPP_HAS_TIME_ZONE_DATABASE");
  if (streq(t.os, "windows") && streq(t.abi, "msvc"))
    sv_push(&flags, "-D_LIBCPP_NO_AUTO_LINK");
  RT_ADD(&flags, "-isystem", path_join(lib, "libcxx", "include"), "-isystem",
         path_join(lib, "libcxxabi", "include"));
  return flags;
}
char *stable_compiler_identity(Toolchain tc) {
  return str_fmt("backend-v4-c-cross-cc-prefix-maps|%s|%s",
                 nz(tc.clang_version), UPSTREAM_COMMIT);
}
uint64_t runtime_hash(const char *text) {
  uint64_t h = UINT64_C(14695981039346656037);
  for (const unsigned char *p = (const unsigned char *)nz(text); *p; p++)
    h = (h ^ *p) * UINT64_C(1099511628211);
  return h;
}
char *runtime_cache(const char *kind, Options o, Target t, Toolchain tc) {
  char *error = NULL;
  StrVec cpu = cpu_flags(t, o.cpu, o.forwarded, &error);
  char *identity =
      str_fmt("recipe-v8-musl-1.2.5-five-arch-time32-dc6ea3336df6b1f5|%s|%s|%s|"
              "%s|%s|%s|%s|%s%s",
              stable_compiler_identity(tc), nz(tc.lib_dir), nz(t.clang),
              nz(o.cpu), rt_join(cpu, " "), kind, nz(t.glibc_version),
              "Os-PIC-threads-abi1", nz(libc_config_cache_identity(t)));
  return path_join(tc.cache_dir, str_fmt("runtime-%s-%016" PRIx64, kind,
                                         runtime_hash(identity)));
}
char *runtime_publish(const char *work, const char *dest) {
  const char marker[] = "cross_cc runtime recipe v5\n";
  if (!write_file(path_join(work, "complete"), marker, sizeof(marker) - 1))
    return str_fmt("cannot write runtime completion marker: %s",
                   strerror(errno));
  if (!move_path(work, dest) && !file_exists(path_join(dest, "complete")))
    return str_fmt("cannot publish runtime cache: %s", strerror(errno));
  return NULL;
}
StrVec runtime_base_flags(Options o, Target t, Toolchain tc) {
  StrVec flags = SV("--no-default-config", str_fmt("--target=%s", nz(t.clang)),
                    "-fPIC", "-ffunction-sections", "-fdata-sections",
                    "-fno-stack-protector", "-w");
  char *error = NULL;
  StrVec cpu = cpu_flags(t, o.cpu, o.forwarded, &error);
  if (error)
    fatal(error);
  sv_extend(&flags, cpu);
  if (*nz(o.sysroot))
    sv_push(&flags, str_fmt("--sysroot=%s", o.sysroot));
  if (streq(t.os, "linux") && *nz(env("ZIG_LIBC"))) {
    Libc_Config l = libc_config_from_file(env("ZIG_LIBC"), t, &error);
    if (error)
      fatal(error);
    sv_extend(&flags, libc_config_include_flags(l, tc));
  } else if (in_target_inventory("linux freebsd netbsd openbsd", t.os) ||
             !*nz(o.sysroot)) {
    RT_ADD(&flags, "-nostdlibinc", "-isystem",
           path_join(tc.lib_dir, "include"));
    sv_extend(&flags, libc_include_flags(t, tc.lib_dir));
    sv_extend(&flags, runtime_glibc_version_flags(t));
    if (in_target_inventory("freebsd netbsd openbsd", t.os))
      sv_extend(&flags, bsd_version_flags(t));
  }
  return flags;
}
char *runtime_compile(const char *source, const char *object, StrVec flags,
                      Toolchain tc, bool verbose) {
  StrVec args = SV(tc.clang);
  sv_extend(&args, flags);
  RT_ADD(&args, "-c", source, "-o", object);
  return run_command(args, verbose)
             ? str_fmt("runtime compilation failed: %s", source)
             : NULL;
}
char *runtime_archive(StrVec objects, const char *path, Toolchain tc,
                      bool verbose) {
  char *rsp = str_fmt("%s.rsp", path);
  StrVec lines = {0};
  for (size_t i = 0; i < objects.len; i++)
    sv_push(&lines, str_fmt("\"%s\"\n", str_replace(str_replace(objects.data[i],
                                                                "\\", "\\\\"),
                                                    "\"", "\\\"")));
  char *text = rt_join(lines, "");
  if (!write_file(rsp, text, strlen(text)))
    return str_fmt("cannot write archive response file %s: %s", rsp,
                   strerror(errno));
  int code = run_command(
      SV(tc.ar, "--rsp-quoting=posix", "rcsD", path, str_fmt("@%s", rsp)),
      verbose);
  remove_file(rsp);
  return code ? str_fmt("runtime archive creation failed: %s", path) : NULL;
}
StrVec runtime_recipe_files(Toolchain tc, const char *recipe, const char *array,
                            char **error) {
  (void)tc;
  (void)recipe;
  if (error)
    *error = NULL;
  if (streq(array, "libcxx_base_files"))
    return libcxx_base_files;
  if (streq(array, "libcxx_thread_files"))
    return libcxx_thread_files;
  if (streq(array, "libcxxabi_files"))
    return libcxxabi_files;
  if (streq(array, "src_files"))
    return musl_src_files;
  if (error)
    *error = "unknown runtime source manifest";
  return (StrVec){0};
}
char *runtime_build_cxx(Options o, Target t, Toolchain tc, bool only_unwind,
                        char **error) {
  if (error)
    *error = NULL;
  char *e = NULL;
  const char *kind =
      only_unwind ? "unwind-v2-canonical-cpu" : "cxx-v4-canonical-cpu";
  if (only_unwind && streq(t.os, "linux"))
    kind = "unwind-v3-prefix-mapped";
  char *dest = runtime_cache(kind, o, t, tc);
  if (file_exists(path_join(dest, "complete")))
    return dest;
  ensure_dir(tc.cache_dir);
  char *work = make_temp_dir(tc.cache_dir, "cxx-build-", &e);
  if (e)
    return rt_error(error,
                    str_fmt("cannot create runtime build directory: %s", e));
  StrVec base = {0};
  if (streq(t.os, "wasi")) {
    base = wasi_base(t, o);
    RT_ADD(&base, "-nostdlibinc", "-isystem", path_join(tc.lib_dir, "include"));
    sv_extend(&base, wasi_include_flags(tc));
    sv_push(&base, "-fno-exceptions");
  } else if (streq(t.os, "windows")) {
    base = SV("--no-default-config", str_fmt("--target=%s", nz(t.clang)),
              "-ffunction-sections", "-fdata-sections", "-fno-stack-protector",
              "-w", "-nostdlibinc", "-isystem",
              path_join(tc.lib_dir, "include"), "-isystem",
              path_join(tc.lib_dir, "libc", "include", "any-windows-any"));
  } else if (streq(t.os, "macos")) {
    base =
        SV("--no-default-config", str_fmt("--target=%s", nz(t.clang)), "-fPIC",
           "-ffunction-sections", "-fdata-sections", "-fno-stack-protector",
           "-w", "-mmacosx-version-min=15.0.0", "-nostdlibinc", "-isystem",
           path_join(tc.lib_dir, "include"), "-isystem",
           path_join(tc.lib_dir, "libc", "include", "any-darwin-any"));
  } else
    base = runtime_base_flags(o, t, tc);
  if (in_target_inventory("windows macos", t.os)) {
    StrVec cpu = cpu_flags(t, o.cpu, o.forwarded, &e);
    if (e)
      goto fail;
    sv_extend(&base, cpu);
  }
  StrVec common = runtime_cxx_flags(t, tc.lib_dir),
         libraries = SV("c++", "c++abi", "unwind");
  for (size_t l = 0; l < libraries.len; l++) {
    char *library = libraries.data[l];
    if (only_unwind && !streq(library, "unwind"))
      continue;
    if (streq(library, "unwind") && streq(t.os, "wasi"))
      continue;
    StrVec sources = {0}, objects = {0};
    if (streq(library, "c++")) {
      StrVec arrays = SV("libcxx_base_files", "libcxx_thread_files");
      for (size_t a = 0; a < arrays.len; a++) {
        if (streq(t.os, "wasi") && a == 1)
          continue;
        StrVec list = runtime_recipe_files(tc, "libcxx", arrays.data[a], &e);
        if (e)
          goto fail;
        for (size_t s = 0; s < list.len; s++) {
          char *src = list.data[s];
          if (starts_with(src, "src/support/win32/") && !streq(t.os, "windows"))
            continue;
          if (starts_with(src, "src/filesystem/") && streq(t.os, "wasi"))
            continue;
          sv_push(&sources, path_join(tc.lib_dir, "libcxx", src));
        }
      }
    } else if (streq(library, "c++abi")) {
      StrVec list = runtime_recipe_files(tc, "libcxx", "libcxxabi_files", &e);
      if (e)
        goto fail;
      for (size_t s = 0; s < list.len; s++) {
        char *src = list.data[s];
        if (streq(t.os, "wasi")) {
          if (in_target_inventory(
                  "src/cxa_thread_atexit.cpp src/cxa_exception.cpp "
                  "src/cxa_personality.cpp",
                  src))
            continue;
        } else if (streq(src, "src/cxa_noexception.cpp"))
          continue;
        sv_push(&sources, path_join(tc.lib_dir, "libcxxabi", src));
      }
    } else {
      StrVec names =
          SV("libunwind.cpp", "Unwind-EHABI.cpp", "Unwind-seh.cpp",
             "UnwindLevel1.c", "UnwindLevel1-gcc-ext.c", "Unwind-sjlj.c",
             "Unwind-wasm.c", "UnwindRegistersRestore.S",
             "UnwindRegistersSave.S", "gcc_personality_v0.c");
      for (size_t s = 0; s < names.len; s++)
        sv_push(&sources,
                path_join(tc.lib_dir, "libunwind", "src", names.data[s]));
    }
    for (size_t i = 0; i < sources.len; i++) {
      char *source = sources.data[i];
      StrVec flags = {0};
      if (ends_with(source, ".cpp"))
        sv_extend(&flags, common);
      sv_extend(&flags, base);
      RT_ADD(&flags, "-O2", "-DNDEBUG", "-fasynchronous-unwind-tables");
      if (only_unwind && streq(t.os, "linux"))
        RT_ADD(&flags, "-g0",
               str_fmt("-ffile-prefix-map=%s=/cross_cc/lib", nz(tc.lib_dir)),
               str_fmt("-ffile-prefix-map=%s=/cross_cc/unwind", work));
      if (streq(library, "unwind")) {
        RT_ADD(&flags, "-I", path_join(tc.lib_dir, "libunwind", "include"),
               "-D_LIBUNWIND_HIDE_SYMBOLS", "-D_LIBUNWIND_IS_NATIVE_ONLY",
               "-fvisibility=hidden", "-fvisibility-inlines-hidden",
               "-fvisibility-global-new-delete=force-hidden");
        if (!streq(t.os, "macos"))
          sv_push(&flags, "-Wa,--noexecstack");
        if (ends_with(source, ".cpp")) {
          sv_extend(&flags, common);
          RT_ADD(&flags, "-std=c++23", "-fno-exceptions", "-fno-rtti");
        } else if (ends_with(source, ".c"))
          RT_ADD(&flags, "-std=c99", "-fexceptions");
      } else {
        sv_extend(&flags, common);
        RT_ADD(&flags, "-std=c++23", "-fvisibility=hidden",
               "-fvisibility-inlines-hidden", "-D_LIBCPP_BUILDING_LIBRARY",
               "-I", path_join(tc.lib_dir, "libcxx", "src"));
        if (streq(library, "c++"))
          RT_ADD(&flags, "-DLIBC_NAMESPACE=__llvm_libc_common_utils",
                 "-DLIBCXX_BUILDING_LIBCXXABI",
                 "-D_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER",
                 "-faligned-allocation", "-I",
                 path_join(tc.lib_dir, "libcxx", "libc"));
        else {
          RT_ADD(&flags, "-D_LIBCXXABI_BUILDING_LIBRARY", "-fstrict-aliasing");
          if (streq(t.os, "wasi"))
            sv_push(&flags, "-D_LIBCXXABI_HAS_NO_THREADS");
          if (starts_with(t.abi, "gnu"))
            sv_push(&flags, "-DHAVE___CXA_THREAD_ATEXIT_IMPL");
        }
      }
      char *object = path_join(work, str_fmt("%s-%zu.o", library, i));
      e = runtime_compile(source, object, flags, tc, o.verbose);
      if (e)
        goto fail;
      sv_push(&objects, object);
    }
    e = runtime_archive(objects, path_join(work, str_fmt("lib%s.a", library)),
                        tc, o.verbose);
    if (e)
      goto fail;
  }
  e = runtime_publish(work, dest);
  if (e)
    goto fail;
  remove_tree(work);
  return dest;
fail:
  remove_tree(work);
  return rt_error(error, e);
}
bool runtime_musl_arch(const char *s) {
  return in_target_inventory("aarch64 arm generic hexagon i386 loongarch64 "
                             "m68k microblaze mips mips64 mipsn32 or1k powerpc "
                             "powerpc64 riscv32 riscv64 s390x sh x32 x86_64",
                             s);
}
char *runtime_musl_arch_name(Target t) {
  if (streq(t.abi, "muslabin32"))
    return "mipsn32";
  if (streq(t.abi, "muslx32"))
    return "x32";
  if (arm_arch(t.arch))
    return "arm";
  if (in_target_inventory("aarch64 aarch64_be", t.arch))
    return "aarch64";
  if (in_target_inventory("mips mipsel", t.arch))
    return "mips";
  if (in_target_inventory("mips64 mips64el", t.arch))
    return "mips64";
  if (in_target_inventory("powerpc64 powerpc64le", t.arch))
    return "powerpc64";
  if (streq(t.arch, "x86"))
    return "i386";
  return t.arch;
}
bool runtime_musl_supported(Target t) {
  if (in_target_inventory("arm armeb aarch64 aarch64_be powerpc64 powerpc64le "
                          "riscv32 riscv64 s390x x86 x86_64",
                          t.arch))
    return true;
  if (in_target_inventory("thumb thumbeb", t.arch))
    return in_target_inventory("musleabi musleabihf", t.abi);
  if (in_target_inventory("mips64 mips64el", t.arch))
    return streq(t.abi, "muslabi64");
  if (in_target_inventory("mips mipsel", t.arch))
    return streq(t.abi, "musleabi");
  return in_target_inventory("loongarch64 hexagon", t.arch) &&
         streq(t.abi, "musl");
}
bool runtime_musl_ptr64(const char *arch) {
  return in_target_inventory(
      "aarch64 aarch64_be loongarch64 mips64 mips64el powerpc64 powerpc64le "
      "riscv64 s390x x86_64 sparc64 wasm64",
      arch);
}
bool runtime_musl_time32(const char *arch) {
  return in_target_inventory(
      "arm i386 m68k microblaze mips mipsn32 or1k powerpc sh", arch);
}
char *runtime_build_musl(Options o, Target t, Toolchain tc, char **error) {
  if (error)
    *error = NULL;
  char *e = NULL, *arch = runtime_musl_arch_name(t),
       *dest = runtime_cache("musl", o, t, tc);
  if (file_exists(path_join(dest, "complete")))
    return dest;
  ensure_dir(tc.cache_dir);
  char *work = make_temp_dir(tc.cache_dir, "musl-build-", &e);
  if (e)
    return rt_error(error,
                    str_fmt("cannot create musl build directory: %s", e));
  StrVec sources = sv_copy(musl_src_files);
  sv_extend(&sources, musl_supplement_files);
  if (runtime_musl_time32(arch)) {
    StrVec names = str_split(
        "adjtime32 adjtimex_time32 aio_suspend_time32 clock_adjtime32 "
        "clock_getres_time32 clock_gettime32 clock_nanosleep_time32 "
        "clock_settime32 cnd_timedwait_time32 ctime32 ctime32_r difftime32 "
        "fstatat_time32 fstat_time32 ftime32 futimens_time32 futimesat_time32 "
        "futimes_time32 getitimer_time32 getrusage_time32 gettimeofday_time32 "
        "gmtime32 gmtime32_r localtime32 localtime32_r lstat_time32 "
        "lutimes_time32 mktime32 mq_timedreceive_time32 mq_timedsend_time32 "
        "mtx_timedlock_time32 nanosleep_time32 ppoll_time32 pselect_time32 "
        "pthread_cond_timedwait_time32 pthread_mutex_timedlock_time32 "
        "pthread_rwlock_timedrdlock_time32 pthread_rwlock_timedwrlock_time32 "
        "pthread_timedjoin_np_time32 recvmmsg_time32 "
        "sched_rr_get_interval_time32 select_time32 semtimedop_time32 "
        "sem_timedwait_time32 setitimer_time32 settimeofday_time32 "
        "sigtimedwait_time32 stat_time32 stime32 thrd_sleep_time32 time32 "
        "time32gm timerfd_gettime32 timerfd_settime32 timer_gettime32 "
        "timer_settime32 timespec_get_time32 utimensat_time32 utimes_time32 "
        "utime_time32 wait3_time32 wait4_time32 __xstat",
        " ");
    for (size_t i = 0; i < names.len; i++)
      sv_push(&sources, str_fmt("musl/compat/time32/%s.c", names.data[i]));
  }
  StrVec base = runtime_base_flags(o, t, tc);
  RT_ADD(&base, "-std=c99", "-ffreestanding", "-fno-builtin",
         "-fexcess-precision=standard", "-frounding-math", "-ffp-contract=off",
         "-fno-strict-aliasing", "-Wa,--noexecstack", "-D_XOPEN_SOURCE=700");
  StrVec dirs = SV(str_fmt("arch/%s", arch), "arch/generic", "src/include",
                   "src/internal", "include");
  for (size_t i = 0; i < dirs.len; i++) {
    char *dir = dirs.data[i],
         *supplement = path_join(tc.root, "vendor", "musl", dir);
    bool first = streq(arch, "hexagon") && streq(dir, "include");
    if (first)
      RT_ADD(&base, "-I", path_join(tc.lib_dir, "libc", "musl", dir));
    if (is_dir(supplement))
      RT_ADD(&base, "-I", supplement);
    if (!first)
      RT_ADD(&base, "-I", path_join(tc.lib_dir, "libc", "musl", dir));
  }
  StrVec objects = {0};
  for (size_t i = 0; i < sources.len; i++) {
    char *source = sources.data[i], *slash = strrchr(source, '/'),
         *name = slash ? slash + 1 : source,
         *dir =
             slash ? str_ncopy(source, (size_t)(slash - source)) : str_copy("");
    char *p = strrchr(dir, '/'), *parent = p ? p + 1 : dir;
    if (runtime_musl_arch(parent)) {
      if (!streq(parent, arch))
        continue;
    } else {
      char *ext = path_ext(name),
           *stem = str_ncopy(name, strlen(name) - strlen(ext));
      bool override = false;
      StrVec exts = SV(".s", ".S", ".c");
      for (size_t j = 0; j < exts.len; j++)
        if (rt_has(sources,
                   str_fmt("%s/%s/%s%s", dir, arch, stem, exts.data[j]))) {
          override = true;
          break;
        }
      if (override)
        continue;
    }
    StrVec flags = sv_copy(base);
    sv_push(&flags, (starts_with(source, "musl/src/string/") ||
                     starts_with(source, "musl/src/internal/"))
                        ? "-O3"
                        : "-Os");
    char *object = path_join(work, str_fmt("musl-%zu.o", objects.len));
    RT_ADD(&flags, "-iquote", path_join(tc.lib_dir, "libc", path_dir(source)),
           "-iquote", path_join(tc.root, "vendor", path_dir(source)));
    char *source_path = first_existing(path_join(tc.lib_dir, "libc", source),
                                       path_join(tc.root, "vendor", source));
    if (!*nz(source_path)) {
      e = str_fmt("missing musl source: %s (need vendor/musl supplement)",
                  source);
      goto fail;
    }
    e = runtime_compile(source_path, object, flags, tc, o.verbose);
    if (e)
      goto fail;
    sv_push(&objects, object);
  }
  e = runtime_archive(objects, path_join(work, "libc.a"), tc, o.verbose);
  if (e)
    goto fail;
  StrVec crts = SV("crt1", "Scrt1", "rcrt1");
  for (size_t i = 0; i < crts.len; i++) {
    StrVec flags = sv_copy(base);
    RT_ADD(&flags, "-Os", "-DCRT");
    e = runtime_compile(path_join(tc.lib_dir, "libc", "musl", "crt",
                                  str_fmt("%s.c", crts.data[i])),
                        path_join(work, str_fmt("%s.o", crts.data[i])), flags,
                        tc, o.verbose);
    if (e)
      goto fail;
  }
  e = runtime_build_musl_libc_so(base, work, o, t, tc);
  if (e)
    goto fail;
  e = runtime_publish(work, dest);
  if (e)
    goto fail;
  remove_tree(work);
  return dest;
fail:
  remove_tree(work);
  return rt_error(error, e);
}
char *runtime_build_musl_libc_so(StrVec base, const char *work, Options o,
                                 Target t, Toolchain tc) {
  StrVec flags = sv_copy(base);
  char *arch = runtime_musl_arch_name(t);
  const char *family = NULL;
  if (arm_arch(t.arch))
    family = "arm";
  else if (in_target_inventory("aarch64 aarch64_be", t.arch))
    family = "aarch64";
  else if (streq(t.arch, "loongarch64"))
    family = "loongarch";
  else if (in_target_inventory("mips mipsel mips64 mips64el", t.arch))
    family = "mips";
  else if (in_target_inventory("powerpc powerpc64 powerpc64le", t.arch))
    family = "powerpc";
  else if (in_target_inventory("riscv32 riscv64", t.arch))
    family = "riscv";
  else if (streq(t.arch, "s390x"))
    family = "s390x";
  else if (in_target_inventory("x86 x86_64", t.arch))
    family = "x86";
  else if (in_target_inventory("m68k hexagon", t.arch))
    family = t.arch;
  if (!family)
    return str_fmt("musl libc.so is not implemented for %s", nz(t.arch));
  if (runtime_musl_ptr64(t.arch) && !streq(t.abi, "muslx32"))
    sv_push(&flags, "-DPTR64");
  else if (runtime_musl_time32(arch))
    sv_push(&flags, "-DTIME32");
  RT_ADD(&flags, str_fmt("-DARCH_%s", arch), str_fmt("-DFAMILY_%s", family));
  char *object = path_join(work, "libc-so.o");
  char *e = runtime_compile(path_join(tc.lib_dir, "libc", "musl", "libc.S"),
                            object, flags, tc, o.verbose);
  if (e)
    return e;
  return run_command(SV(tc.lld, "-shared", "-soname", "libc.so", "-o",
                        path_join(work, "libc.so"), object),
                     o.verbose)
             ? "linking musl libc.so failed"
             : NULL;
}

bool linux_libc_name(Target t, const char *name) {
  return in_target_inventory("c m rt pthread util resolv dl", name) ||
         (starts_with(t.abi, "musl") &&
          in_target_inventory("crypt xnet", name));
}
char *linux_library_dir(const char *dir, const char *sysroot) {
  if (*nz(sysroot) && rt_abs(dir)) {
    while (*dir == '/' || *dir == '\\')
      dir++;
    return path_join(sysroot, dir);
  }
  return str_copy(dir);
}
char linux_library_arg(const char *a, char **value, bool *separate) {
  *value = "";
  *separate = false;
  if (streq(a, "-l") || streq(a, "--library")) {
    *separate = true;
    return 'l';
  }
  if (streq(a, "-L") || streq(a, "--library-directory")) {
    *separate = true;
    return 'L';
  }
  if (starts_with(a, "-l") && strlen(a) > 2) {
    *value = (char *)a + 2;
    return 'l';
  }
  if (starts_with(a, "-L") && strlen(a) > 2) {
    *value = (char *)a + 2;
    return 'L';
  }
  if (starts_with(a, "--library=")) {
    *value = (char *)a + 10;
    return 'l';
  }
  if (starts_with(a, "--library-directory=")) {
    *value = (char *)a + 20;
    return 'L';
  }
  return 0;
}
static bool linker_strip(const char *f) {
  return in_target_inventory("-s --strip-all -S --strip-debug", f);
}
bool linux_strip_link(Options o) {
  char *optimization = "-O0";
  int strip = -1;
  for (size_t i = 0; i < o.forwarded.len; i++) {
    char *a = o.forwarded.data[i];
    if (streq(a, "--"))
      break;
    bool known = false;
    char *flag = compiler_optimization(a, &known);
    if (known)
      optimization = flag;
    else if (streq(a, "-s"))
      strip = 1;
    else if (debug_info_flag(a))
      strip = 0;
    else if (starts_with(a, "-Wl,")) {
      StrVec fs = str_split(a + 4, ",");
      for (size_t j = 0; j < fs.len; j++)
        if (linker_strip(fs.data[j]))
          return true;
    } else if ((streq(a, "-Xlinker") || streq(a, "--for-linker")) &&
               i + 1 < o.forwarded.len) {
      if (linker_strip(o.forwarded.data[i + 1]))
        return true;
    }
    if (takes_value(a))
      i++;
  }
  return strip < 0 ? streq(optimization, "-Os") : strip == 1;
}
bool linux_shared_input(const char *path, StrVec dirs, StrVec parents,
                        char **error) {
  if (error)
    *error = NULL;
  FILE *f = open_file(path, "rb");
  if (!f)
    return false;
  unsigned char header[20];
  size_t n = fread(header, 1, sizeof(header), f);
  if (n >= 18 && !memcmp(header, "\177ELF", 4)) {
    fclose(f);
    return header[5] == 1 ? (header[16] == 3 && header[17] == 0)
                          : (header[16] == 0 && header[17] == 3);
  }
  if ((n >= 8 &&
       (!memcmp(header, "!<arch>\n", 8) || !memcmp(header, "!<thin>\n", 8))) ||
      (n >= 4 && (!memcmp(header, "BC\300\336", 4) ||
                  !memcmp(header, "\336\300\027\013", 4)))) {
    fclose(f);
    return false;
  }
  if (!in_target_inventory(".so .ld .lds .a .o", path_ext(path))) {
    fclose(f);
    return false;
  }
  char *identity = absolute_path(path), *e = NULL;
  if (parents.len >= 8) {
    e = str_fmt("linker script nesting limit: %s", path);
    goto fail;
  }
  if (rt_has(parents, identity)) {
    e = str_fmt("cyclic linker script: %s", path);
    goto fail;
  }
  StrVec chain = sv_copy(parents);
  sv_push(&chain, identity);
  char data[65538];
  rewind(f);
  size_t size = fread(data, 1, 65537, f);
  if (size > 65536) {
    e = str_fmt("linker script exceeds 64 KiB: %s", path);
    goto fail;
  }
  data[size] = 0;
  StrVec tokens = {0};
  for (size_t i = 0; i < size;) {
    char c = data[i];
    if (strchr(" \t\r\n,;", c)) {
      i++;
      continue;
    }
    if (c == '/' && i + 1 < size && data[i + 1] == '*') {
      char *end = strstr(data + i + 2, "*/");
      if (!end) {
        e = str_fmt("unterminated linker script comment: %s", path);
        goto fail;
      }
      i = (size_t)(end - data) + 2;
      continue;
    }
    if (c == '(' || c == ')') {
      sv_push(&tokens, str_ncopy(data + i, 1));
      i++;
      continue;
    }
    size_t start = i;
    if (c == '"' || c == '\'') {
      start = ++i;
      while (i < size && data[i] != c)
        i++;
      if (i == size) {
        e = str_fmt("unterminated linker script quote: %s", path);
        goto fail;
      }
      sv_push(&tokens, str_ncopy(data + start, i - start));
      i++;
    } else {
      while (i < size && !strchr(" \t\r\n,;()", data[i]))
        i++;
      sv_push(&tokens, str_ncopy(data + start, i - start));
    }
  }
  bool *lists = xmalloc((tokens.len + 1) * sizeof(bool));
  size_t depth = 0;
  bool shared = false;
  for (size_t i = 0; i < tokens.len; i++) {
    char *token = tokens.data[i];
    if (streq(token, ")")) {
      if (!depth) {
        e = str_fmt("unbalanced linker script: %s", path);
        goto fail;
      }
      depth--;
      continue;
    }
    if (depth && !lists[depth - 1])
      continue;
    if (in_target_inventory("INPUT GROUP AS_NEEDED OUTPUT_FORMAT OUTPUT_ARCH",
                            token)) {
      if (i + 1 >= tokens.len || !streq(tokens.data[i + 1], "(")) {
        e = str_fmt("unsupported linker script syntax: %s", path);
        goto fail;
      }
      lists[depth++] = !in_target_inventory("OUTPUT_FORMAT OUTPUT_ARCH", token);
      i++;
      continue;
    }
    if (!depth || streq(token, "(")) {
      e = str_fmt("unsupported linker script directive %s in %s", token, path);
      goto fail;
    }
    char *input = "";
    StrVec search = SV(path_dir(path));
    sv_extend(&search, dirs);
    if (starts_with(token, "-l")) {
      for (size_t j = 0; j < search.len; j++) {
        input = first_existing(
            path_join(search.data[j], str_fmt("lib%s.so", token + 2)),
            path_join(search.data[j], str_fmt("lib%s.a", token + 2)));
        if (*nz(input))
          break;
      }
    } else {
      input = first_existing(token);
      if (!*nz(input) && !rt_abs(token)) {
        for (size_t j = 0; j < search.len; j++) {
          input = first_existing(path_join(search.data[j], token));
          if (*nz(input))
            break;
        }
      }
    }
    if (!*nz(input)) {
      e = str_fmt("unable to resolve linker script input %s in %s", token,
                  path);
      goto fail;
    }
    bool dynamic = linux_shared_input(input, dirs, chain, &e);
    if (e)
      goto fail;
    shared |= dynamic;
  }
  if (depth) {
    e = str_fmt("unbalanced linker script: %s", path);
    goto fail;
  }
  fclose(f);
  return shared;
fail:
  fclose(f);
  if (error)
    *error = e;
  return false;
}
StrVec linux_link_arguments(Options o, Target t, StrVec args, char **error) {
  if (error)
    *error = NULL;
  if (!streq(t.os, "linux") || !needs_link(o) || o.relocatable)
    return args;
  bool isolated = !t.native || *nz(o.sysroot);
  StrVec dirs = {0}, result = {0};
  char *e = NULL;
  for (size_t i = 0; i < args.len; i++) {
    char *a = args.data[i], *value;
    bool separate;
    if (streq(a, "--"))
      break;
    char kind = linux_library_arg(a, &value, &separate);
    if (kind) {
      if (separate) {
        i++;
        if (i >= args.len) {
          e = str_fmt("missing argument to %s", a);
          goto fail;
        }
        value = args.data[i];
      }
      if (kind == 'L')
        sv_push(&dirs, linux_library_dir(value, o.sysroot));
    } else if (takes_value(a))
      i++;
  }
  int mode = o.static_link ? 1 : 0;
  int *states = xmalloc((args.len + 1) * sizeof(int));
  size_t nstates = 0;
  bool bare =
           !o.explicit_libc && !o.explicit_libcxx && (o.no_stdlib || o.no_libc),
       shared_input = false;
  for (size_t i = 0; i < args.len; i++) {
    char *a = args.data[i];
    if (streq(a, "--")) {
      if (bare)
        for (size_t j = i + 1; j < args.len; j++) {
          bool s = linux_shared_input(args.data[j], dirs, (StrVec){0}, &e);
          if (e)
            goto fail;
          shared_input |= s;
        }
      sv_extend(&result, sv_slice(args, i, args.len));
      break;
    }
    StrVec linker_args = {0};
    if (starts_with(a, "-Wl,"))
      linker_args = str_split(a + 4, ",");
    if ((streq(a, "-Xlinker") || streq(a, "--for-linker")) && i + 1 < args.len)
      linker_args = sv_slice(args, i + 1, i + 2);
    for (size_t j = 0; j < linker_args.len; j++) {
      char *flag = linker_args.data[j], *v;
      bool sep;
      if (linux_library_arg(flag, &v, &sep)) {
        e = str_fmt("unsupported linker arg: %s", flag);
        goto fail;
      }
      if (in_target_inventory("-Bstatic -dn -non_shared -static", flag))
        mode = 1;
      else if (in_target_inventory("-Bdynamic -dy -call_shared", flag))
        mode = 2;
      else if (streq(flag, "--push-state"))
        states[nstates++] = mode;
      else if (streq(flag, "--pop-state") && nstates)
        mode = states[--nstates];
      if (bare && !starts_with(flag, "-")) {
        bool s = linux_shared_input(flag, dirs, (StrVec){0}, &e);
        if (e)
          goto fail;
        shared_input |= s;
      }
    }
    char *value;
    bool separate;
    char kind = linux_library_arg(a, &value, &separate);
    if (!kind) {
      if (bare && !starts_with(a, "-")) {
        bool s = linux_shared_input(a, dirs, (StrVec){0}, &e);
        if (e)
          goto fail;
        shared_input |= s;
      }
      sv_push(&result, a);
      if (takes_value(a) && i + 1 < args.len)
        sv_push(&result, args.data[++i]);
      continue;
    }
    if (separate)
      value = args.data[++i];
    if (kind == 'L') {
      RT_ADD(&result, "-L", linux_library_dir(value, o.sysroot));
      continue;
    }
    if ((o.explicit_libc || o.explicit_libcxx ||
         (!o.no_stdlib && !o.no_defaultlibs)) &&
        linux_libc_name(t, value))
      continue;
    bool exact = starts_with(value, ":");
    char *name = exact ? value + 1 : value;
    if (isolated &&
        (!*name || rt_abs(name) || strchr(name, '/') || strchr(name, '\\'))) {
      e = str_fmt("invalid system library name: %s", value);
      goto fail;
    }
    char *found = "";
    if (isolated || bare) {
      for (size_t j = 0; j < dirs.len; j++) {
        if (exact)
          found = first_existing(path_join(dirs.data[j], name));
        else {
          if (mode != 1)
            found = first_existing(
                path_join(dirs.data[j], str_fmt("lib%s.so", name)));
          if (!*nz(found) && mode != 2)
            found = first_existing(
                path_join(dirs.data[j], str_fmt("lib%s.a", name)));
        }
        if (*nz(found))
          break;
      }
      if (isolated && !*nz(found)) {
        e = str_fmt(
            "unable to find system library %s in explicit -L directories",
            value);
        goto fail;
      }
    }
    if (bare) {
      if (!*nz(found)) {
        e = str_fmt("cannot determine bare link mode for library %s; add an "
                    "explicit -L directory or direct library path",
                    value);
        goto fail;
      }
      bool s = linux_shared_input(found, dirs, (StrVec){0}, &e);
      if (e)
        goto fail;
      shared_input |= s;
    }
    RT_ADD(&result, "-l", value);
  }
  if (bare && !o.shared) {
    if (o.static_link && shared_input) {
      e = "using shared libraries requires dynamic linking";
      goto fail;
    }
    if (o.dynamic_link && !shared_input) {
      e = "dynamic linking with lld requires at least one shared library";
      goto fail;
    }
    if (!shared_input)
      rt_insert(&result, 0,
                o.pie == 1 ? "-Wl,-pie,--no-dynamic-linker"
                           : "-Wl,--no-pie,--no-dynamic-linker");
    else {
      if (o.pie == 0)
        rt_insert(&result, 0, "-Wl,--no-pie");
      if (*nz(t.dynamic_linker))
        rt_insert(&result, 0,
                  str_fmt("-Wl,--dynamic-linker=%s", t.dynamic_linker));
    }
  }
  if (linux_strip_link(o)) {
    size_t at = result.len;
    for (size_t i = 0; i < result.len; i++)
      if (streq(result.data[i], "--")) {
        at = i;
        break;
      }
    rt_insert(&result, at, "-Wl,-s");
  } else {
    StrVec kept = {0};
    bool value = false, operands = false;
    for (size_t i = 0; i < result.len; i++) {
      char *a = result.data[i];
      if (!value && !operands && streq(a, "--"))
        operands = true;
      if (!streq(a, "-s") || value || operands)
        sv_push(&kept, a);
      value = !value && !operands && takes_value(a);
    }
    return kept;
  }
  return result;
fail:
  if (error)
    *error = e;
  return (StrVec){0};
}
Options linux_libc_runtime_options(Options o) {
  if (o.explicit_libcxx)
    o.explicit_libc = true;
  if (o.no_stdlib) {
    if (!o.explicit_libcxx)
      o.cxx = false;
    if (o.explicit_libc) {
      o.no_stdlib = false;
      o.no_defaultlibs = false;
      o.no_startfiles = false;
    }
  }
  return o;
}
Runtime prepare_runtime(Options options, Target t, Toolchain tools,
                        char **error) {
  if (error)
    *error = NULL;
  Runtime r = {0};
  Options o = options;
  Toolchain tc = tools;
  char *e = NULL;
  if (streq(t.os, "linux")) {
    o = linux_libc_runtime_options(options);
    if (options.no_stdlib)
      tc.builtins = "";
  }
  if (streq(t.os, "linux") && *nz(env("ZIG_LIBC"))) {
    Libc_Config l = libc_config_from_file(env("ZIG_LIBC"), t, &e);
    if (e)
      goto fail;
    return prepare_linux_external_libc(options, t, tools, l, error);
  }
  if (streq(t.os, "freestanding")) {
    sv_push(&r.compile_flags, "-ffreestanding");
    if (!o.no_stdinc && !*nz(o.sysroot))
      RT_ADD(&r.compile_flags, "-nostdlibinc", "-isystem",
             path_join(tc.lib_dir, "include"));
    return r;
  }
  if (o.cxx && !o.no_stdinc && !o.no_stdincxx)
    sv_extend(&r.compile_flags, runtime_cxx_flags(t, tc.lib_dir));
  if (!o.no_stdinc && (streq(t.os, "linux") || !*nz(o.sysroot))) {
    RT_ADD(&r.compile_flags, "-nostdlibinc", "-isystem",
           path_join(tc.lib_dir, "include"));
    if (((!o.no_stdlib || !streq(t.os, "linux")) && !o.no_libc) ||
        o.explicit_libc) {
      sv_extend(&r.compile_flags, libc_include_flags(t, tc.lib_dir));
      sv_extend(&r.compile_flags, runtime_glibc_version_flags(t));
    }
  }
  if (!needs_link(o) || o.no_stdlib || o.relocatable)
    return r;
  if (o.no_libc && !o.explicit_libc) {
    sv_push(&r.link_flags, "-nostdlib");
    return r;
  }
  if (!streq(t.os, "linux") && *nz(o.sysroot))
    return r;
  bool musl = streq(t.os, "linux") && starts_with(t.abi, "musl"),
       gnu = streq(t.os, "linux") && starts_with(t.abi, "gnu");
  if (!musl && !gnu) {
    e = str_fmt("runtime linking is not implemented for %s; provide --sysroot "
                "or -nostdlib",
                nz(t.name));
    goto fail;
  }
  sv_push(&r.link_flags, "-nostdlib");
  char *libc_dir = "";
  if (musl) {
    if (!runtime_musl_supported(t)) {
      e = "unsupported musl runtime architecture";
      goto fail;
    }
    if (!o.no_startfiles || !o.no_defaultlibs) {
      libc_dir = runtime_build_musl(o, t, tc, &e);
      if (e)
        goto fail;
    }
    if (!o.no_startfiles && !o.shared) {
      const char *crt = "crt1.o";
      if (o.static_link) {
        if (o.pie == 1) {
          crt = "rcrt1.o";
          RT_ADD(&r.link_flags, "-Wl,-pie", "-Wl,--no-dynamic-linker");
        }
      } else {
        if (o.pie == 1) {
          crt = "Scrt1.o";
          sv_push(&r.link_flags, "-Wl,-pie");
        } else if (!streq(t.arch, "hexagon"))
          sv_push(&r.link_flags, "-no-pie");
        if (*nz(t.dynamic_linker))
          sv_push(&r.link_flags,
                  str_fmt("-Wl,--dynamic-linker=%s", t.dynamic_linker));
      }
      sv_push(&r.link_flags, path_join(libc_dir, crt));
    }
  } else {
    Runtime g = prepare_glibc(o, t, tc, &e);
    if (e)
      goto fail;
    sv_extend(&r.link_flags, g.link_flags);
  }
  if (!o.no_defaultlibs) {
    if (o.cxx) {
      char *dir = runtime_build_cxx(o, t, tc, false, &e);
      if (e)
        goto fail;
      RT_ADD(&r.link_flags, "-Wl,--start-group", path_join(dir, "libc++.a"),
             path_join(dir, "libc++abi.a"), path_join(dir, "libunwind.a"),
             "-Wl,--end-group");
    }
    if (musl) {
      if (o.shared)
        RT_ADD(&r.link_flags, "-Wl,--push-state,--as-needed,-Bdynamic",
               path_join(libc_dir, "libc.so"), "-Wl,--pop-state");
      else if (o.static_link)
        sv_push(&r.link_flags, path_join(libc_dir, "libc.a"));
      else
        RT_ADD(&r.link_flags, "-Wl,--push-state,-Bdynamic",
               path_join(libc_dir, "libc.so"), "-Wl,--pop-state");
    }
    if (*nz(tc.builtins))
      sv_push(&r.link_flags, tc.builtins);
  }
  return r;
fail:
  if (error)
    *error = e;
  return r;
}
