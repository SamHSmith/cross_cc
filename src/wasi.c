#include "runtime.h"
#include <inttypes.h>

/* wasm32-wasi recipes preserve preview-1 include and archive ordering. */
static char *wasi_vendor_data[] = {
    "musl/src/string/strlen.c", "musl/src/string/strnlen.c",
    "musl/src/string/memchr.c", "musl/src/math/frexpl.c",
    "wasi/libc-bottom-half/cloudlibc/src/libc/unistd/close.c"};
StrVec wasi_vendor_files = {
    wasi_vendor_data, sizeof(wasi_vendor_data) / sizeof(*wasi_vendor_data), 0};
#define WASI_BUILTINS_VERSION "22.1.8"
StrVec wasi_include_flags(Toolchain tc) {
  return SV(
      "-isystem", path_join(tc.lib_dir, "libc", "include", "wasm-wasi-musl"),
      "-isystem", path_join(tc.lib_dir, "libc", "include", "generic-musl"));
}
StrVec wasi_cc_args(Toolchain tc, bool o3) {
  return SV(
      "-std=gnu17", "-fno-trapping-math", "-w", o3 ? "-O3" : "-Os",
      "-mthread-model", "single", "-I",
      path_join(tc.lib_dir, "libc", "wasi", "libc-bottom-half", "cloudlibc",
                "src"),
      "-isystem", path_join(tc.lib_dir, "libc", "include", "wasm-wasi-musl"),
      "-isystem", path_join(tc.lib_dir, "libc", "include", "generic-musl"),
      "-DBULK_MEMORY_THRESHOLD=32");
}
StrVec wasi_bottom_includes(Toolchain tc) {
  char *w = path_join(tc.lib_dir, "libc", "wasi", "libc-bottom-half"),
       *t = path_join(tc.lib_dir, "libc", "wasi", "libc-top-half", "musl");
  return SV("-I", path_join(w, "headers", "private"), "-I",
            path_join(w, "cloudlibc", "src", "include"), "-I",
            path_join(w, "cloudlibc", "src"), "-I",
            path_join(t, "src", "include"), "-I",
            path_join(tc.lib_dir, "libc", "musl", "src", "include"), "-I",
            path_join(t, "src", "internal"), "-I",
            path_join(tc.lib_dir, "libc", "musl", "src", "internal"));
}
StrVec wasi_top_includes(Toolchain tc) {
  char *t = path_join(tc.lib_dir, "libc", "wasi", "libc-top-half");
  return SV("-I", path_join(t, "musl", "src", "include"), "-I",
            path_join(tc.lib_dir, "libc", "musl", "src", "include"), "-I",
            path_join(t, "musl", "src", "internal"), "-I",
            path_join(tc.lib_dir, "libc", "musl", "src", "internal"), "-I",
            path_join(t, "musl", "arch", "wasm32"), "-I",
            path_join(tc.lib_dir, "libc", "musl", "arch", "generic"), "-I",
            path_join(t, "headers", "private"));
}
char *wasi_cache(const char *kind, Options o, Target t, Toolchain tc) {
  char *error = NULL;
  StrVec cpu = cpu_flags(t, o.cpu, o.forwarded, &error);
  char *id =
      str_fmt("wasi-recipe-v7-libc-supplement-%zu|%s|%s|%s|%s|%s|%s",
              wasi_supplement_files.len, stable_compiler_identity(tc),
              nz(tc.lib_dir), nz(t.clang), nz(o.cpu), rt_join(cpu, " "), kind);
  return path_join(tc.cache_dir,
                   str_fmt("wasi-%s-%016" PRIx64, kind, runtime_hash(id)));
}
StrVec wasi_base(Target t, Options o) {
  StrVec b =
      SV("--no-default-config", str_fmt("--target=%s", nz(t.clang)),
         "-ffunction-sections", "-fdata-sections", "-fno-stack-protector");
  char *e = NULL;
  sv_extend(&b, cpu_flags(t, o.cpu, o.forwarded, &e));
  return b;
}
char *wasi_compile_list(StrVec files, const char *root, StrVec base,
                        StrVec extra, const char *work, StrVec *objects,
                        Toolchain tc, bool verbose) {
  for (size_t i = 0; i < files.len; i++) {
    StrVec flags = sv_copy(base);
    RT_ADD(&flags, "-nostdlibinc", "-isystem",
           path_join(tc.lib_dir, "include"));
    sv_extend(&flags, extra);
    char *object = path_join(work, str_fmt("o%zu.o", objects->len));
    char *e = runtime_compile(path_join(root, files.data[i]), object, flags, tc,
                              verbose);
    if (e)
      return e;
    sv_push(objects, object);
  }
  return NULL;
}
char *wasi_build_libc(Options o, Target t, Toolchain tc, char **error) {
  if (error)
    *error = NULL;
  char *e = NULL, *dest = wasi_cache("libc", o, t, tc);
  if (file_exists(path_join(dest, "complete")))
    return dest;
  ensure_dir(tc.cache_dir);
  char *work = make_temp_dir(tc.cache_dir, "wasi-libc-", &e);
  if (e)
    return rt_error(error,
                    str_fmt("cannot create wasi build directory: %s", e));
  char *root = path_join(tc.lib_dir, "libc");
  StrVec base = wasi_base(t, o), o3 = wasi_cc_args(tc, true),
         os = wasi_cc_args(tc, false), objects = {0};
  StrVec bottom = wasi_bottom_includes(tc), top = wasi_top_includes(tc),
         fts = SV("-I", path_join(tc.lib_dir, "libc", "wasi", "fts")),
         sigtop = SV("-D_WASI_EMULATED_SIGNAL");
  sv_extend(&sigtop, top);
  struct Group {
    StrVec files, inc;
  } groups[] = {{wasi_bottom_half_files, bottom},
                {wasi_top_half_files, top},
                {wasi_fts_files, fts},
                {wasi_dl_files, top},
                {wasi_process_clocks_files, {0}},
                {wasi_getpid_files, {0}},
                {wasi_mman_files, {0}},
                {wasi_signal_bottom_files, {0}},
                {wasi_signal_top_files, sigtop}};
  char *vroot = path_join(tc.root, "vendor");
  StrVec vflags = sv_copy(o3);
  sv_extend(&vflags, top);
  RT_ADD(&vflags, "-I", path_join(vroot, "musl", "src", "internal"), "-I",
         path_join(vroot, "musl", "src", "include"), "-I",
         path_join(vroot, "musl", "include"));
  RT_ADD(&vflags, "-I",
         path_join(tc.lib_dir, "libc", "wasi", "libc-bottom-half", "headers",
                   "private"),
         "-I",
         path_join(tc.lib_dir, "libc", "wasi", "libc-bottom-half", "cloudlibc",
                   "src", "include"),
         "-I",
         path_join(tc.lib_dir, "libc", "wasi", "libc-bottom-half", "cloudlibc",
                   "src"));
  e = wasi_compile_list(wasi_vendor_files, vroot, base, vflags, work, &objects,
                        tc, o.verbose);
  if (e)
    goto fail;
  StrVec sflags = sv_copy(o3);
  sv_extend(&sflags, top);
  RT_ADD(&sflags, "-I", path_join(tc.lib_dir, "libc", "musl", "src", "math"));
  e = wasi_compile_list(wasi_supplement_files, vroot, base, sflags, work,
                        &objects, tc, o.verbose);
  if (e)
    goto fail;
  StrVec alloc = SV("-I", path_join(vroot, "wasi", "dlmalloc", "include"));
  sv_extend(&alloc, o3);
  e = wasi_compile_list(SV("wasi/dlmalloc/src/dlmalloc.c"), vroot, base, alloc,
                        work, &objects, tc, o.verbose);
  if (e)
    goto fail;
  for (size_t i = 0; i < sizeof(groups) / sizeof(*groups); i++) {
    StrVec flags = sv_copy(o3);
    sv_extend(&flags, groups[i].inc);
    e = wasi_compile_list(groups[i].files, root, base, flags, work, &objects,
                          tc, o.verbose);
    if (e)
      goto fail;
  }
  e = runtime_archive(objects, path_join(work, "libc.a"), tc, o.verbose);
  if (e)
    goto fail;
  StrVec flags = sv_copy(base);
  RT_ADD(&flags, "-nostdlibinc", "-isystem", path_join(tc.lib_dir, "include"));
  sv_extend(&flags, os);
  sv_extend(&flags, bottom);
  RT_ADD(&flags, "-nostdlibinc", "-isystem", path_join(tc.lib_dir, "include"));
  StrVec crt = SV("crt1-command", "crt1-reactor");
  for (size_t i = 0; i < crt.len; i++) {
    e = runtime_compile(path_join(root, "wasi", "libc-bottom-half", "crt",
                                  str_fmt("%s.c", crt.data[i])),
                        path_join(work, str_fmt("%s.o", crt.data[i])), flags,
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
char *wasi_build_builtins(Options o, Target t, Toolchain tc, char **error) {
  if (error)
    *error = NULL;
  char *src =
      first_existing(path_join(tc.lib_dir, "compiler-rt", "lib", "builtins"),
                     path_join(tc.root, ".cache", "upstream",
                               "compiler-rt-" WASI_BUILTINS_VERSION,
                               "compiler-rt", "lib", "builtins"));
  if (!is_dir(src))
    return rt_error(error, str_fmt("compiler-rt %s sources not found at %s",
                                   WASI_BUILTINS_VERSION, nz(src)));
  char *dest = wasi_cache("builtins-mem1", o, t, tc), *e = NULL;
  if (file_exists(path_join(dest, "complete")))
    return path_join(dest, "libclang_rt.builtins.a");
  ensure_dir(tc.cache_dir);
  char *work = make_temp_dir(tc.cache_dir, "wasi-rt-", &e);
  if (e)
    return rt_error(error,
                    str_fmt("cannot create builtins build directory: %s", e));
  StrVec list = {0};
  for (size_t i = 0; i < BUILTINS_GENERIC.len; i++) {
    char *s = BUILTINS_GENERIC.data[i];
    if (!in_target_inventory(
            "apple_versioning.c os_version_check.c trampoline_setup.c atomic.c",
            s))
      sv_push(&list, s);
  }
  sv_extend(&list, BUILTINS_TF);
  StrVec base = wasi_base(t, o);
  RT_ADD(&base, "-O2", "-std=c11", "-fno-builtin", "-w", "-DVISIBILITY_HIDDEN");
  sv_extend(&base, wasi_include_flags(tc));
  StrVec objects = {0};
  e = wasi_compile_list(list, src, base, (StrVec){0}, work, &objects, tc,
                        o.verbose);
  if (e)
    goto fail;
  char *cpu_error = NULL;
  StrVec cpu = cpu_flags(t, o.cpu, o.forwarded, &cpu_error);
  if (!rt_has(cpu, "+bulk-memory")) {
    char *ir = path_join(work, "memory.ll"), *text = builtins_memory_ir(t);
    if (!write_file(ir, text, strlen(text))) {
      e = str_fmt("cannot write wasm memory IR: %s", strerror(errno));
      goto fail;
    }
    char *object = path_join(work, "memory.o");
    StrVec flags = wasi_base(t, o);
    RT_ADD(&flags, "-O0", "-w");
    e = runtime_compile(ir, object, flags, tc, o.verbose);
    if (e)
      goto fail;
    sv_push(&objects, object);
  }
  e = runtime_archive(objects, path_join(work, "libclang_rt.builtins.a"), tc,
                      o.verbose);
  if (e)
    goto fail;
  e = runtime_publish(work, dest);
  if (e)
    goto fail;
  remove_tree(work);
  return path_join(dest, "libclang_rt.builtins.a");
fail:
  remove_tree(work);
  return rt_error(error, e);
}
Runtime prepare_wasi(Options o, Target t, Toolchain tc, char **error) {
  Runtime r = {0};
  char *e = NULL;
  if (error)
    *error = NULL;
  if (!streq(t.os, "wasi")) {
    e = "prepare_wasi requires a wasm32-wasi target";
    goto fail;
  }
  if (o.cxx && !o.no_stdinc && !o.no_stdincxx) {
    StrVec flags = runtime_cxx_flags(t, tc.lib_dir);
    for (size_t i = 0; i < flags.len; i++) {
      if (streq(flags.data[i], "-mthread-model")) {
        i++;
        continue;
      }
      sv_push(&r.compile_flags, flags.data[i]);
    }
  }
  if (!o.no_stdinc) {
    RT_ADD(&r.compile_flags, "-nostdlibinc", "-isystem",
           path_join(tc.lib_dir, "include"));
    sv_extend(&r.compile_flags, wasi_include_flags(tc));
  }
  if (o.query || o.dry_run || !o.has_input || !needs_link(o) || o.no_stdlib ||
      o.relocatable)
    return r;
  RT_ADD(&r.link_flags, "-nostdlib",
         str_fmt("-fuse-ld=%s", path_join(path_dir(tc.lld), "wasm-ld")));
  char *libc_dir = wasi_build_libc(o, t, tc, &e);
  if (e)
    goto fail;
  bool reactor = false;
  for (size_t i = 0; i < o.forwarded.len; i++) {
    char *a = o.forwarded.data[i];
    if (starts_with(a, "-mexec-model=")) {
      char *m = a + 13;
      if (!in_target_inventory("command reactor", m)) {
        e = str_fmt(
            "expected [command|reactor] for -mexec-mode=[value], found \"%s\"",
            m);
        goto fail;
      }
      reactor = streq(m, "reactor");
    }
  }
  sv_push(&r.link_flags,
          path_join(libc_dir, reactor ? "crt1-reactor.o" : "crt1-command.o"));
  if (reactor)
    sv_push(&r.link_flags, "-Wl,--entry,_initialize");
  if (o.cxx) {
    char *dir = runtime_build_cxx(o, t, tc, false, &e);
    if (e)
      goto fail;
    RT_ADD(&r.link_flags, path_join(dir, "libc++.a"),
           path_join(dir, "libc++abi.a"));
  }
  sv_push(&r.link_flags, path_join(libc_dir, "libc.a"));
  char *b = wasi_build_builtins(o, t, tc, &e);
  if (e)
    goto fail;
  sv_push(&r.link_flags, b);
  return r;
fail:
  if (error)
    *error = e;
  return r;
}
