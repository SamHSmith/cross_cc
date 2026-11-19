#include "platforms.h"
/* Pinned recipe: Zig 0.17.1 src/libs/libtsan.zig, MIT license.
   Copyright (c) Zig contributors. See licenses/runtime-recipes.txt. */
extern StrVec tsan_sources, tsan_darwin_sources, tsan_unix_sources,
    tsan_windows_sources;
extern StrVec tsan_sanitizer_common_sources, tsan_sanitizer_nolibc_sources;
extern StrVec tsan_sanitizer_libcdep_sources, tsan_sanitizer_symbolizer_sources,
    tsan_interception_sources;
bool tsan_enabled(Options o) {
  return compiler_sanitizer_enabled(o, SV("thread"));
}
char *tsan_asm_source(const char *arch) {
  if (streq(arch, "aarch64") || streq(arch, "aarch64_be"))
    return "tsan_rtl_aarch64.S";
  if (streq(arch, "loongarch64"))
    return "tsan_rtl_loongarch64.S";
  if (streq(arch, "mips64") || streq(arch, "mips64el"))
    return "tsan_rtl_mips64.S";
  if (streq(arch, "powerpc64") || streq(arch, "powerpc64le"))
    return "tsan_rtl_ppc64.S";
  if (streq(arch, "riscv64"))
    return "tsan_rtl_riscv64.S";
  if (streq(arch, "s390x"))
    return "tsan_rtl_s390x.S";
  if (streq(arch, "x86_64"))
    return "tsan_rtl_amd64.S";
  return "";
}
char *tsan_build(Options o, Target t, Toolchain tc, char **error) {
  *error = NULL;
  char *root = path_join(tc.lib_dir, "libtsan");
  if (!file_exists(path_join(root, "tsan_rtl.cpp")))
    return rt_error(error,
                    "bundled ThreadSanitizer sources not found (lib/libtsan); "
                    "run setup or reinstall the complete toolchain");
  StrVec flags = runtime_base_flags(o, t, tc);
  sv_extend(&flags, SV("-O2", "-DNDEBUG", "-DTSAN_CONTAINS_UBSAN=0",
                       "-fno-builtin", "-fno-lto", "-fno-sanitize=all",
                       "-fasynchronous-unwind-tables", "-fomit-frame-pointer"));
  char *identity = str_fmt("tsan-recipe-v2-bundled-unwind|%s|%s|%s|%s|%s",
                           UPSTREAM_COMMIT, stable_compiler_identity(tc),
                           tc.lib_dir, t.clang, rt_join(flags, "\n"));
  char *dest =
           path_join(tc.cache_dir,
                     str_fmt("tsan-%s", str_ncopy(sha256_text(identity), 32))),
       *archive = path_join(dest, "libtsan.a");
  if (file_exists(path_join(dest, "complete")) && file_exists(archive))
    return archive;
  ensure_dir(tc.cache_dir);
  char *work = make_temp_dir(tc.cache_dir, "tsan-build-", error);
  if (*error)
    return NULL;
  StrVec sources = sv_copy(tsan_sources);
  sv_extend(&sources, tsan_unix_sources);
  StrVec lists[] = {tsan_sanitizer_common_sources,
                    tsan_sanitizer_libcdep_sources,
                    tsan_sanitizer_symbolizer_sources};
  for (size_t i = 0; i < 3; i++)
    for (size_t j = 0; j < lists[i].len; j++)
      sv_push(&sources, path_join("sanitizer_common", lists[i].data[j]));
  for (size_t i = 0; i < tsan_interception_sources.len; i++)
    sv_push(&sources,
            path_join("interception", tsan_interception_sources.data[i]));
  sv_push(&sources, tsan_asm_source(t.arch));
  Builtins_Job *jobs = xmalloc(sources.len * sizeof(*jobs));
  StrVec objects = {0};
  for (size_t i = 0; i < sources.len; i++) {
    const char *source = sources.data[i];
    StrVec args = SV(tc.clang);
    sv_extend(&args, flags);
    sv_extend(&args, SV("-I", root));
    if (starts_with(source, "sanitizer_common/"))
      sv_extend(&args, SV("-I", path_join(root, "sanitizer_common")));
    if (starts_with(source, "interception/"))
      sv_extend(&args, SV("-I", path_join(tc.lib_dir, "interception")));
    if (ends_with(source, ".cpp"))
      sv_extend(&args, SV("-nostdinc++", "-fvisibility=hidden",
                          "-fvisibility-inlines-hidden", "-std=c++17",
                          "-fno-rtti", "-fno-exceptions"));
    char *object = path_join(work, str_fmt("tsan-%zu.o", i));
    sv_extend(&args, SV("-c", path_join(root, source), "-o", object));
    jobs[i] = (Builtins_Job){args, object};
    sv_push(&objects, object);
  }
  char *bad = builtins_run_jobs(jobs, sources.len, o.verbose);
  free(jobs);
  if (bad) {
    *error = str_fmt("ThreadSanitizer runtime compilation failed: %s", bad);
    goto fail;
  }
  *error =
      runtime_archive(objects, path_join(work, "libtsan.a"), tc, o.verbose);
  if (*error)
    goto fail;
  for (size_t i = 0; i < objects.len; i++)
    remove_file(objects.data[i]);
  StrVec unwind_objects = {0};
  StrVec unwind_sources =
      SV("libunwind.cpp", "Unwind-EHABI.cpp", "Unwind-seh.cpp",
         "UnwindLevel1.c", "UnwindLevel1-gcc-ext.c", "Unwind-sjlj.c",
         "Unwind-wasm.c", "UnwindRegistersRestore.S", "UnwindRegistersSave.S",
         "gcc_personality_v0.c");
  for (size_t i = 0; i < unwind_sources.len; i++) {
    const char *source = unwind_sources.data[i];
    StrVec f = sv_copy(flags);
    sv_extend(&f, SV("-I", path_join(tc.lib_dir, "libunwind", "include"),
                     "-D_LIBUNWIND_HIDE_SYMBOLS", "-D_LIBUNWIND_IS_NATIVE_ONLY",
                     "-fvisibility=hidden", "-fvisibility-inlines-hidden",
                     "-fvisibility-global-new-delete=force-hidden",
                     "-Wa,--noexecstack"));
    if (ends_with(source, ".cpp")) {
      sv_extend(&f, runtime_cxx_flags(t, tc.lib_dir));
      sv_extend(&f, SV("-std=c++23", "-fno-exceptions", "-fno-rtti"));
    } else if (ends_with(source, ".c"))
      sv_extend(&f, SV("-std=c99", "-fexceptions"));
    char *object = path_join(work, str_fmt("unwind-%zu.o", i));
    *error = runtime_compile(path_join(tc.lib_dir, "libunwind", "src", source),
                             object, f, tc, o.verbose);
    if (*error)
      goto fail;
    sv_push(&unwind_objects, object);
  }
  *error = runtime_archive(unwind_objects, path_join(work, "libunwind.a"), tc,
                           o.verbose);
  if (*error)
    goto fail;
  for (size_t i = 0; i < unwind_objects.len; i++)
    remove_file(unwind_objects.data[i]);
  *error = runtime_publish(work, dest);
  if (*error)
    goto fail;
  remove_tree(work);
  return archive;
fail:
  remove_tree(work);
  return NULL;
}
Runtime prepare_tsan(Options o, Target t, Toolchain tc, char **error) {
  Runtime r = {0};
  *error = NULL;
  if (!tsan_enabled(o))
    return r;
  bool link_runtime = true;
  for (size_t i = 0; i < o.forwarded.len; i++) {
    if (streq(o.forwarded.data[i], "-fno-sanitize-link-runtime"))
      link_runtime = false;
    if (streq(o.forwarded.data[i], "-fsanitize-link-runtime"))
      link_runtime = true;
  }
  if (!link_runtime || !needs_link(o) || o.no_stdlib || o.no_defaultlibs ||
      o.relocatable)
    return r;
  if (o.shared) {
    sv_push(&r.link_flags, "-fno-sanitize-link-runtime");
    return r;
  }
  bool gnu = starts_with(t.abi, "gnu"), musl = starts_with(t.abi, "musl");
  if (!streq(t.os, "linux") || !(gnu || musl) ||
      streq(tsan_asm_source(t.arch), "")) {
    *error = str_fmt("ThreadSanitizer runtime for %s is not supported: "
                     "currently Linux GNU/musl 64-bit targets only",
                     t.name);
    return r;
  }
  if (gnu && o.static_link) {
    *error = "ThreadSanitizer requires dynamically linked libc; -static is not "
             "supported";
    return r;
  }
  if (musl && o.static_link) {
    *error = "ThreadSanitizer on musl currently requires -dynamic; static musl "
             "runtime linking is not supported";
    return r;
  }
  char *archive = tsan_build(o, t, tc, error);
  if (*error)
    return r;
  sv_extend(&r.link_flags,
            SV("-fno-sanitize-link-runtime", "-Wl,--whole-archive", archive,
               "-Wl,--no-whole-archive",
               path_join(path_dir(archive), "libunwind.a")));
  if (gnu)
    sv_extend(&r.link_flags, SV("-lpthread", "-ldl", "-lrt", "-lm"));
  return r;
}

