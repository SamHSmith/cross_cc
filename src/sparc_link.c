#include "runtime.h"

bool wants_sparc_static_link(Options o, Target t) {
  return streq(t.arch, "sparc64") && streq(t.os, "linux") && o.static_link &&
         !o.shared && !o.relocatable && o.pie <= 0 && !o.dry_run &&
         needs_link(o) && o.has_input;
}
static char *sparc_stage_input(const char *path, const char *work, size_t index,
                               bool *changed, char **error) {
  *changed = false;
  *error = NULL;
  if (!is_linker_input(path) || !file_exists(path))
    return str_copy(path);
  char *directory = path_join(work, str_fmt("tls-%zu", index));
  ensure_dir(directory);
  char *destination = path_join(directory, path_base(path));
  *changed = sparc_tls_relax_file(path, destination, error);
  return !*error && *changed ? destination : str_copy(path);
}
StrVec sparc_tls_prepare_command(StrVec command, const char *work,
                                 char **error) {
  if (error)
    *error = NULL;
  StrVec directories = {0};
  char *e = NULL;
  if (!command.len)
    return command;
  for (size_t i = 1; i < command.len; i++) {
    char *a = command.data[i];
    if (streq(a, "--"))
      break;
    char *value;
    bool separate;
    char kind = linux_library_arg(a, &value, &separate);
    if (kind) {
      if (separate) {
        if (++i >= command.len) {
          e = "missing SPARC linker search argument";
          goto fail;
        }
        value = command.data[i];
      }
      if (kind == 'L')
        sv_push(&directories, value);
    } else if (takes_value(a))
      i++;
  }
  StrVec result = SV(command.data[0]);
  bool modified = false, script = false;
  char *failure = NULL;
  for (size_t i = 1; i < command.len; i++) {
    char *a = command.data[i];
    if (starts_with(a, "-Wl,")) {
      StrVec parts = str_split(a + 4, ",");
      for (size_t j = 0; j < parts.len; j++) {
        char *part = parts.data[j];
        if (streq(part, "-T") || starts_with(part, "--script") ||
            starts_with(part, "-T") || starts_with(part, "@"))
          script = true;
        if (j > 0 && (streq(parts.data[j - 1], "-T") ||
                      streq(parts.data[j - 1], "--script")))
          continue;
        bool changed;
        char *message = NULL;
        parts.data[j] = sparc_stage_input(part, work, result.len * 1000 + j,
                                          &changed, &message);
        if (message)
          failure = message;
        modified |= changed;
      }
      sv_push(&result, str_fmt("-Wl,%s", rt_join(parts, ",")));
      continue;
    }
    char *value;
    bool separate;
    char kind = linux_library_arg(a, &value, &separate);
    if (kind == 'l') {
      if (separate) {
        if (++i >= command.len) {
          e = "missing SPARC library name";
          goto fail;
        }
        value = command.data[i];
      }
      char *name =
               starts_with(value, ":") ? value + 1 : str_fmt("lib%s.a", value),
           *archive = "";
      for (size_t j = 0; j < directories.len; j++) {
        char *candidate = path_join(directories.data[j], name);
        if (file_exists(candidate)) {
          archive = candidate;
          break;
        }
      }
      if (*archive) {
        bool changed;
        char *message = NULL,
             *staged = sparc_stage_input(archive, work, result.len * 1000,
                                         &changed, &message);
        if (message)
          failure = message;
        modified |= changed;
        if (changed) {
          sv_push(&result, staged);
          continue;
        }
      }
      sv_push(&result, a);
      if (separate)
        sv_push(&result, value);
      continue;
    }
    if (streq(a, "-T") || streq(a, "--script") || starts_with(a, "--script=") ||
        (starts_with(a, "-T") && strlen(a) > 2))
      script = true;
    if (streq(a, "-Xlinker") || streq(a, "--for-linker")) {
      sv_push(&result, a);
      if (++i >= command.len) {
        e = "missing SPARC linker argument";
        goto fail;
      }
      value = command.data[i];
      if (streq(value, "-T") || starts_with(value, "--script") ||
          starts_with(value, "@"))
        script = true;
      bool changed;
      char *message = NULL,
           *staged = sparc_stage_input(value, work, result.len * 1000, &changed,
                                       &message);
      if (message)
        failure = message;
      modified |= changed;
      sv_push(&result, staged);
      continue;
    }
    if (takes_value(a)) {
      sv_push(&result, a);
      if (i + 1 < command.len)
        sv_push(&result, command.data[++i]);
      continue;
    }
    if (starts_with(a, "-") && !streq(a, "-")) {
      sv_push(&result, a);
      continue;
    }
    if (in_target_inventory(".ld .lds", path_ext(a)))
      script = true;
    bool changed;
    char *message = NULL,
         *staged =
             sparc_stage_input(a, work, result.len * 1000, &changed, &message);
    if (message)
      failure = message;
    modified |= changed;
    sv_push(&result, staged);
  }
  if (!modified)
    return command;
  if (failure) {
    e = failure;
    goto fail;
  }
  if (script) {
    e = "SPARC static TLS adaptation with linker scripts or linker response "
        "files is not supported";
    goto fail;
  }
  return result;
fail:
  if (error)
    *error = e;
  return (StrVec){0};
}
int run_sparc_static_link(Options o, Target t, Toolchain tc, Runtime rt) {
  return run_profile_link_prepared(o, t, tc, rt, sparc_tls_prepare_command);
}
