/* Standalone driver-policy/ABI tests. Runtime build entry points are
   intercepted; no compiler, linker or SDK is required. Parsing and filesystem
   helpers are real. */
#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "../src/arguments.c"
#include "../src/bsd.c"
#include "../src/glibc.c"
#include "../src/macos.c"
#include "../src/profile.c"
#include "../src/support.c"
#include "../src/target.c"
#include "../src/tsan.c"
#include "../src/ubsan.c"
#include <assert.h>

static bool has(StrVec v, const char *a) {
  for (size_t i = 0; i < v.len; i++)
    if (streq(v.data[i], a))
      return true;
  return false;
}
static void expect(StrVec actual, StrVec expected) {
  assert(actual.len == expected.len);
  for (size_t i = 0; i < actual.len; i++)
    assert(streq(actual.data[i], expected.data[i]));
}
char *compiler_optimization(const char *a, bool *known) {
  *known = streq(a, "-O0") || streq(a, "-O1") || streq(a, "-O2") ||
           streq(a, "-O3") || streq(a, "-Os") || streq(a, "-Oz") ||
           streq(a, "-Og") || streq(a, "-Ofast");
  return (char *)a;
}
StrVec compiler_command(Options o, Target t, Toolchain tc, Runtime rt,
                        bool resource_dependencies, char **error) {
  (void)o;
  (void)t;
  (void)tc;
  (void)rt;
  (void)resource_dependencies;
  *error = "command execution is not exercised in policy tests";
  return (StrVec){0};
}
char *deterministic_link_stage(const char *cache, const char *prefix,
                               const char *output, char **error) {
  (void)output;
  return make_temp_dir(cache, prefix, error);
}
StrVec msvc_stage_runtime(StrVec flags, const char *work, bool managed) {
  (void)work;
  (void)managed;
  return flags;
}
char *output_root_name(const char *first, const char *fallback) {
  (void)fallback;
  char *s = path_base(first), *dot = strrchr(s, '.');
  if (dot)
    *dot = 0;
  return s;
}

bool compiler_sanitizer_contains(const char *arg, const char *prefix,
                                 const char *name) {
  if (!starts_with(arg, prefix))
    return false;
  StrVec parts = str_split(arg + strlen(prefix), ",");
  return has(parts, name);
}
uint64_t runtime_hash(const char *text) {
  uint64_t h = UINT64_C(14695981039346656037);
  for (const unsigned char *p = (const unsigned char *)nz(text); *p; p++)
    h = (h ^ *p) * UINT64_C(1099511628211);
  return h;
}
char *stable_compiler_identity(Toolchain tc) {
  (void)tc;
  return "test-compiler";
}
StrVec runtime_base_flags(Options o, Target t, Toolchain tc) {
  (void)o;
  (void)t;
  (void)tc;
  return (StrVec){0};
}
StrVec runtime_cxx_flags(Target t, const char *lib) {
  (void)t;
  (void)lib;
  return SV("-nostdinc++");
}
char *runtime_cache(const char *kind, Options o, Target t, Toolchain tc) {
  (void)o;
  (void)t;
  return path_join(tc.cache_dir, kind);
}
char *runtime_compile(const char *source, const char *object, StrVec flags,
                      Toolchain tc, bool verbose) {
  (void)source;
  (void)object;
  (void)flags;
  (void)tc;
  (void)verbose;
  assert(!"runtime compilation must not occur in policy tests");
  return "not built";
}
char *runtime_archive(StrVec objects, const char *path, Toolchain tc,
                      bool verbose) {
  (void)objects;
  (void)path;
  (void)tc;
  (void)verbose;
  assert(!"runtime archiving must not occur in policy tests");
  return "not built";
}
char *runtime_publish(const char *work, const char *dest) {
  (void)work;
  (void)dest;
  assert(!"runtime publishing must not occur in policy tests");
  return "not built";
}
char *runtime_build_cxx(Options o, Target t, Toolchain tc, bool only_unwind,
                        char **error) {
  (void)o;
  (void)t;
  (void)tc;
  (void)only_unwind;
  assert(!"C++ runtime builds must not occur in policy tests");
  *error = "not built";
  return NULL;
}
char *builtins_run_jobs(Builtins_Job *jobs, size_t count, bool verbose) {
  (void)jobs;
  (void)count;
  (void)verbose;
  assert(!"compiler jobs must not occur in policy tests");
  return "not built";
}
char *sha256_text(const char *text) {
  (void)text;
  return "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
}

