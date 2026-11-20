#include "../src/arguments.c"
#include "../src/cross.h"
#include "../src/libc_config.c"
#include "../src/support.c"
#include "../src/target.c"
#include <assert.h>

static size_t assertions;
#define CHECK(expr)                                                                                \
    do                                                                                             \
    {                                                                                              \
        assertions++;                                                                              \
        if (!(expr))                                                                               \
        {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr);                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#ifndef _WIN32
#include <signal.h>
#include <sys/file.h>

/* Observe only embedded_tool_dir's side effects, not the fixture's setup. */
static bool tools_forbid_mutations, tools_interrupt_lock, tools_die_in_stage;
static int tools_lock_probe = -1;
static unsigned tools_lock_calls, tools_stage_calls, tools_publish_calls;
static int test_tools_flock(int fd, int operation)
{
    tools_lock_calls++;
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    if (tools_interrupt_lock)
    {
        tools_interrupt_lock = false;
        errno = EINTR;
        return -1;
    }
    if (tools_lock_probe >= 0)
    {
        /* Signal only after proving this process reached a contended lock. */
        CHECK(flock(fd, operation | LOCK_NB) == -1);
        CHECK(errno == EWOULDBLOCK || errno == EAGAIN);
        CHECK(write(tools_lock_probe, "L", 1) == 1);
        close(tools_lock_probe);
        tools_lock_probe = -1;
    }
    return flock(fd, operation);
}
static char *test_tools_stage(const char *parent, const char *prefix, char **error)
{
    CHECK(!tools_forbid_mutations);
    tools_stage_calls++;
    if (tools_die_in_stage)
        raise(SIGKILL);
    return make_temp_dir(parent, prefix, error);
}
static bool test_tools_link(const char *src, const char *dest)
{
    CHECK(!tools_forbid_mutations);
    return link_file(src, dest);
}
static bool test_tools_copy(const char *dest, const char *src)
{
    CHECK(!tools_forbid_mutations);
    return copy_file(dest, src);
}
static bool test_tools_remove(const char *path)
{
    CHECK(!tools_forbid_mutations);
    return remove_tree(path);
}
#define flock test_tools_flock
#define make_temp_dir test_tools_stage
#define link_file test_tools_link
#define copy_file test_tools_copy
#define remove_tree test_tools_remove
#endif
#include "../src/toolchain.c"
#ifndef _WIN32
#undef flock
#undef make_temp_dir
#undef link_file
#undef copy_file
#undef remove_tree
#endif

/* Isolate argument/command construction from runtime builds and LLVM in this unit binary.
   The real callbacks and their integration are covered by the runtime/resource suites. */
uint64_t runtime_hash(const char *text)
{
    uint64_t h = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    {
        h ^= *p;
        h *= UINT64_C(1099511628211);
    }
    return h;
}
char *runtime_publish(const char *work, const char *dest)
{
#ifndef _WIN32
    CHECK(!tools_forbid_mutations);
    tools_publish_calls++;
    /* Match runtime_publish's marker-before-rename contract without runtime builds. */
    CHECK(write_file(path_join(work, "complete"), "test\n", 5));
    CHECK(move_path(work, dest));
    return NULL;
#else
    (void)work;
    (void)dest;
    assert(!"unit test unexpectedly provisions embedded tools");
    return "unexpected provision";
#endif
}
StrVec runtime_cxx_flags(Target t, const char *lib)
{
    (void)t;
    return SV("-nostdinc++", "-isystem", path_join(lib, "libcxx", "include"));
}
char *runtime_build_cxx(Options o, Target t, Toolchain tc, bool only_unwind, char **error)
{
    (void)o;
    (void)t;
    (void)tc;
    (void)only_unwind;
    *error = "unit test must not build runtimes";
    return "";
}
Options linux_libc_runtime_options(Options o)
{
    return o;
}
StrVec linux_link_arguments(Options o, Target t, StrVec args, char **error)
{
    (void)o;
    (void)t;
    *error = NULL;
    return args;
}
StrVec msvc_link_arguments(Options o, Target t, StrVec args, char **error)
{
    (void)o;
    (void)t;
    *error = NULL;
    return args;
}
bool resource_vfs_eligible(Options o, Toolchain tc, bool dependencies)
{
    (void)o;
    (void)tc;
    (void)dependencies;
    return false;
}
char *resource_vfs_overlay(Toolchain tc, char **error)
{
    (void)tc;
    *error = "unexpected VFS call";
    return "";
}
char *resource_vfs_root(void)
{
    return "/cross_cc/lib";
}
char *resource_vfs_path(const char *path, const char *lib, bool *mapped)
{
    (void)lib;
    if (mapped)
        *mapped = false;
    return (char *)path;
}
char *resource_vfs_prefix_map(const char *arg, const char *lib)
{
    (void)arg;
    (void)lib;
    return "";
}
char *builtins_driver_path(const char *path)
{
    return (char *)path;
}
bool bsd_pic_enabled(Options o, Target t)
{
    (void)t;
    return o.pic_mode >= 0;
}
char *output_root_name(const char *first, const char *fallback)
{
    (void)fallback;
    char *base = path_base(first), *dot = strrchr(base, '.');
    if (dot)
        *dot = 0;
    return base;
}
bool tsan_enabled(Options o)
{
    (void)o;
    return false;
}
bool compiler_sanitizer_consumed(const char *arg)
{
    (void)arg;
    return false;
}
bool compiler_sanitizer_enabled(Options o, StrVec names)
{
    (void)o;
    (void)names;
    return false;
}
Ubsan_Mode compiler_ubsan_mode(Options o)
{
    (void)o;
    return UBSAN_OFF;
}
bool ubsan_full_enabled(Options o)
{
    (void)o;
    return false;
}
bool ubsan_full_supported(Target t)
{
    (void)t;
    return true;
}

#define EQ(a, b)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        const char *eq_a = (a), *eq_b = (b);                                                       \
        if (!streq(eq_a, eq_b))                                                                    \
        {                                                                                          \
            fprintf(stderr, "%s:%d: expected [", __FILE__, __LINE__);                              \
            for (const char *c = eq_b; *c; c++)                                                    \
                fprintf(stderr, "\\x%02x", (unsigned char)*c);                                     \
            fprintf(stderr, "] actual [");                                                         \
            for (const char *c = eq_a; *c; c++)                                                    \
                fprintf(stderr, "\\x%02x", (unsigned char)*c);                                     \
            fprintf(stderr, "]\n");                                                                \
        }                                                                                          \
        CHECK(streq(eq_a, eq_b));                                                                  \
    } while (0)
