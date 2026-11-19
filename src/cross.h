#ifndef CROSS_H
#define CROSS_H
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#endif
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <wchar.h>
wchar_t *windows_wide(const char *utf8);
char *windows_utf8(const wchar_t *wide);
#define NATIVE_EXE_SUFFIX ".exe"
#define PATH_LIST_SEPARATOR ";"
#else
#define NATIVE_EXE_SUFFIX ""
#define PATH_LIST_SEPARATOR ":"
#endif
#define EMBEDDED_DSYMUTIL_NAME "cross_dsymutil"
#define UPSTREAM_VERSION "0.17.1"
#define UPSTREAM_COMMIT "be248cc1fdc3cc654f9ec8ba68514bcb93b204fe"
typedef struct
{
    char **data;
    size_t len, cap;
} StrVec;
#ifdef _WIN32
char *windows_command_line(StrVec args);
#endif
typedef struct
{
    StrVec forwarded;
    char *target_name, *cpu, *output, *sysroot, *lib_dir, *cache_dir;
    bool msvc_system, msvc_accept_license;
    char *msvc_libc_file;
    bool cxx, compile_only, preprocess_only, assembly_only, syntax_only, dependency_only, shared,
        strip, static_link, municode, dynamic_link, relocatable, no_stdinc, no_stdincxx,
        no_stdlibxx, explicit_libcxx, no_stdlib, no_defaultlibs, no_libc, explicit_libc,
        no_startfiles, freestanding, verbose, dry_run, query, has_input, header_emit;
    int pie, pic_mode, pie_mode;
} Options;
typedef struct
{
    char *name, *clang, *arch, *os, *abi, *glibc_version, *dynamic_linker;
    bool native;
} Target;
typedef struct
{
    char *root, *lib_dir, *cache_dir, *clang, *lld, *ar, *builtins, *clang_version;
} Toolchain;
typedef struct
{
    StrVec compile_flags, link_flags;
} Runtime;
typedef struct
{
    char *include_dir, *sys_include_dir, *crt_dir, *msvc_lib_dir, *kernel32_lib_dir, *cc_dir,
        *darwin_sdk_dir;
} Libc_Config;
typedef enum
{
    UBSAN_OFF,
    UBSAN_TRAP,
    UBSAN_FULL
} Ubsan_Mode;
typedef struct
{
    char *include_dir, *sys_include_dir, *crt_dir, *msvc_lib_dir, *kernel32_lib_dir;
} Msvc_Libc;
bool has_prefix_any_n(const char *s, size_t count, ...);
#define has_prefix_any(s, ...)                                                                     \
    has_prefix_any_n(s, sizeof((const char *[]){__VA_ARGS__}) / sizeof(char *), __VA_ARGS__)
bool is_space(unsigned char c);
bool bundled_runtime_library_name(Target t, const char *name);
bool msvc_link_subsystem(const char *value);
bool msvc_safe_relative(const char *p);
Msvc_Libc msvc_libc_from_file(const char *path, char **error);
Msvc_Libc msvc_libc_detect(Target t, char **error);
Msvc_Libc managed_msvc(Options o, Target t, Toolchain tc, char **error);
StrVec msvc_include_dirs(Msvc_Libc l);
int BridgeClang_main(int argc, char **argv);
int BridgeLlvmAr_main(int argc, char **argv);
bool BridgeLLDLinkMinGW(int argc, const char **argv, bool can_exit_early, bool disable_output);
char *resource_vfs_overlay_root(Toolchain tc, const char *virtual_root, const char *physical,
                                char **error);
char *host_arch(void);
char *host_os(void);
char *clang_triple(const char *arch, const char *os, const char *abi, const char *version);
/* Strings and vector storage live for the driver process. Errors are NULL on success.
   All multi-result ports return the primary value and write char **error.
   Functions must treat NULL strings as empty; use nz() or streq(). */
const char *nz(const char *s);
bool streq(const char *a, const char *b);
bool starts_with(const char *s, const char *prefix);
bool ends_with(const char *s, const char *suffix);
bool contains(const char *s, const char *part);
char *str_copy(const char *s);
char *str_ncopy(const char *s, size_t n);
char *str_fmt(const char *format, ...);
char *str_lower(const char *s);
char *str_replace(const char *s, const char *old, const char *replacement);
char *str_trim(const char *s);
StrVec str_split(const char *s, const char *delimiter);
void sv_push(StrVec *v, const char *s);
void sv_extend(StrVec *v, StrVec other);
StrVec sv_copy(StrVec v);
StrVec sv_slice(StrVec v, size_t begin, size_t end);
StrVec sv_make(size_t count, ...);
#define SV(...) sv_make(sizeof((const char *[]){__VA_ARGS__}) / sizeof(char *), __VA_ARGS__)
void *xmalloc(size_t size);
void *xrealloc(void *p, size_t size);
_Noreturn void fatal(const char *message);
char *env(const char *name);
bool set_env(const char *name, const char *value);
char *path_join_n(size_t count, ...);
#define path_join(...)                                                                             \
    path_join_n(sizeof((const char *[]){__VA_ARGS__}) / sizeof(char *), __VA_ARGS__)