static void test_sanitizer_policy(void) {
  struct {
    StrVec args;
    bool enabled;
  } rows[] = {
      {SV("-fsanitize=thread"), true},
      {SV("-fsanitize=undefined,thread", "-fno-sanitize=undefined"), true},
      {SV("-fsanitize=thread", "-fno-sanitize=thread"), false},
      {SV("-fsanitize=thread", "-fno-sanitize=all"), true},
      {SV("-fno-sanitize=all", "-fsanitize=thread"), true},
      {SV("-fno-sanitize=thread", "-fsanitize=thread"), true},
      {SV("-fsanitize=undefined"), false},
      {SV("-Xclang", "-fsanitize=thread"), false},
      {SV("-o", "-fsanitize=thread"), false},
      {SV("--", "-fsanitize=thread"), false}};
  for (size_t i = 0; i < sizeof(rows) / sizeof(*rows); i++)
    assert(tsan_enabled((Options){.forwarded = rows[i].args}) ==
           rows[i].enabled);
  char *error = NULL;
  Options o = {.forwarded = SV("-fsanitize=thread")};
  Runtime r = prepare_tsan(o,
                           (Target){.name = "x86_64-windows-gnu",
                                    .os = "windows",
                                    .arch = "x86_64",
                                    .abi = "gnu"},
                           (Toolchain){0}, &error);
  assert(contains(error, "not supported"));
  o.compile_only = true;
  r = prepare_tsan(o, (Target){0}, (Toolchain){0}, &error);
  assert(!error && !r.link_flags.len);
  o.compile_only = false;
  o.shared = true;
  r = prepare_tsan(o, (Target){.os = "linux"}, (Toolchain){0}, &error);
  assert(!error);
  expect(r.link_flags, SV("-fno-sanitize-link-runtime"));
  o.shared = false;
  o.static_link = true;
  r = prepare_tsan(o, (Target){.os = "linux", .arch = "x86_64", .abi = "gnu"},
                   (Toolchain){0}, &error);
  assert(contains(error, "requires dynamically linked libc"));
  r = prepare_tsan(o, (Target){.os = "linux", .arch = "x86_64", .abi = "musl"},
                   (Toolchain){0}, &error);
  assert(contains(error, "requires -dynamic"));
  assert(streq(tsan_asm_source("x86"), ""));
  assert(streq(tsan_asm_source("aarch64"), "tsan_rtl_aarch64.S"));
  assert(tsan_sources.len == 30 && tsan_unix_sources.len == 2 &&
         tsan_sanitizer_common_sources.len == 45);
  struct {
    StrVec args;
    Ubsan_Mode mode;
    bool full;
  } modes[] = {
      {(StrVec){0}, UBSAN_FULL, true},
      {SV("-O2"), UBSAN_OFF, false},
      {SV("-O2", "-fsanitize=undefined"), UBSAN_FULL, true},
      {SV("-fsanitize-trap=undefined"), UBSAN_TRAP, false},
      {SV("-fno-sanitize-trap=undefined"), UBSAN_OFF, false},
      {SV("-fsanitize-trap=undefined", "-fno-sanitize-trap=undefined"),
       UBSAN_FULL, true},
      {SV("-fsanitize-trap=undefined", "-fsanitize=undefined"), UBSAN_FULL,
       true},
      {SV("-fsanitize=undefined", "-fno-sanitize=undefined"), UBSAN_OFF, false},
      {SV("-O2", "-fsanitize=signed-integer-overflow"), UBSAN_OFF, true},
      {SV("-O2", "-fsanitize=signed-integer-overflow",
          "-fsanitize-trap=signed-integer-overflow"),
       UBSAN_OFF, false},
      {SV("-fsanitize-trap=undefined", "-fno-sanitize-trap=null"), UBSAN_TRAP,
       true},
      {SV("-O2", "-fsanitize=undefined,address"), UBSAN_FULL, true},
      {SV("-O2", "-Xclang", "-fsanitize=undefined"), UBSAN_OFF, false},
      {SV("-O2", "--", "-fsanitize=undefined"), UBSAN_OFF, false},
      {SV("-O2", "-fsanitize=unsigned-integer-overflow"), UBSAN_OFF, true},
      {SV("-O2", "-fsanitize=integer", "-fsanitize-trap=all"), UBSAN_OFF,
       false},
      {SV("-fno-sanitize=all"), UBSAN_FULL, false}};
  for (size_t i = 0; i < sizeof(modes) / sizeof(*modes); i++) {
    o = (Options){.forwarded = modes[i].args};
    assert(compiler_ubsan_mode(o) == modes[i].mode);
    assert(ubsan_full_enabled(o) == modes[i].full);
  }
  assert(!ubsan_full_supported(
      (Target){.os = "linux", .arch = "x86_64", .abi = "gnux32"}));
  assert(ubsan_full_supported(
      (Target){.os = "linux", .arch = "mips64", .abi = "muslabi64"}));
  assert(!ubsan_full_supported(
      (Target){.os = "linux", .arch = "mips64", .abi = "gnuabi64"}));
  assert(ubsan_full_supported(
      (Target){.os = "windows", .arch = "aarch64", .abi = "msvc"}));
}