static Options parse(StrVec args)
{
    char *error = NULL;
    Options o = parse_arguments_with_env(args, false, "", &error);
    if (error)
        fprintf(stderr, "parse error: %s\n", error);
    CHECK(!error);
    return o;
}
static Target target(const char *name)
{
    char *error = NULL;
    Target t = resolve_target(name, &error);
    if (error)
        fprintf(stderr, "target error: %s\n", error);
    CHECK(!error);
    return t;
}
static bool has(StrVec v, const char *s)
{
    for (size_t i = 0; i < v.len; i++)
        if (streq(v.data[i], s))
            return true;
    return false;
}
static ptrdiff_t index_of(StrVec v, const char *s)
{
    for (size_t i = 0; i < v.len; i++)
        if (streq(v.data[i], s))
            return (ptrdiff_t)i;
    return -1;
}
static void parse_tests(void)
{
    Options o = parse(SV("--cross-cc-msvc=system", "--cross-cc-accept-msvc-license", "--libc",
                         "sdk.txt", "-c", "hello.c"));
    CHECK(o.msvc_system && o.msvc_accept_license);
    EQ(o.msvc_libc_file, "sdk.txt");
    CHECK(o.forwarded.len == 2);
    o = parse(SV("--cross-cc-msvc", "system", "--cross-cc-msvc=managed", "--libc=sdk.txt", "-c",
                 "hello.c"));
    CHECK(!o.msvc_system && !o.msvc_accept_license);
    EQ(o.msvc_libc_file, "sdk.txt");
    char *error = NULL;
    parse_arguments(SV("--cross-cc-msvc=latest"), false, &error);
    CHECK(error);
    parse_arguments(SV("--cross-cc-msvc"), false, &error);
    CHECK(error);
    o = parse(SV("-target", "aarch64-linux-gnu.2.31", "-mcpu=baseline", "-c", "a.c", "-ofoo.o",
                 "-x", "c", "-I", "inc", "--zig-lib-dir=/z"));
    EQ(o.target_name, "aarch64-linux-gnu.2.31");
    EQ(o.cpu, "baseline");
    EQ(o.lib_dir, "/z");
    EQ(o.output, "foo.o");
    CHECK(o.compile_only && o.has_input);
    CHECK(o.forwarded.len == 7);
    o = parse_arguments_with_env(SV("-x", "c", "-m32"), false, "", &error);
    CHECK(!o.has_input);
    if (streq(host_arch(), "x86_64") || streq(host_arch(), "x86"))
    {
        CHECK(!error);
        Target t = target(o.target_name), native = target("");
        EQ(t.arch, "x86");
        EQ(t.os, native.os);
        EQ(t.abi, native.abi);
    }
    else if (streq(host_arch(), "aarch64") || streq(host_arch(), "arm"))
    {
        CHECK(!error);
        Target t = target(o.target_name);
        EQ(t.arch, "arm");
    }
    else
        CHECK(error);
    StrVec bad[] = {SV("-target"), SV("--target=", "-c"), SV("-o"),     SV("-x"), SV("-mcpu"),
                    SV("-I"),      SV("--sysroot="),      SV("--libc=")};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++)
    {
        parse_arguments(bad[i], false, &error);
        CHECK(error);
    }
    o = parse_arguments_with_env(SV("-c", "a.c"), false, "system", &error);
    CHECK(!error && o.msvc_system);
    o = parse_arguments_with_env(SV("--cross-cc-msvc=managed", "-c", "a.c"), false, "system",
                                 &error);
    CHECK(!error && !o.msvc_system);
    o = parse_arguments_with_env(SV("--cross-cc-msvc", "system", "-c", "a.c"), false, "managed",
                                 &error);
    CHECK(!error && o.msvc_system);
    parse_arguments_with_env(SV("-c", "a.c"), false, "bogus", &error);
    CHECK(error);
    o = parse_arguments_with_env(SV("--cross-cc-msvc=managed", "-c", "a.c"), false, "bogus",
                                 &error);
    CHECK(!error && !o.msvc_system);
    o = parse(SV("-nostdlib++", "-lc++", "-c", "a.cpp"));
    CHECK(o.no_stdlibxx && o.explicit_libcxx && o.no_stdincxx);
    CHECK(!has(o.forwarded, "-lc++"));
    o = parse(SV("-target", "x86_64-windows-gnu", "-lSTDC++", "-c", "a.cpp"));
    CHECK(o.explicit_libcxx);
    o = parse(SV("-target", "x86_64-linux-gnu", "-lSTDC++", "-c", "a.cpp"));
    CHECK(!o.explicit_libcxx);
    o = parse(SV("-x", "c-header", "x.h"));
    CHECK(o.header_emit && !needs_link(o));
    o = parse(SV("-x", "c", "x.c"));
    CHECK(has(o.forwarded, "none"));
    o = parse(SV("-static", "-dynamic", "x.c"));
    CHECK(o.dynamic_link && !o.static_link);
    o = parse(SV("-dynamic", "-static", "x.c"));
    CHECK(o.static_link && !o.dynamic_link);
    o = parse(SV("-c", "-s", "-g", "x.c"));
    CHECK(!o.strip && !has(o.forwarded, "-s"));
    o = parse(SV("-c", "-g", "-s", "x.c"));
    CHECK(o.strip && !has(o.forwarded, "-s"));
    o = parse(SV("-c", "-shared", "-pie", "-L", "lib", "-lc", "-Wl,-z,now", "x.c"));
    CHECK(!has(o.forwarded, "-shared") && !has(o.forwarded, "-pie") && !has(o.forwarded, "-lc") &&
          !has(o.forwarded, "-Wl,-z,now"));
    o = parse(SV("-c", "-Wl,--unknown", "x.c"));
    CHECK(has(o.forwarded, "-Wl,--unknown"));
    o = parse(SV("-c", "--", "-O2", "-lfoo"));
    CHECK(o.has_input && has(o.forwarded, "-lfoo"));
}
static void response_tests(void)
{
    char *error = NULL, *tmp = env("TMPDIR");
    if (!*tmp)
        tmp = ".";
    char *dir = make_temp_dir(tmp, "cross-args-", &error);
    CHECK(!error);
    const char *a = "-DX='a b' \"c d\\\"e\" -v f\\ g";
    CHECK(write_file(path_join(dir, "a.rsp"), a, strlen(a)));
    char *nested = str_fmt("-DX @%s/b.rsp -DY", dir), *loop = str_fmt("@%s/loop.rsp", dir);
    CHECK(write_file(path_join(dir, "nested.rsp"), nested, strlen(nested)));
    CHECK(write_file(path_join(dir, "b.rsp"), "-v", 2));
    CHECK(write_file(path_join(dir, "loop.rsp"), loop, strlen(loop)));
    Options o = parse(SV(str_fmt("@%s/a.rsp", dir)));
    EQ(o.forwarded.data[0], "-DX=a b");
    EQ(o.forwarded.data[1], "c d\"e");
    CHECK(o.forwarded.len == 5);
    EQ(o.forwarded.data[3], "f\\");
    EQ(o.forwarded.data[4], "g");
    CHECK(o.verbose);
    parse_arguments(SV(str_fmt("@%s/nested.rsp", dir)), true, &error);
    CHECK(contains(error, "NestedResponseFile"));
    parse_arguments(SV(str_fmt("@%s/loop.rsp", dir)), true, &error);
    CHECK(error);
    StrVec words = {0};
    tokenize_response("# comment\n'' \"\" a#b 'c d' a\\\"b z\\", &words);
    CHECK(words.len == 6);
    EQ(words.data[0], "");
    EQ(words.data[1], "");
    EQ(words.data[2], "a#b");
    EQ(words.data[3], "c d");
    EQ(words.data[5], "z\\");
    CHECK(remove_tree(dir));
}
static void target_tests(void)
{
    StrVec good = SV("x86_64-linux-gnu.2.28", "aarch64-linux-musl", "arm-linux-gnueabihf",
                     "riscv64-linux-gnu", "x86_64-windows-gnu", "aarch64-windows-msvc",
                     "aarch64-macos", "x86_64-freebsd", "wasm32-wasi", "wasm32-freestanding",
                     "sparc-linux-gnu", "x86_64-linux-msvc");
    for (size_t i = 0; i < good.len; i++)
    {
        Target t = target(good.data[i]);
        CHECK(*t.clang);
    }
    Target t = target("x86_64-linux-gnu.2.28");
    EQ(t.glibc_version, "2.28");
    EQ(t.clang, "x86_64-unknown-linux-gnu");
    char *error = NULL;
    resolve_target("bogus-linux-gnu", &error);
    CHECK(contains(error, "unknown architecture"));
    resolve_target("x86_64-bogus-gnu", &error);
    CHECK(contains(error, "unknown operating system"));
    resolve_target("x86_64-linux-bogus", &error);
    CHECK(error);
    CHECK(target("").native);
    StrVec bad = SV("x86_64", "x86_64-linux-gnu-extra", "x86_64-linux-gnu.2",
                    "x86_64-linux-gnu.2.3.4.5", "x86_64-linux-musl.1.2", "aarch64-freestanding.1.0",
                    "aarch64-macos.11", "x86_64-windows.bad", "x86_64-linux.5...6");
    for (size_t i = 0; i < bad.len; i++)
    {
        resolve_target(bad.data[i], &error);
        CHECK(error);
    }
    EQ(target("aarch64-macos.11.0...15.0").clang, "aarch64-apple-macos11.0");
    EQ(target("x86_64-freebsd").clang, "x86_64-unknown-freebsd14.0.0");
    EQ(target("wasm32-wasi").clang, "wasm32-unknown-wasip1");
    EQ(target("wasm32-wasi.0.2.0").clang, "wasm32-unknown-wasip2");
    EQ(target("aarch64-linux-android.24").clang, "aarch64-unknown-linux-android24");
    EQ(target("aarch64-linux-android").glibc_version, "");
    StrVec flags = cpu_flags(target("wasm32-wasi"), "baseline", (StrVec){0}, &error);
    CHECK(!error);
    EQ(flags.data[0], "-mcpu=lime1");
    flags = cpu_flags(target("wasm32-wasi"), "generic", (StrVec){0}, &error);
    CHECK(!error);
    EQ(flags.data[0], "-mcpu=generic");
    flags = cpu_flags(t, "skylake+avx2-sse4_1", (StrVec){0}, &error);
    CHECK(!error && flags.len == 3);
    EQ(flags.data[2], "-mno-sse4.1");
    flags = cpu_flags(target("aarch64-linux-musl"), "baseline", (StrVec){0}, &error);
    CHECK(!error && has(flags, "-fmv"));
    flags = cpu_flags(target("mips-linux-musleabi"), "baseline", SV("-mhard-float"), &error);
    CHECK(!error);
    EQ(flags.data[0], "-msoft-float");
    EQ(flags.data[flags.len - 1], "-mhard-float");
    cpu_flags(t, "baseline+", (StrVec){0}, &error);
    CHECK(contains(error, "empty CPU feature"));
    cpu_flags(t, "baseline+BAD", (StrVec){0}, &error);
    CHECK(contains(error, "invalid CPU feature"));
    cpu_flags(target("wasm32-wasi"), "unknown", (StrVec){0}, &error);
    CHECK(contains(error, "unknown CPU"));
    cpu_flags(target("wasm32-wasi"), "baseline+unknown", (StrVec){0}, &error);
    CHECK(contains(error, "unknown CPU feature"));
}
static void directory_tests(void)
{
    char *error = NULL, *tmp = env("TMPDIR");
    if (!*tmp)
        tmp = ".";
    char *root = make_temp_dir(tmp, "cross-spelling-", &error);
    CHECK(!error);
    StrVec parts = SV("cross_cc", "cache", "pinned-0.17.1", "lib");
    char *canonical =
        absolute_directory_path(path_join(root, "cross_cc", "cache", "pinned-0.17.1", "lib"));
    for (size_t trial = 0; trial < 5000; trial++)
    {
        char *s = str_copy(root);
        for (size_t i = 0; i < parts.len; i++)
        {
            unsigned repeats = 1 + (unsigned)rand() % 3;
            while (repeats--)
                s = str_fmt("%s/", s);
            unsigned mode = (unsigned)rand() % 4;
            if (mode == 0)
                s = str_fmt("%s./", s);
            else if (mode == 1)
                s = str_fmt("%sdetour/../", s);
            s = str_fmt("%s%s", s, parts.data[i]);
        }
        unsigned repeats = (unsigned)rand() % 3;
        while (repeats--)
            s = str_fmt("%s/", s);
        EQ(absolute_directory_path(s), canonical);
    }
    EQ(absolute_directory_path(canonical), canonical);
    CHECK(remove_tree(root));
}
static void symlink_path_tests(void)
{
#ifndef _WIN32
    char *error = NULL;
    char *tmp = env("TMPDIR");
    if (!*tmp)
        tmp = ".";
    char *fixture = make_temp_dir(tmp, "cross-symlink-", &error);
    CHECK(!error);
    char *root = absolute_path(fixture);
    ensure_dir(path_join(root, "actual", "nested"));
    CHECK(write_file(path_join(root, "actual", "input.c"), "int x;", 6));
    CHECK(symlink("actual/nested", path_join(root, "alias")) == 0);
    CHECK(symlink("actual/input.c", path_join(root, "input-alias.c")) == 0);

    EQ(absolute_path(path_join(root, "alias")), path_join(root, "actual", "nested"));
    EQ(absolute_directory_path(path_join(root, "alias")), path_join(root, "actual", "nested"));
    EQ(absolute_path(path_join(root, "alias", "..")), path_join(root, "actual"));
    EQ(absolute_directory_path(path_join(root, "alias", "..")), path_join(root, "actual"));
    EQ(absolute_path(path_join(root, "input-alias.c")), path_join(root, "actual", "input.c"));

    /* A nonexistent path cannot be resolved physically. Keep the existing
       lexical behavior for outputs and not-yet-created cache directories. */
    char *missing = path_join(root, "alias", "missing", "..", "output.o");
    EQ(absolute_path(missing), missing);
    EQ(absolute_directory_path(missing), path_join(root, "alias", "output.o"));
    CHECK(remove_tree(root));
#endif
}
static StrVec command(StrVec flags, const char *target_name)
{
    Options o = parse(flags);
    Target t = target(target_name);
    Toolchain tc = {.lib_dir = "/real/pinned/lib", .clang = "/c/cross_clang", .lld = "/c/ld.lld"};
    char *error = NULL;
    StrVec cmd = compiler_command(o, t, tc, (Runtime){0}, false, &error);
    CHECK(!error);
    return cmd;
}
static void command_tests(void)
{
    const char *owned = "-ffile-prefix-map=/real/pinned/lib=/cross_cc/lib";
    StrVec cmd = command(SV("-c", "-O2", "-g", "-ffile-prefix-map=/real/pinned/lib=/mine", "x.cpp"),
                         "x86_64-linux-musl");
    CHECK(index_of(cmd, owned) >= 0 &&
          index_of(cmd, "-ffile-prefix-map=/real/pinned/lib=/mine") > index_of(cmd, owned));
    CHECK(!has(command(SV("-c", "x.s"), "x86_64-linux-musl"), owned));
    cmd = command(SV("-E", "x.c"), "x86_64-linux-musl");
    CHECK(has(cmd, owned));
    for (size_t i = 0; i < cmd.len; i++)
        CHECK(!starts_with(cmd.data[i], "-Wa,"));
    cmd = command(SV("-c", "-O1", "x.c"), "x86_64-linux-musl");
    CHECK(has(cmd, "-O2") && has(cmd, "-DNDEBUG") && !has(cmd, "-O1"));
    cmd = command(SV("-c", "-Oz", "x.c"), "x86_64-linux-musl");
    CHECK(has(cmd, "-Os") && !has(cmd, "-gdwarf-4"));
    cmd = command(SV("-c", "-Oz", "-g", "x.c"), "x86_64-linux-musl");
    CHECK(has(cmd, "-gdwarf-4") && !has(cmd, "-g"));
    cmd = command(SV("-c", "-fno-stack-protector", "-fstack-protector-all", "x.c"),
                  "x86_64-linux-musl");
    CHECK(has(cmd, "-fno-stack-protector") && !has(cmd, "-fstack-protector-strong"));
    cmd = command(SV("-c", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "x.c"),
                  "x86_64-linux-musl");
    CHECK(has(cmd, "-fno-unwind-tables") && has(cmd, "-fno-asynchronous-unwind-tables"));
    cmd = command(SV("-c", "-Xclang", "-O1", "-o", "-Os", "x.c"), "x86_64-linux-musl");
    CHECK(has(cmd, "-O0") && has(cmd, "-O1") && has(cmd, "-Os"));
    cmd = command(SV("-S", "-o", "@out.s", "x.c"), "x86_64-linux-musl");
    CHECK(has(cmd, "./@out.s"));
    Options o = parse(SV("-c", "-g0", "x.c"));
    CHECK(!emits_debug_info(o, false) && emits_debug_info(o, true));
    o = parse(SV("-c", "-O2", "x.c"));
    CHECK(optimized_build(o));
    o = parse(SV("-c", "-O2", "-Og", "x.c"));
    CHECK(!optimized_build(o));
    CHECK(compiler_plain_assembly(SV("-c", "x.s")));
    CHECK(!compiler_plain_assembly(SV("-c", "x.S")));
    CHECK(!compiler_plain_assembly(SV("x.s", "y.c")));
}
static void libc_tests(void)
{
    const char *valid = "include_dir=inc\r\nsys_include_dir=sys\ncrt_dir=crt\nmsvc_lib_dir="
                        "\nkernel32_lib_dir=\ncc_dir=cc\ndarwin_sdk_dir=\n";
    char *error = NULL;
    Libc_Config l = libc_config_parse(valid, target("x86_64-linux-gnu"), &error);
    CHECK(!error);
    EQ(l.include_dir, "inc");
    EQ(l.cc_dir, "cc");
    libc_config_parse("include_dir=inc\n", target("x86_64-linux-gnu"), &error);
    CHECK(contains(error, "missing field"));
    libc_config_parse(str_replace(valid, "cc_dir=cc", "gcc_dir=cc"), target("x86_64-linux-gnu"),
                      &error);
    CHECK(!error);
    libc_config_parse(str_replace(valid, "cc_dir=cc", "cc_dir="), target("x86_64-linux-gnu"),
                      &error);
    CHECK(contains(error, "cc_dir may not be empty"));
    libc_config_parse(valid, target("x86_64-windows-msvc"), &error);
    CHECK(contains(error, "msvc_lib_dir"));
    libc_config_parse(valid, target("aarch64-macos"), &error);
    CHECK(contains(error, "darwin_sdk_dir"));
    l = libc_config_parse(str_fmt("%sinclude_dir=last\n", valid), target("x86_64-linux-gnu"),
                          &error);
    CHECK(!error);
    EQ(l.include_dir, "last");
    l = libc_config_parse(str_replace(valid, "include_dir=inc", "include_dir= inc "),
                          target("x86_64-linux-gnu"), &error);
    CHECK(!error);
    EQ(l.include_dir, " inc ");
    StrVec flags = libc_config_include_flags(l, (Toolchain){.lib_dir = "/lib"});
    CHECK(has(flags, "/lib/include") && has(flags, "sys"));
}
static void support_tests(void)
{
    char *error = NULL;
    char *tmp = env("TMPDIR");
    if (!*tmp)
        tmp = ".";
    char *dir = make_temp_dir(tmp, "cross-support-", &error);
    CHECK(!error);
    char *first = path_join(dir, "café-测试.txt");
    char *second = path_join(dir, "moved-测试.txt");
    CHECK(write_file(first, "abc", 3));
    CHECK(file_exists(first) && is_file(first) && !is_dir(first));
    size_t size = 0;
    EQ(read_file(first, &size), "abc");
    CHECK(size == 3);
    CHECK(move_path(first, second));
    CHECK(!file_exists(first) && file_exists(second));
    CHECK(remove_file(second));
    CHECK(remove_tree(dir));
    int code;
    char *out = capture_command(SV(executable_path(), "--capture-child", "0"), &code);
    CHECK(code == 0);
    EQ(out, "stdout");
    out = capture_command(SV(executable_path(), "--capture-child", "7"), &code);
    CHECK(code == 7);
    EQ(out, "stderr\n");
    EQ(path_base(""), ".");
    EQ(path_base("/a/b/"), "b");
    EQ(path_dir("name"), ".");
}
static void embedded_tools_tests(void)
{
#ifndef _WIN32
    /* Every wait is pipe-synchronized; alarm bounds failures without sleeps. */
    alarm(20);
    char *error = NULL, *tmp = env("TMPDIR");
    char *cache = make_temp_dir(*tmp ? tmp : ".", "cross-tools-", &error);
    CHECK(!error);
    Toolchain tc = {.cache_dir = cache};
    tools_interrupt_lock = true;
    char *dest = embedded_tool_dir(tc);
    CHECK(tools_lock_calls == 2 && tools_stage_calls == 1 && tools_publish_calls == 1);
    CHECK(file_exists(path_join(dest, "complete")));
    CHECK(file_exists(path_join(dest, EMBEDDED_DSYMUTIL_NAME)));
    char *lock_path = str_fmt("%s.lock", dest);
    int lock = open(lock_path, O_RDWR | O_CLOEXEC);
    CHECK(lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0);
    struct stat before, after;
    CHECK(fstat(lock, &before) == 0);
    CHECK(remove_tree(dest));

    int ready[2];
    CHECK(pipe(ready) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child)
    {
        alarm(10);
        close(lock); /* Do not retain the parent's shared flock description. */
        close(ready[0]);
        tools_lock_probe = ready[1];
        tools_forbid_mutations = true;
        tools_lock_calls = tools_stage_calls = tools_publish_calls = 0;
        CHECK(streq(embedded_tool_dir(tc), dest));
        CHECK(tools_lock_calls == 1 && !tools_stage_calls && !tools_publish_calls);
        _exit(0);
    }
    close(ready[1]);
    char signal;
    CHECK(read(ready[0], &signal, 1) == 1 && signal == 'L');
    close(ready[0]);
    /* A different destination is not serialized behind this cold publisher. */
    char *other = path_join(cache, "other");
    ensure_dir(other);
    CHECK(file_exists(path_join(embedded_tool_dir((Toolchain){.cache_dir = other}), "complete")));
    /* Model the winning publisher's marker becoming visible before unlock. */
    ensure_dir(dest);
    CHECK(write_file(path_join(dest, "cross_clang"), "winner", 6));
    CHECK(write_file(path_join(dest, "complete"), "test\n", 5));
    unsigned locks = tools_lock_calls;
    tools_forbid_mutations = true;
    EQ(embedded_tool_dir(tc), dest); /* Hot readers must not wait on our lock. */
    CHECK(tools_lock_calls == locks);
    tools_forbid_mutations = false;
    CHECK(close(lock) == 0);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    EQ(read_file(path_join(dest, "cross_clang"), NULL), "winner");
    CHECK(stat(lock_path, &after) == 0 && before.st_dev == after.st_dev && before.st_ino == after.st_ino);

    /* An abruptly killed cold publisher cannot leave a stale held lock. */
    CHECK(remove_tree(dest));
    child = fork();
    CHECK(child >= 0);
    if (!child)
    {
        alarm(10);
        tools_die_in_stage = true;
        embedded_tool_dir(tc);
        _exit(99);
    }
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    lock = open(lock_path, O_RDWR | O_CLOEXEC);
    CHECK(lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0);
    CHECK(close(lock) == 0);
    CHECK(file_exists(path_join(embedded_tool_dir(tc), "complete")));
    CHECK(remove_tree(cache));
    alarm(0);
#endif
}
int main(int argc, char **argv)
{
    if (argc > 2 && streq(argv[1], "--capture-child"))
    {
#ifdef _WIN32
        /* capture_command reads raw bytes; keep CRT text mode from turning "\n" into "\r\n". */
        _setmode(_fileno(stdout), _O_BINARY);
        _setmode(_fileno(stderr), _O_BINARY);
#endif
        fputs(" stdout \n", stdout);
        fputs("stderr\n", stderr);
        return atoi(argv[2]);
    }
    support_tests();
    parse_tests();
    response_tests();
    target_tests();
    directory_tests();
    symlink_path_tests();
    command_tests();
    libc_tests();
    embedded_tools_tests();
    printf("core: %zu assertions passed\n", assertions);
    return 0;
}