static char *tsan_sources_data[] = {"tsan_debugging.cpp",
                                    "tsan_external.cpp",
                                    "tsan_fd.cpp",
                                    "tsan_flags.cpp",
                                    "tsan_ignoreset.cpp",
                                    "tsan_interceptors_memintrinsics.cpp",
                                    "tsan_interceptors_posix.cpp",
                                    "tsan_interface.cpp",
                                    "tsan_interface_ann.cpp",
                                    "tsan_interface_atomic.cpp",
                                    "tsan_interface_java.cpp",
                                    "tsan_malloc_mac.cpp",
                                    "tsan_md5.cpp",
                                    "tsan_mman.cpp",
                                    "tsan_mutexset.cpp",
                                    "tsan_new_delete.cpp",
                                    "tsan_platform_windows.cpp",
                                    "tsan_preinit.cpp",
                                    "tsan_report.cpp",
                                    "tsan_rtl.cpp",
                                    "tsan_rtl_access.cpp",
                                    "tsan_rtl_mutex.cpp",
                                    "tsan_rtl_proc.cpp",
                                    "tsan_rtl_report.cpp",
                                    "tsan_rtl_thread.cpp",
                                    "tsan_stack_trace.cpp",
                                    "tsan_suppressions.cpp",
                                    "tsan_symbolize.cpp",
                                    "tsan_sync.cpp",
                                    "tsan_vector_clock.cpp"};
