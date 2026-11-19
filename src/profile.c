#include "platforms.h"

bool is_profile_arg(const char *a) {
  const char *prefixes[] = {
      "-fprofile-",         "-fno-profile-",      "-fcs-profile-",
      "-fprofile=",         "--coverage",         "-ftest-coverage",
      "-fno-test-coverage", "-fcoverage-mapping", "-fno-coverage-mapping",
      "-fcoverage-",        "-fno-coverage-"};
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(*prefixes); i++)
    if (starts_with(a, prefixes[i]))
      return true;
  return false;
}
bool wants_profile_split(Options o) {
  if (!needs_link(o) || !o.has_input || o.dry_run)
    return false;
  for (size_t i = 0; i < o.forwarded.len; i++) {
    const char *a = o.forwarded.data[i];
    if (streq(a, "--"))
      break;
    if (takes_value(a)) {
      i++;
      continue;
    }
    if (is_profile_arg(a))
      return true;
  }
  return false;
}
bool is_linker_input(const char *a) {
  const char *ext = path_ext(a);
  const char *extensions[] = {".o",  ".obj",   ".bc",  ".a",  ".lib",
                              ".so", ".dylib", ".tbd", ".res"};
  for (size_t i = 0; i < sizeof(extensions) / sizeof(*extensions); i++)
    if (streq(ext, extensions[i]))
      return true;
  return contains(path_base(a), ".so.");
}
bool split_msvc_driver_arg(const char *a) {
  bool known = false;
  compiler_optimization(a, &known);
  if (known)
    return true;
  if (starts_with(a, "-Wp,") || starts_with(a, "-Wa,"))
    return false;
  const char *prefixes[] = {"-g",          "-W",
                            "-fsanitize=", "-fno-sanitize=",
                            "-fsanitize-", "-fno-sanitize-",
                            "-flto",       "-fno-lto",
                            "-fuse-ld=",   "-stdlib=",
                            "-rtlib=",     "--stdlib",
                            "--rtlib",     "--sysroot",
                            "-B"};
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(*prefixes); i++)
    if (starts_with(a, prefixes[i]))
      return true;
  return streq(a, "-w") || streq(a, "-pthread") || streq(a, "-pthreads");
}
Split_Link split_link_arguments(Options o, const char *work, bool msvc) {
  Split_Link s = {0};
  const char *language = "";
  bool operands = false;
  for (size_t i = 0; i < o.forwarded.len; i++) {
    const char *a = o.forwarded.data[i];
    if (!operands) {
      if (streq(a, "--")) {
        operands = true;
        continue;
      }
      if (streq(a, "-x") || streq(a, "--language")) {
        if (i + 1 < o.forwarded.len)
          language = o.forwarded.data[++i];
        continue;
      }
      if (starts_with(a, "-x") && strlen(a) > 2) {
        language = a + 2;
        continue;
      }
      if (starts_with(a, "--language=")) {
        language = a + 11;
        continue;
      }
      if (streq(a, "-o") || streq(a, "--output")) {
        sv_push(&s.link_args, a);
        if (i + 1 < o.forwarded.len)
          sv_push(&s.link_args, o.forwarded.data[++i]);
        continue;
      }
      if ((starts_with(a, "-o") && !starts_with(a, "-objc") &&
           !starts_with(a, "-openmp") && !starts_with(a, "-obj")) ||
          starts_with(a, "--output=")) {
        sv_push(&s.link_args, a);
        continue;
      }
      bool link_value = streq(a, "-L") || streq(a, "-l") ||
                        streq(a, "-Xlinker") || streq(a, "-framework") ||
                        streq(a, "-F") || streq(a, "-u") || streq(a, "-T") ||
                        streq(a, "-z");
      if (msvc)
        link_value = link_value || streq(a, "-e") ||
                     streq(a, "--library-directory") ||
                     streq(a, "--for-linker") || streq(a, "--force-link");
      if (link_value) {
        sv_push(&s.link_args, a);
        if (i + 1 < o.forwarded.len)
          sv_push(&s.link_args, o.forwarded.data[++i]);
        continue;
      }
      if (starts_with(a, "-Wl,") || starts_with(a, "-L") ||
          starts_with(a, "-l") || starts_with(a, "-fuse-ld=") ||
          streq(a, "-static") || streq(a, "-shared") ||
          streq(a, "-static-pie") || streq(a, "-pie") || streq(a, "-no-pie") ||
          streq(a, "-s") || streq(a, "-rdynamic") ||
          starts_with(a, "-nostdlib") || starts_with(a, "-nodefaultlibs") ||
          starts_with(a, "-nostartfiles") ||
          (msvc && (starts_with(a, "--library-directory=") ||
                    starts_with(a, "--for-linker=") ||
                    starts_with(a, "--force-link=")))) {
        sv_push(&s.link_args, a);
        continue;
      }
      if (is_profile_arg(a)) {
        sv_push(&s.compile_args, a);
        continue;
      }
      bool both = !msvc || split_msvc_driver_arg(a);
      if (takes_value(a)) {
        sv_push(&s.compile_args, a);
        if (both)
          sv_push(&s.link_args, a);
        if (i + 1 < o.forwarded.len) {
          a = o.forwarded.data[++i];
          sv_push(&s.compile_args, a);
          if (both)
            sv_push(&s.link_args, a);
        }
        continue;
      }
      if (starts_with(a, "-") && !streq(a, "-")) {
        sv_push(&s.compile_args, a);
        if (both)
          sv_push(&s.link_args, a);
        continue;
      }
    }
    bool linker_input = is_linker_input(a);
    if (msvc) {
      const char *ext = str_lower(path_ext(a));
      if (streq(ext, ".o") || streq(ext, ".obj") || streq(ext, ".bc") ||
          streq(ext, ".a") || streq(ext, ".lib") || streq(ext, ".dll") ||
          streq(ext, ".def") || streq(ext, ".res"))
        linker_input = true;
    }
    if ((streq(language, "") || streq(language, "none")) && linker_input) {
      sv_push(&s.link_args, starts_with(a, "-") ? str_fmt("./%s", a) : a);
      continue;
    }
    char *object = path_join(
        work, str_fmt("input-%zu%s", s.inputs.len, msvc ? ".obj" : ".o"));
    sv_push(&s.inputs, a);
    sv_push(&s.languages, language);
    sv_push(&s.objects, object);
    sv_push(&s.link_args, object);
  }
  return s;
}
int run_profile_link_prepared(Options o, Target t, Toolchain tc, Runtime rt,
                              Link_Prepare transform) {
  bool msvc = streq(t.os, "windows") && streq(t.abi, "msvc");
  char *error = NULL, *work = NULL;
  if (msvc) {
    const char *output = streq(o.output, "") ? "a.exe" : o.output;
    bool owned = true;
    work = deterministic_link_stage(path_dir(output), ".cross-cc-link-", output,
                                    &error);
    if (error) {
      owned = false;
      error = NULL;
      work =
          deterministic_link_stage(tc.cache_dir, "msvc-link-", output, &error);
    }
    if (!error && owned) {
      bool managed = !o.msvc_system && streq(o.msvc_libc_file, "") &&
                     streq(env("ZIG_LIBC"), "") && streq(o.sysroot, "");
      rt.link_flags = msvc_stage_runtime(rt.link_flags, work, managed);
    }
  } else
    work = make_temp_dir(tc.cache_dir, "profile-stage-", &error);
  if (error)
    fatal("cannot create compile/link work directory");
  Split_Link s = split_link_arguments(o, work, msvc);
  int status = 1;
  for (size_t i = 0; i < s.inputs.len; i++) {
    Options co = o;
    co.compile_only = true;
    co.forwarded = sv_copy(s.compile_args);
    if (!streq(s.languages.data[i], ""))
      sv_extend(&co.forwarded, SV("-x", s.languages.data[i]));
    const char *source = s.inputs.data[i];
    if (msvc && !streq(source, "-") && starts_with(source, "-"))
      source = str_fmt("./%s", source);
    sv_extend(&co.forwarded, SV("-c", "-o", s.objects.data[i], "--", source));
    StrVec cmd = compiler_command(co, t, tc, rt, false, &error);
    if (error)
      goto fail;
    status = run_command(cmd, o.verbose);
    if (status)
      goto done;
  }
  Options lo = o;
  lo.forwarded = sv_copy(s.link_args);
  if (msvc && !streq(o.output, "") && !o.relocatable) {
    bool user_implib = false;
    for (size_t i = 0; i < o.forwarded.len; i++) {
      const char *a = o.forwarded.data[i];
      if (streq(a, "--"))
        break;
      if (starts_with(a, "-") && contains(str_lower(a), "implib"))
        user_implib = true;
      if ((streq(a, "-Xlinker") || streq(a, "--for-linker")) &&
          i + 1 < o.forwarded.len &&
          contains(str_lower(o.forwarded.data[i + 1]), "implib"))
        user_implib = true;
      if (takes_value(a))
        i++;
    }
    if (!user_implib && s.inputs.len)
      sv_push(&lo.forwarded,
              str_fmt("-Wl,/implib:%s",
                      path_join(path_dir(o.output),
                                str_fmt("%s.lib", output_root_name(
                                                      s.inputs.data[0], "")))));
  }
  StrVec cmd = compiler_command(lo, t, tc, rt, false, &error);
  if (error)
    goto fail;
  if (transform) {
    cmd = transform(cmd, work, &error);
    if (error)
      goto fail;
  }
  status = run_command(cmd, o.verbose);
  goto done;
fail:
  fprintf(stderr, "cross_cc: error: %s\n", error);
  status = 1;
done:
  remove_tree(work);
  return status;
}
int run_profile_link(Options o, Target t, Toolchain tc, Runtime rt) {
  return run_profile_link_prepared(o, t, tc, rt, NULL);
}
