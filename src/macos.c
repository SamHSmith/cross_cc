#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "platforms.h"
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MACOS_STAGE_SEPARATOR "\\"
#else
#include <unistd.h>
#define MACOS_STAGE_SEPARATOR "/"
#endif

char *apple_min_version(const char *os) {
  if (streq(os, "ios"))
    return "18.0.0";
  if (streq(os, "tvos"))
    return "26.0.0";
  if (streq(os, "visionos"))
    return "27.0.0";
  if (streq(os, "watchos"))
    return "11.0.0";
  return "15.0.0";
}
char *apple_llvm_os(const char *os) {
  return streq(os, "visionos") ? "xros" : (char *)nz(os);
}
char *apple_explicit_version(Target t) {
  StrVec parts = str_split(nz(t.clang), "-");
  if (parts.len < 3)
    return "";
  const char *names[] = {t.os, apple_llvm_os(t.os)};
  for (int i = 0; i < 2; i++)
    if (starts_with(parts.data[2], names[i]))
      return str_copy(parts.data[2] + strlen(nz(names[i])));
  return "";
}
char *apple_deployment_version(Target t) {
  char *v = apple_explicit_version(t);
  return streq(v, "") ? apple_min_version(t.os) : v;
}
char *apple_triple(Target t) {
  const char *dash = strchr(nz(t.clang), '-');
  char *arch =
      dash ? str_ncopy(t.clang, (size_t)(dash - t.clang)) : str_copy(t.clang);
  return str_fmt("%s-apple-%s%s%s", arch, apple_llvm_os(t.os),
                 apple_deployment_version(t),
                 streq(t.abi, "simulator") ? "-simulator" : "");
}
Runtime prepare_macos(Options o, Target t, Toolchain tc, char **error) {
  Runtime r = {0};
  *error = NULL;
  if (o.cxx && !o.no_stdinc && !o.no_stdincxx)
    sv_extend(&r.compile_flags, runtime_cxx_flags(t, tc.lib_dir));
  if (!o.no_stdinc && (streq(t.os, "macos") || streq(o.sysroot, "")))
    sv_extend(&r.compile_flags,
              SV("-nostdlibinc", "-isystem", path_join(tc.lib_dir, "include"),
                 "-isystem",
                 path_join(tc.lib_dir, "libc", "include", "any-darwin-any")));
  sv_push(&r.compile_flags, "-mlinker-version=820");
  if (streq(t.os, "macos")) {
    if (streq(apple_explicit_version(t), ""))
      sv_push(&r.compile_flags, "-mmacosx-version-min=15.0.0");
  } else
    sv_push(&r.compile_flags, str_fmt("--target=%s", apple_triple(t)));
  if (!needs_link(o) || o.query || o.dry_run || !o.has_input)
    return r;
  if (o.static_link && !o.no_stdlib) {
    *error = "Darwin does not provide a static system libc";
    return r;
  }
  if (!streq(t.os, "macos")) {
    char *plat = apple_llvm_os(t.os);
    if (streq(t.abi, "simulator"))
      plat = str_fmt("%s-simulator", plat);
    sv_push(&r.link_flags, str_fmt("-Wl,-platform_version,%s,%s,27.0.0", plat,
                                   apple_deployment_version(t)));
  }
  if (o.no_stdlib || o.relocatable ||
      (!streq(t.os, "macos") && !streq(o.sysroot, "")))
    return r;
  sv_push(&r.link_flags, "-nostdlib");
  if (!o.no_defaultlibs) {
    char *dest = runtime_cache("darwin-tapi-v1", o, t, tc);
    if (!file_exists(path_join(dest, "complete"))) {
      char *work = make_temp_dir(tc.cache_dir, "darwin-tapi-", error);
      if (*error)
        return r;
      size_t size = 0;
      char *data = read_file(
          path_join(tc.lib_dir, "libc", "darwin", "libSystem.tbd"), &size);
      if (!data) {
        *error = "cannot read pinned libSystem.tbd";
        remove_tree(work);
        return r;
      }
      char *text = str_replace(data, "arm64e.x1", "arm64");
      free(data);
      if (!streq(t.os, "macos")) {
        char *plat = apple_llvm_os(t.os);
        if (streq(t.abi, "simulator"))
          plat = str_fmt("%s-simulator", plat);
        text = str_replace(text, "-maccatalyst", str_fmt("-%s", plat));
        text = str_replace(text, "-macos", str_fmt("-%s", plat));
      }
      if (!write_file(path_join(work, "libSystem.tbd"), text, strlen(text)))
        *error = "cannot write Darwin stub";
      if (!*error)
        *error = runtime_publish(work, dest);
      remove_tree(work);
      if (*error)
        return r;
    }
    sv_push(&r.link_flags, str_fmt("-Wl,-syslibroot,%s", dest));
    if (o.cxx) {
      Target bt = t;
      if (!streq(t.os, "macos")) {
        bt.os = "macos";
        bt.clang = apple_triple(t);
      }
      char *dir = runtime_build_cxx(o, bt, tc, false, error);
      if (*error)
        return r;
      sv_extend(&r.link_flags,
                SV(path_join(dir, "libc++.a"), path_join(dir, "libc++abi.a"),
                   path_join(dir, "libunwind.a")));
    }
    sv_push(&r.link_flags, path_join(dest, "libSystem.tbd"));
  }
  return r;
}
char *macos_stage_path(const char *path) {
#ifdef _WIN32
  int count = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
  if (!count)
    return str_copy(path);
  wchar_t *wide = xmalloc((size_t)count * sizeof(*wide));
  MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, count);
  HANDLE handle = CreateFileW(
      wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(wide);
  if (handle == INVALID_HANDLE_VALUE)
    return str_copy(path);
  wchar_t buffer[4096];
  DWORD n = GetFinalPathNameByHandleW(handle, buffer, 4096, 0);
  CloseHandle(handle);
  if (!n || n >= 4096)
    return str_copy(path);
  wchar_t *text = buffer;
  if (n >= 8 && !wcsncmp(text, L"\\\\?\\UNC\\", 8)) {
    text += 6;
    *text = L'\\';
  } else if (n >= 4 && !wcsncmp(text, L"\\\\?\\", 4))
    text += 4;
  count = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
  if (!count)
    return str_copy(path);
  char *out = xmalloc((size_t)count);
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out, count, NULL, NULL);
  return out;