StrVec tsan_sources = {tsan_sources_data,
                       sizeof(tsan_sources_data) / sizeof(char *), 0};

static char *tsan_darwin_sources_data[] = {
    "tsan_interceptors_mac.cpp", "tsan_interceptors_mach_vm.cpp",
    "tsan_platform_mac.cpp", "tsan_platform_posix.cpp"};
StrVec tsan_darwin_sources = {tsan_darwin_sources_data,
                              sizeof(tsan_darwin_sources_data) / sizeof(char *),
                              0};

static char *tsan_unix_sources_data[] = {"tsan_platform_linux.cpp",
                                         "tsan_platform_posix.cpp"};
StrVec tsan_unix_sources = {tsan_unix_sources_data,
                            sizeof(tsan_unix_sources_data) / sizeof(char *), 0};

static char *tsan_windows_sources_data[] = {"tsan_platform_windows.cpp"};
StrVec tsan_windows_sources = {
    tsan_windows_sources_data,
    sizeof(tsan_windows_sources_data) / sizeof(char *), 0};

static char *tsan_sanitizer_common_sources_data[] = {
    "sanitizer_allocator.cpp",
    "sanitizer_chained_origin_depot.cpp",
    "sanitizer_common.cpp",
    "sanitizer_deadlock_detector1.cpp",
    "sanitizer_deadlock_detector2.cpp",
    "sanitizer_errno.cpp",
    "sanitizer_file.cpp",
    "sanitizer_flag_parser.cpp",
    "sanitizer_flags.cpp",
    "sanitizer_fuchsia.cpp",
    "sanitizer_haiku.cpp",
    "sanitizer_libc.cpp",
    "sanitizer_libignore.cpp",
    "sanitizer_linux.cpp",
    "sanitizer_linux_s390.cpp",
    "sanitizer_mac.cpp",
    "sanitizer_mutex.cpp",
    "sanitizer_netbsd.cpp",
    "sanitizer_platform_limits_freebsd.cpp",
    "sanitizer_platform_limits_linux.cpp",
    "sanitizer_platform_limits_netbsd.cpp",
    "sanitizer_platform_limits_posix.cpp",
    "sanitizer_platform_limits_solaris.cpp",
    "sanitizer_posix.cpp",
    "sanitizer_printf.cpp",
    "sanitizer_procmaps_bsd.cpp",
    "sanitizer_procmaps_common.cpp",
    "sanitizer_procmaps_fuchsia.cpp",
    "sanitizer_procmaps_haiku.cpp",
    "sanitizer_procmaps_linux.cpp",
    "sanitizer_procmaps_mac.cpp",
    "sanitizer_procmaps_solaris.cpp",
    "sanitizer_range.cpp",
    "sanitizer_solaris.cpp",
    "sanitizer_stoptheworld_fuchsia.cpp",
    "sanitizer_stoptheworld_mac.cpp",
    "sanitizer_stoptheworld_win.cpp",
    "sanitizer_suppressions.cpp",
    "sanitizer_termination.cpp",
    "sanitizer_thread_arg_retval.cpp",
    "sanitizer_thread_registry.cpp",
    "sanitizer_tls_get_addr.cpp",
    "sanitizer_type_traits.cpp",
    "sanitizer_win.cpp",
    "sanitizer_win_interception.cpp"};