char *path_base(const char *path);
char *path_dir(const char *path);
char *path_ext(const char *path);
char *absolute_path(const char *path);
char *absolute_directory_path(const char *path);
bool file_exists(const char *path);
bool is_dir(const char *path);
bool is_file(const char *path);
FILE *open_file(const char *path, const char *mode);
bool move_path(const char *from, const char *to);
bool change_directory(const char *path);
bool remove_file(const char *path);
char *read_file(const char *path, size_t *size);
bool write_file(const char *path, const void *data, size_t size);
bool copy_file(const char *dest, const char *source);
bool link_file(const char *source, const char *dest);
bool rename_file(const char *source, const char *dest);
bool remove_tree(const char *path);
void ensure_dir(const char *path);
char *make_temp_dir(const char *parent, const char *prefix, char **error);
char *executable_path(void);
int process_id(void);
int run_command(StrVec args, bool verbose);
char *capture_command(StrVec args, int *code);
void print_command(StrVec args);
char *find_program(const char *name);
char *first_existing_n(size_t count, ...);
#define first_existing(...)                                                                        \
    first_existing_n(sizeof((const char *[]){__VA_ARGS__}) / sizeof(char *), __VA_ARGS__)
bool needs_link(Options o);
bool takes_value(const char *arg);
bool debug_info_flag(const char *arg);
StrVec expand_response_files(StrVec args, char **error);
Options parse_arguments(StrVec args, bool cxx, char **error);
Options parse_arguments_with_env(StrVec args, bool cxx, const char *msvc_env, char **error);
StrVec stage_arguments(Options o);
Target resolve_target(const char *name, char **error);
bool in_target_inventory(const char *inventory, const char *value);
bool arm_arch(const char *arch);
bool is_apple_os(const char *os);
StrVec cpu_flags(Target t, const char *cpu, StrVec forwarded, char **error);
char *project_root(void);
char *default_cache_dir(const char *root);
char *embedded_tool_dir(Toolchain tc);
Toolchain discover_toolchain(Options o, Target t);
bool handle_query(Options o, Target t, Toolchain tc, int *code);
char *compiler_optimization(const char *arg, bool *known);
bool emits_debug_info(Options o, bool include_runtime);
bool optimized_build(Options o);
bool compiler_sanitizer_contains(const char *arg, const char *prefix, const char *name);
bool compiler_plain_assembly(StrVec args);
StrVec compiler_command(Options o, Target t, Toolchain tc, Runtime rt, bool resource_dependencies,
                        char **error);
void dispatch_backend(int argc, char **argv);
Libc_Config libc_config_parse(const char *text, Target t, char **error);
Libc_Config libc_config_from_file(const char *path, Target t, char **error);
StrVec libc_config_include_flags(Libc_Config l, Toolchain tc);
char *libc_config_cache_identity(Target t);
Runtime prepare_linux_external_libc(Options o, Target t, Toolchain tc, Libc_Config l, char **error);
/* Runtime, platform and tools contracts. */
uint64_t runtime_hash(const char *text);
char *runtime_publish(const char *work, const char *dest);
StrVec runtime_cxx_flags(Target t, const char *lib);
char *runtime_build_cxx(Options o, Target t, Toolchain tc, bool only_unwind, char **error);
Options linux_libc_runtime_options(Options o);
StrVec linux_link_arguments(Options o, Target t, StrVec args, char **error);
StrVec msvc_link_arguments(Options o, Target t, StrVec args, char **error);
Options normalize_linker_flags(Options o, Target t, char **error);
StrVec drop_bundled_runtime_libraries(Options o, Target t, bool *libunwind);
Runtime prepare_runtime(Options o, Target t, Toolchain tc, char **error);
Runtime prepare_bsd(Options o, Target t, Toolchain tc, char **error);
Runtime prepare_wasi(Options o, Target t, Toolchain tc, char **error);
Runtime prepare_macos(Options o, Target t, Toolchain tc, char **error);
Runtime prepare_mingw(Options o, Target t, Toolchain tc, char **error);
Runtime prepare_msvc(Options o, Target t, Toolchain tc, char **error);
Runtime prepare_tsan(Options o, Target t, Toolchain tc, char **error);
char *prepare_ubsan(Options o, Target t, Toolchain tc, char **error);
char *prepare_builtins(Options o, Target t, Toolchain tc, char **error);
bool tsan_enabled(Options o);
bool compiler_sanitizer_consumed(const char *arg);
bool compiler_sanitizer_enabled(Options o, StrVec names);
Ubsan_Mode compiler_ubsan_mode(Options o);
bool ubsan_full_enabled(Options o);
bool ubsan_full_supported(Target t);
bool builtins_freestanding_supported(Target t);
bool builtins_msvc_supported(Target t);
char *builtins_driver_path(const char *path);
bool bsd_pic_enabled(Options o, Target t);
bool resource_vfs_eligible(Options o, Toolchain tc, bool dependencies);
char *resource_vfs_overlay(Toolchain tc, char **error);
char *resource_vfs_root(void);
char *resource_vfs_path(const char *path, const char *lib, bool *mapped);
char *resource_vfs_prefix_map(const char *arg, const char *lib);
char *output_root_name(const char *first, const char *fallback);
int run_rc(StrVec args);
int run_llvm_ar(StrVec args);
int run_objcopy(StrVec args);
int run_static_library(Options o, Target t, Toolchain tc, Runtime rt);
int run_mingw_link(Options o, Target t, Toolchain tc, Runtime rt);
int run_macos_link(Options o, Target t, Toolchain tc, Runtime rt);
int run_freestanding_link(Options o, Target t, Toolchain tc, Runtime rt);
bool wants_sparc_static_link(Options o, Target t);
int run_sparc_static_link(Options o, Target t, Toolchain tc, Runtime rt);
bool wants_profile_split(Options o);
int run_profile_link(Options o, Target t, Toolchain tc, Runtime rt);
int run_resource_compiler(Options o, Target t, Toolchain tc, Runtime rt);
int cross_main(int argc, char **argv);
#endif
