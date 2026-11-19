#include "windows_runtime.h"

/* The upstream driver parses cc's linker transports before selecting a linker
   flavor; GNU options must not reach lld-link verbatim. This deliberately
   bounded table is separate from internally generated MSVC flags. */
static void msvc_link_token(StrVec *tokens, const char *arg) {
    size_t n = strlen(arg);
    /* Joined values are split in both -Wl and -Xlinker/--for-linker, not merely
       --long options. Slash options and their path values stay intact. */
    if (n >= 3 && arg[0] == '-' && arg[2] != '-') {
        const char *eq = strchr(arg, '=');
        if (eq) {
            char *name = str_ncopy(arg, (size_t)(eq - arg));
            /* This option's joined form is ignored before the token stream is built. */
            if (streq(name, "--sort-common")) return;
            sv_push(tokens, name);
            sv_push(tokens, str_copy(eq + 1));
            return;
        }
    }
    sv_push(tokens, arg);
}

bool msvc_link_subsystem(const char *value) {
    static const char *const names[] = {"console", "windows", "posix", "native", "efi_application",
        "efi_boot_service_driver", "efi_rom", "efi_runtime_driver"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (streq(value, names[i])) return true;
    return false;
}

static bool in_list(const char *a, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++) if (streq(a, list[i])) return true;
    return false;
}
#define IN_LIST(a, ...) in_list(a, (const char *const[]){__VA_ARGS__}, sizeof((const char *const[]){__VA_ARGS__}) / sizeof(char *))

