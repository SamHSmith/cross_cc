/* Standalone runtime policy and recipe tests. The only fixtures are platform
 * provisioning entry points: these tests must never compile a target runtime.
 * Optional compiler-rt cross builds use explicit CROSS_CC_BUILTINS_TEST_*
 * paths. */
#include "../src/arguments.c"
#include "../src/builtins.c"
#include "../src/builtins_memory.c"
#include "../src/builtins_recipes.c"
#include "../src/libc_config.c"
#include "../src/library_names.c"
#include "../src/runtime.c"
#include "../src/runtime_musl_supplement.c"
#include "../src/runtime_recipes.c"
#include "../src/sparc_tls.c"
#include "../src/support.c"
#include "../src/target.c"
#include "../src/wasi.c"
#include "../src/wasi_recipes.c"
#include "../src/wasi_supplement.c"
#include <assert.h>

char *compiler_optimization(const char *a, bool *known) {
  *known = true;
  if (in_target_inventory("-O0 -Og", a))
    return "-O0";
  if (in_target_inventory("-O1 -O2 -O3 -O4 -Ofast", a))
    return "-O2";
  if (in_target_inventory("-Os -Oz", a))
    return "-Os";
  *known = false;
  return (char *)a;
}
char *glibc_default_version(Target t) {
  if (in_target_inventory("x86 arm armeb", t.arch))
    return "2.34";
  if (streq(t.arch, "powerpc64le"))
    return "2.32";
  if (streq(t.arch, "riscv32"))
    return "2.33";
  if (streq(t.arch, "loongarch64"))
    return "2.36";
  return "2.31";
}
StrVec bsd_include_flags(Target t, const char *lib) {
  (void)t;
  (void)lib;
  assert(!"unexpected BSD provisioning");
  return (StrVec){0};
}
StrVec bsd_version_flags(Target t) {
  (void)t;
  assert(!"unexpected BSD provisioning");
  return (StrVec){0};
}
Runtime prepare_glibc(Options o, Target t, Toolchain tc, char **error) {
  (void)o;
  (void)t;
  (void)tc;
  (void)error;
  assert(!"unexpected glibc provisioning");
  return (Runtime){0};
}
Runtime prepare_msvc(Options o, Target t, Toolchain tc, char **error) {
  (void)o;
  (void)t;
  (void)tc;
  (void)error;
  assert(!"unexpected MSVC provisioning");
  return (Runtime){0};
}

