#include "cross.h"
#ifndef _WIN32
#include <dlfcn.h>
#endif
/* Target query grammar and CPU baselines follow the pinned upstream reference. */
#define TARGET_ARCHES                                                                              \
    "aarch64 aarch64_be alpha amdgcn arc arceb arm armeb avr bpfeb bpfel csky ez80 hexagon hppa "  \
    "hppa64 kalimba kvx lanai loongarch32 loongarch64 m68k m88k microblaze microblazeel mips "     \
    "mipsel mips64 mips64el msp430 nvptx nvptx64 or1k powerpc powerpcle powerpc64 powerpc64le "    \
    "propeller riscv32 riscv32be riscv64 riscv64be s390x sh sheb sparc sparc64 spork8 spirv32 "    \
    "spirv64 thumb thumbeb ve wasm32 wasm64 x86_16 x86 x86_64 xcore xtensa xtensaeb"
#define TARGET_OSES                                                                                \
    "freestanding other contiki fuchsia hermit managarm haiku hurd illumos linux plan9 rtems "     \
    "serenity dragonfly freebsd netbsd openbsd driverkit ios maccatalyst macos tvos visionos "     \
    "watchos windows uefi 3ds wiiu switch gba psx ps3 ps4 ps5 psp vita emscripten wasi amdhsa "    \
    "amdpal cuda mesa3d nvcl opencl opengl vulkan tios ashetos"
#define TARGET_ABIS                                                                                \
    "none gnu gnuabin32 gnuabi64 gnueabi gnueabihf gnuf32 gnusf gnux32 eabi eabihf abin32 x32 "    \
    "ilp32 android androideabi musl muslabin32 muslabi64 musleabi musleabihf muslf32 muslsf "      \
    "muslx32 msvc itanium simulator ohos ohoseabi call0"
char *host_arch(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__riscv) && __riscv_xlen == 64
    return "riscv64";
#else
    return "";