StrVec msvc_link_arguments(Options o, Target t, StrVec args, char **error) {
    StrVec result = {0}, tokens = {0}, flags = {0}, tail = {0};
    *error = NULL;
    if (!streq(t.os, "windows") || !streq(t.abi, "msvc")) return args;
    bool pic = true;
    const char *definition = "", *driver_entry = "", *language = "";
    for (size_t i = 0; i < args.len; i++) {
        const char *a = args.data[i];
        if (streq(a, "--")) { tail = sv_slice(args, i, args.len); break; }
        if ((streq(a, "-x") || streq(a, "--language")) && i + 1 < args.len) language = args.data[i + 1];
        else if (starts_with(a, "--language=")) language = a + strlen("--language=");
        else if (starts_with(a, "-x") && strlen(a) > 2) language = a + 2;
        if (streq(a, "-fPIC") || streq(a, "-fpic")) { pic = true; continue; }
        if (streq(a, "-fno-PIC") || streq(a, "-fno-pic")) { pic = false; continue; }
        if (IN_LIST(a, "-fPIE", "-fpie", "-fno-PIE", "-fno-pie")) continue;
        if (starts_with(a, "-Wl,")) {
            StrVec parts = str_split(a + 4, ",");
            for (size_t k = 0; k < parts.len; k++) msvc_link_token(&tokens, parts.data[k]);
            continue;
        }
        if (streq(a, "-Xlinker") || streq(a, "--for-linker")) {
            if (i + 1 >= args.len) { *error = str_fmt("expected parameter after %s", a); return (StrVec){0}; }
            i++;
            msvc_link_token(&tokens, args.data[i]);
            continue;
        }
        /* The compiler driver's direct options are consumed as well. */
        if (streq(a, "-e") || streq(a, "-u") || streq(a, "-z")) {
            if (i + 1 >= args.len) { *error = str_fmt("expected parameter after %s", a); return (StrVec){0}; }
            if (streq(a, "-e")) driver_entry = str_fmt("/entry:%s", args.data[i + 1]);
            else { sv_push(&tokens, a); sv_push(&tokens, args.data[i + 1]); }
            i++;
            continue;
        }
        if ((!language[0] || streq(language, "none")) && a[0] != 0 && a[0] != '-' && ends_with(a, ".def")) {
            definition = a;
            continue;
        }
        sv_push(&result, a);
        /* A compiler flag operand is never another transport, PIC flag, or .def. */
        if (takes_value(a) && i + 1 < args.len) { i++; sv_push(&result, args.data[i]); }
    }
    if (!pic) { *error = str_copy("the selected target requires position independent code"); return (StrVec){0}; }

    const char *stack = "", *entry = driver_entry, *subsystem = "", *image_base = "", *implib = "", *pdb = "", *version = "";
    const char *major_subsystem = "6", *minor_subsystem = "0", *major_image = "0", *minor_image = "0";
    bool have_image_version = false, have_subsystem_version = false;
    const char *dynamicbase = "", *allow_undefined = "";
    for (size_t i = 0; i < tokens.len; i++) {
        const char *a = tokens.data[i];
        const char *value = "";
        bool takes_operand = IN_LIST(a, "--stack", "-stack_size", "--entry", "-e", "--subsystem", "--image-base",
            "--major-subsystem-version", "--minor-subsystem-version", "--major-image-version", "--minor-image-version",
            "--out-implib", "-implib", "-u", "-z", "--major-os-version", "--minor-os-version",
            "-rpath", "--rpath", "-R", "-rpath-link", "--rpath-link");
        if (takes_operand) {
            if (i + 1 >= tokens.len) { *error = str_fmt("expected parameter after %s", a); return (StrVec){0}; }
            i++;
            value = tokens.data[i];
        }
        char *err = NULL;
        if (IN_LIST(a, "--stack", "-stack_size", "--image-base")) {
            uint64_t number = objcopy_parse_uint(value, 0, UINT64_MAX, &err);
            if (err) {
                const char *what = streq(a, "--image-base") ? "image base" : "stack size";
                *error = str_fmt("unable to parse %s \"%s\": %s", what, value, err);
                return (StrVec){0};
            }
            if (streq(a, "--image-base")) image_base = str_fmt("/base:%llu", (unsigned long long)number);
            else if (!o.shared) stack = str_fmt("/stack:%llu", (unsigned long long)number);
        } else if (streq(a, "--entry") || streq(a, "-e")) {
            entry = str_fmt("/entry:%s", value);
        } else if (streq(a, "--no-entry")) {
            entry = "/noentry";
        } else if (streq(a, "--subsystem")) {
            if (!msvc_link_subsystem(value)) { *error = str_fmt("invalid: --subsystem: \"%s\"", value); return (StrVec){0}; }
            subsystem = value;
        } else if (IN_LIST(a, "--major-subsystem-version", "--minor-subsystem-version", "--major-image-version", "--minor-image-version")) {
            uint64_t limit = contains(a, "subsystem") ? 65535u : 4294967295u;
            uint64_t number = objcopy_parse_uint(value, 10, limit, &err);
            if (err) { *error = str_fmt("unable to parse %s \"%s\": %s", a + 2, value, err); return (StrVec){0}; }
            const char *normalized = str_fmt("%llu", (unsigned long long)number);
            if (streq(a, "--major-subsystem-version")) { major_subsystem = normalized; have_subsystem_version = true; }
            else if (streq(a, "--minor-subsystem-version")) { minor_subsystem = normalized; have_subsystem_version = true; }
            else if (streq(a, "--major-image-version")) { major_image = normalized; have_image_version = true; }
            else { minor_image = normalized; have_image_version = true; }
        } else if (streq(a, "--out-implib") || streq(a, "-implib")) {
            implib = str_fmt("/implib:%s", value);
        } else if (streq(a, "-u")) {
            sv_push(&flags, str_fmt("/include:%s", value));
        } else if (streq(a, "-Brepro") || streq(a, "/Brepro")) {
            sv_push(&flags, "/Brepro");
        } else if (streq(a, "--tsaware")) {
            sv_push(&flags, "/tsaware");
        } else if (streq(a, "--nxcompat")) {
            sv_push(&flags, "/nxcompat");
        } else if (streq(a, "--dynamicbase")) {
            dynamicbase = "/dynamicbase";
        } else if (streq(a, "--no-dynamicbase")) {
            dynamicbase = "/dynamicbase:no";
        } else if (streq(a, "--allow-shlib-undefined") || streq(a, "-allow-shlib-undefined")) {
            allow_undefined = "/force:unresolved";
        } else if (streq(a, "--no-allow-shlib-undefined") || streq(a, "-no-allow-shlib-undefined")) {
            allow_undefined = "";
        } else if (streq(a, "-z")) {
            if (starts_with(value, "stack-size=")) {
                const char *text = value + strlen("stack-size=");
                uint64_t number = objcopy_parse_uint(text, 0, UINT64_MAX, &err);
                if (err) { *error = str_fmt("unable to parse stack size \"%s\": %s", text, err); return (StrVec){0}; }
                if (!o.shared) stack = str_fmt("/stack:%llu", (unsigned long long)number);
            } else if (!IN_LIST(value, "nodelete", "notext", "defs", "undefs", "origin", "nocopyreloc", "noexecstack", "now", "lazy", "relro", "norelro")) {
                /* ELF-only settings are accepted but unused by the COFF backend. */
                *error = str_fmt("unsupported linker extension flag: -z %s", value);
                return (StrVec){0};
            }
        } else if (IN_LIST(a, "--major-os-version", "--minor-os-version", "-rpath", "--rpath", "-R", "-rpath-link", "--rpath-link",
                           "--start-group", "--end-group", "--gc-sections", "--no-gc-sections", "--eh-frame-hdr", "--sort-common", "-w")) {
            /* Accepted by the driver; these have no effect in its COFF link path. */
        } else if (starts_with(a, "/subsystem:")) {
            subsystem = a + strlen("/subsystem:");
            if (!msvc_link_subsystem(subsystem)) { *error = str_fmt("invalid: --subsystem: \"%s\"", subsystem); return (StrVec){0}; }
        } else if (starts_with(a, "/implib:")) {
            implib = str_fmt("/implib:%s", a + strlen("/implib:"));
        } else if (starts_with(a, "/pdb:")) {
            pdb = str_fmt("/pdb:%s", a + strlen("/pdb:"));
        } else {
            *error = str_fmt("unsupported linker arg: %s", a);
            return (StrVec){0};
        }
    }
    if (have_image_version) version = str_fmt("/version:%s.%s", major_image, minor_image);
    if (subsystem[0]) sv_push(&flags, str_fmt("/subsystem:%s,%s.%s", subsystem, major_subsystem, minor_subsystem));
    else if (have_subsystem_version && needs_link(o)) { *error = str_copy("MSVC subsystem version requires an explicit --subsystem"); return (StrVec){0}; }
    const char *ordered[] = {stack, entry, image_base, implib, pdb, version, dynamicbase, allow_undefined};
    for (size_t i = 0; i < sizeof(ordered) / sizeof(ordered[0]); i++) if (ordered[i][0]) sv_push(&flags, ordered[i]);
    if (definition[0]) sv_push(&flags, str_fmt("/def:%s", definition));
    /* Emit each argument through -Xlinker, never comma-join a filename or value.
       Place global options before -- so literal input names remain literal. */
    if (needs_link(o)) for (size_t i = 0; i < flags.len; i++) { sv_push(&result, "-Xlinker"); sv_push(&result, flags.data[i]); }
    sv_extend(&result, tail);
    return result;
}