StrVec tsan_sanitizer_common_sources = {
    tsan_sanitizer_common_sources_data,
    sizeof(tsan_sanitizer_common_sources_data) / sizeof(char *), 0};

static char *tsan_sanitizer_nolibc_sources_data[] = {
    "sanitizer_common_nolibc.cpp"};
StrVec tsan_sanitizer_nolibc_sources = {
    tsan_sanitizer_nolibc_sources_data,
    sizeof(tsan_sanitizer_nolibc_sources_data) / sizeof(char *), 0};

static char *tsan_sanitizer_libcdep_sources_data[] = {
    "sanitizer_common_libcdep.cpp",
    "sanitizer_allocator_checks.cpp",
    "sanitizer_dl.cpp",
    "sanitizer_linux_libcdep.cpp",
    "sanitizer_mac_libcdep.cpp",
    "sanitizer_posix_libcdep.cpp",
    "sanitizer_stoptheworld_linux_libcdep.cpp",
    "sanitizer_stoptheworld_netbsd_libcdep.cpp"};
StrVec tsan_sanitizer_libcdep_sources = {
    tsan_sanitizer_libcdep_sources_data,
    sizeof(tsan_sanitizer_libcdep_sources_data) / sizeof(char *), 0};

static char *tsan_sanitizer_symbolizer_sources_data[] = {
    "sanitizer_allocator_report.cpp",
    "sanitizer_stack_store.cpp",
    "sanitizer_stackdepot.cpp",
    "sanitizer_stacktrace.cpp",
    "sanitizer_stacktrace_libcdep.cpp",
    "sanitizer_stacktrace_printer.cpp",
    "sanitizer_stacktrace_sparc.cpp",
    "sanitizer_symbolizer.cpp",
    "sanitizer_symbolizer_libbacktrace.cpp",
    "sanitizer_symbolizer_libcdep.cpp",
    "sanitizer_symbolizer_mac.cpp",
    "sanitizer_symbolizer_markup.cpp",
    "sanitizer_symbolizer_markup_fuchsia.cpp",
    "sanitizer_symbolizer_posix_libcdep.cpp",
    "sanitizer_symbolizer_report.cpp",
    "sanitizer_symbolizer_report_fuchsia.cpp",
    "sanitizer_symbolizer_win.cpp",
    "sanitizer_thread_history.cpp",
    "sanitizer_unwind_linux_libcdep.cpp",
    "sanitizer_unwind_fuchsia.cpp",
    "sanitizer_unwind_win.cpp"};
StrVec tsan_sanitizer_symbolizer_sources = {
    tsan_sanitizer_symbolizer_sources_data,
    sizeof(tsan_sanitizer_symbolizer_sources_data) / sizeof(char *), 0};

static char *tsan_interception_sources_data[] = {
    "interception_linux.cpp", "interception_mac.cpp", "interception_win.cpp",
    "interception_type_test.cpp"};
StrVec tsan_interception_sources = {
    tsan_interception_sources_data,
    sizeof(tsan_interception_sources_data) / sizeof(char *), 0};
