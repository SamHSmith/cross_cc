#ifndef CROSS_RUNTIME_H
#define CROSS_RUNTIME_H
#include "cross.h"

/* Recipe arrays retain pinned upstream order. A zero cap marks static storage.
 */
extern StrVec libcxxabi_files, libcxx_base_files, libcxx_thread_files,
    musl_src_files, musl_supplement_files;
extern StrVec BUILTINS_GENERIC, BUILTINS_TF, BUILTINS_X86_80, BUILTINS_BF16,
    BUILTINS_X86_64_EXTRA, BUILTINS_AARCH64_EXTRA, BUILTINS_OA_PATTERNS;
extern const int BUILTINS_OA_SIZES[], BUILTINS_OA_MODELS[];
extern const size_t BUILTINS_OA_SIZES_count, BUILTINS_OA_MODELS_count;
extern StrVec wasi_bottom_half_files, wasi_top_half_files, wasi_fts_files,
    wasi_setjmp_files, wasi_dl_files, wasi_process_clocks_files,
    wasi_getpid_files, wasi_mman_files, wasi_signal_bottom_files,
    wasi_signal_top_files, wasi_supplement_files;

/* Error-only functions return NULL on success, as do char **error outputs. */
char *stable_compiler_identity(Toolchain tc);
StrVec runtime_glibc_version_flags(Target t);
StrVec libc_include_flags(Target t, const char *lib_dir);
StrVec runtime_base_flags(Options o, Target t, Toolchain tc);
char *runtime_cache(const char *kind, Options o, Target t, Toolchain tc);
char *runtime_compile(const char *source, const char *object, StrVec flags,
                      Toolchain tc, bool verbose);
char *runtime_archive(StrVec objects, const char *path, Toolchain tc,
                      bool verbose);
StrVec runtime_recipe_files(Toolchain tc, const char *recipe, const char *array,
                            char **error);
bool runtime_musl_arch(const char *s);
char *runtime_musl_arch_name(Target t);
bool runtime_musl_supported(Target t);
bool runtime_musl_ptr64(const char *arch);
bool runtime_musl_time32(const char *arch);
char *runtime_build_musl(Options o, Target t, Toolchain tc, char **error);
char *runtime_build_musl_libc_so(StrVec base, const char *work, Options o,
                                 Target t, Toolchain tc);
bool linux_libc_name(Target t, const char *name);
char *linux_library_dir(const char *dir, const char *sysroot);
char linux_library_arg(const char *a, char **value, bool *separate);
bool linux_strip_link(Options o);
bool linux_shared_input(const char *path, StrVec dirs, StrVec parents,
                        char **error);
bool libunwind_library_name(const char *name);
bool links_bundled_libunwind(Target t);
bool bundled_runtime_library_name(Target t, const char *name);

StrVec builtins_sources(const char *arch, const char *target_os,
                        const char *target_abi, bool *ok);
typedef struct {
  StrVec args;
  char *object;
} Builtins_Job;
char *builtins_run_jobs(Builtins_Job *jobs, size_t count, bool verbose);
bool builtins_ppc_ieee_tf(Target t);
void builtins_mkdir(const char *path);
char *builtins_find_source(Toolchain tc, char **error);
StrVec builtins_include_dirs(Target t, Toolchain tc);
char *builtins_memory_ir(Target t);
char *sha256_text(const char *text);

StrVec wasi_include_flags(Toolchain tc);
StrVec wasi_cc_args(Toolchain tc, bool o3);
StrVec wasi_bottom_includes(Toolchain tc);
StrVec wasi_top_includes(Toolchain tc);
char *wasi_cache(const char *kind, Options o, Target t, Toolchain tc);
StrVec wasi_base(Target t, Options o);
char *wasi_compile_list(StrVec files, const char *root, StrVec base,
                        StrVec extra, const char *work, StrVec *objects,
                        Toolchain tc, bool verbose);
char *wasi_build_libc(Options o, Target t, Toolchain tc, char **error);
char *wasi_build_builtins(Options o, Target t, Toolchain tc, char **error);

/* Platform module entry points shared by runtime recipes. */
char *glibc_default_version(Target t);
Runtime prepare_glibc(Options o, Target t, Toolchain tc, char **error);
StrVec bsd_include_flags(Target t, const char *lib_dir);
StrVec bsd_version_flags(Target t);
char *deterministic_link_stage(const char *cache, const char *prefix,
                               const char *output, char **error);
bool is_linker_input(const char *path);
typedef StrVec (*Link_Prepare)(StrVec command, const char *work, char **error);
int run_profile_link_prepared(Options o, Target t, Toolchain tc, Runtime rt,
                              Link_Prepare prepare);
StrVec sparc_tls_prepare_command(StrVec command, const char *work,
                                 char **error);
bool sparc_tls_relax_elf(unsigned char *bytes, size_t len, char **error);
bool sparc_tls_relax_file(const char *source, const char *destination,
                          char **error);

/* Small runtime-local vector operations; ownership is process lifetime. */
static inline void rt_add(StrVec *v, StrVec items) { sv_extend(v, items); }
#define RT_ADD(v, ...) rt_add((v), SV(__VA_ARGS__))
static inline bool rt_has(StrVec v, const char *s) {
  for (size_t i = 0; i < v.len; i++)
    if (streq(v.data[i], s))
      return true;
  return false;
}
static inline char *rt_join(StrVec v, const char *sep) {
  size_t n = 1, z = strlen(nz(sep));
  for (size_t i = 0; i < v.len; i++)
    n += strlen(nz(v.data[i])) + (i ? z : 0);
  char *s = xmalloc(n), *p = s;
  for (size_t i = 0; i < v.len; i++) {
    if (i) {
      memcpy(p, nz(sep), z);
      p += z;
    }
    size_t m = strlen(nz(v.data[i]));
    memcpy(p, nz(v.data[i]), m);
    p += m;
  }
  *p = 0;
  return s;
}
static inline void rt_insert(StrVec *v, size_t i, const char *s) {
  sv_push(v, "");
  memmove(v->data + i + 1, v->data + i, (v->len - i - 1) * sizeof(char *));
  v->data[i] = str_copy(s);
}
static inline bool rt_abs(const char *p) {
  return p && (p[0] == '/' || p[0] == '\\' ||
               (isalpha((unsigned char)p[0]) && p[1] == ':'));
}
static inline char *rt_error(char **e, char *s) {
  if (e)
    *e = s;
  return NULL;
}
#endif