typedef struct {
  unsigned char data[2048];
  size_t len;
} Test_Bytes;
static void put_byte(Test_Bytes *b, unsigned x) {
  assert(b->len < sizeof(b->data));
  b->data[b->len++] = (unsigned char)x;
}
static void put_text(Test_Bytes *b, const char *text) {
  for (; *text; text++)
    put_byte(b, (unsigned char)*text);
  put_byte(b, 0);
}
static void put_u16(Test_Bytes *b, unsigned x) {
  put_byte(b, x);
  put_byte(b, x >> 8);
}
static void set_u32(unsigned char *b, unsigned x) {
  b[0] = (unsigned char)x;
  b[1] = (unsigned char)(x >> 8);
  b[2] = (unsigned char)(x >> 16);
  b[3] = (unsigned char)(x >> 24);
}
static void test_abi_and_apple(void) {
  char *error = NULL;
  char *work = make_temp_dir(".", "platform-test-", &error);
  assert(work && !error);
  bool ok = false;
  Glibc_Version v = glibc_version("2.31", &ok);
  assert(ok && v.data[0] == 2 && v.data[1] == 31 && !v.data[2]);
  glibc_version("2.31.256", &ok);
  assert(!ok);
  glibc_version("2", &ok);
  assert(!ok);
  glibc_version("2..1", &ok);
  assert(!ok);
  assert(streq(glibc_default_version((Target){.arch = "x86"}), "2.34"));
  assert(streq(glibc_source_arch("sparc64"), "sparc/sparc64"));
  assert(
      streq(glibc_word((Target){.arch = "x86_64", .abi = "gnux32"}), ".long"));
  unsigned char overflow[] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 2};
  Glibc_Reader reader = {overflow, sizeof(overflow), 0, false};
  glibc_uleb(&reader);
  assert(reader.bad);
  Test_Bytes b = {0};
  put_byte(&b, 8);
  for (int i = 0; i < 8; i++)
    put_text(&b, GLIBC_LIBS[i].name);
  put_byte(&b, 2);
  put_byte(&b, 2);
  put_byte(&b, 2);
  put_byte(&b, 5);
  put_byte(&b, 2);
  put_byte(&b, 34);
  put_byte(&b, 0);
  put_byte(&b, 1);
  put_text(&b, "x86_64-linux-gnu");
  put_u16(&b, 2);
  put_text(&b, "foo");
  put_byte(&b, 1);
  put_byte(&b, 1);
  put_byte(&b, 128);
  put_byte(&b, 1);
  put_byte(&b, 129);
  put_byte(&b, 129);
  put_u16(&b, 1);
  put_text(&b, "global");
  put_byte(&b, 1);
  put_byte(&b, 8);
  put_byte(&b, 129);
  put_byte(&b, 129);
  put_u16(&b, 0);
  Target t = {.arch = "x86_64", .os = "linux", .abi = "gnu"};
  error = glibc_emit(b.data, b.len, t, GV(2, 34, 0), work);
  assert(!error);
  size_t size = 0;
  char *text = read_file(path_join(work, "c.s"), &size);
  assert(text && contains(text, "foo@GLIBC_2.2.5") &&
         contains(text, "foo@@GLIBC_2.34") &&
         contains(text, ".size global_1, 8"));
  free(text);
  error = glibc_emit(b.data, b.len - 1, t, GV(2, 34, 0), work);
  assert(error);
  error = glibc_emit(b.data, b.len, t, GV(2, 31, 1), work);
  assert(contains(error, "unsupported glibc version"));
  t.arch = "invalid";
  error = glibc_emit(b.data, b.len, t, GV(2, 34, 0), work);
  assert(contains(error, "no target"));
  b = (Test_Bytes){0};
  put_byte(&b, 10);
  for (int i = 0; i < 10; i++)
    put_text(&b, BSD_FREEBSD_LIBS[i].name);
  put_byte(&b, 1);
  put_byte(&b, 1);
  put_byte(&b, 7);
  put_byte(&b, 0);
  put_byte(&b, 1);
  put_text(&b, "x86_64-freebsd-none");
  put_u16(&b, 1);
  put_text(&b, "bsd_foo");
  put_byte(&b, 1);
  put_byte(&b, 131);
  put_byte(&b, 128);
  put_u16(&b, 0);
  put_u16(&b, 0);
  t = (Target){.name = "x86_64-freebsd.14.0",
               .arch = "x86_64",
               .os = "freebsd",
               .abi = "none"};
  error = bsd_emit(b.data, b.len, t, GV(14, 0, 0), work);
  assert(!error);
  text = read_file(path_join(work, "c.s"), &size);
  assert(text && contains(text, "bsd_foo@@FBSD_1.7") &&
         contains(text, ".globl environ"));
  free(text);
  error = bsd_emit(b.data, b.len - 1, t, GV(14, 0, 0), work);
  assert(error);
  assert(streq(bsd_filename(t, (Glibc_Lib){"util", 9}, GV(15, 0, 0)),
               "libutil.so.10"));
  v = bsd_version(
      (Target){.name = "x86_64-netbsd.10.1...11.0", .os = "netbsd"});
  assert(v.data[0] == 10 && v.data[1] == 1 && !v.data[2]);
  Options o = {0};
  Target openbsd = {.os = "openbsd"};
  assert(bsd_pie_enabled(o, openbsd) && bsd_pic_enabled(o, openbsd));
  o.pic_mode = -1;
  assert(contains(bsd_pic_error(o, openbsd), "requires enabling"));
  o.shared = true;
  o.pie_mode = 1;
  assert(contains(bsd_pic_error(o, openbsd), "dynamic libraries"));
  t = (Target){.os = "ios",
               .clang = "aarch64-apple-ios17.0-simulator",
               .arch = "aarch64",
               .abi = "simulator"};
  assert(streq(apple_explicit_version(t), "17.0"));
  assert(streq(apple_triple(t), "aarch64-apple-ios17.0-simulator"));
  t.os = "visionos";
  t.clang = "aarch64-apple-xros";
  assert(streq(apple_triple(t), "aarch64-apple-xros27.0.0-simulator"));
  assert(streq(macos_output_path(SV("-o", "one", "--output=two", "-othree")),
               "three"));
  o = (Options){.compile_only = true};
  Runtime rt =
      prepare_macos(o, (Target){.os = "macos", .clang = "x86_64-apple-macos"},
                    (Toolchain){.lib_dir = "lib"}, &error);
  assert(!error && has(rt.compile_flags, "-mmacosx-version-min=15.0.0") &&
         has(rt.compile_flags, "-mlinker-version=820"));
  unsigned char image[79] = {0};
  set_u32(image, 0xfeedfacf);
  set_u32(image + 12, 2);
  set_u32(image + 20, 24);
  set_u32(image + 32, 2);
  set_u32(image + 36, 24);
  set_u32(image + 40, 56);
  set_u32(image + 44, 1);
  set_u32(image + 48, 72);
  set_u32(image + 52, 7);
  set_u32(image + 56, 1);
  image[60] = 0x66;
  memcpy(image + 72, "\0foo.o\0", 7);
  char *image_path = path_join(work, "image");
  assert(write_file(image_path, image, sizeof(image)));
  StrVec map = macho_debug_map(image_path, &ok);
  assert(ok);
  expect(map, SV("foo.o"));
  set_u32(image + 56, 7);
  assert(write_file(image_path, image, sizeof(image)));
  macho_debug_map(image_path, &ok);
  assert(!ok);
  set_u32(image + 12, 1);
  assert(write_file(image_path, image, sizeof(image)));
  macho_debug_map(image_path, &ok);
  assert(!ok);
  remove_tree(work);
}

