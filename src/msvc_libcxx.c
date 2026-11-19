#include "windows_runtime.h"

/* The pinned libc++ VCRuntime branch includes Microsoft's <new.h>, which
   declares set_new_handler but omits the standard get_new_handler declaration.
   Match the official CRT <new> ABI (unversioned std, __cdecl, DLL import when
   _DLL). The implementation remains the real CRT helper, not a local stub.
   A narrowly scoped include_next wrapper leaves both vendors' headers intact. */
const char MSVC_LIBCXX_NEW_HANDLER_HEADER[] =
    "#ifndef CROSS_CC_MSVC_NEW_HANDLER_H\n"
    "#define CROSS_CC_MSVC_NEW_HANDLER_H\n"
    "#include_next <__new/new_handler.h>\n"
    "#if defined(_LIBCPP_ABI_VCRUNTIME)\n"
    "namespace std {\n"
    "_LIBCPP_CRT_FUNC new_handler __cdecl get_new_handler() _NOEXCEPT;\n"
    "}\n"
    "#endif\n"
    "#endif\n";

char *msvc_libcxx_headers(Toolchain tc, char **error) {
    *error = NULL;
    char *dest = path_join(tc.cache_dir, str_fmt("headers-msvc-libcxx-%016llx", (unsigned long long)runtime_hash(MSVC_LIBCXX_NEW_HANDLER_HEADER)));
    if (file_exists(path_join(dest, "complete"))) return dest;
    ensure_dir(tc.cache_dir);
    char *err = NULL;
    char *work = make_temp_dir(tc.cache_dir, "msvc-libcxx-headers-", &err);
    if (err) { *error = str_fmt("cannot create MSVC libc++ header directory: %s", err); return NULL; }
    ensure_dir(path_join(work, "__new"));
    if (!write_file(path_join(work, "__new", "new_handler.h"), MSVC_LIBCXX_NEW_HANDLER_HEADER, strlen(MSVC_LIBCXX_NEW_HANDLER_HEADER))) {
        remove_tree(work);
        *error = str_fmt("cannot write MSVC libc++ header: %s", strerror(errno));
        return NULL;
    }
    err = runtime_publish(work, dest);
    remove_tree(work);
    if (err) { *error = err; return NULL; }
    return dest;
}

StrVec msvc_libcxx_flags(Target t, Toolchain tc, char **error) {
    StrVec flags = {0};
    char *headers = msvc_libcxx_headers(tc, error);
    if (*error) return flags;
    sv_push(&flags, "-isystem");
    sv_push(&flags, headers);
    sv_extend(&flags, runtime_cxx_flags(t, tc.lib_dir));
    return flags;
}

char *msvc_libcxx_cache_kind(Options o, Msvc_Libc l) {
    /* SDK header definitions and CRT imports affect the archive ABI. Include
       the selected installation, wrapper content and static-vs-DLL CRT. */
    char *sdk = str_fmt("%s|%s|%s|%s|%s", nz(l.include_dir), nz(l.sys_include_dir), nz(l.crt_dir), nz(l.msvc_lib_dir), nz(l.kernel32_lib_dir));
    return str_fmt("msvc-libcxx-v2-%s-%016llx-%016llx", o.shared ? "dllcrt" : "staticcrt",
        (unsigned long long)runtime_hash(sdk), (unsigned long long)runtime_hash(MSVC_LIBCXX_NEW_HANDLER_HEADER));
}

/* libc++ supports the Microsoft C++ ABI directly. Its exception/typeinfo/new
   adapters use VCRuntime; libc++abi and libunwind implement the Itanium ABI and
   must not be built or linked for this target. Keep the pinned libc++ sources
   unchanged, including their real exception, RTTI and Win32 thread support. */
char *msvc_build_libcxx(Options o, Target t, Toolchain tc, Msvc_Libc l, char **error) {
    *error = NULL;
    char *dest = runtime_cache(msvc_libcxx_cache_kind(o, l), o, t, tc);
    if (file_exists(path_join(dest, "complete"))) return dest;
    ensure_dir(tc.cache_dir);
    char *err = NULL;
    char *work = make_temp_dir(tc.cache_dir, "msvc-libcxx-", &err);
    if (err) { *error = str_fmt("cannot create MSVC libc++ build directory: %s", err); return NULL; }
    StrVec flags = SV("--no-default-config", str_fmt("--target=%s", nz(t.clang)),
        "-fms-compatibility-version=19.33", "-mno-incremental-linker-compatible",
        "-ffunction-sections", "-fdata-sections", "-fno-stack-protector", "-w",
        "-std=c++23", "-O2", "-g0", "-DNDEBUG", "-fexceptions", "-frtti",
        "-D_LIBCPP_BUILDING_LIBRARY", "-D_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER",
        "-DLIBC_NAMESPACE=__llvm_libc_common_utils", "-faligned-allocation");
    sv_push(&flags, o.shared ? "-fms-runtime-lib=dll" : "-fms-runtime-lib=static");
    StrVec cpu = cpu_flags(t, o.cpu, o.forwarded, &err);
    if (err) { *error = err; goto fail; }
    sv_extend(&flags, cpu);
    StrVec cxx_flags = msvc_libcxx_flags(t, tc, &err);
    if (err) { *error = err; goto fail; }
    sv_extend(&flags, cxx_flags);
    sv_push(&flags, "-nostdlibinc");
    sv_push(&flags, "-isystem");
    sv_push(&flags, path_join(tc.lib_dir, "include"));
    StrVec dirs = msvc_include_dirs(l);
    for (size_t i = 0; i < dirs.len; i++) { sv_push(&flags, "-isystem"); sv_push(&flags, dirs.data[i]); }
    sv_push(&flags, "-I"); sv_push(&flags, path_join(tc.lib_dir, "libcxx", "src"));
    sv_push(&flags, "-I"); sv_push(&flags, path_join(tc.lib_dir, "libcxx", "libc"));
    sv_push(&flags, str_fmt("-ffile-prefix-map=%s=/cross_cc/lib", tc.lib_dir));
    sv_push(&flags, str_fmt("-ffile-prefix-map=%s=/cross_cc/libcxx-build", work));
    StrVec objects = {0};
    StrVec lists[2] = {libcxx_base_files, libcxx_thread_files};
    for (int k = 0; k < 2; k++) {
        for (size_t i = 0; i < lists[k].len; i++) {
            char *object = path_join(work, str_fmt("libcxx-%zu.obj", objects.len));
            err = runtime_compile(path_join(tc.lib_dir, "libcxx", lists[k].data[i]), object, flags, tc, o.verbose);
            if (err) { *error = err; goto fail; }
            sv_push(&objects, object);
        }
    }
    err = runtime_archive(objects, path_join(work, "libc++.lib"), tc, o.verbose);
    if (err) { *error = err; goto fail; }
    err = runtime_publish(work, dest);
    if (err) { *error = err; goto fail; }
    remove_tree(work);
    return dest;
fail:
    remove_tree(work);
    return NULL;
}