#endif
}
char *host_os(void)
{
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#else
    return "";
#endif
}
bool in_target_inventory(const char *inventory, const char *name)
{
    if (!*nz(name) || contains(name, " "))
        return false;
    size_t n = strlen(name);
    for (const char *p = inventory; *p;)
    {
        const char *e = strchr(p, ' ');
        if (!e)
            e = p + strlen(p);
        if ((size_t)(e - p) == n && !memcmp(p, name, n))
            return true;
        p = *e ? e + 1 : e;
    }
    return false;
}
bool arm_arch(const char *arch)
{
    return in_target_inventory("arm armeb thumb thumbeb", arch);
}
bool is_apple_os(const char *os)
{
    return in_target_inventory("driverkit ios maccatalyst macos tvos visionos watchos", os);
}
char *default_abi(const char *arch, const char *os)
{
    if (streq(os, "freestanding") || streq(os, "other"))
    {
        if (arm_arch(arch) ||
            in_target_inventory("csky hppa mips mipsel powerpc powerpcle sh sheb", arch))
            return "eabi";
    }
    else if (streq(os, "linux"))
    {
        if (arm_arch(arch) || in_target_inventory("powerpc powerpcle sh sheb", arch))
            return "musleabihf";
        if (in_target_inventory("mips mipsel", arch))
            return "musleabi";
        if (in_target_inventory("mips64 mips64el", arch))
            return "muslabi64";
        if (in_target_inventory("alpha arc arceb loongarch32 sparc sparc64", arch))
            return "gnu";
        if (streq(arch, "csky"))
            return "gnueabi";
        if (streq(arch, "hppa"))
            return "gnueabihf";
        if (streq(arch, "hppa64"))
            return "eabihf";
        if (in_target_inventory("xtensa xtensaeb", arch))
            return "none";
        return "musl";
    }
    else if (streq(os, "fuchsia"))
    {
        if (streq(arch, "arm") || streq(arch, "thumb"))
            return "eabihf";
    }
    else if (streq(os, "haiku") || streq(os, "freebsd"))
    {
        if (streq(arch, "arm"))
            return "eabihf";
    }
    else if (streq(os, "hurd") || streq(os, "windows"))
        return "gnu";
    else if (streq(os, "rtems"))
    {
        if (arm_arch(arch) || streq(arch, "mips") || streq(arch, "mipsel"))
            return "eabi";
        if (streq(arch, "powerpc"))
            return "eabihf";
    }
    else if (streq(os, "netbsd"))
    {
        if (in_target_inventory("arm armeb powerpc", arch))
            return "eabihf";
        if (in_target_inventory("mips mipsel", arch))
            return "eabi";
    }
    else if (streq(os, "openbsd"))
    {
        if (streq(arch, "arm"))
            return "eabi";
        if (streq(arch, "powerpc"))
            return "eabihf";
    }
    else if (streq(os, "uefi"))
        return "msvc";
    else if (in_target_inventory("3ds wiiu psp vita", os))
        return "eabihf";
    else if (in_target_inventory("gba psx ashetos", os))
        return "eabi";
    else if (streq(os, "ps3"))
        return "ilp32";
    else if (streq(os, "wasi") || streq(os, "emscripten"))
        return "musl";
    return "none";
}
static bool uint_valid(const char *s, int base, uint64_t max)
{
    if (!*s || *s == '-' || *s == '+')
        return false;
    for (const char *p = s; *p; p++)
        if (isspace((unsigned char)*p))
            return false;
    errno = 0;
    char *end;
    unsigned long long n = strtoull(s, &end, base);
    return !errno && !*end && n <= max;
}
bool target_semver_valid(const char *s)
{
    StrVec parts = str_split(s, ".");
    if (parts.len < 2 || parts.len > 3)
        return false;
    for (size_t i = 0; i < parts.len; i++)
        if (!uint_valid(parts.data[i], 10, UINT64_MAX))
            return false;
    return true;
}
static bool target_windows_version_valid(const char *s)
{
    return in_target_inventory(
               "nt4 win2k xp ws2003 vista win7 win8 win8_1 win10 win10_th2 win10_rs1 win10_rs2 "
               "win10_rs3 win10_rs4 win10_rs5 win10_19h1 win10_vb win10_mn win10_fe win10_co "
               "win10_ni win10_cu win11_zn win11_ga win11_ge win11_dt win11_br win11_kr",
               s) ||
           uint_valid(s, 0, UINT32_MAX);
}
static bool target_os_version_valid(const char *os, const char *version)
{
    if (in_target_inventory("freestanding other managarm haiku illumos plan9 serenity psx ps3 ps4 "
                            "ps5 gba emscripten mesa3d ashetos",
                            os))
        return false;
    StrVec parts = str_split(version, "...");
    if (parts.len > 2)
        return false;
    for (size_t i = 0; i < parts.len; i++)
        if (streq(os, "windows") ? !target_windows_version_valid(parts.data[i])
                                 : !target_semver_valid(parts.data[i]))
            return false;
    return true;
}
char *dynamic_linker_for(const char *arch, const char *os, const char *abi)
{
    if (streq(os, "freebsd"))
    {
        if (in_target_inventory("arm aarch64 powerpc powerpc64 powerpc64le riscv64 x86 x86_64",
                                arch))
            return "/libexec/ld-elf.so.1";
    }
    else if (streq(os, "dragonfly"))
        return streq(arch, "x86_64") ? "/libexec/ld-elf.so.2" : "";
    else if (streq(os, "managarm"))
    {
        if (in_target_inventory("aarch64 riscv64 x86_64", arch))
            return str_fmt("/lib/%s-managarm-mlibc-ld.so", arch);
    }
    else if (streq(os, "illumos"))
    {
        if (streq(arch, "x86_64"))
            return "/lib/64/ld.so.1";
        if (streq(arch, "x86"))
            return "/lib/ld.so.1";
    }
    else if (streq(os, "hurd"))
    {
        if ((streq(arch, "aarch64") || streq(arch, "aarch64_be")) && streq(abi, "gnu"))
            return str_fmt("/lib/ld-%s.so.1", arch);
        if (streq(arch, "x86") && streq(abi, "gnu"))
            return "/lib/ld.so.1";
        if (streq(arch, "x86_64"))
        {
            if (streq(abi, "gnu"))
                return "/lib/ld-x86-64.so.1";
            if (streq(abi, "gnux32"))
                return "/lib/ld-x32.so.1";
        }
    }
    else if (streq(os, "netbsd"))
    {
        if (in_target_inventory("alpha arm armeb aarch64 aarch64_be hppa m68k mips mipsel mips64 "
                                "mips64el powerpc riscv32 riscv64 sh sheb sparc sparc64 x86 x86_64",
                                arch))
            return "/libexec/ld.elf_so";
    }
    else if (streq(os, "openbsd"))
    {
        if (in_target_inventory("alpha arm aarch64 hppa m88k mips64 mips64el powerpc powerpc64 "
                                "riscv64 sh sparc64 x86 x86_64",
                                arch))
            return "/usr/libexec/ld.so";
    }
    else if (streq(os, "haiku"))
        return "/system/runtime_loader";
    else if (streq(os, "fuchsia"))
        return "ld.so.1";
    else if (streq(os, "linux"))
    {
        if (streq(abi, "android") || streq(abi, "androideabi"))
            return in_target_inventory("aarch64 x86_64 riscv64", arch) ? "/system/bin/linker64"
                                                                       : "/system/bin/linker";
        if (starts_with(abi, "musl"))
        {
            if (streq(arch, "x86_64"))
                return streq(abi, "muslx32") ? "/lib/ld-musl-x32.so.1" : "/lib/ld-musl-x86_64.so.1";
            if (streq(arch, "x86"))
                return "/lib/ld-musl-i386.so.1";
            if (in_target_inventory("aarch64 aarch64_be s390x loongarch64 hexagon riscv32 riscv64 "
                                    "powerpc64 powerpc64le",
                                    arch))
                return str_fmt("/lib/ld-musl-%s.so.1", arch);
            if (arm_arch(arch))
                return str_fmt("/lib/ld-musl-arm%s%s.so.1",
                               streq(arch, "armeb") || streq(arch, "thumbeb") ? "eb" : "",
                               streq(abi, "musleabihf") ? "hf" : "");
            if (streq(arch, "mips") || streq(arch, "mipsel"))
                return str_fmt("/lib/ld-musl-mips%s%s.so.1", streq(arch, "mipsel") ? "el" : "",
                               streq(abi, "musleabi") ? "-sf" : "");
            if (streq(arch, "mips64") || streq(arch, "mips64el"))
                return str_fmt("/lib/ld-musl-mips%s%s.so.1",
                               streq(abi, "muslabin32") ? "n32" : "64",
                               streq(arch, "mips64el") ? "el" : "");
            if (streq(arch, "powerpc"))
                return "/lib/ld-musl-powerpc.so.1";
        }
        else if (starts_with(abi, "gnu"))
        {
            if (streq(arch, "x86_64"))
                return streq(abi, "gnux32") ? "/libx32/ld-linux-x32.so.2"
                                            : "/lib64/ld-linux-x86-64.so.2";
            if (in_target_inventory("x86 sparc alpha", arch))
                return "/lib/ld-linux.so.2";
            if (streq(arch, "aarch64") || streq(arch, "aarch64_be"))
                return str_fmt("/lib/ld-linux-%s.so.1", arch);
            if (streq(arch, "riscv32"))
                return "/lib/ld-linux-riscv32-ilp32d.so.1";
            if (streq(arch, "riscv64"))
                return "/lib/ld-linux-riscv64-lp64d.so.1";
            if (arm_arch(arch))
                return streq(abi, "gnueabihf") ? "/lib/ld-linux-armhf.so.3" : "/lib/ld-linux.so.3";
            if (in_target_inventory("mips mipsel powerpc powerpcle", arch))
                return "/lib/ld.so.1";
            if (streq(arch, "mips64") || streq(arch, "mips64el"))
                return streq(abi, "gnuabin32") ? "/lib32/ld.so.1" : "/lib64/ld.so.1";
            if (streq(arch, "powerpc64"))
                return "/lib64/ld64.so.1";
            if (streq(arch, "powerpc64le"))
                return "/lib64/ld64.so.2";
            if (streq(arch, "s390x"))
                return "/lib/ld64.so.1";
            if (streq(arch, "sparc64"))
                return "/lib64/ld-linux.so.2";
            if (streq(arch, "loongarch64"))
                return streq(abi, "gnusf")    ? "/lib64/ld-linux-loongarch-lp64s.so.1"
                       : streq(abi, "gnuf32") ? "/lib64/ld-linux-loongarch-lp64f.so.1"
                                              : "/lib64/ld-linux-loongarch-lp64d.so.1";
        }
    }
    return "";
}
char *clang_triple(const char *arch, const char *os, const char *abi, const char *os_version)
{
    const char *a = arch, *vendor = "unknown", *llvm_os = os, *llvm_abi = abi;
    if (streq(a, "x86"))
        a = "i386";
    if (streq(a, "aarch64") && streq(abi, "ilp32"))
        a = "aarch64_32";
    if (streq(a, "arm"))
        a = "armv7";
    else if (streq(a, "armeb"))
        a = "armebv7";
    else if (streq(a, "thumb"))
        a = "thumbv7";
    else if (streq(a, "thumbeb"))
        a = "thumbebv7";
    if (streq(a, "spirv32") && (streq(os, "vulkan") || streq(os, "opengl")))
        a = "spirv";
    if (is_apple_os(os))
        vendor = "apple";
    else if (streq(os, "ps4") || streq(os, "ps5"))
        vendor = "scei";
    else if (streq(os, "amdhsa") || streq(os, "amdpal"))
        vendor = "amd";
    else if (streq(os, "cuda") || streq(os, "nvcl"))
        vendor = "nvidia";
    else if (streq(os, "mesa3d"))
        vendor = "mesa";
    if (streq(os, "illumos"))
        llvm_os = "solaris";
    else if (streq(os, "uefi"))
        llvm_os = "windows";
    else if (streq(os, "ps3"))
        llvm_os = "lv2";
    else if (streq(os, "maccatalyst"))
        llvm_os = "ios";
    else if (streq(os, "visionos"))
        llvm_os = "xros";
    else if (in_target_inventory("contiki freestanding opencl opengl other plan9 psx psp vita tios "
                                 "3ds wiiu switch gba ashetos",
                                 os))
        llvm_os = "unknown";
    char *version = str_copy(os_version);
    char *range = strstr(version, "...");
    if (range)
        *range = 0;
    if (streq(os, "windows"))
        version = "";
    if (!*version)
    {
        if (streq(os, "freebsd"))
            version = "14.0.0";
        if (streq(os, "netbsd"))
            version = "10.1.0";
        if (streq(os, "openbsd"))
            version = "7.8.0";
    }
    if (streq(os, "wasi") && starts_with(version, "0."))
    {
        char *minor = str_copy(version + 2), *dot = strchr(minor, '.');
        if (dot)
            *dot = 0;
        version = str_fmt("p%s", minor);
    }
    if (streq(os, "wasi") && !*version)
        version = "p1";
    if (streq(os, "macos") && streq(abi, "none"))
        return str_fmt("%s-apple-macos%s", a, version);
    if (in_target_inventory("none ilp32 abin32 call0", abi))
        llvm_abi = "unknown";
    else if (streq(abi, "x32"))
        llvm_abi = "muslx32";
    else if (streq(abi, "ohoseabi"))
        llvm_abi = "ohos";
    else if (streq(abi, "musl") && (streq(os, "wasi") || streq(os, "emscripten")))
        llvm_abi = "unknown";
    if (streq(os, "maccatalyst") && streq(abi, "none"))
        llvm_abi = "macabi";
    if (streq(os, "windows"))
        vendor = streq(abi, "gnu") ? "w64" : "pc";
    if (streq(llvm_abi, "unknown"))
        return str_fmt("%s-%s-%s%s", a, vendor, llvm_os, version);
    return str_fmt("%s-%s-%s%s-%s", a, vendor, llvm_os, version, llvm_abi);
}
char *host_glibc_version(void)
{
#ifdef __linux__
    void *lib = dlopen("libc.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!lib)
        return "";
    char *(*version)(void) = NULL;
    void *sym = dlsym(lib, "gnu_get_libc_version");
    memcpy(&version, &sym, sizeof(version));
    char *result = version ? str_copy(version()) : str_copy("");
    dlclose(lib);
    return target_semver_valid(result) ? result : "";
#else
    return "";
#endif
}
Target build_target(const char *arch, const char *os, const char *os_version, const char *abi,
                    const char *glibc)
{
    Target t = {0};
    t.arch = str_copy(arch);
    t.os = str_copy(os);
    t.abi = str_copy(abi);
    bool android = streq(abi, "android") || streq(abi, "androideabi");
    t.glibc_version = android ? "" : str_copy(glibc);
    t.name = str_fmt("%s-%s%s%s-%s", arch, os, *nz(os_version) ? "." : "", nz(os_version), abi);
    if (*nz(glibc))
        t.name = str_fmt("%s.%s", t.name, glibc);
    t.clang = clang_triple(arch, os, abi, os_version);
    if (android && streq(os, "linux"))
        t.clang = str_fmt("%s%s", t.clang, *nz(glibc) ? glibc : "21");
    t.dynamic_linker = dynamic_linker_for(arch, os, abi);
    return t;
}
Target resolve_target(const char *name, char **error)
{
    Target t = {0};
    *error = NULL;
    if (!*nz(name) || streq(name, "native"))
    {
        char *arch = host_arch(), *os = host_os();
        if (!*arch || !*os)
        {
            *error = "unsupported host platform for native target";
            return t;
        }
        char *abi = default_abi(arch, os), *glibc = host_glibc_version();
        if (streq(os, "linux") && *glibc)
            abi = arm_arch(arch) ? "gnueabihf" : "gnu";
        t = build_target(arch, os, "", abi, glibc);
        t.native = true;
        return t;
    }
    StrVec parts = str_split(name, "-");
    if (parts.len < 2 || parts.len > 3)
    {
        *error = str_fmt("invalid target '%s': expected arch-os[-abi]", name);
        return t;
    }
    char *arch = parts.data[0], *os = str_copy(parts.data[1]), *os_ver = "", *dot = strchr(os, '.');
    if (dot)
    {
        *dot = 0;
        os_ver = dot + 1;
    }
    if (streq(arch, "native"))
        arch = host_arch();
    if (streq(os, "native"))
        os = host_os();
    if (!in_target_inventory(TARGET_OSES, os))
    {
        *error = str_fmt("unknown operating system '%s' in target '%s'", os, name);
        return t;
    }
    if (!in_target_inventory(TARGET_ARCHES, arch))
    {
        *error = str_fmt("unknown architecture '%s' in target '%s'", arch, name);
        return t;
    }
    if (*os_ver && !target_os_version_valid(os, os_ver))
    {
        *error = str_fmt("invalid OS version '%s' in target '%s'", os_ver, name);
        return t;
    }
    char *abi = default_abi(arch, os), *glibc = "";
    if (parts.len == 3)
    {
        abi = str_copy(parts.data[2]);
        dot = strchr(abi, '.');
        if (dot)
        {
            *dot = 0;
            glibc = dot + 1;
            bool valid = false;
            if (starts_with(abi, "gnu"))
                valid = target_semver_valid(glibc);
            if (streq(abi, "android") || streq(abi, "androideabi"))
                valid = uint_valid(glibc, 10, UINT32_MAX);
            if (!valid)
            {
                *error = str_fmt("invalid ABI version suffix in target '%s'", name);
                return t;
            }
        }
        if (!in_target_inventory(TARGET_ABIS, abi))
        {
            *error = str_fmt("unknown ABI '%s' for target '%s'", abi, name);
            return t;
        }
    }
    return build_target(arch, os, os_ver, abi, glibc);
}
char *clang_cpu_name(const char *s)
{
    return str_replace(s, "_", "-");
}
static char *x86_feature_name(const char *s)
{
    if (streq(s, "sse4_1"))
        return "sse4.1";
    if (streq(s, "sse4_2"))
        return "sse4.2";
    return clang_cpu_name(s);
}
static const char *const wasm_features[] = {"atomics",
                                            "bulk_memory",
                                            "bulk_memory_opt",
                                            "call_indirect_overlong",
                                            "exception_handling",
                                            "extended_const",
                                            "fp16",
                                            "gc",
                                            "multimemory",
                                            "multivalue",
                                            "mutable_globals",
                                            "nontrapping_bulk_memory_len0",
                                            "nontrapping_fptoint",
                                            "reference_types",
                                            "relaxed_simd",
                                            "sign_ext",
                                            "simd128",
                                            "tail_call",
                                            "wide_arithmetic"};
static uint32_t wasm_bit(const char *s)
{
    for (size_t i = 0; i < 19; i++)
        if (streq(s, wasm_features[i]))
            return UINT32_C(1) << i;
    return 0;
}
static uint32_t wasm_deps(uint32_t set)
{
    for (;;)
    {
        uint32_t old = set;
        if (set & (wasm_bit("bulk_memory") | wasm_bit("nontrapping_bulk_memory_len0")))
            set |= wasm_bit("bulk_memory_opt");
        if (set & wasm_bit("reference_types"))
            set |= wasm_bit("call_indirect_overlong");
        if (old == set)
            return set;
    }
}
static char *wasm_cpu_flags(StrVec *out, const char *name_in, const char *rest, const char *cpu)
{
    const char *name = streq(name_in, "baseline") ? "lime1" : name_in, *model_features = "",
               *llvm = name;
    if (streq(name, "mvp"))
    {
    }
    else if (streq(name, "generic"))
        model_features =
            "bulk_memory multivalue mutable_globals nontrapping_fptoint reference_types sign_ext";
    else if (streq(name, "lime1"))
        model_features = "bulk_memory_opt call_indirect_overlong extended_const multivalue "
                         "mutable_globals nontrapping_fptoint sign_ext";
    else if (streq(name, "bleeding_edge"))
    {
        llvm = "bleeding-edge";
        model_features = "atomics bulk_memory exception_handling extended_const fp16 gc "
                         "multimemory multivalue mutable_globals nontrapping_fptoint "
                         "reference_types relaxed_simd sign_ext simd128 tail_call";
    }
    else
        return str_fmt("unknown CPU '%s' for wasm", name_in);
    uint32_t model = 0, add = 0, sub = 0;
    for (size_t i = 0; i < 19; i++)
        if (in_target_inventory(model_features, wasm_features[i]))
            model |= UINT32_C(1) << i;
    model = wasm_deps(model);
    while (*rest)
    {
        char sign = *rest++;
        size_t n = strcspn(rest, "+-");
        char *feat = str_ncopy(rest, n);
        rest += n;
        if (!n)
            return str_fmt("empty CPU feature in '%s'", cpu);
        uint32_t bit = wasm_bit(feat);
        if (!bit)
            return str_fmt("unknown CPU feature '%s' in '%s'", feat, cpu);
        if (sign == '+')
            add |= bit;
        else
            sub |= bit;
    }
    uint32_t final = wasm_deps((model & ~sub) | add) & ~sub;
    sv_push(out, str_fmt("-mcpu=%s", llvm));
    for (size_t i = 0; i < 19; i++)
        if (!streq(wasm_features[i], "nontrapping_bulk_memory_len0"))
            sv_extend(out, SV("-Xclang", "-target-feature", "-Xclang",
                              str_fmt("%c%s", final & (UINT32_C(1) << i) ? '+' : '-',
                                      clang_cpu_name(wasm_features[i]))));
    return NULL;
}
StrVec cpu_flags(Target t, const char *cpu_in, StrVec forwarded, char **error)
{
    StrVec out = {0};
    *error = NULL;
    if (streq(t.arch, "thumb") || streq(t.arch, "thumbeb"))
        sv_push(&out, "-mthumb");
    bool mips_o32 = (streq(t.arch, "mips") || streq(t.arch, "mipsel")) && streq(t.abi, "musleabi");
    if (mips_o32)
        sv_extend(&out, SV("-msoft-float", "-mfp32"));
    if (streq(t.arch, "hexagon"))
        sv_extend(&out, SV("-Xclang", "-target-feature", "-Xclang", "-small-data"));
    if (streq(t.arch, "powerpc64le") && streq(t.abi, "gnu"))
        sv_extend(&out, SV("-mabi=ieeelongdouble", "-Wno-unsupported-abi"));
    const char *cpu = *nz(cpu_in) ? cpu_in : t.native ? "native" : "baseline";
    size_t end = strcspn(cpu, "+-");
    char *name = str_ncopy(cpu, end);
    const char *rest = cpu + end;
    if (!*name)
        name = "baseline";
    if ((streq(t.arch, "aarch64") || streq(t.arch, "aarch64_be")) && !contains(rest, "+fmv"))
        sv_extend(&out, SV("-Xclang", "-target-feature", "-Xclang", "-fmv"));
    bool x86 = streq(t.arch, "x86") || streq(t.arch, "x86_64");
    if (streq(name, "native"))
    {
        if (!streq(t.arch, host_arch()))
        {
            *error = str_fmt("-mcpu=native requires the host architecture, not %s", t.name);
            return (StrVec){0};
        }
        sv_push(&out, x86 ? "-march=native" : "-mcpu=native");
    }
    else if (streq(t.arch, "wasm32") || streq(t.arch, "wasm64"))
    {
        *error = wasm_cpu_flags(&out, name, rest, cpu);
        if (*error)
            return (StrVec){0};
        rest = "";
    }
    else if (streq(name, "baseline") || (streq(t.arch, "ve") && streq(name, "generic")))
    {
        if (streq(t.arch, "x86_64"))
            sv_push(&out, streq(t.os, "macos") ? "-march=core2" : "-march=x86-64");
        else if (streq(t.arch, "x86"))
            sv_push(&out, "-march=pentium4");
        else if (arm_arch(t.arch))
        {
            sv_extend(&out,
                      SV("-mcpu=generic", "-mfpu=neon",
                         streq(t.os, "windows") || ends_with(t.abi, "hf") ? "-mfloat-abi=hard"
                                                                          : "-mfloat-abi=soft"));
            if (streq(t.os, "linux") && (streq(t.arch, "thumb") || streq(t.arch, "thumbeb")) &&
                !ends_with(t.abi, "hf"))
            {
                StrVec features =
                    SV("+fpregs", "+fpregs64", "+vfp2", "+vfp2sp", "+vfp3", "+vfp3sp");
                for (size_t i = 0; i < features.len; i++)
                    sv_extend(&out, SV("-Xclang", "-target-feature", "-Xclang", features.data[i]));
            }
        }
        else if (streq(t.arch, "riscv32") || streq(t.arch, "riscv32be"))
            sv_extend(&out, SV("-march=rv32imafdc", "-mabi=ilp32d"));
        else if (streq(t.arch, "riscv64") || streq(t.arch, "riscv64be"))
            sv_extend(&out, SV("-march=rv64imafdc", "-mabi=lp64d"));
        else if (streq(t.arch, "mips") || streq(t.arch, "mipsel"))
            sv_push(&out, "-march=mips32r2");
        else if (streq(t.arch, "mips64") || streq(t.arch, "mips64el"))
            sv_push(&out, "-march=mips64r2");
        else if (streq(t.arch, "loongarch32"))
            sv_push(&out, "-march=loongarch32");
        else if (streq(t.arch, "loongarch64"))
            sv_push(&out, "-march=loongarch64");
        else if (streq(t.arch, "s390x"))
            sv_push(&out, "-march=arch11");
        else if (streq(t.arch, "sparc"))
            sv_push(&out, streq(t.os, "linux") ? "-mcpu=v9" : "-mcpu=v8");
        else if (streq(t.arch, "sparc64"))
            sv_push(&out, "-mcpu=ultrasparc");
        else if (streq(t.arch, "powerpc64"))
            sv_push(&out, streq(t.os, "linux") || streq(t.os, "freebsd") ? "-mcpu=pwr8"
                          : streq(t.os, "openbsd")                       ? "-mcpu=pwr9"
                                                                         : "-mcpu=generic");
        else if (streq(t.arch, "powerpc64le"))
            sv_push(&out, "-mcpu=ppc64le");
        else if (streq(t.arch, "aarch64"))
        {
            if (in_target_inventory("macos maccatalyst driverkit", t.os))
                sv_push(&out, "-mcpu=apple-m1");
            else if (streq(t.os, "ios") || streq(t.os, "tvos"))
                sv_push(&out, "-mcpu=apple-a7");
            else if (streq(t.os, "visionos"))
                sv_push(&out, "-mcpu=apple-m2");
            else if (streq(t.os, "watchos"))
                sv_push(&out, "-mcpu=apple-s4");
            else
                sv_push(&out, "-mcpu=generic");
        }
        else if (streq(t.arch, "bpfel") || streq(t.arch, "bpfeb"))
            sv_push(&out, "-mcpu=v3");
        else if (streq(t.arch, "avr"))
            sv_push(&out, "-mmcu=avr2");
        else if (streq(t.arch, "msp430"))
        {
        }
        else if (streq(t.arch, "ve"))
            sv_extend(&out, SV("-Xclang", "-target-feature", "-Xclang", "+vpu"));
        else if (streq(t.arch, "hexagon"))
            sv_push(&out, "-mcpu=hexagonv68");
        else if (streq(t.arch, "lanai"))
            sv_push(&out, "-mcpu=v11");
        else if (streq(t.arch, "nvptx") || streq(t.arch, "nvptx64"))
            sv_push(&out, "-march=sm_52");
        else if (streq(t.arch, "amdgcn"))
            sv_push(&out, "-mcpu=gfx906");
        else if (streq(t.arch, "xcore"))
            sv_push(&out, "-mcpu=xs1b-generic");
        else
            sv_push(&out, "-mcpu=generic");
    }
    else
    {
        const char *flag =
            x86 || in_target_inventory(
                       "mips mipsel mips64 mips64el loongarch32 loongarch64 s390x nvptx nvptx64",
                       t.arch)
                ? "-march"
            : streq(t.arch, "avr") ? "-mmcu"
                                   : "-mcpu";
        sv_push(&out, str_fmt("%s=%s", flag, streq(t.arch, "avr") ? name : clang_cpu_name(name)));
    }
    while (*rest)
    {
        char sign = *rest++;
        size_t n = strcspn(rest, "+-");
        char *feat = str_ncopy(rest, n);
        rest += n;
        if (!n)
        {
            *error = str_fmt("empty CPU feature in '%s'", cpu);
            return (StrVec){0};
        }
        for (size_t i = 0; i < n; i++)
            if (!((feat[i] >= 'a' && feat[i] <= 'z') || (feat[i] >= '0' && feat[i] <= '9') ||
                  feat[i] == '_'))
            {
                *error = str_fmt("invalid CPU feature '%s' in '%s'", feat, cpu);
                return (StrVec){0};
            }
        if (x86)
            sv_push(&out, str_fmt("%s%s", sign == '+' ? "-m" : "-mno-", x86_feature_name(feat)));
        else
            sv_extend(&out, SV("-Xclang", "-target-feature", "-Xclang",
                               str_fmt("%c%s", sign, clang_cpu_name(feat))));
    }
    if (mips_o32)
        for (size_t i = 0; i < forwarded.len; i++)
        {
            char *flag = forwarded.data[i];
            if (in_target_inventory("-msoft-float -mhard-float -mfp32 -mfp64 -mfpxx", flag) ||
                starts_with(flag, "-mfloat-abi="))
                sv_push(&out, flag);
        }
    return out;
}
