#include "runtime.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Pinned compiler-rt.lock.json. Sources are shipped, never host objects. */
#define BUILTINS_VERSION "22.1.8"
#define BUILTINS_SHA                                                           \
  "922f1817a0df7b1489272d18134ee0087a8b068828f87ac63b9861b1a9965888"
#define BUILTINS_RECIPE_VERSION 1
#define BUILTINS_JOBS 2

StrVec builtins_sources(const char *arch, const char *target_os,
                        const char *target_abi, bool *ok) {
  StrVec list = {0};
  *ok = false;
  if (streq(target_os, "windows") && streq(target_abi, "msvc")) {
    sv_extend(&list, BUILTINS_GENERIC);
    if (streq(arch, "x86_64"))
      RT_ADD(&list, "cpu_model/x86.c", "x86_64/chkstk.S");
    else if (streq(arch, "x86"))
      RT_ADD(&list, "cpu_model/x86.c", "i386/chkstk2.S");
    else if (streq(arch, "aarch64"))
      sv_push(&list, "aarch64/chkstk.S");
    else
      return list;
    *ok = true;
    return list;
  }
  if (streq(arch, "aarch64_be"))
    return builtins_sources("aarch64", target_os, "", ok);
  if (arm_arch(arch) && !streq(arch, "arm"))
    return builtins_sources("arm", target_os, "", ok);
  if (streq(arch, "mips"))
    return builtins_sources("mipsel", target_os, "", ok);
  if (streq(arch, "mips64"))
    return builtins_sources("mips64el", target_os, "", ok);
  sv_extend(&list, BUILTINS_GENERIC);
  if (!in_target_inventory("arm x86 mipsel powerpc64 powerpc64le", arch))
    sv_extend(&list, BUILTINS_TF);
  if (streq(arch, "x86_64")) {
    sv_push(&list, "cpu_model/x86.c");
    sv_extend(&list, BUILTINS_X86_80);
    sv_extend(&list, BUILTINS_X86_64_EXTRA);
  } else if (streq(arch, "aarch64")) {
    sv_extend(&list, BUILTINS_BF16);
    sv_extend(&list, BUILTINS_AARCH64_EXTRA);
    sv_push(&list, "cpu_model/aarch64.c");
  } else if (in_target_inventory("riscv32 riscv64", arch))
    RT_ADD(&list, "riscv/fp_mode.c", "riscv/save.S", "riscv/restore.S",
           "cpu_model/riscv.c",
           streq(arch, "riscv32") ? "riscv/mulsi3.S" : "riscv/muldi3.S");
  else if (streq(arch, "loongarch64"))
    sv_push(&list, "loongarch/fp_mode.c");
  else if (in_target_inventory("wasm32 s390x mips64el sparc64 mipsel", arch)) {
  } else if (streq(arch, "hexagon"))
    sv_extend(
        &list,
        str_split(
            "hexagon/common_entry_exit_abi1.S hexagon/common_entry_exit_abi2.S "
            "hexagon/common_entry_exit_legacy.S hexagon/dfaddsub.S "
            "hexagon/dfdiv.S hexagon/dffma.S hexagon/dfminmax.S "
            "hexagon/dfmul.S hexagon/dfsqrt.S hexagon/divdi3.S "
            "hexagon/divsi3.S hexagon/fastmath2_dlib_asm.S "
            "hexagon/fastmath2_ldlib_asm.S hexagon/fastmath_dlib_asm.S "
            "hexagon/memcpy_forward_vp4cp4n2.S hexagon/memcpy_likely_aligned.S "
            "hexagon/moddi3.S hexagon/modsi3.S hexagon/sfdiv_opt.S "
            "hexagon/sfsqrt_opt.S hexagon/udivdi3.S hexagon/udivmoddi4.S "
            "hexagon/udivmodsi4.S hexagon/udivsi3.S hexagon/umoddi3.S "
            "hexagon/umodsi3.S",
            " "));
  else if (in_target_inventory("powerpc64 powerpc64le", arch))
    sv_extend(
        &list,
        str_split("ppc/floattitf.c ppc/fixtfti.c ppc/fixunstfti.c ppc/divtc3.c "
                  "ppc/fixtfdi.c ppc/fixunstfdi.c ppc/floatditf.c "
                  "ppc/floatunditf.c ppc/gcc_qadd.c ppc/gcc_qdiv.c "
                  "ppc/gcc_qmul.c ppc/gcc_qsub.c ppc/multc3.c",
                  " "));
  else if (streq(arch, "x86")) {
    sv_extend(&list, BUILTINS_X86_80);
    sv_extend(
        &list,
        str_split(
            "i386/fp_mode.c i386/ashldi3.S i386/ashrdi3.S i386/divdi3.S "
            "i386/floatdidf.S i386/floatdisf.S i386/floatundidf.S "
            "i386/floatundisf.S i386/lshrdi3.S i386/moddi3.S i386/muldi3.S "
            "i386/udivdi3.S i386/umoddi3.S i386/floatdixf.S i386/floatundixf.S",
            " "));
  } else if (streq(arch, "arm")) {
    sv_extend(
        &list,
        str_split("arm/fp_mode.c arm/bswapdi2.S arm/bswapsi2.S arm/clzdi2.S "
                  "arm/clzsi2.S arm/comparesf2.S arm/divmodsi4.S arm/divsi3.S "
                  "arm/modsi3.S arm/udivmodsi4.S arm/udivsi3.S arm/umodsi3.S",
                  " "));
    if (streq(target_os, "windows"))
      RT_ADD(&list, "arm/aeabi_idivmod.S", "arm/aeabi_ldivmod.S",
             "arm/aeabi_uidivmod.S", "arm/aeabi_uldivmod.S");
    else
      sv_extend(
          &list,
          str_split(
              "arm/aeabi_cdcmp.S arm/aeabi_cdcmpeq_check_nan.c "
              "arm/aeabi_cfcmp.S arm/aeabi_cfcmpeq_check_nan.c "
              "arm/aeabi_dcmp.S arm/aeabi_div0.c arm/aeabi_drsub.c "
              "arm/aeabi_fcmp.S arm/aeabi_frsub.c arm/aeabi_idivmod.S "
              "arm/aeabi_ldivmod.S arm/aeabi_uidivmod.S arm/aeabi_uldivmod.S "
              "arm/aeabi_memcmp.S arm/aeabi_memcpy.S arm/aeabi_memmove.S "
              "arm/aeabi_memset.S arm/switch16.S arm/switch32.S arm/switch8.S "
              "arm/switchu8.S arm/restore_vfp_d8_d15_regs.S "
              "arm/save_vfp_d8_d15_regs.S",
              " "));
    StrVec ops = str_split("add and max min nand or sub umax umin xor", " ");
    for (size_t i = 0; i < ops.len; i++)
      for (int size = 4; size <= 8; size += 4)
        sv_push(&list,
                str_fmt("arm/sync_fetch_and_%s_%d.S", ops.data[i], size));
  } else
    return list;
  /* Last basename wins; retain the selected source's original order. */
  StrVec stems = {0};
  for (size_t i = 0; i < list.len; i++) {
    char *base = path_base(list.data[i]);
    sv_push(&stems, str_ncopy(base, strlen(base) - strlen(path_ext(base))));
  }
  StrVec result = {0};
  for (size_t i = 0; i < list.len; i++) {
    size_t chosen = i;
    for (size_t j = i + 1; j < list.len; j++)
      if (streq(stems.data[i], stems.data[j]))
        chosen = j;
    if (streq(list.data[chosen], list.data[i]))
      sv_push(&result, list.data[i]);
  }
  *ok = true;
  return result;
}
#ifdef _WIN32
typedef struct {
  HANDLE process;
  size_t job;
  bool active;
} Builtins_Slot;
#else
typedef struct {
  pid_t process;
  size_t job;
  bool active;
} Builtins_Slot;
#endif
static void builtins_reap(Builtins_Slot *slot, Builtins_Job *jobs,
                          char **failed) {
  bool bad = false;
#ifdef _WIN32
  DWORD code = 1;
  if (WaitForSingleObject(slot->process, INFINITE) != WAIT_OBJECT_0 ||
      !GetExitCodeProcess(slot->process, &code) || code)
    bad = true;
  CloseHandle(slot->process);
#else
  int state = 0;
  pid_t p;
  do {
    p = waitpid(slot->process, &state, 0);
  } while (p < 0 && errno == EINTR);
  bad = p < 0 || !WIFEXITED(state) || WEXITSTATUS(state) != 0;
#endif
  if (bad && !*failed)
    *failed = jobs[slot->job].object;
  slot->active = false;
}
char *builtins_run_jobs(Builtins_Job *jobs, size_t count, bool verbose) {
  Builtins_Slot slots[BUILTINS_JOBS] = {0};
  size_t next = 0;
  char *failed = NULL;
  while (next < count && !failed) {
    for (size_t i = 0; i < BUILTINS_JOBS; i++) {
      if (next >= count)
        break;
      if (slots[i].active)
        builtins_reap(&slots[i], jobs, &failed);
      if (failed)
        break;
      if (verbose)
        print_command(jobs[next].args);
#ifdef _WIN32
      STARTUPINFOW si = {0};
      PROCESS_INFORMATION pi = {0};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESTDHANDLES;
      si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
      si.hStdOutput = si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
      if (!CreateProcessW(NULL,
                          windows_wide(windows_command_line(jobs[next].args)),
                          NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        failed = jobs[next].object;
        break;
      }
      CloseHandle(pi.hThread);
      slots[i].process = pi.hProcess;
#else
      pid_t p = fork();
      if (p < 0) {
        failed = jobs[next].object;
        break;
      }
      if (!p) {
        dup2(STDERR_FILENO, STDOUT_FILENO);
        StrVec args = sv_copy(jobs[next].args);
        execvp(args.data[0], args.data);
        _exit(127);
      }
      slots[i].process = p;
#endif
      slots[i].job = next++;
      slots[i].active = true;
    }
  }
  for (size_t i = 0; i < BUILTINS_JOBS; i++)
    if (slots[i].active)
      builtins_reap(&slots[i], jobs, &failed);
  return failed;
}
char *builtins_driver_path(const char *p) {
#ifdef _WIN32
  return str_replace(p, "\\", "/");
#else
  return str_copy(p);
#endif
}
bool builtins_freestanding_supported(Target t) {
  return streq(t.os, "freestanding") &&
         in_target_inventory("x86 x86_64 aarch64 arm wasm32", t.arch);
}
bool builtins_msvc_supported(Target t) {
  return streq(t.os, "windows") && streq(t.abi, "msvc") &&
         in_target_inventory("x86 x86_64 aarch64", t.arch);
}
bool builtins_ppc_ieee_tf(Target t) {
  return streq(t.os, "linux") && streq(t.arch, "powerpc64le") &&
         streq(t.abi, "gnu");
}
void builtins_mkdir(const char *path) {
  if (!is_dir(path))
    ensure_dir(path);
}
char *builtins_find_source(Toolchain tc, char **error) {
  if (error)
    *error = NULL;
  char *name = str_fmt("compiler-rt-%s", BUILTINS_VERSION);
  StrVec candidates =
      SV(tc.lib_dir, path_join(tc.root, ".cache", "upstream", name),
         path_join(tc.lib_dir, "..", name),
         path_join(tc.lib_dir, "..", "compiler-rt"));
  for (size_t i = 0; i < candidates.len; i++)
    if (file_exists(path_join(candidates.data[i], "compiler-rt", "lib",
                              "builtins", "CMakeLists.txt")))
      return candidates.data[i];
  return rt_error(error,
                  str_fmt("compiler-rt %s sources not found (looked in %s); "
                          "run the cross_cc setup step to fetch dependencies",
                          BUILTINS_VERSION, nz(tc.lib_dir)));
}
StrVec builtins_include_dirs(Target t, Toolchain tc) {
  if (streq(t.os, "freestanding"))
    return SV(path_join(tc.lib_dir, "include"));
  if (streq(t.os, "windows"))
    return SV(path_join(tc.lib_dir, "libc", "include", "any-windows-any"));
  StrVec flags = libc_include_flags(t, tc.lib_dir), out = {0};
  for (size_t i = 1; i < flags.len; i += 2)
    sv_push(&out, flags.data[i]);
  return out;
}
static const char *const BUILTINS_PPC_KF_NAMES[][2] = {
    {"__addtf3", "__addkf3"},
    {"__subtf3", "__subkf3"},
    {"__multf3", "__mulkf3"},
    {"__divtf3", "__divkf3"},
    {"__letf2", "__lekf2"},
    {"__getf2", "__gekf2"},
    {"__unordtf2", "__unordkf2"},
    {"__cmptf2", "__cmpkf2"},
    {"__eqtf2", "__eqkf2"},
    {"__lttf2", "__ltkf2"},
    {"__netf2", "__nekf2"},
    {"__gttf2", "__gtkf2"},
    {"__extenddftf2", "__extenddfkf2"},
    {"__extendsftf2", "__extendsfkf2"},
    {"__extendhftf2", "__extendhfkf2"},
    {"__trunctfdf2", "__trunckfdf2"},
    {"__trunctfsf2", "__trunckfsf2"},
    {"__trunctfhf2", "__trunckfhf2"},
    {"__fixtfsi", "__fixkfsi"},
    {"__fixtfdi", "__fixkfdi"},
    {"__fixtfti", "__fixkfti"},
    {"__fixunstfsi", "__fixunskfsi"},
    {"__fixunstfdi", "__fixunskfdi"},
    {"__fixunstfti", "__fixunskfti"},
    {"__floatsitf", "__floatsikf"},
    {"__floatditf", "__floatdikf"},
    {"__floattitf", "__floattikf"},
    {"__floatunsitf", "__floatunsikf"},
    {"__floatunditf", "__floatundikf"},
    {"__floatuntitf", "__floatuntikf"},
    {"__powitf2", "__powikf2"}};
typedef struct {
  Builtins_Job *data;
  size_t len, cap;
} Builtins_JobVec;
static void builtins_add(Builtins_JobVec *jobs, StrVec *objects, StrVec base,
                         const char *object, const char *source, StrVec defs) {
  StrVec args = sv_copy(base);
  sv_extend(&args, defs);
  RT_ADD(&args, "-o", object, builtins_driver_path(source));
  if (jobs->len == jobs->cap) {
    jobs->cap = jobs->cap ? jobs->cap * 2 : 128;
    jobs->data = xrealloc(jobs->data, jobs->cap * sizeof(*jobs->data));
  }
  jobs->data[jobs->len++] = (Builtins_Job){args, str_copy(object)};
  sv_push(objects, object);
}
char *prepare_builtins(Options o, Target t, Toolchain tc, char **error) {
  if (error)
    *error = NULL;
  char *e = NULL;
  if (!needs_link(o) || o.no_stdlib || o.no_defaultlibs ||
      (streq(t.os, "linux") && o.no_libc))
    return "";
  if (t.native && streq(t.arch, host_arch()) && streq(t.os, host_os()) &&
      *nz(tc.builtins) && file_exists(tc.builtins))
    return tc.builtins;
  bool linux_target =
      streq(t.os, "linux") &&
      ((streq(t.arch, "hexagon") && streq(t.abi, "musl")) ||
       in_target_inventory("x86_64 aarch64 aarch64_be riscv64", t.arch) ||
       (streq(t.arch, "riscv32") && in_target_inventory("gnu musl", t.abi)) ||
       (in_target_inventory("loongarch64 sparc64", t.arch) &&
        streq(t.abi, "gnu")) ||
       arm_arch(t.arch) || streq(t.arch, "x86") ||
       (in_target_inventory("s390x powerpc64le", t.arch) &&
        streq(t.abi, "gnu")) ||
       (in_target_inventory("s390x powerpc64le powerpc64 loongarch64",
                            t.arch) &&
        starts_with(t.abi, "musl")) ||
       (in_target_inventory("mips64 mips64el", t.arch) &&
        in_target_inventory("muslabi64 gnuabi64", t.abi)) ||
       (in_target_inventory("mips mipsel", t.arch) &&
        streq(t.abi, "musleabi")));
  bool mingw = streq(t.os, "windows") && streq(t.abi, "gnu") &&
               in_target_inventory("x86 x86_64 aarch64 thumb", t.arch);
  bool bsd =
      (in_target_inventory("freebsd netbsd openbsd", t.os) &&
       in_target_inventory("x86_64 aarch64", t.arch)) ||
      (in_target_inventory("freebsd netbsd", t.os) && streq(t.arch, "x86")) ||
      (streq(t.os, "freebsd") && streq(t.arch, "riscv64"));
  if (!linux_target && !mingw && !bsd && !builtins_freestanding_supported(t) &&
      !builtins_msvc_supported(t))
    return rt_error(
        error,
        str_fmt("compiler-rt builtins for %s are not supported", nz(t.name)));
  if (!*nz(tc.clang))
    return rt_error(error, "compiler-rt builtins need a clang compiler");
  const char *cache = *nz(tc.cache_dir) ? tc.cache_dir : o.cache_dir;
  if (!*nz(cache))
    return rt_error(error, "no cache directory for compiler-rt builtins");
  char *triple = clang_triple(t.arch, t.os, t.abi, "");
  StrVec cpu = cpu_flags(t, o.cpu, o.forwarded, &e);
  if (e)
    return rt_error(error, e);
  bool ppc_kf = builtins_ppc_ieee_tf(t);
  const char *recipe_base =
      builtins_msvc_supported(t) ? "portable-builtins-v3-msvc"
      : streq(t.os, "openbsd")   ? "portable-builtins-v5-openbsd-emutls"
      : streq(t.arch, "riscv32") ? "portable-builtins-v4-riscv32-int128"
      : ppc_kf                   ? "portable-builtins-v4-ppc64le-kf1"
                                 : "portable-builtins-v4";
  char *recipe = str_fmt("%s-pm2", recipe_base);
  char *key = str_ncopy(
      sha256_text(str_fmt(
          "%s\nrecipe=%d\nsrc=%s\nsha=%s\nclang=%s\ntriple=%s\nos=%s\ncpu=%s\n",
          recipe, BUILTINS_RECIPE_VERSION, BUILTINS_VERSION, BUILTINS_SHA,
          stable_compiler_identity(tc), triple, nz(t.os), rt_join(cpu, " "))),
      24);
  char *out_dir = path_join(cache, "builtins",
                            str_fmt("%s-%s-%s", nz(t.arch), nz(t.abi), key)),
       *out_archive = path_join(out_dir, "libclang_rt.builtins.a");
  if (file_exists(out_archive))
    return out_archive;
  char *src_root = builtins_find_source(tc, &e);
  if (e)
    return rt_error(error, e);
  char *dir = builtins_driver_path(
      path_join(src_root, "compiler-rt", "lib", "builtins"));
  char *ar = *nz(tc.ar) ? tc.ar : find_program("ar");
  if (!*nz(ar))
    return rt_error(error, "ar was not found");
  char *stage =
      path_join(cache, "builtins", str_fmt(".stage-%s-%d", key, process_id()));
  remove_tree(stage);
  char *obj_dir = path_join(stage, "obj");
  builtins_mkdir(obj_dir);
  StrVec base = SV(tc.clang, "-target", triple, "-c", "-O2", "-w",
                   "-ffreestanding", "-fno-builtin", "-fvisibility=hidden",
                   "-fno-stack-protector", "-fomit-frame-pointer", "-fno-lto",
                   "-nostdlibinc", "-DDISABLE_AARCH64_FMV",
                   str_fmt("-ffile-prefix-map=%s=/cross_cc/stage",
                           builtins_driver_path(stage)),
                   str_fmt("-ffile-prefix-map=%s=/cross_cc/lib",
                           builtins_driver_path(tc.lib_dir)),
                   str_fmt("-ffile-prefix-map=%s=/cross_cc/compiler-rt/%s", dir,
                           BUILTINS_VERSION),
                   "-I", dir);
  sv_push(&base, builtins_msvc_supported(t)
                     ? "-mno-incremental-linker-compatible"
                     : "-fPIC");
  sv_extend(&base, cpu);
  if (builtins_msvc_supported(t)) {
    Options mo = o;
    mo.cxx = false;
    Runtime mr = prepare_msvc(mo, t, tc, &e);
    if (e)
      goto fail;
    sv_extend(&base, mr.compile_flags);
  } else {
    StrVec includes = builtins_include_dirs(t, tc);
    for (size_t i = 0; i < includes.len; i++)
      RT_ADD(&base, "-isystem", builtins_driver_path(includes.data[i]));
  }
  if (in_target_inventory("aarch64 aarch64_be", t.arch))
    sv_push(&base, builtins_msvc_supported(t) ? "-mno-outline-atomics"
                                              : "-moutline-atomics");
  if (streq(t.arch, "riscv32"))
    sv_push(&base, "-fforce-enable-int128");
  Builtins_JobVec jobs = {0};
  StrVec objects = {0};
  bool ok = false;
  StrVec list = builtins_sources(t.arch, t.os, t.abi, &ok);
  if (!ok) {
    e = "unsupported architecture";
    goto fail;
  }
  if (streq(t.os, "openbsd"))
    sv_push(&list, "emutls.c");
  for (size_t i = 0; i < list.len; i++) {
    char *s = list.data[i];
    if (streq(t.os, "freestanding") && streq(s, "cpu_model/x86.c"))
      continue;
    if (arm_arch(t.arch) && !ends_with(t.abi, "hf") &&
        in_target_inventory(
            "arm/restore_vfp_d8_d15_regs.S arm/save_vfp_d8_d15_regs.S", s))
      continue;
    char *obj = path_join(obj_dir, str_fmt("%s.o", str_replace(s, "/", "_")));
    StrVec defs = {0};
    if (builtins_msvc_supported(t) &&
        in_target_inventory("x86_64/chkstk.S i386/chkstk2.S", s)) {
      sv_push(&defs, streq(t.arch, "x86_64") ? "-D___chkstk_ms=__chkstk"
                                             : "-D___chkstk=__chkstk");
      if (streq(t.arch, "x86"))
        sv_push(&defs, "-Wa,-defsym,@feat.00=1");
    } else if (ppc_kf && starts_with(s, "ppc/")) {
      if (in_target_inventory("ppc/multc3.c ppc/divtc3.c", s))
        continue;
      sv_push(&defs, "-mabi=ibmlongdouble");
    }
    builtins_add(&jobs, &objects, base, obj, path_join(dir, s), defs);
  }
  if (ppc_kf) {
    char *alias_h = builtins_driver_path(path_join(stage, "kf_alias.h"));
    const char *alias_text =
        "#include \"int_lib.h\"\n#undef COMPILER_RT_ALIAS\n#define "
        "COMPILER_RT_ALIAS(name, aliasname) COMPILER_RT_ABI __typeof(name) "
        "aliasname __attribute__((__alias__(XSTR(name))));\n";
    if (!write_file(alias_h, alias_text, strlen(alias_text))) {
      e = str_fmt("cannot write %s: %s", alias_h, strerror(errno));
      goto fail;
    }
    StrVec kf = SV("-mabi=ieeelongdouble", "-include", alias_h);
    for (size_t i = 0;
         i < sizeof(BUILTINS_PPC_KF_NAMES) / sizeof(*BUILTINS_PPC_KF_NAMES);
         i++)
      sv_push(&kf, str_fmt("-D%s=%s", BUILTINS_PPC_KF_NAMES[i][0],
                           BUILTINS_PPC_KF_NAMES[i][1]));
    for (size_t i = 0; i < BUILTINS_TF.len; i++) {
      char *s = BUILTINS_TF.data[i], *src = path_join(dir, s);
      if (in_target_inventory("multc3.c divtc3.c", s)) {
        const char *name = streq(s, "multc3.c") ? "mul" : "div";
        src = builtins_driver_path(path_join(stage, str_fmt("kf_%s", s)));
        char *text =
            str_fmt("#include \"%s\"\nCOMPILER_RT_ALIAS(__%stc3, __%skc3)\n",
                    builtins_driver_path(path_join(dir, s)), name, name);
        if (!write_file(src, text, strlen(text))) {
          e = str_fmt("cannot write %s: %s", src, strerror(errno));
          goto fail;
        }
      }
      builtins_add(
          &jobs, &objects, base,
          path_join(obj_dir, str_fmt("kf_%s.o", str_replace(s, ".c", ""))), src,
          kf);
    }
  }
  if (streq(t.os, "freestanding")) {
    char *ir = path_join(stage, "memory.ll"), *text = builtins_memory_ir(t);
    if (!write_file(ir, text, strlen(text))) {
      e = str_fmt("cannot write freestanding memory IR: %s", strerror(errno));
      goto fail;
    }
    builtins_add(&jobs, &objects, base, path_join(obj_dir, "memory.o"), ir,
                 (StrVec){0});
  }
  if (in_target_inventory("aarch64 aarch64_be", t.arch) &&
      !builtins_msvc_supported(t)) {
    for (size_t p = 0; p < BUILTINS_OA_PATTERNS.len; p++)
      for (size_t s = 0; s < BUILTINS_OA_SIZES_count; s++) {
        char *pat = BUILTINS_OA_PATTERNS.data[p];
        int size = BUILTINS_OA_SIZES[s];
        if (!streq(pat, "cas") && size == 16)
          continue;
        for (size_t m = 0; m < BUILTINS_OA_MODELS_count; m++) {
          int model = BUILTINS_OA_MODELS[m];
          builtins_add(&jobs, &objects, base,
                       path_join(obj_dir, str_fmt("outline_atomic_%s%d_%d.o",
                                                  pat, size, model)),
                       path_join(dir, "aarch64", "lse.S"),
                       SV(str_fmt("-DL_%s", pat), str_fmt("-DSIZE=%d", size),
                          str_fmt("-DMODEL=%d", model)));
        }
      }
  }
  char *bad = builtins_run_jobs(jobs.data, jobs.len, o.verbose);
  if (bad) {
    e = str_fmt("compiling compiler-rt builtin %s failed", bad);
    goto fail;
  }
  Toolchain archive_tc = tc;
  archive_tc.ar = ar;
  e = runtime_archive(objects, path_join(stage, "libclang_rt.builtins.a"),
                      archive_tc, o.verbose);
  if (e)
    goto fail;
  remove_tree(obj_dir);
  if (!move_path(stage, out_dir)) {
    remove_tree(stage);
    if (!file_exists(out_archive))
      return rt_error(
          error, str_fmt("cannot publish %s: %s", out_dir, strerror(errno)));
  }
  return out_archive;
fail:
  remove_tree(stage);
  return rt_error(error, e);
}

/* SHA-256 for recipe identities; no platform crypto dependency. */
static uint32_t sha_rotr(uint32_t x, unsigned n) {
  return (x >> n) | (x << (32 - n));
}
char *sha256_text(const char *text) {
  static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t n = strlen(nz(text)), size = ((n + 9 + 63) / 64) * 64;
  unsigned char *bytes = xmalloc(size);
  memcpy(bytes, nz(text), n);
  bytes[n] = 0x80;
  uint64_t bits = (uint64_t)n * 8;
  for (int i = 0; i < 8; i++)
    bytes[size - 1 - (size_t)i] = (unsigned char)(bits >> (8 * i));
  for (size_t p = 0; p < size; p += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      size_t j = p + (size_t)i * 4;
      w[i] = ((uint32_t)bytes[j] << 24) | ((uint32_t)bytes[j + 1] << 16) |
             ((uint32_t)bytes[j + 2] << 8) | bytes[j + 3];
    }
    for (int i = 16; i < 64; i++) {
      uint32_t a = w[i - 15], b = w[i - 2];
      w[i] = w[i - 16] + (sha_rotr(a, 7) ^ sha_rotr(a, 18) ^ (a >> 3)) +
             w[i - 7] + (sha_rotr(b, 17) ^ sha_rotr(b, 19) ^ (b >> 10));
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5],
             g = h[6], v = h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t t1 = v + (sha_rotr(e, 6) ^ sha_rotr(e, 11) ^ sha_rotr(e, 25)) +
                    ((e & f) ^ (~e & g)) + k[i] + w[i],
               t2 = (sha_rotr(a, 2) ^ sha_rotr(a, 13) ^ sha_rotr(a, 22)) +
                    ((a & b) ^ (a & c) ^ (b & c));
      v = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += v;
  }
  free(bytes);
  return str_fmt("%08x%08x%08x%08x%08x%08x%08x%08x", h[0], h[1], h[2], h[3],
                 h[4], h[5], h[6], h[7]);
}
