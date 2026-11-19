#ifndef RESOURCES_H
#define RESOURCES_H
/* Resources, archives, objcopy, linker-flag translation and link determinism.
   Core declarations (Options, Target, StrVec, run_rc, run_objcopy, ...) live in cross.h. */
#include "cross.h"

/* Growable byte buffer for binary data that may contain NUL bytes. */
typedef struct { unsigned char *data; size_t len, cap; } Bytes;
void bytes_push(Bytes *b, unsigned char c);
void bytes_append(Bytes *b, const void *data, size_t n);

/* objcopy.c: std.fmt.parseInt for u64; base 0 accepts 0x/0o/0b and '_' separators.
   error is "InvalidCharacter" or "Overflow", NULL on success. */
uint64_t objcopy_parse_uint(const char *text, int base, uint64_t max, char **error);

/* resource_vfs.c */
#define RESOURCE_VFS_ROOT "/cross_cc/lib"
char *resource_vfs_overlay_root(Toolchain tc, const char *virtual_root, const char *physical, char **error);
typedef struct { char *path, *targets; bool enabled; } Resource_Dependency_Plan;
char *resource_make_quote(const char *text);
Resource_Dependency_Plan resource_dependency_plan(Options o, Toolchain tc);
char *resource_dependency_text(const char *text, const char *targets, const char *lib, char **error);

/* determinism.c */
char *deterministic_link_stage(const char *cache_dir, const char *prefix, const char *output, char **error);
StrVec msvc_stage_runtime(StrVec flags, const char *work, bool managed_sdk);
bool msvc_stage_libraries(const char *dir, const char *alias, bool managed_sdk, int depth);
bool canonical_mingw_link(Options o);
uint32_t coff_u32(const unsigned char *b, size_t at);
bool canonical_mingw_debug_paths(const unsigned char *data, size_t len);
bool canonical_mingw_object(const char *path);
bool canonical_mingw_inputs(StrVec link);
char *canonical_mingw_relative(const char *work, const char *path);
char *canonical_mingw_path(const char *path);
StrVec canonical_mingw_runtime(StrVec link, const char *work, size_t managed_start, char **error);
StrVec canonical_mingw_arguments(StrVec link, const char *work, StrVec *outputs, StrVec *destinations, char **error);
int run_canonical_mingw_link(StrVec link, const char *stage, size_t managed_start, bool verbose);
/* Lexical relative path from base to target (both cleaned); NULL when they cannot be related. */
char *path_relative(const char *base, const char *target);
/* Lexical path cleaning: collapse separators, drop "." and resolve "..". */
char *path_clean(const char *path);

/* rc.c: little-endian .res serialization helpers and the resource compiler. */
void rc_u16(Bytes *b, uint16_t v);
void rc_u32(Bytes *b, uint32_t v);
void rc_set16(unsigned char *b, size_t at, uint16_t v);
void rc_set32(unsigned char *b, size_t at, uint32_t v);
uint16_t rc_get16(const unsigned char *b, size_t at);
uint32_t rc_get32(const unsigned char *b, size_t at);

#endif