#else
  char *resolved = realpath(path, NULL);
  if (resolved)
    return resolved;
  return str_copy(path);
#endif
}
static uint32_t macho_u32le(const unsigned char *b) {
  return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
         (uint32_t)b[3] << 24;
}
static bool macho_read_at(FILE *f, void *buffer, size_t size, uint64_t offset) {
  /* All accepted offsets fit signed 64 bits. Use the native wide seek API. */
#ifdef _WIN32
  if (_fseeki64(f, (__int64)offset, SEEK_SET))
    return false;
#else
  if (fseeko(f, (off_t)offset, SEEK_SET))
    return false;
#endif
  return fread(buffer, 1, size, f) == size;
}
StrVec macho_debug_map(const char *path, bool *ok) {
  StrVec objects = {0};
  *ok = false;
  FILE *f = open_file(path, "rb");
  if (!f)
    return objects;
  unsigned char header[32], *commands = NULL, *symbols = NULL,
                            *strings_data = NULL;
  if (!macho_read_at(f, header, 32, 0))
    goto done;
  uint32_t magic = macho_u32le(header);
  if (magic != 0xfeedface && magic != 0xfeedfacf)
    goto done;
  bool wide = magic == 0xfeedfacf;
  uint32_t filetype = macho_u32le(header + 12), size = macho_u32le(header + 20);
  if (filetype == 1 || size > (64u << 20))
    goto done;
  commands = xmalloc(size ? size : 1);
  if (!macho_read_at(f, commands, size, wide ? 32 : 28))
    goto done;
  for (size_t at = 0; at + 8 <= size;) {
    uint32_t cmd = macho_u32le(commands + at),
             cmdsize = macho_u32le(commands + at + 4);
    if (cmdsize < 8 || cmdsize > size - at)
      goto done;
    if (cmd == 2 && cmdsize >= 24) {
      uint32_t symoff = macho_u32le(commands + at + 8),
               nsyms = macho_u32le(commands + at + 12),
               stroff = macho_u32le(commands + at + 16),
               strsize = macho_u32le(commands + at + 20);
      size_t entry = wide ? 16 : 12;
      if (nsyms > (256u << 20) / entry || strsize > (256u << 20))
        goto done;
      free(symbols);
      free(strings_data);
      symbols = xmalloc(nsyms ? nsyms * entry : 1);
      strings_data = xmalloc(strsize ? strsize : 1);
      if (!macho_read_at(f, symbols, nsyms * entry, symoff) ||
          !macho_read_at(f, strings_data, strsize, stroff))
        goto done;
      for (uint32_t i = 0; i < nsyms; i++) {
        unsigned char *s = symbols + i * entry;
        if (s[4] != 0x66)
          continue;
        uint32_t strx = macho_u32le(s);
        if (strx >= strsize)
          goto done;
        size_t end = strx;
        while (end < strsize && strings_data[end])
          end++;
        sv_push(&objects,
                str_ncopy((const char *)strings_data + strx, end - strx));
      }
    }
    at += cmdsize;
  }
  *ok = true;
done:
  free(commands);
  free(symbols);
  free(strings_data);
  fclose(f);
  return objects;
}
char *macos_output_path(StrVec link) {
  char *output = "a.out";
  for (size_t i = 0; i < link.len; i++) {
    char *a = link.data[i];
    if ((streq(a, "-o") || streq(a, "--output")) && i + 1 < link.len)
      output = link.data[++i];
    else if (starts_with(a, "--output="))
      output = a + 9;
    else if (starts_with(a, "-o") && strlen(a) > 2)
      output = a + 2;
    else if (takes_value(a))
      i++;
  }
  return output;
}
/* dsymutil must resolve relative N_OSO names from the private stage.
   run_command is synchronous, so restoring cwd before returning also covers
   child failures. */
