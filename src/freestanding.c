#include "runtime.h"

/* Bare-metal links never delegate unknown-OS links to a host GCC driver. */
int run_freestanding_link(Options o, Target t, Toolchain tc, Runtime rt) {
  const char *output = *nz(o.output)
                           ? o.output
                           : (starts_with(t.arch, "wasm") ? "a.wasm" : "a.out");
  char *error = NULL, *work = deterministic_link_stage(
                          tc.cache_dir, "freestanding-link-", output, &error);
  if (error) {
    fprintf(stderr, "cannot create freestanding link stage\n");
    return 1;
  }
  bool wasm = starts_with(t.arch, "wasm");
  StrVec link = SV(tc.lld);
  int code = 1;
  if (!o.relocatable) {
    RT_ADD(&link, "--entry", "_start", "--build-id=none");
    if (wasm)
      RT_ADD(&link, "--export-memory", "--stack-first", "-z",
             "stack-size=1048576");
    else {
      RT_ADD(&link, "-static", "-no-pie", "--eh-frame-hdr", "-z", "now", "-z",
             "stack-size=16777216");
      sv_push(&link,
              str_fmt("--image-base=%s", in_target_inventory("arm x86", t.arch)
                                             ? "65536"
                                             : "16777216"));
    }
  }
  RT_ADD(&link, "-o", output);
  Options compile = o;
  compile.compile_only = true;
  compile.forwarded = (StrVec){0};
  StrVec inputs = {0}, languages = {0};
  size_t *positions = xmalloc((o.forwarded.len + 1) * sizeof(size_t));
  const char *language = "";
  bool operands = false;
  for (size_t i = 0; i < o.forwarded.len; i++) {
    char *arg = o.forwarded.data[i];
    if (!operands) {
      if (streq(arg, "--")) {
        operands = true;
        continue;
      }
      if (streq(arg, "-x") || streq(arg, "--language")) {
        if (i + 1 >= o.forwarded.len) {
          error = "missing source language";
          goto done;
        }
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
      if (streq(arg, "-o") || streq(arg, "--output")) {
        i++;
        continue;
      }
      if (starts_with(arg, "--output=") ||
          (starts_with(arg, "-o") && !starts_with(arg, "-objc") &&
           !starts_with(arg, "-openmp") && !starts_with(arg, "-obj") &&
           strlen(arg) > 2))
        continue;
      if (streq(arg, "-Xlinker") || streq(arg, "-Wl")) {
        if (i + 1 >= o.forwarded.len) {
          error = "missing linker argument";
          goto done;
        }
        i++;
        if (streq(arg, "-Wl"))
          sv_extend(&link, str_split(o.forwarded.data[i], ","));
        else
          sv_push(&link, o.forwarded.data[i]);
        continue;
      }
      if (starts_with(arg, "-Wl,")) {
        sv_extend(&link, str_split(arg + 4, ","));
        continue;
      }
      if (in_target_inventory("-L -l -u -e -T -z", arg)) {
        if (i + 1 >= o.forwarded.len) {
          error = "missing linker option value";
          goto done;
        }
        RT_ADD(&link, arg, o.forwarded.data[i + 1]);
        i++;
        continue;
      }
      if (starts_with(arg, "-L") || starts_with(arg, "-l") ||
          starts_with(arg, "-T")) {
        sv_push(&link, arg);
        continue;
      }
      if (in_target_inventory("--no-entry -shared -static -s -r -pie -no-pie",
                              arg)) {
        sv_push(&link, arg);
        continue;
      }
      if (streq(arg, "-rdynamic")) {
        sv_push(&link, wasm ? "--export-dynamic" : "-export-dynamic");
        continue;
      }
      if (in_target_inventory("-nostdlib -nodefaultlibs -nostartfiles", arg))
        continue;
      if (takes_value(arg)) {
        sv_push(&compile.forwarded, arg);
        if (i + 1 >= o.forwarded.len) {
          error = "missing compiler option value";
          goto done;
        }
        sv_push(&compile.forwarded, o.forwarded.data[++i]);
        continue;
      }
    }
    if (operands || !starts_with(arg, "-") || streq(arg, "-")) {
      bool object =
          (!*language || streq(language, "none")) &&
          in_target_inventory(".o .obj .a .lib .so .bc", path_ext(arg));
      if (object)
        sv_push(&link, arg);
      else {
        positions[inputs.len] = link.len;
        sv_push(&inputs, arg);
        sv_push(&languages, language);
        sv_push(&link, "");
      }
    } else
      sv_push(&compile.forwarded, arg);
  }
  for (size_t i = 0; i < inputs.len; i++) {
    Options co = compile;
    co.forwarded = sv_copy(compile.forwarded);
    if (*languages.data[i] && !streq(languages.data[i], "none"))
      RT_ADD(&co.forwarded, "-x", languages.data[i]);
    char *object = path_join(work, str_fmt("input-%zu.o", i));
    RT_ADD(&co.forwarded, "-c", "-o", object, "--", inputs.data[i]);
    StrVec cmd = compiler_command(co, t, tc, rt, false, &error);
    if (error)
      goto done;
    code = run_command(cmd, o.verbose);
    if (code)
      goto done;
    link.data[positions[i]] = object;
  }
  sv_extend(&link, rt.link_flags);
  if (o.dry_run) {
    print_command(link);
    code = 0;
  } else
    code = run_command(link, o.verbose);
done:
  if (error) {
    fprintf(stderr, "%s\n", error);
    code = 1;
  }
  remove_tree(work);
  return code;
}