int main(void) {
  test_sanitizer_policy();
  test_abi_and_apple();
  assert(is_profile_arg("--coverage"));
  assert(is_profile_arg("-fprofile-instr-generate"));
  assert(is_profile_arg("-fno-profile-arcs"));
  assert(is_profile_arg("-fcoverage-mapping"));
  assert(!is_profile_arg("-fdiagnostics-color"));
  assert(is_linker_input("a.o"));
  assert(is_linker_input("/x/libc.so.6"));
  assert(is_linker_input("stub.tbd"));
  assert(!is_linker_input("a.c"));
  Options o = {.has_input = true, .forwarded = SV("-O2", "--coverage", "a.c")};
  assert(wants_profile_split(o));
  o.dry_run = true;
  assert(!wants_profile_split(o));
  o.dry_run = false;
  o.compile_only = true;
  assert(!wants_profile_split(o));
  o.compile_only = false;
  o.forwarded = SV("-Xclang", "--coverage", "a.c");
  assert(!wants_profile_split(o));
  o.forwarded = SV("--", "--coverage");
  assert(!wants_profile_split(o));
  o.forwarded = SV("-o", "--coverage", "a.c");
  assert(!wants_profile_split(o));
  o.forwarded = SV("-O2", "--coverage", "a.c", "b.cpp", "libx.so.1", "-L",
                   "libs", "-lfoo", "-o", "app");
  Split_Link s = split_link_arguments(o, "stage", false);
  expect(s.compile_args, SV("-O2", "--coverage"));
  expect(s.inputs, SV("a.c", "b.cpp"));
  expect(s.objects, SV("stage/input-0.o", "stage/input-1.o"));
  expect(s.link_args, SV("-O2", "stage/input-0.o", "stage/input-1.o",
                         "libx.so.1", "-L", "libs", "-lfoo", "-o", "app"));
  assert(!has(s.link_args, "--coverage"));
  o.forwarded = SV("-x", "c", "a.o", "-xnone", "b.o", "--language=c++", "c.lib",
                   "--language=none", "--", "-d.o", "-source.c");
  s = split_link_arguments(o, "stage", false);
  expect(s.inputs, SV("a.o", "c.lib", "-source.c"));
  expect(s.languages, SV("c", "c++", "none"));
  assert(has(s.link_args, "./-d.o"));
  o.forwarded = SV("-I", "inc", "-DVALUE=1", "-include", "forced.h", "-std=c11",
                   "-O2", "-g", "-Werror", "-Wp,a", "-Wa,b",
                   "-fsanitize=undefined", "-x", "c", "a.c", "--for-linker",
                   "/debug", "--force-link=x", "b.DLL", "-oapp.exe");
  s = split_link_arguments(o, "stage", true);
  assert(has(s.compile_args, "-I"));
  assert(has(s.compile_args, "forced.h"));
  assert(!has(s.link_args, "-I"));
  assert(!has(s.link_args, "-std=c11"));
  assert(!has(s.link_args, "-Wp,a"));
  assert(!has(s.link_args, "-Wa,b"));
  assert(has(s.link_args, "-Werror"));
  assert(has(s.link_args, "-g"));
  assert(has(s.link_args, "-O2"));
  assert(has(s.link_args, "-fsanitize=undefined"));
  assert(has(s.link_args, "--for-linker"));
  assert(has(s.link_args, "-oapp.exe"));
  expect(s.inputs, SV("a.c", "b.DLL"));
  o.forwarded = SV("a.c", "b.DLL", "c.DEF", "d.OBJ");
  s = split_link_arguments(o, "stage", true);
  expect(s.inputs, SV("a.c"));
  assert(has(s.link_args, "b.DLL"));
  assert(has(s.link_args, "c.DEF"));
  assert(has(s.link_args, "d.OBJ"));
  o.forwarded = SV("-o", "app", "--", "-literal.c");
  s = split_link_arguments(o, "stage", true);
  expect(s.inputs, SV("-literal.c"));
  expect(s.objects, SV("stage/input-0.obj"));
  puts("profile policy tests passed");
  return 0;
}
