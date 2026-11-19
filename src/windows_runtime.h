#ifndef CROSS_WINDOWS_RUNTIME_H
#define CROSS_WINDOWS_RUNTIME_H
#include "cross.h"
#include "runtime.h"

/* Windows runtime and SDK handling: MSVC (managed/system SDK) and MinGW. */

/* ---- Contracts implemented by other modules (see runtime.h for the runtime ones). ---- */
uint64_t objcopy_parse_uint(const char *text, int base, uint64_t max, char **error);
bool canonical_mingw_link(Options o);
bool canonical_mingw_inputs(StrVec link);
char *canonical_mingw_path(const char *path);
int run_canonical_mingw_link(StrVec link, const char *stage, size_t managed_start, bool verbose);

/* ---- msvc.c ---- */
typedef struct { char *name; int type; } Msvc_Dir_Entry; /* type: 0 file, 1 directory, 2 symlink/junction, 3 other */
typedef struct { Msvc_Dir_Entry *data; size_t len, cap; } Msvc_Dir_Entries;

const char *msvc_arch_dir(const char *arch);
Msvc_Libc msvc_libc_from_file(const char *path, char **error);
bool msvc_version_less(const char *a, const char *b);
bool msvc_read_dir(const char *dir, Msvc_Dir_Entries *out); /* false: cannot read */
char *msvc_newest_subdir(const char *dir, bool (*ok)(const char *path));
StrVec msvc_include_dirs(Msvc_Libc l);
char *msvc_dirname(const char *path);
bool msvc_user_maps_resources(StrVec args, const char *root);
Msvc_Libc msvc_libc_detect(Target t, char **error); /* msvc_windows.c or msvc_other.c */

/* ---- msvc_libcxx.c ---- */
extern const char MSVC_LIBCXX_NEW_HANDLER_HEADER[];
char *msvc_libcxx_headers(Toolchain tc, char **error);
StrVec msvc_libcxx_flags(Target t, Toolchain tc, char **error);
char *msvc_libcxx_cache_kind(Options o, Msvc_Libc l);
char *msvc_build_libcxx(Options o, Target t, Toolchain tc, Msvc_Libc l, char **error);

/* ---- msvc_link.c ---- */
bool msvc_link_subsystem(const char *value);

/* ---- msvc_managed.c ---- */
typedef struct { char *url, *sha256, *member, *binary_sha256; } Managed_Msvc_Helper;
typedef struct {
    char *resource_version, *crt_version, *sdk_version, *sdk_package_version, *license_version;
    StrVec license_urls;
    char *manifest_json; /* compact JSON text of the pinned manifest */
    char *helper_version;
    char **host_names;
    Managed_Msvc_Helper *hosts;
    size_t host_count;
} Managed_Msvc_Lock;
typedef struct { char *path, *sha256, *modified, *link; int64_t size; } Managed_Msvc_File;
typedef struct { Managed_Msvc_File *data; size_t len, cap; } Managed_Msvc_Files;

extern const char MSVC_LOCK_TEXT[];
char *msvc_digest(const void *data, size_t size);
char *msvc_file_digest(const char *path, char **error);
Managed_Msvc_Lock msvc_lock(char **error);
const char *msvc_host(void);
char *msvc_resource_dir(Toolchain tc, Target t);
Msvc_Libc msvc_managed_layout(const char *root, Target t);
bool msvc_safe_relative(const char *p);
bool msvc_layout_valid(Msvc_Libc l);
char *msvc_reparse_target(const uint8_t *data, size_t size, char **error);
char *msvc_read_link(const char *path, char **error);
char *msvc_final_path(const char *path, char **error);
bool msvc_path_within(const char *root, const char *target, bool windows);
char *msvc_inventory(const char *root, const char *relative, Managed_Msvc_Files *files); /* error or NULL */
char *msvc_receipt_json(const char *arch, const Managed_Msvc_Files *files);
Msvc_Libc msvc_cached(Toolchain tc, Target t, char **error);
char *msvc_license_receipt(Managed_Msvc_Lock lock);
char *msvc_consent(Options o, Toolchain tc, Managed_Msvc_Lock lock); /* error or NULL */
Msvc_Libc managed_msvc(Options o, Target t, Toolchain tc, char **error);
bool msvc_source_needs_sdk(const uint8_t *source, size_t size);
bool msvc_without_sdk(Options o, Target t, Toolchain tc, Runtime r);

/* ---- mingw_recipes.c ---- */
extern const char *const mingw32_generic_src[];
extern const size_t mingw32_generic_src_len;
extern const char *const mingw32_x86_32_src[];
extern const size_t mingw32_x86_32_src_len;
extern const char *const mingw32_arm_src[];
extern const size_t mingw32_arm_src_len;
extern const char *const mingw32_x86_src[];
extern const size_t mingw32_x86_src_len;
extern const char *const mingw32_winpthreads_src[];
extern const size_t mingw32_winpthreads_src_len;

/* ---- mingw.c ---- */
extern const char *const mingw_always_link_libs[];
extern const size_t mingw_always_link_libs_len;
extern const char MINGW_SPINLOCK_SOURCE[];
const char *mingw_arch_directory(Target t);
const char *mingw_coff_machine(Target t);
char *mingw_import_definition(const char *library, Target t, Toolchain tc);
char *mingw_build_import(const char *library, const char *definition, const char *work, Target t, Toolchain tc, bool verbose);
char *mingw_build_uuid(Options o, Target t, Toolchain tc, char **error);
StrVec mingw_user_imports(StrVec args, Options o, Target t, Toolchain tc, char **error);
char *runtime_build_mingw_threads(Options o, Target t, Toolchain tc, char **error);
#endif
