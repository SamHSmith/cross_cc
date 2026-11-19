#ifndef CROSS_PLATFORMS_H
#define CROSS_PLATFORMS_H
#include "cross.h"
#include "runtime.h"
#include <inttypes.h>
typedef struct {
  const char *name;
  int sover;
} Glibc_Lib;
typedef struct {
  int data[3];
} Glibc_Version;
typedef struct {
  const unsigned char *data;
  size_t len, pos;
  bool bad;
} Glibc_Reader;
unsigned char glibc_byte(Glibc_Reader *r);
int glibc_u16(Glibc_Reader *r);
char *glibc_zstr(Glibc_Reader *r);
uint64_t glibc_uleb(Glibc_Reader *r);
Glibc_Version glibc_version(const char *s, bool *ok);
int glibc_ver_cmp(Glibc_Version a, Glibc_Version b);
char *glibc_ver_name(Glibc_Version v);
char *glibc_write(const char *path, const char *text);
char *glibc_emit(const unsigned char *data, size_t size, Target t,
                 Glibc_Version version, const char *dir);
const char *glibc_word(Target t);
const char *glibc_source_arch(const char *arch);
char *glibc_default_version(Target t);
StrVec glibc_includes(Target t, Toolchain tc);
Runtime prepare_glibc(Options o, Target t, Toolchain tc, char **error);
typedef struct {
  StrVec compile_args, link_args, inputs, objects, languages;
} Split_Link;
bool is_profile_arg(const char *a);
bool is_linker_input(const char *a);
bool split_msvc_driver_arg(const char *a);
Split_Link split_link_arguments(Options o, const char *work, bool msvc);
StrVec msvc_stage_runtime(StrVec flags, const char *work, bool managed);
const Glibc_Lib *bsd_libs(Target t, size_t *count);
Glibc_Version bsd_version(Target t);
char *bsd_filename(Target t, Glibc_Lib lib, Glibc_Version version);
char *bsd_emit(const unsigned char *data, size_t size, Target t,
               Glibc_Version version, const char *dir);
bool bsd_pie_enabled(Options o, Target t);
char *bsd_pic_error(Options o, Target t);
char *tsan_asm_source(const char *arch);
char *tsan_build(Options o, Target t, Toolchain tc, char **error);
/* Small append-only text builder used by the binary ABI decoders. */
typedef struct {
  char *data;
  size_t len, cap;
} Platform_Text;
static inline void pt_append(Platform_Text *b, const char *text) {
  size_t n = strlen(nz(text));
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2;
    b->data = xrealloc(b->data, b->cap);
  }
  memcpy(b->data + b->len, nz(text), n + 1);
  b->len += n;
}
static inline void pt_printf(Platform_Text *b, const char *fmt, ...) {
  va_list ap, copy;
  va_start(ap, fmt);
  va_copy(copy, ap);
  int n = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (n >= 0) {
    char *s = xmalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    pt_append(b, s);
    free(s);
  }
  va_end(ap);
}
#endif