static Target target(const char *name) {
  char *error = NULL;
  Target t = resolve_target(name, &error);
  if (error)
    fprintf(stderr, "target %s: %s\n", name, error);
  assert(!error);
  return t;
}
static void expect_vec(StrVec actual, StrVec expected) {
  assert(actual.len == expected.len);
  for (size_t i = 0; i < actual.len; i++)
    assert(streq(actual.data[i], expected.data[i]));
}
static void expect_text(StrVec actual, const char *expected) {
  char *text = rt_join(actual, " ");
  if (!streq(text, expected))
    fprintf(stderr, "got [%s], expected [%s]\n", text, expected);
  assert(streq(text, expected));
}
#ifdef _WIN32
#define PINNED_LIB "C:/pinned/lib"
#define PINNED_INCLUDE "C:/pinned/lib/include"
#define EXTERNAL_CRT "C:/external/crt"
#else
#define PINNED_LIB "/pinned/lib"
#define PINNED_INCLUDE "/pinned/lib/include"
#define EXTERNAL_CRT "/external/crt"
#endif
static void test_freestanding(void) {
  StrVec names = SV("x86_64-freestanding", "aarch64-freestanding",
                    "wasm32-freestanding", "arm-freestanding");
  char *e = NULL;
  for (size_t i = 0; i < names.len; i++) {
    Target t = target(names.data[i]);
    assert(builtins_freestanding_supported(t));
    Runtime rt = prepare_runtime((Options){0}, t,
                                 (Toolchain){.lib_dir = PINNED_LIB}, &e);
    assert(!e);
    expect_text(rt.compile_flags,
                "-ffreestanding -nostdlibinc -isystem " PINNED_INCLUDE);
    assert(!rt.link_flags.len);
    char *ir = builtins_memory_ir(t);
    assert(contains(
        ir,
        str_fmt("define ptr @memcpy(ptr %%dst, ptr %%src, %s %%n)",
                in_target_inventory("arm wasm32", t.arch) ? "i32" : "i64")));
    assert(!contains(ir, "SIZE"));
    Runtime no_headers =
        prepare_runtime((Options){.no_stdinc = true}, t, (Toolchain){0}, &e);
    assert(!e);
    expect_text(no_headers.compile_flags, "-ffreestanding");
  }
  assert(!builtins_freestanding_supported(target("x86_64-linux-musl")));
  Target uefi = target("x86_64-uefi");
  assert(!builtins_freestanding_supported(uefi));
  prepare_runtime((Options){0}, uefi, (Toolchain){0}, &e);
  assert(e);
  assert(streq(prepare_builtins((Options){.compile_only = true}, (Target){0},
                                (Toolchain){0}, &e),
               ""));
  assert(!e);
  assert(streq(prepare_builtins((Options){.no_defaultlibs = true}, (Target){0},
                                (Toolchain){0}, &e),
               ""));
  assert(!e);
}
static void test_libc_suppression(void) {
  Target t = target("x86_64-linux-musl");
  Toolchain tc = {.lib_dir = PINNED_LIB};
  Options o = {.no_libc = true};
  char *e = NULL;
  Runtime rt = prepare_runtime(o, t, tc, &e);
  assert(!e);
  expect_text(rt.compile_flags, "-nostdlibinc -isystem " PINNED_INCLUDE);
  expect_text(rt.link_flags, "-nostdlib");
  assert(streq(prepare_builtins(o, t, tc, &e), ""));
  assert(!e);
  o.explicit_libc = o.compile_only = true;
  Runtime with_libc = prepare_linux_external_libc(
      o, t, tc,
      (Libc_Config){.include_dir = "/external/include",
                    .sys_include_dir = "/external/sys"},
      &e);
  assert(!e);
  assert(rt_has(with_libc.compile_flags, "/external/include"));
  o.compile_only = false;
  assert(streq(prepare_builtins(o, t, tc, &e), ""));
  assert(!e);
  Libc_Config l = {.include_dir = "/external/include",
                   .sys_include_dir = "/external/sys",
                   .crt_dir = EXTERNAL_CRT,
                   .cc_dir = "/external/cc"};
  rt = prepare_linux_external_libc((Options){.no_libc = true}, t, tc, l, &e);
  assert(!e);
  expect_text(rt.compile_flags, "-nostdlibinc -isystem " PINNED_INCLUDE);
  expect_text(rt.link_flags, "-nostdlib");
  t = target("x86_64-linux-gnu");
  tc.builtins = "/must-not-link-builtins.a";
  Options original = {.no_stdlib = true,
                      .no_defaultlibs = true,
                      .no_startfiles = true,
                      .cxx = true,
                      .explicit_libc = true};
  Options normalized = linux_libc_runtime_options(original);
  assert(!normalized.no_stdlib && !normalized.no_defaultlibs &&
         !normalized.no_startfiles && !normalized.cxx);
  assert(original.no_stdlib && original.no_defaultlibs &&
         original.no_startfiles && original.cxx);
  rt = prepare_linux_external_libc(original, t, tc, l, &e);
  assert(!e);
  assert(rt_has(rt.link_flags, path_join(EXTERNAL_CRT, "crt1.o")) &&
         rt_has(rt.link_flags, "-lc"));
  assert(!rt_has(rt.link_flags, tc.builtins) &&
         !contains(rt_join(rt.link_flags, " "), "libc++.a"));
  rt = prepare_runtime(
      (Options){.no_stdlib = true, .cxx = true, .compile_only = true}, t, tc,
      &e);
  assert(!e);
  expect_text(rt.compile_flags, "-nostdlibinc -isystem " PINNED_INCLUDE);
  Options cpp = linux_libc_runtime_options(
      (Options){.no_stdlib = true, .cxx = true, .explicit_libcxx = true});
  assert(cpp.explicit_libc && cpp.cxx && !cpp.no_stdlib);
}
static void test_bare_links(const char *dir) {
  Target t = target("x86_64-linux-gnu");
  char *e = NULL;
  for (int i = 0; i < 2; i++) {
    Options o = {0};
    o.no_libc = i == 0;
    o.no_stdlib = i != 0;
    expect_text(linux_link_arguments(o, t, SV("start.o"), &e),
                "-Wl,--no-pie,--no-dynamic-linker start.o");
    assert(!e);
    Options pie = o;
    pie.pie = 1;
    expect_text(linux_link_arguments(pie, t, SV("start.o"), &e),
                "-Wl,-pie,--no-dynamic-linker start.o");
    assert(!e);
    Options dyn = o;
    dyn.dynamic_link = true;
    linux_link_arguments(dyn, t, SV("start.o"), &e);
    assert(streq(
        e, "dynamic linking with lld requires at least one shared library"));
    expect_text(
        linux_link_arguments(o, t, SV("-o", "-dynamic", "--", "-pie"), &e),
        "-Wl,--no-pie,--no-dynamic-linker -o -dynamic -- -pie");
    assert(!e);
  }
  char *archive = path_join(dir, "only.a"), *so = path_join(dir, "libonly.so"),
       *nested = path_join(dir, "nested.so");
  assert(write_file(archive, "!<arch>\n", 8));
  const char *text = "/* archive, not DSO */ INPUT ( only.a )";
  assert(write_file(so, text, strlen(text)));
  text = "GROUP ( AS_NEEDED ( libonly.so ) )";
  assert(write_file(nested, text, strlen(text)));
  assert(!linux_shared_input(nested, (StrVec){0}, (StrVec){0}, &e));
  assert(!e);
  Target native = t;
  native.native = true;
  linux_link_arguments((Options){.no_libc = true}, native,
                       SV("-lbare_nonexistent"), &e);
  assert(contains(e, "add an explicit -L directory or direct library path"));
  StrVec args = linux_link_arguments((Options){.no_libc = true}, native,
                                     SV("-L", dir, "-lonly"), &e);
  assert(!e && contains(rt_join(args, " "), "--no-dynamic-linker"));
  unsigned char elf[20] = {0};
  memcpy(elf, "\177ELF", 4);
  elf[5] = 1;
  elf[16] = 3;
  assert(write_file(path_join(dir, "libc.so.6"), elf, sizeof(elf)));
  text = "OUTPUT_FORMAT(elf64-x86-64) GROUP ( libc.so.6 AS_NEEDED ( only.a ) )";
  assert(write_file(so, text, strlen(text)));
  assert(linux_shared_input(nested, (StrVec){0}, (StrVec){0}, &e) && !e);
  args = linux_link_arguments((Options){.no_libc = true}, t,
                              SV("-L", dir, "-lonly"), &e);
  assert(!e && contains(rt_join(args, " "), "--dynamic-linker="));
  linux_link_arguments((Options){.no_libc = true, .static_link = true}, t,
                       SV(so), &e);
  assert(streq(e, "using shared libraries requires dynamic linking"));
  text = "INPUT ( nested.so )";
  assert(write_file(so, text, strlen(text)));
  linux_shared_input(nested, (StrVec){0}, (StrVec){0}, &e);
  assert(contains(e, "cyclic linker script"));
  text = "SECTIONS {}";
  assert(write_file(so, text, strlen(text)));
  linux_shared_input(so, (StrVec){0}, (StrVec){0}, &e);
  assert(contains(e, "unsupported linker script directive"));
  char *bitcode = path_join(dir, "input.o");
  const unsigned char magic[2][4] = {{'B', 'C', 0xc0, 0xde},
                                     {0xde, 0xc0, 0x17, 0x0b}};
  for (size_t i = 0; i < 2; i++) {
    assert(write_file(bitcode, magic[i], 4));
    assert(!linux_shared_input(bitcode, (StrVec){0}, (StrVec){0}, &e));
    assert(!e);
  }
  linux_link_arguments((Options){0}, t, SV("-Wl,-lfoo"), &e);
  assert(contains(e, "unsupported linker arg"));
  linux_link_arguments((Options){0}, t, SV("-L"), &e);
  assert(streq(e, "missing argument to -L"));
  linux_link_arguments((Options){0}, t, SV("-l../bad"), &e);
  assert(contains(e, "invalid system library name"));
  assert(linux_strip_link((Options){.forwarded = SV("-Os")}));
  assert(!linux_strip_link((Options){.forwarded = SV("-Os", "-g")}));
  assert(
      linux_strip_link((Options){.forwarded = SV("-g", "-Wl,--strip-debug")}));
  assert(!linux_strip_link((Options){.forwarded = SV("-s", "-g")}));
  args = linux_link_arguments((Options){.forwarded = SV("-s", "-g")}, t,
                              SV("-s", "-g", "--", "-s"), &e);
  assert(!e);
  expect_text(args, "-g -- -s");
}
static void test_cache_and_hashes(void) {
  assert(streq(
      sha256_text(""),
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  assert(streq(
      sha256_text("abc"),
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  assert(streq(
      sha256_text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  assert(runtime_hash("") == UINT64_C(14695981039346656037));
  assert(runtime_hash("hello") == UINT64_C(0xa430d84680aabd0b));
  Target t = target("x86_64-linux-musl");
  Toolchain a = {.lib_dir = "/lib",
                 .clang = "/c/tools-1/cross_clang",
                 .lld = "/c/tools-1/ld.lld",
                 .ar = "/c/tools-1/llvm-ar",
                 .cache_dir = "/c",
                 .clang_version = "clang version 22.1.8"},
            b = a;
  b.clang = "/d/tools-2/cross_clang";
  b.lld = "/d/tools-2/ld.lld";
  b.ar = "/d/tools-2/llvm-ar";
  StrVec kinds = SV("musl", "cxx-v2-target-features");
  for (size_t i = 0; i < kinds.len; i++)
    assert(streq(runtime_cache(kinds.data[i], (Options){0}, t, a),
                 runtime_cache(kinds.data[i], (Options){0}, t, b)));
  assert(streq(wasi_cache("libc", (Options){0}, t, a),
               wasi_cache("libc", (Options){0}, t, b)));
  Toolchain c = a;
  c.clang_version = "clang version 23.0.0";
  assert(!streq(runtime_cache("musl", (Options){0}, t, a),
                runtime_cache("musl", (Options){0}, t, c)));
  assert(!streq(wasi_cache("libc", (Options){0}, t, a),
                wasi_cache("libc", (Options){0}, t, c)));
  Options cpu = {.cpu = "native"};
  assert(!streq(runtime_cache("musl", (Options){0}, t, a),
                runtime_cache("musl", cpu, t, a)));
  assert(!streq(wasi_cache("libc", (Options){0}, t, a),
                wasi_cache("libc", cpu, t, a)));
  c = a;
  c.lib_dir = "/other";
  assert(!streq(runtime_cache("musl", (Options){0}, t, a),
                runtime_cache("musl", (Options){0}, t, c)));
}
static void test_msvc_stackprobe_ownership(void) {
  const char *arches[] = {"x86_64", "x86", "aarch64"};
  const char *probes[] = {"x86_64/chkstk.S", "i386/chkstk2.S",
                          "aarch64/chkstk.S"};
  for (size_t i = 0; i < 3; i++) {
    Target t = {.arch = (char *)arches[i], .os = "windows", .abi = "msvc"};
    bool ok = false;
    StrVec standalone = builtins_link_sources((Options){.no_libc = true}, t,
                                               &ok);
    assert(ok && rt_has(standalone, probes[i]));
    StrVec crt = builtins_link_sources((Options){0}, t, &ok);
    assert(ok && !rt_has(crt, probes[i]) && crt.len + 1 == standalone.len);
    assert(rt_has(crt, "divdi3.c"));
    if (i < 2)
      assert(rt_has(crt, "cpu_model/x86.c"));
    for (int mode = 0; mode < 2; mode++) {
      Options o = {.no_libc = true};
      o.no_stdlib = mode == 0;
      o.no_defaultlibs = mode == 1;
      char *e = NULL;
      assert(streq(prepare_builtins(o, t, (Toolchain){0}, &e), "") && !e);
    }
    /* GNU and freestanding inventories retain their existing recipes. */
    t.abi = "gnu";
    expect_vec(builtins_link_sources((Options){0}, t, &ok),
               builtins_sources(t.arch, t.os, t.abi, &ok));
    assert(ok);
    t.os = "freestanding";
    t.abi = "";
    expect_vec(builtins_link_sources((Options){0}, t, &ok),
               builtins_sources(t.arch, t.os, t.abi, &ok));
    assert(ok);
  }
}
static void test_recipes(void) {
  StrVec recipes[] = {
      wasi_bottom_half_files, wasi_top_half_files, wasi_fts_files,
      wasi_setjmp_files,      wasi_dl_files,       wasi_process_clocks_files,
      wasi_getpid_files,      wasi_mman_files,     wasi_signal_bottom_files,
      wasi_signal_top_files,  wasi_vendor_files};
  StrVec taken = {0}, seen = {0};
  for (size_t i = 0; i < sizeof(recipes) / sizeof(*recipes); i++)
    for (size_t j = 0; j < recipes[i].len; j++)
      sv_push(&taken, path_base(recipes[i].data[j]));
  for (size_t i = 0; i < wasi_supplement_files.len; i++) {
    char *f = wasi_supplement_files.data[i];
    assert(file_exists(path_join("vendor", f)));
    assert(!rt_has(seen, f));
    sv_push(&seen, f);
    assert(!rt_has(taken, path_base(f)));
  }
  assert(musl_src_files.len > 1000 && musl_supplement_files.len > 400 &&
         libcxxabi_files.len == 19 && libcxx_thread_files.len == 10);
  bool ok = false;
  StrVec list = builtins_sources("x86_64", "linux", "gnu", &ok);
  assert(ok && rt_has(list, "x86_64/floatdidf.c") &&
         !rt_has(list, "floatdidf.c"));
  list = builtins_sources("x86", "windows", "msvc", &ok);
  assert(ok && rt_has(list, "i386/chkstk2.S") && rt_has(list, "floatdidf.c") &&
         !rt_has(list, "i386/floatdidf.S"));
  list = builtins_sources("armeb", "linux", "gnueabi", &ok);
  assert(ok && rt_has(list, "arm/aeabi_memcpy.S") &&
         rt_has(list, "arm/sync_fetch_and_umax_8.S"));
  list = builtins_sources("thumb", "windows", "gnu", &ok);
  assert(ok && rt_has(list, "arm/aeabi_idivmod.S") &&
         !rt_has(list, "arm/aeabi_memcpy.S"));
  list = builtins_sources("powerpc64le", "linux", "gnu", &ok);
  assert(ok && rt_has(list, "ppc/multc3.c") && !rt_has(list, "multf3.c"));
  list = builtins_sources("riscv32", "linux", "musl", &ok);
  assert(ok && rt_has(list, "riscv/mulsi3.S") && rt_has(list, "addtf3.c"));
  builtins_sources("unsupported", "linux", "gnu", &ok);
  assert(!ok);
  assert(streq(runtime_musl_arch_name(target("x86-linux-musl")), "i386"));
  assert(streq(
      runtime_musl_arch_name((Target){.arch = "mips64", .abi = "muslabin32"}),
      "mipsn32"));
  assert(runtime_musl_time32("arm") && !runtime_musl_time32("aarch64"));
  assert(runtime_musl_ptr64("s390x") && !runtime_musl_ptr64("arm"));
  Target wasi = target("wasm32-wasi");
  char *e = NULL;
  Runtime r = prepare_wasi((Options){.cxx = true, .compile_only = true}, wasi,
                           (Toolchain){.lib_dir = PINNED_LIB}, &e);
  assert(!e && rt_has(r.compile_flags, "-D_LIBCPP_HAS_THREADS=0") &&
         !rt_has(r.compile_flags, "-mthread-model"));
}
static void test_library_names(void) {
  struct Case {
    const char *target, *name;
    bool want;
  } cases[] = {{"x86_64-windows-gnu", "m", true},
               {"x86_64-windows-gnu", "M", true},
               {"x86_64-windows-gnu", "mingwex", true},
               {"x86_64-windows-gnu", "uuid", true},
               {"x86_64-windows-gnu", "pthread", true},
               {"x86_64-windows-gnu", "gcc", true},
               {"x86_64-windows-gnu", "kernel32", false},
               {"x86_64-windows-gnu", "dl", false},
               {"x86_64-windows-gnu", "unwind", true},
               {"x86_64-windows-msvc", "unwind", false},
               {"aarch64-macos", "gcc_s", true},
               {"wasm32-wasi", "gcc_eh", true},
               {"x86_64-freebsd", "gcc_eh", true},
               {"x86_64-linux-gnu", "Unwind", false},
               {"x86_64-windows-msvc", "c", true},
               {"x86_64-windows-msvc", "m", false},
               {"aarch64-macos", "m", true},
               {"aarch64-macos", "System", true},
               {"aarch64-macos", "mx", true},
               {"aarch64-ios", "mx", false},
               {"aarch64-macos", "objc", false},
               {"wasm32-wasi", "wasi-emulated-mman", true},
               {"wasm32-wasi", "setjmp", true},
               {"wasm32-wasi", "M", false},
               {"x86_64-linux-musl", "m", false},
               {"x86_64-linux-musl", "atomic", true},
               {"x86_64-linux-musl", "GCC", false},
               {"x86_64-linux-gnu", "gcc_s", true},
               {"x86_64-freebsd", "m", false}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++)
    assert(bundled_runtime_library_name(target(cases[i].target),
                                        cases[i].name) == cases[i].want);
  Target t = target("aarch64-macos");
  Options o = {.forwarded = SV("-lm", "a.c", "-l", "pthread", "-lz",
                               "--library=dl", "--library", "objc", "-L", "-lm",
                               "-o", "-lm", "--", "-lm")};
  bool unwind = false;
  expect_text(drop_bundled_runtime_libraries(o, t, &unwind),
              "a.c -lz --library objc -L -lm -o -lm -- -lm");
  assert(!unwind);
  o.no_stdlib = true;
  expect_vec(drop_bundled_runtime_libraries(o, t, &unwind), o.forwarded);
  o.explicit_libc = true;
  expect_text(drop_bundled_runtime_libraries(o, t, &unwind),
              "a.c -lz --library objc -L -lm -o -lm -- -lm");
  Options u = {.forwarded = SV("a.c", "-lgcc_s", "-l", "unwind")};
  expect_text(drop_bundled_runtime_libraries(u, t, &unwind), "a.c");
  assert(!unwind);
  expect_text(
      drop_bundled_runtime_libraries(u, target("x86_64-linux-musl"), &unwind),
      "a.c");
  assert(unwind);
  expect_text(
      drop_bundled_runtime_libraries(u, target("x86_64-windows-msvc"), &unwind),
      "a.c -lgcc_s -l unwind");
  assert(!unwind);
}
static uint32_t random_state = 0x6c69626e;
static unsigned pick(unsigned n) {
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return random_state % n;
}
static void test_library_fuzz(void) {
  StrVec targets = SV("x86_64-windows-gnu", "aarch64-windows-gnu",
                      "x86_64-windows-msvc", "aarch64-macos", "x86_64-macos",
                      "wasm32-wasi", "x86_64-linux-musl", "aarch64-linux-gnu",
                      "x86_64-freebsd"),
         names = SV("m", "M", "c", "pthread", "uuid", "dxguid", "System",
                    "system", "mx", "dl", "rt", "setjmp", "gcc", "atomic",
                    "ssp", "compiler_rt", "unwind", "gcc_s", "gcc_eh", "z",
                    "kernel32", "objc", ":m", ""),
         others = SV("a.c", "-O2", "-o", "-L", "-x", "-Wl,-lm", "-Xlinker",
                     "--", "-c", "b.o");
  for (int trial = 0; trial < 20000; trial++) {
    Target t = target(targets.data[pick((unsigned)targets.len)]);
    Options o = {.no_stdlib = pick(8) == 0, .explicit_libc = pick(8) == 0};
    unsigned count = pick(10);
    for (unsigned i = 0; i < count; i++) {
      char *name = names.data[pick((unsigned)names.len)];
      switch (pick(5)) {
      case 0:
        sv_push(&o.forwarded, str_fmt("-l%s", name));
        break;
      case 1:
        RT_ADD(&o.forwarded, "-l", name);
        break;
      case 2:
        sv_push(&o.forwarded, str_fmt("--library=%s", name));
        break;
      case 3:
        RT_ADD(&o.forwarded, "--library", name);
        break;
      default:
        sv_push(&o.forwarded, others.data[pick((unsigned)others.len)]);
        break;
      }
    }
    bool unwind = false, want_unwind = false;
    StrVec got = drop_bundled_runtime_libraries(o, t, &unwind);
    bool links = o.explicit_libc || !o.no_stdlib;
    size_t j = 0;
    for (size_t i = 0; i < o.forwarded.len; i++) {
      char *a = o.forwarded.data[i];
      if (streq(a, "--")) {
        expect_vec(sv_slice(o.forwarded, i, o.forwarded.len),
                   sv_slice(got, j, got.len));
        j = got.len;
        break;
      }
      char *value;
      bool separate;
      char kind = linux_library_arg(a, &value, &separate);
      size_t width = 1;
      if (kind == 'l' && separate) {
        if (i + 1 < o.forwarded.len) {
          value = o.forwarded.data[i + 1];
          width = 2;
        } else
          value = "";
      } else if (kind != 'l' && takes_value(a) && i + 1 < o.forwarded.len)
        width = 2;
      bool absorbed = kind == 'l' && links && (width == 2 || !separate) &&
                      bundled_runtime_library_name(t, value);
      if (absorbed && libunwind_library_name(value) &&
          links_bundled_libunwind(t))
        want_unwind = true;
      if (!absorbed)
        for (size_t k = 0; k < width; k++) {
          assert(j < got.len && streq(got.data[j], o.forwarded.data[i + k]));
          j++;
        }
      i += width - 1;
    }
    assert(j == got.len && unwind == want_unwind);
    o.forwarded = got;
    StrVec again = drop_bundled_runtime_libraries(o, t, &unwind);
    expect_vec(again, got);
    assert(!unwind);
  }
}
static void put_be(unsigned char *b, size_t p, int n, uint64_t value) {
  for (int i = n - 1; i >= 0; i--) {
    b[p + (size_t)i] = (unsigned char)value;
    value >>= 8;
  }
}
static uint32_t sparc_op(uint32_t op, uint32_t rd, uint32_t op3, uint32_t rs1,
                         bool imm, uint32_t rs2) {
  return (op << 30) | (rd << 25) | (op3 << 19) | (rs1 << 14) |
         ((uint32_t)imm << 13) | rs2;
}
static unsigned char *tls_object(int mode, size_t *len) {
  /* Modes: general dynamic, initial exec, local dynamic, GD O0 GOT-copy. */
  size_t nr = mode == 2 ? 7 : 4;
  *len = 400 + nr * 24;
  unsigned char *b = xmalloc(*len);
  memcpy(b, "\177ELF", 4);
  b[4] = b[5] = 2;
  put_be(b, 16, 2, 1);
  put_be(b, 18, 2, 43);
  put_be(b, 40, 8, 64);
  put_be(b, 58, 2, 64);
  put_be(b, 60, 2, 4);
  put_be(b, 128 + 4, 4, 1);
  put_be(b, 128 + 24, 8, 320);
  put_be(b, 128 + 32, 8, 32);
  put_be(b, 192 + 4, 4, 2);
  put_be(b, 192 + 24, 8, 352);
  put_be(b, 192 + 32, 8, 48);
  put_be(b, 192 + 56, 8, 24);
  put_be(b, 256 + 4, 4, 4);
  put_be(b, 256 + 24, 8, 400);
  put_be(b, 256 + 32, 8, nr * 24);
  put_be(b, 256 + 40, 4, 2);
  put_be(b, 256 + 44, 4, 1);
  put_be(b, 256 + 56, 8, 24);
  b[352 + 24 + 4] = 0x16;
  put_be(b, 352 + 24 + 6, 2, 1);
  uint32_t types[7] = {56, 57, 58, 59}, offsets[7] = {0, 4, 8, 12};
  uint32_t code[8] = {0x03000000, sparc_op(2, 1, 0, 1, true, 0),
                      sparc_op(2, 8, 0, 2, false, 1), 0x40000000, 0x01000000};
  if (mode == 1) {
    types[0] = 67;
    types[1] = 68;
    types[2] = 70;
    types[3] = 71;
    offsets[3] = 28;
    code[2] = sparc_op(3, 3, 0xb, 2, false, 1);
    code[7] = sparc_op(2, 3, 0, 7, false, 3);
  }
  if (mode == 2) {
    uint32_t ty[7] = {64, 60, 61, 62, 65, 63, 66},
             of[7] = {0, 4, 8, 12, 16, 20, 28};
    memcpy(types, ty, sizeof(ty));
    memcpy(offsets, of, sizeof(of));
    code[1] = 0x05000000;
    code[2] = sparc_op(2, 2, 0, 2, true, 0);
    code[3] = sparc_op(2, 8, 0, 3, false, 2);
    code[4] = sparc_op(2, 1, 3, 1, true, 0);
    code[5] = 0x40000000;
    code[6] = 0x01000000;
    code[7] = sparc_op(2, 4, 0, 8, false, 1);
  }
  if (mode == 3) {
    offsets[2] = 12;
    offsets[3] = 16;
    code[1] = sparc_op(2, 2, 0, 1, true, 0);
    code[2] = sparc_op(2, 1, 2, 0, false, 3);
    code[3] = sparc_op(2, 8, 0, 1, false, 2);
    code[4] = 0x40000000;
    code[5] = 0x01000000;
  }
  for (size_t i = 0; i < 8; i++)
    put_be(b, 320 + i * 4, 4, code[i]);
  for (size_t i = 0; i < nr; i++) {
    put_be(b, 400 + i * 24, 8, offsets[i]);
    put_be(b, 400 + i * 24 + 8, 8, (UINT64_C(1) << 32) | types[i]);
  }
  return b;
}
static void test_sparc_tls(const char *dir) {
  char *e = NULL;
  for (int mode = 0; mode < 4; mode++) {
    size_t len;
    unsigned char *b = tls_object(mode, &len);
    assert(sparc_tls_relax_elf(b, len, &e) && !e);
    assert(sparc_tls_u(b, 408, 8) == ((UINT64_C(1) << 32) | 72));
    assert(!sparc_tls_relax_elf(b, len, &e) && !e);
    free(b);
  }
  size_t len;
  unsigned char *b = tls_object(0, &len);
  b[352 + 24 + 4] = 0x26;
  put_be(b, 352 + 24 + 6, 2, 0);
  assert(!sparc_tls_relax_elf(b, len, &e) && contains(e, "undefined weak"));
  free(b);
  b = tls_object(0, &len);
  put_be(b, 320 + 16, 4, 0);
  assert(!sparc_tls_relax_elf(b, len, &e) && contains(e, "delay slot"));
  free(b);
  b = tls_object(0, &len);
  char *source = path_join(dir, "tls.o"), *dest = path_join(dir, "tls-copy.o");
  assert(write_file(source, b, len));
  assert(sparc_tls_relax_file(source, dest, &e) && !e);
  size_t after_len;
  char *after = read_file(source, &after_len);
  assert(after_len == len && !memcmp(after, b, len));
  assert(!sparc_tls_relax_file(source, source, &e) &&
         contains(e, "private copy"));
  assert(!sparc_tls_relax_file(source, dest, &e) &&
         contains(e, "already exists"));
  size_t archive_len = 68 + len;
  unsigned char *archive = xmalloc(archive_len);
  memcpy(archive, "!<arch>\n", 8);
  memset(archive + 8, ' ', 60);
  memcpy(archive + 8, "member.o/", 9);
  char *size = str_fmt("%zu", len);
  memcpy(archive + 56, size, strlen(size));
  memcpy(archive + 66, "`\n", 2);
  memcpy(archive + 68, b, len);
  source = path_join(dir, "tls.a");
  dest = path_join(dir, "tls-copy.a");
  assert(write_file(source, archive, archive_len));
  assert(sparc_tls_relax_file(source, dest, &e) && !e);
  after = read_file(dest, &after_len);
  assert(after_len == archive_len && !memcmp(after, archive, 68));
  source = path_join(dir, "thin.a");
  assert(write_file(source, "!<thin>\n", 8));
  assert(!sparc_tls_relax_file(source, dest, &e) &&
         contains(e, "thin archives"));
  free(b);
  free(archive);
}
static void optional_builtins_cross(void) {
  char *clang = env("CROSS_CC_BUILTINS_TEST_CLANG"),
       *cache = env("CROSS_CC_BUILTINS_TEST_CACHE"),
       *root = env("CROSS_CC_BUILTINS_TEST_ROOT"),
       *lib = env("CROSS_CC_BUILTINS_TEST_LIB");
  if (!*clang || !*cache || !*root || !*lib)
    return;
  StrVec names =
      SV("aarch64-linux-musl", "aarch64-linux-gnu", "x86_64-linux-gnu",
         "x86-freestanding", "x86_64-freestanding", "aarch64-freestanding",
         "wasm32-freestanding", "arm-freestanding");
  Toolchain tc = {.root = root,
                  .lib_dir = lib,
                  .clang = clang,
                  .ar = path_join(path_dir(clang), "llvm-ar"),
                  .cache_dir = cache,
                  .clang_version = "22.1.8"};
  char *e = NULL;
  for (size_t i = 0; i < names.len; i++) {
    char *archive =
        prepare_builtins((Options){0}, target(names.data[i]), tc, &e);
    if (e)
      fprintf(stderr, "%s\n", e);
    assert(!e && file_exists(archive));
  }
  tc.root = cache;
  tc.lib_dir = cache;
  tc.cache_dir = cache;
  tc.clang_version = "missing-source-test";
  prepare_builtins((Options){0}, target("aarch64-linux-musl"), tc, &e);
  assert(e);
}
static void test_runtime_io(const char *dir) {
  char *e = NULL, *exe = executable_path();
  char *a = path_join(dir, "job spaced-\xc3\xa9.out");
  char *b = path_join(dir, "job two.out"), *c = path_join(dir, "never.out");
  Builtins_Job jobs[] = {{SV(exe, "--job-child", a, "1"), a},
                         {SV(exe, "--job-child", b, "0"), b},
                         {SV(exe, "--job-child", c, "0"), c}};
  assert(streq(builtins_run_jobs(jobs, 3, false), a));
  assert(file_exists(a) && file_exists(b) && !file_exists(c));
  jobs[0].args = SV(exe, "--job-child", a, "0");
  assert(!builtins_run_jobs(jobs, 3, false));
  assert(file_exists(c));
  Toolchain tc = {.ar = exe};
  char *archive = path_join(dir, "response file.a");
  assert(
      !runtime_archive(SV("space name.o", "quote\"name.o", "back\\\\slash.o"),
                       archive, tc, false));
  assert(streq(
      read_file(archive, NULL),
      "\"space name.o\"\n\"quote\\\"name.o\"\n\"back\\\\\\\\slash.o\"\n"));
  assert(!file_exists(str_fmt("%s.rsp", archive)));
  char *work = make_temp_dir(dir, "publish-", &e);
  assert(!e);
  char *dest = path_join(dir, "published"), *winner = path_join(work, "winner");
  assert(write_file(winner, "first", 5));
  assert(!runtime_publish(work, dest));
  assert(streq(read_file(path_join(dest, "complete"), NULL),
               "cross_cc runtime recipe v5\n"));
  work = make_temp_dir(dir, "publish-", &e);
  assert(!e && write_file(path_join(work, "winner"), "second", 6));
  assert(!runtime_publish(work, dest));
  assert(streq(read_file(path_join(dest, "winner"), NULL), "first"));
  assert(remove_tree(work));
}
int main(int argc, char **argv) {
  if (argc == 4 && streq(argv[1], "--job-child")) {
    if (!write_file(argv[2], "done", 4))
      return 2;
    return atoi(argv[3]);
  }
  if (argc == 5 && streq(argv[1], "--rsp-quoting=posix")) {
    assert(streq(argv[2], "rcsD") && argv[4][0] == '@');
    size_t size;
    char *text = read_file(argv[4] + 1, &size);
    return text && write_file(argv[3], text, size) ? 0 : 1;
  }
  char *saved_libc = str_copy(env("ZIG_LIBC"));
  assert(set_env("ZIG_LIBC", ""));
  char *e = NULL, *dir = make_temp_dir("tests", "runtime-test-", &e);
  assert(!e && dir);
  test_freestanding();
  test_libc_suppression();
  test_bare_links(dir);
  test_cache_and_hashes();
  test_msvc_stackprobe_ownership();
  test_recipes();
  test_library_names();
  test_library_fuzz();
  test_sparc_tls(dir);
  test_runtime_io(dir);
  optional_builtins_cross();
  assert(remove_tree(dir));
  assert(set_env("ZIG_LIBC", saved_libc));
  puts("runtime tests passed (including 20000 library argument fuzz cases)");
  return 0;
}