static int macos_run_dsymutil(StrVec args, const char *work, bool verbose) {
  if (streq(work, ""))
    return run_command(args, verbose);
  char *cwd = absolute_path(".");
  if (!change_directory(work))
    return 127;
  int code = run_command(args, verbose);
  if (!change_directory(cwd))
    fatal("cannot restore working directory after dsymutil");
  return code;
}
int run_macos_link(Options o, Target t, Toolchain tc, Runtime rt) {
  tc.lld = path_join(path_dir(tc.clang), "ld64.lld" NATIVE_EXE_SUFFIX);
  char *error = NULL;
  int code = 1;
  if (o.dry_run) {
    StrVec cmd = compiler_command(o, t, tc, rt, false, &error);
    if (error) {
      fprintf(stderr, "%s\n", error);
      return 1;
    }
    return run_command(cmd, o.verbose);
  }
  char *work0 = make_temp_dir(tc.cache_dir, "darwin-objects-", &error);
  if (error) {
    fprintf(stderr, "cannot create Darwin object stage\n");
    return 1;
  }
  char *work = macos_stage_path(work0);
  bool keep = false;
  StrVec compile_args = {0}, link_args = {0}, inputs = {0}, objects = {0},
         languages = {0};
  const char *language = "";
  for (size_t i = 0; i < o.forwarded.len; i++) {
    char *arg = o.forwarded.data[i];
    if (streq(arg, "-o") || streq(arg, "--output")) {
      sv_push(&link_args, arg);
      if (i + 1 < o.forwarded.len)
        sv_push(&link_args, o.forwarded.data[++i]);
      continue;
    }
    if ((starts_with(arg, "-o") && strlen(arg) > 2) ||
        starts_with(arg, "--output=")) {
      sv_push(&link_args, arg);
      continue;
    }
    if (streq(arg, "-x") || streq(arg, "--language")) {
      if (i + 1 < o.forwarded.len)
        language = o.forwarded.data[++i];
      continue;
    }
    if (starts_with(arg, "-x") && strlen(arg) > 2) {
      language = arg + 2;
      continue;
    }
    if (starts_with(arg, "--language=")) {
      language = arg + 11;
      continue;
    }
    if (takes_value(arg)) {
      bool link_only = streq(arg, "-l") || streq(arg, "-L") ||
                       streq(arg, "-Xlinker") || streq(arg, "-framework");
      if (!link_only)
        sv_push(&compile_args, arg);
      sv_push(&link_args, arg);
      if (i + 1 < o.forwarded.len) {
        arg = o.forwarded.data[++i];
        if (!link_only)
          sv_push(&compile_args, arg);
        sv_push(&link_args, arg);
      }
      continue;
    }
    if (!starts_with(arg, "-") || streq(arg, "-")) {
      const char *ext = path_ext(arg);
      bool object_input =
          (streq(language, "") || streq(language, "none")) &&
          (streq(ext, ".o") || streq(ext, ".obj") || streq(ext, ".a") ||
           streq(ext, ".lib") || streq(ext, ".dylib") || streq(ext, ".so") ||
           streq(ext, ".tbd") || streq(ext, ".bc"));
      if (object_input)
        sv_push(&link_args, arg);
      else {
        char *object =
            str_fmt("%s%sinput-%zu.o", work, MACOS_STAGE_SEPARATOR, inputs.len);
        sv_push(&inputs, arg);
        sv_push(&objects, object);
        sv_push(&link_args, object);
        sv_push(&languages, language);
      }
      continue;
    }
    if (starts_with(arg, "-Wl,") || starts_with(arg, "-l") ||
        starts_with(arg, "-L") || streq(arg, "-shared") ||
        streq(arg, "-dynamiclib") || streq(arg, "-static") ||
        streq(arg, "-pie") || streq(arg, "-no-pie") || streq(arg, "-r") ||
        streq(arg, "-s")) {
      sv_push(&link_args, arg);
      continue;
    }
    sv_push(&compile_args, arg);
    sv_push(&link_args, arg);
  }
  for (size_t i = 0; i < inputs.len; i++) {
    Options co = o;
    co.compile_only = true;
    co.forwarded = sv_copy(compile_args);
    if (!streq(languages.data[i], ""))
      sv_extend(&co.forwarded, SV("-x", languages.data[i]));
    sv_extend(&co.forwarded, SV("-c", inputs.data[i], "-o", objects.data[i]));
    StrVec cmd = compiler_command(co, t, tc, rt, false, &error);
    if (error)
      goto fail;
    code = run_command(cmd, o.verbose);
    if (code)
      goto done;
  }
  bool virtual_names = inputs.len > 0;
  for (size_t i = 0; i < o.forwarded.len; i++) {
    char *a = o.forwarded.data[i];
    if (contains(a, "oso_prefix") || starts_with(a, "@") ||
        starts_with(a, "-fuse-ld") || starts_with(a, "--ld-path"))
      virtual_names = false;
  }
  Options lo = o;
  lo.forwarded = (StrVec){0};
  if (virtual_names)
    sv_extend(&lo.forwarded, SV("-Xlinker", "-oso_prefix", "-Xlinker",
                                str_fmt("%s%s", work, MACOS_STAGE_SEPARATOR)));
  sv_extend(&lo.forwarded, link_args);
  StrVec cmd = compiler_command(lo, t, tc, rt, false, &error);
  if (error)
    goto fail;
  code = run_command(cmd, o.verbose);
  if (code || o.relocatable)
    goto done;
  char *output = absolute_path(macos_output_path(link_args));
  bool parsed = false;
  StrVec debug_map = macho_debug_map(output, &parsed);
  if (!parsed) {
    keep = inputs.len > 0;
    goto done;
  }
  if (!debug_map.len)
    goto done;
  char *dsymutil =
      path_join(path_dir(tc.clang), EMBEDDED_DSYMUTIL_NAME NATIVE_EXE_SUFFIX);
  code = macos_run_dsymutil(SV(dsymutil, "--linker=classic", "--num-threads",
                               "1", "--no-object-timestamp", output),
                            virtual_names ? work : "", o.verbose);
  goto done;
fail:
  fprintf(stderr, "%s\n", error);
  code = 1;
done:
  if (!keep)
    remove_tree(work0);
  return code;
}
