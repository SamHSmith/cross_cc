#include "platforms.h"
/* ABI decoder and CRT recipes translated from Zig 0.17.1
   src/libs/{freebsd,netbsd,openbsd}.zig. Copyright (c) Zig contributors.
   MIT license: licenses/runtime-recipes.txt. Bundled BSD startup sources retain
   their original licenses. These DSOs describe link-time ABI, not a host libc.
 */
#define GV(a, b, c) ((Glibc_Version){{a, b, c}})
static const Glibc_Lib BSD_FREEBSD_LIBS[] = {
    {"m", 5},  {"stdthreads", 0}, {"thr", 3},  {"c", 7},        {"dl", 1},
    {"rt", 1}, {"ld", 1},         {"util", 9}, {"execinfo", 1}, {"sys", 7}};
static const Glibc_Lib BSD_NETBSD_LIBS[] = {
    {"m", 0},  {"pthread", 1}, {"c", 12},      {"rt", 1},
    {"ld", 0}, {"util", 7},    {"execinfo", 0}};
static const Glibc_Lib BSD_OPENBSD_LIBS[] = {{"m", 0},    {"pthread", 0},
                                             {"c", 0},    {"ld", 0},
                                             {"util", 0}, {"execinfo", 0}};
const Glibc_Lib *bsd_libs(Target t, size_t *count) {
  *count = 0;
  if (streq(t.os, "freebsd")) {
    *count = 10;
    return BSD_FREEBSD_LIBS;
  }
  if (streq(t.os, "netbsd")) {
    *count = 7;
    return BSD_NETBSD_LIBS;
  }
  if (streq(t.os, "openbsd")) {
    *count = 6;
    return BSD_OPENBSD_LIBS;
  }
  return NULL;
}
char *bsd_filename(Target t, Glibc_Lib lib, Glibc_Version version) {
  if (streq(t.os, "openbsd"))
    return str_fmt("lib%s.so", lib.name);
  int sover = lib.sover;
  if (streq(t.os, "freebsd") && streq(lib.name, "util") &&
      version.data[0] >= 15)
    sover = 10;
  return str_fmt("lib%s.so.%d", lib.name, sover);
}
Glibc_Version bsd_version(Target t) {
  Glibc_Version v = streq(t.os, "freebsd")   ? GV(14, 0, 0)
                    : streq(t.os, "openbsd") ? GV(7, 8, 0)
                                             : GV(10, 1, 0);
  StrVec parts = str_split(nz(t.name), "-");
  if (parts.len < 2)
    return v;
  char *dot = strchr(parts.data[1], '.');
  if (dot) {
    char *s = str_copy(dot + 1), *end = strstr(s, "...");
    if (end)
      *end = 0;
    StrVec nums = str_split(s, ".");
    v = GV(0, 0, 0);
    for (size_t i = 0; i < nums.len && i < 3; i++) {
      char *tail = NULL;
      errno = 0;
      long n = strtol(nums.data[i], &tail, 10);
      if (!errno && tail != nums.data[i] && !*tail)
        v.data[i] = (int)n;
    }
  }
  return v;
}
StrVec bsd_include_flags(Target t, const char *lib) {
  const char *arch = t.arch, *abi = t.abi;
  if (streq(t.os, "freebsd") && streq(arch, "powerpc64le"))
    arch = "powerpc64";
  if (streq(t.os, "netbsd")) {
    if (streq(arch, "aarch64_be"))
      arch = "aarch64";
    else if (streq(arch, "armeb"))
      arch = "arm";
    else if (streq(arch, "mipsel"))
      arch = "mips";
    else if (streq(arch, "mips64el"))
      arch = "mips64";
    abi = streq(abi, "eabi") || streq(abi, "eabihf") ? "eabi" : "none";
  }
  return SV(
      "-isystem",
      path_join(lib, "libc/include", str_fmt("%s-%s-%s", arch, t.os, abi)),
      "-isystem", path_join(lib, "libc/include", str_fmt("generic-%s", t.os)));
}
StrVec bsd_version_flags(Target t) {
  Glibc_Version v = bsd_version(t);
  if (streq(t.os, "openbsd"))
    return SV("-D___OpenBSD=202510",
              str_fmt("-DOpenBSD%d_%d", v.data[0], v.data[1]));
  return SV(streq(t.os, "freebsd")
                ? str_fmt("-D__FreeBSD_version=%d", v.data[0] * 100000 + 500)
                : str_fmt("-D__NetBSD_Version__=%d",
                          v.data[0] * 100000000 + v.data[1] * 1000000));
}
char *bsd_compile(Options o, Target t, Toolchain tc, const char *src,
                  const char *out, StrVec flags) {
  StrVec a =
      SV(tc.clang, "--no-default-config", str_fmt("--target=%s", t.clang), "-c",
         "-O2", "-fPIC", "-fno-stack-protector", "-nostdinc", "-isystem",
         path_join(tc.lib_dir, "include"), "-w", "-Qunused-arguments");
  char *error = NULL;
  StrVec cpu = cpu_flags(t, "baseline", (StrVec){0}, &error);
  if (error)
    return error;
  sv_extend(&a, cpu);
  sv_extend(&a, bsd_include_flags(t, tc.lib_dir));
  sv_extend(&a, bsd_version_flags(t));
  sv_extend(&a, flags);
  sv_extend(&a, SV(src, "-o", out));
  if (run_command(a, o.verbose))
    return str_fmt("compiling BSD runtime failed: %s", src);
  return NULL;
}
char *bsd_emit(const unsigned char *data, size_t size, Target t,
               Glibc_Version version, const char *dir) {
  Glibc_Reader r = {data, size, 0, false};
  size_t nlibs = 0;
  const Glibc_Lib *libs = bsd_libs(t, &nlibs);
  if (glibc_byte(&r) != nlibs)
    return "invalid BSD abilists library count";
  for (size_t i = 0; i < nlibs; i++)
    if (!streq(glibc_zstr(&r), libs[i].name))
      return "invalid BSD abilists library order";
  int nv = glibc_byte(&r);
  if (nv == 0 || nv > 128)
    return "invalid BSD abilists version count";
  Glibc_Version versions[128] = {0}, wanted = version;
  if (streq(t.os, "freebsd"))
    wanted = GV(1, version.data[0] - 7, 0);
  int vi = -1;
  for (int i = 0; i < nv; i++) {
    for (int j = 0; j < 3; j++)
      versions[i].data[j] = glibc_byte(&r);
    if (!glibc_ver_cmp(versions[i], wanted))
      vi = i;
  }
  if (vi < 0 && glibc_ver_cmp(wanted, versions[nv - 1]) > 0)
    vi = nv - 1;
  if (vi < 0)
    return str_fmt("unsupported %s ABI version %s", t.os,
                   glibc_ver_name(wanted));
  int nt = glibc_byte(&r), ti = -1;
  char *target = str_fmt("%s-%s-%s", t.arch, t.os, t.abi);
  for (int i = 0; i < nt; i++)
    if (streq(glibc_zstr(&r), target))
      ti = i;
  if (ti < 0 || ti >= 64)
    return str_fmt("BSD ABI metadata has no target %s", target);
  if (r.bad)
    return "truncated BSD ABI metadata";
  size_t inclusions = r.pos;
  bool freebsd = streq(t.os, "freebsd");
  char *error = NULL;
  if (freebsd) {
    Platform_Text map = {0};
    for (int i = 0; i <= vi; i++)
      pt_printf(&map, "FBSD_%d.%d { };\n", versions[i].data[0],
                versions[i].data[1]);
    error = glibc_write(path_join(dir, "all.map"), map.data);
    free(map.data);
    if (error)
      return error;
  }
  const char *word = glibc_word(t);
  int alignment = streq(word, ".quad") ? 8 : 4;
  for (size_t li = 0; li < nlibs; li++) {
    const Glibc_Lib lib = libs[li];
    if (freebsd && streq(lib.name, "sys") && version.data[0] < 15)
      continue;
    r.pos = inclusions;
    Platform_Text a = {0};
    int tables = freebsd ? 3 : 2;
    for (int kind = 0; kind < tables; kind++) {
      if (kind == 0)
        pt_append(&a, ".text\n");
      else if (kind == 1) {
        pt_append(&a, ".data\n");
        if (freebsd && streq(lib.name, "c"))
          pt_printf(&a,
                    ".balign %d\n.globl __progname\n.globl environ\n%s "
                    "__progname\n%s environ\n",
                    alignment, word, word);
      } else
        pt_append(&a, ".section .tdata,\"awT\",%progbits\n");
      int count = glibc_u16(&r);
      char *name = "";
      bool present[128] = {0}, weak[128] = {0};
      uint64_t sizes[128] = {0};
      int default_vi = -1, unversioned = -1;
      for (int k = 0; k < count; k++) {
        if (streq(name, "")) {
          name = glibc_zstr(&r);
          memset(present, 0, sizeof(present));
          memset(weak, 0, sizeof(weak));
          memset(sizes, 0, sizeof(sizes));
          default_vi = -1;
          unversioned = -1;
        }
        uint64_t targets = glibc_uleb(&r), sz = 0;
        if (kind) {
          sz = glibc_uleb(&r);
          if (sz > 65535)
            r.bad = true;
        }
        unsigned lb = glibc_byte(&r);
        bool applies = (lb & 31) == li && (targets & (UINT64_C(1) << ti));
        for (;;) {
          unsigned b = glibc_byte(&r);
          int idx = b & 127;
          if (idx >= nv)
            r.bad = true;
          else if (applies && idx <= vi) {
            if (freebsd) {
              sizes[idx] = sz;
              weak[idx] = (lb & 64) != 0;
            }
            if (!freebsd || (lb & 32)) {
              if (idx > unversioned) {
                unversioned = idx;
                sizes[idx] = sz;
                weak[idx] = (lb & 64) != 0;
              }
            } else {
              if (idx > default_vi)
                default_vi = idx;
              present[idx] = true;
              sizes[idx] = sz;
              weak[idx] = (lb & 64) != 0;
            }
          }
          if ((b & 128) || r.bad)
            break;
        }
        if (r.bad) {
          free(a.data);
          return "malformed BSD ABI inclusions";
        }
        if (!(lb & 128))
          continue;
        const char *typ = kind == 0   ? "function"
                          : kind == 1 ? "object"
                                      : "tls_object";
        if (unversioned >= 0) {
          pt_printf(&a, ".balign %d\n.%s %s\n.type %s, %%%s\n", alignment,
                    weak[unversioned] ? "weak" : "globl", name, name, typ);
          if (kind)
            pt_printf(&a, ".size %s, %" PRIu64 "\n", name, sizes[unversioned]);
          pt_printf(&a, "%s: %s 0\n", name, word);
        }
        for (int i = 0; i < nv; i++) {
          if (!present[i])
            continue;
          Glibc_Version v = versions[i];
          char *local = str_fmt("%s_FBSD_%d_%d", name, v.data[0], v.data[1]);
          pt_printf(&a, ".balign %d\n.%s %s\n.type %s, %%%s\n", alignment,
                    weak[i] ? "weak" : "globl", local, local, typ);
          if (kind)
            pt_printf(&a, ".size %s, %" PRIu64 "\n", local, sizes[i]);
          pt_printf(&a, ".symver %s, %s%sFBSD_%d.%d%s\n", local, name,
                    i == default_vi ? "@@" : "@", v.data[0], v.data[1],
                    kind ? "" : ", remove");
          if (!kind)
            pt_printf(&a, "%s: %s 0\n", local, word);
          else
            pt_printf(&a, "%s: .fill %" PRIu64 ", 1, 0\n", local, sizes[i]);
        }
        name = "";
      }
      if (!streq(name, "")) {
        free(a.data);
        return "unterminated BSD ABI symbol";
      }
    }
    if (!freebsd && r.pos + 2 == size && !data[r.pos] && !data[r.pos + 1])
      r.pos += 2;
    if (r.bad || r.pos != size) {
      free(a.data);
      return "invalid BSD ABI data length";
    }
    pt_append(&a, ".section .note.GNU-stack,\"\",%progbits\n");
    error = glibc_write(path_join(dir, str_fmt("%s.s", lib.name)), a.data);
    free(a.data);
    if (error)
      return error;
  }
  return NULL;
}
char *bsd_crt(Options o, Target t, Toolchain tc, const char *dir) {
  char *base = path_join(tc.lib_dir, "libc", t.os, "lib/csu");
  StrVec sources = {0}, flags = SV("-I", path_join(base, "common"), "-DPIC");
  const char *arch = t.arch;
  if (streq(t.os, "freebsd")) {
    if (streq(arch, "x86_64"))
      arch = "amd64";
    else if (streq(arch, "x86"))
      arch = "i386";
    else if (streq(arch, "powerpc64le"))
      arch = "powerpc64";
    else if (streq(arch, "riscv64"))
      arch = "riscv";
    sv_extend(&flags, SV("-I", path_join(base, arch), "-I",
                         path_join(tc.lib_dir, "libc/freebsd/lib/libc/include"),
                         "-DSTRIP_FBSDID", "-fno-common", "-std=gnu99"));
    sv_extend(&sources, SV("common/crtbegin.c", "common/crtbrand.S",
                           "common/feature_note.S", "common/ignore_init_note.S",
                           path_join(arch, "crt1_c.c")));
    if (streq(arch, "powerpc"))
      sv_push(&sources, "powerpc/crtsavres.S");
    else if (!streq(arch, "powerpc64"))
      sv_push(&sources, path_join(arch, "crt1_s.S"));
  } else if (streq(t.os, "openbsd")) {
    if (streq(arch, "x86_64"))
      arch = "amd64";
    else if (streq(arch, "x86"))
      arch = "i386";
    else if (streq(arch, "mips64el"))
      arch = "mips64";
    sv_extend(&flags, SV("-I", path_join(base, arch)));
    sv_extend(&sources, SV("crt0.c", "crtbegin.c"));
  } else {
    if (streq(arch, "x86"))
      arch = "i386";
    else if (streq(arch, "aarch64_be"))
      arch = "aarch64";
    else if (streq(arch, "arm") || streq(arch, "armeb"))
      arch = "earm";
    else if (streq(arch, "mipsel") || streq(arch, "mips64") ||
             streq(arch, "mips64el"))
      arch = "mips";
    else if (streq(arch, "riscv32") || streq(arch, "riscv64"))
      arch = "riscv";
    sv_push(&flags, "-DHAVE_INITFINI_ARRAY");
    sv_extend(&sources,
              SV("common/crt0-common.c", "common/crtbegin.c",
                 "common/sysident.S", path_join("arch", arch, "crt0.S")));
  }
  StrVec objects = {0};
  for (size_t i = 0; i < sources.len; i++) {
    const char *s = sources.data[i];
    char *obj = path_join(dir, str_fmt("crt-%zu.o", i));
    if (streq(t.os, "openbsd") && streq(s, "crtbegin.c"))
      obj = path_join(dir, "crtbegin.o");
    StrVec f = sv_copy(flags);
    if (streq(t.os, "freebsd") && ends_with(s, ".S"))
      sv_push(&f, "-DLOCORE");
    char *e = bsd_compile(o, t, tc, path_join(base, s), obj, f);
    if (e)
      return e;
    sv_push(&objects, obj);
  }
  StrVec args = SV(tc.lld, "-r", "-o", path_join(dir, "crt.o"));
  sv_extend(&args, objects);
  if (run_command(args, o.verbose))
    return "linking BSD startup object failed";
  return NULL;
}
bool bsd_pie_enabled(Options o, Target t) {
  if (o.pie_mode)
    return o.pie_mode == 1;
  return streq(t.os, "openbsd") && needs_link(o) && !o.shared && !o.relocatable;
}
bool bsd_pic_enabled(Options o, Target t) {
  if (o.pic_mode)
    return o.pic_mode == 1;
  return bsd_pie_enabled(o, t) || streq(t.os, "openbsd") || o.dynamic_link ||
         (needs_link(o) && !o.relocatable && !o.static_link);
}
char *bsd_pic_error(Options o, Target t) {
  if (needs_link(o) && !o.relocatable && o.shared && bsd_pie_enabled(o, t))
    return "dynamic libraries cannot be position independent executables";
  if (o.pic_mode != -1)
    return NULL;
  if (bsd_pie_enabled(o, t))
    return "making a Position Independent Executable requires enabling "
           "Position Independent Code";
  if ((!needs_link(o) && !o.dynamic_link) || o.relocatable || o.static_link)
    return NULL;
  return "dynamic linking requires enabling Position Independent Code";
}
Runtime prepare_bsd(Options o, Target t, Toolchain tc, char **error) {
  Runtime rt = {0};
  *error = NULL;
  if (o.cxx && !o.no_stdinc && !o.no_stdincxx)
    sv_extend(&rt.compile_flags, runtime_cxx_flags(t, tc.lib_dir));
  if (!o.no_stdinc) {
    sv_extend(&rt.compile_flags,
              SV("-nostdlibinc", "-isystem", path_join(tc.lib_dir, "include")));
    sv_extend(&rt.compile_flags, bsd_include_flags(t, tc.lib_dir));
    sv_extend(&rt.compile_flags, bsd_version_flags(t));
  }
  *error = bsd_pic_error(o, t);
  if (*error)
    return rt;
  if (needs_link(o) && !o.relocatable && !o.shared) {
    if (bsd_pie_enabled(o, t))
      sv_push(&rt.link_flags, "-Wl,-pie");
    else if (streq(t.os, "openbsd"))
      sv_push(&rt.link_flags, "-Wl,-no-pie");
  }
  if (!needs_link(o) || o.relocatable || o.no_stdlib)
    return rt;
  if (o.static_link) {
    *error = str_fmt("static %s libc is not supplied by the bundled ABI stubs",
                     t.os);
    return rt;
  }
  bool supported = in_target_inventory("x86_64 aarch64", t.arch) ||
                   ((streq(t.os, "freebsd") || streq(t.os, "netbsd")) &&
                    streq(t.arch, "x86")) ||
                   (streq(t.os, "freebsd") && streq(t.arch, "riscv64"));
  if (!supported || !streq(t.abi, "none")) {
    *error = str_fmt("bundled %s runtime does not support %s with ABI %s", t.os,
                     t.arch, t.abi);
    return rt;
  }
  Glibc_Version version = bsd_version(t);
  size_t size = 0;
  unsigned char *data = (unsigned char *)read_file(
      path_join(tc.lib_dir, "libc", t.os, "abilists"), &size);
  if (!data) {
    *error = str_fmt("cannot read BSD abilists: %s", strerror(errno));
    return rt;
  }
  if (size > 300 * 1024) {
    free(data);
    *error = "oversized BSD abilists";
    return rt;
  }
  char *identity = str_fmt("bsd-recipe-4-loader-soname|%s|%s|%s|[%d, %d, %d]",
                           stable_compiler_identity(tc), tc.lib_dir, t.clang,
                           version.data[0], version.data[1], version.data[2]);
  uint64_t hash = runtime_hash(identity);
  for (size_t i = 0; i < size; i++)
    hash = (hash ^ data[i]) * UINT64_C(1099511628211);
  char *dir = path_join(tc.cache_dir, str_fmt("bsd-%016" PRIx64, hash));
  if (!file_exists(path_join(dir, "complete"))) {
    ensure_dir(tc.cache_dir);
    char *work = make_temp_dir(tc.cache_dir, "bsd-build-", error);
    if (*error)
      goto done;
    *error = bsd_emit(data, size, t, version, work);
    if (*error)
      goto cleanup;
    size_t count = 0;
    const Glibc_Lib *libs = bsd_libs(t, &count);
    for (size_t i = 0; i < count; i++) {
      Glibc_Lib lib = libs[i];
      if (streq(t.os, "freebsd") && streq(lib.name, "sys") &&
          version.data[0] < 15)
        continue;
      char *obj = path_join(work, str_fmt("%s.o", lib.name));
      *error = bsd_compile(o, t, tc, path_join(work, str_fmt("%s.s", lib.name)),
                           obj, (StrVec){0});
      if (*error)
        goto cleanup;
      char *filename = bsd_filename(t, lib, version),
           *soname =
               streq(lib.name, "ld") ? path_base(t.dynamic_linker) : filename;
      StrVec args = SV(tc.lld, "-shared", "-z", "notext", "-soname", soname,
                       "-o", path_join(work, filename), obj);
      if (streq(t.os, "freebsd"))
        sv_extend(&args, SV("--no-undefined-version",
                            str_fmt("--version-script=%s",
                                    path_join(work, "all.map"))));
      if (run_command(args, o.verbose)) {
        *error = "linking BSD ABI stubs failed";
        goto cleanup;
      }
      if (!streq(t.os, "openbsd")) {
        *error = glibc_write(
            path_join(work, str_fmt("lib%s.so", lib.name)),
            str_fmt("INPUT ( \"%s\" )\n", path_join(dir, filename)));
        if (*error)
          goto cleanup;
      }
    }
    *error = bsd_crt(o, t, tc, work);
    if (*error)
      goto cleanup;
    *error = runtime_publish(work, dir);
  cleanup:
    remove_tree(work);
    if (*error)
      goto done;
  }
  sv_extend(&rt.link_flags, SV("-nostdlib", "-L", dir));
  if (!o.no_startfiles) {
    if (!o.shared)
      sv_push(&rt.link_flags, path_join(dir, "crt.o"));
    else if (streq(t.os, "openbsd"))
      sv_push(&rt.link_flags, path_join(dir, "crtbegin.o"));
  }
  if (!o.no_defaultlibs) {
    if (o.cxx) {
      char *cxx = runtime_build_cxx(o, t, tc, false, error);
      if (*error)
        goto done;
      sv_extend(&rt.link_flags,
                SV("-Wl,--start-group", path_join(cxx, "libc++.a"),
                   path_join(cxx, "libc++abi.a"), path_join(cxx, "libunwind.a"),
                   "-Wl,--end-group"));
    }
    sv_push(&rt.link_flags, "-Wl,--push-state,-Bdynamic,--as-needed");
    size_t count = 0;
    const Glibc_Lib *libs = bsd_libs(t, &count);
    for (size_t i = 0; i < count; i++) {
      if (streq(t.os, "freebsd") && streq(libs[i].name, "sys") &&
          version.data[0] < 15)
        continue;
      sv_push(&rt.link_flags,
              path_join(dir, bsd_filename(t, libs[i], version)));
    }
    sv_push(&rt.link_flags, "-Wl,--pop-state");
  }
  if (!o.shared)
    sv_push(&rt.link_flags,
            str_fmt("-Wl,--dynamic-linker=%s", t.dynamic_linker));
done:
  free(data);
  return rt;
}
