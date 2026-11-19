/* cc linker transports are a Zig command-line language, not raw LLD arguments.
   Port the bounded common options from Zig 0.17.1 src/main.zig (.wl,
   .for_linker and "Parse linker args", MIT). Keep unknown format-specific
   options intact for the existing link drivers, rather than silencing LLD. */
#include "cross.h"
#include "resources.h"

typedef struct { char *value; size_t slot; } Linker_Flag_Token;
typedef struct { Linker_Flag_Token *data; size_t len, cap; } Linker_Flag_Tokens;

static void linker_flag_push(Linker_Flag_Tokens *tokens, const char *value, size_t slot) {
    if (tokens->len == tokens->cap) {
        tokens->cap = tokens->cap ? tokens->cap * 2 : 16;
        tokens->data = xrealloc(tokens->data, tokens->cap * sizeof *tokens->data);
    }
    tokens->data[tokens->len].value = str_copy(value);
    tokens->data[tokens->len].slot = slot;
    tokens->len++;
}

static void linker_flag_emit1(StrVec *out, const char *value) { sv_push(out, "-Xlinker"); sv_push(out, value); }
static void linker_flag_emit2(StrVec *out, const char *a, const char *b) { linker_flag_emit1(out, a); linker_flag_emit1(out, b); }

/* Double-quoted with backslash escapes, as a %q format verb prints a string. */
static char *linker_flag_quote(const char *s) {
    Bytes out = {0};
    bytes_push(&out, '"');
    for (const unsigned char *p = (const unsigned char *)nz(s); *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"': bytes_append(&out, "\\\"", 2); break;
        case '\\': bytes_append(&out, "\\\\", 2); break;
        case '\a': bytes_append(&out, "\\a", 2); break;
        case '\b': bytes_append(&out, "\\b", 2); break;
        case '\f': bytes_append(&out, "\\f", 2); break;
        case '\n': bytes_append(&out, "\\n", 2); break;
        case '\r': bytes_append(&out, "\\r", 2); break;
        case '\t': bytes_append(&out, "\\t", 2); break;
        case '\v': bytes_append(&out, "\\v", 2); break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\x%02x", c);
                bytes_append(&out, buf, 4);
            } else bytes_push(&out, c);
        }
    }
    bytes_push(&out, '"');
    bytes_push(&out, 0);
    return (char *)out.data;
}

static bool linker_flag_inline(const char *a) {
    return streq(a, "--as-needed") || streq(a, "--no-as-needed") || streq(a, "--whole-archive") || streq(a, "-whole-archive") ||
           streq(a, "--no-whole-archive") || streq(a, "-no-whole-archive");
}

/* Split joined values exactly where Zig does; -zstack-size=... becomes two
   tokens too (and is consequently not a valid joined spelling in Zig). */
static void linker_flag_token(Linker_Flag_Tokens *tokens, const char *a, size_t slot) {
    size_t n = strlen(a);
    const char *eq = strchr(a, '=');
    if (starts_with(a, "-z") && n > 2 && !eq) {
        linker_flag_push(tokens, "-z", slot);
        linker_flag_push(tokens, a + 2, slot);
        return;
    }
    if (n >= 3 && a[0] == '-' && a[2] != '-' && eq) {
        char *key = str_ncopy(a, (size_t)(eq - a));
        if (streq(key, "--sort-common")) return;
        /* --build-id is ambiguous without its joined spelling. */
        if (!streq(key, "--build-id")) {
            linker_flag_push(tokens, key, slot);
            linker_flag_push(tokens, eq + 1, slot);
            return;
        }
    }
    linker_flag_push(tokens, a, slot);
}

static bool linker_flag_operand(const char *a) {
    static const char *names[] = {
        "--version-script", "-version-script", "--hash-style", "--subsystem", "--stack", "-stack_size",
        "-soname", "--soname", "-rpath", "--rpath", "-R", "-rpath-link", "--rpath-link",
        "-z", "-O", "--compress-debug-sections", "--major-os-version", "--minor-os-version",
        "--sort-section", "-install_name", "-e", "--entry", "-u", "-T", "--script", "--out-implib", "--pdb",
        "-headerpad", "-pagezero_size", "--export", "--initial-memory", "--max-memory", "--global-base",
        "-force_load", "-exported_symbols_list",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) if (streq(a, names[i])) return true;
    return false;
}

static bool linker_flag_in(const char *a, const char *const *names, size_t n) {
    for (size_t i = 0; i < n; i++) if (streq(a, names[i])) return true;
    return false;
}
#define LINKER_FLAG_IN(a, ...) linker_flag_in(a, (const char *const[]){__VA_ARGS__}, sizeof((const char *[]){__VA_ARGS__}) / sizeof(char *))

static Options linker_flag_fail(Options result, char **error, char *message) {
    if (error) *error = message;
    return result;
}

/* Apply only to user arguments, before runtime-generated format-specific flags.
   Slots retain input order while the separate token stream lets operands span
   -Wl and -Xlinker transports, without eating ordinary compiler operands. */
Options normalize_linker_flags(Options o, Target t, char **error) {
    Options result = o;
    if (error) *error = NULL;
    /* The MSVC translator has its own validated GNU-to-COFF option language. */
    if (streq(t.os, "windows") && streq(t.abi, "msvc")) return result;
    bool apple = is_apple_os(t.os);
    bool wasm = streq(t.arch, "wasm32") || streq(t.arch, "wasm64");
    bool coff = streq(t.os, "windows");
    bool elf = !apple && !wasm && !coff;
    StrVec args = o.forwarded;
    StrVec *pieces = xmalloc((args.len + 1) * sizeof *pieces);
    memset(pieces, 0, (args.len + 1) * sizeof *pieces);
    Linker_Flag_Tokens tokens = {0};
    bool whole = false, needed = false;
    StrVec dirs = {0};
    /* Library search paths apply globally, including ones after the library. */
    for (size_t i = 0; i < args.len; i++) {
        const char *a = args.data[i];
        if (streq(a, "--")) break;
        if (streq(a, "-L") || streq(a, "--library-directory")) {
            if (i+1 < args.len) { i++; sv_push(&dirs, args.data[i]); }
        } else if (starts_with(a, "-L") && strlen(a) > 2) sv_push(&dirs, a + 2);
        else if (starts_with(a, "--library-directory=")) sv_push(&dirs, a + strlen("--library-directory="));
        else if (takes_value(a)) i++;
    }
    size_t tail = args.len;
    for (size_t i = 0; i < args.len; i++) {
        const char *a = args.data[i];
        if (streq(a, "--")) {
            tail = i;
            for (size_t k = i; k < args.len; k++) sv_push(&pieces[i], args.data[k]);
            break;
        }
        StrVec payload = {0};
        bool have_payload = false;
        if (starts_with(a, "-Wl,")) {
            /* Splitting always yields at least one (possibly empty) token. */
            const char *s = a + 4;
            for (;;) {
                const char *comma = strchr(s, ',');
                if (!comma) { sv_push(&payload, s); break; }
                sv_push(&payload, str_ncopy(s, (size_t)(comma - s)));
                s = comma + 1;
            }
            have_payload = true;
        } else if (streq(a, "-Xlinker") || streq(a, "--for-linker")) {
            if (i+1 >= args.len) return linker_flag_fail(result, error, str_fmt("expected parameter after %s", a));
            i++;
            sv_push(&payload, args.data[i]);
            have_payload = true;
        }
        if (have_payload) {
            for (size_t k = 0; k < payload.len; k++) {
                const char *token = payload.data[k];
                if (linker_flag_inline(token)) {
                    if (streq(token, "--as-needed")) { needed = false; if (elf) linker_flag_emit1(&pieces[i], token); }
                    else if (streq(token, "--no-as-needed")) { needed = true; if (elf) linker_flag_emit1(&pieces[i], token); }
                    else if (streq(token, "--whole-archive") || streq(token, "-whole-archive")) { whole = true; if (!apple) linker_flag_emit1(&pieces[i], "--whole-archive"); }
                    else { whole = false; if (!apple) linker_flag_emit1(&pieces[i], "--no-whole-archive"); }
                } else linker_flag_token(&tokens, token, i);
            }
            continue;
        }
        /* Direct -z shares the same parser in Zig. Other driver options keep
           their Clang meanings (-x is a language, -O is optimization, etc.). */
        if (streq(a, "-z")) {
            if (i+1 >= args.len) return linker_flag_fail(result, error, "expected parameter after -z");
            linker_flag_push(&tokens, a, i);
            linker_flag_push(&tokens, args.data[i+1], i);
            i++;
            continue;
        }
        if (starts_with(a, "-z") && strlen(a) > 2) { linker_flag_token(&tokens, a, i); continue; }
        if (apple && (streq(a, "-l") || (starts_with(a, "-l") && strlen(a) > 2))) {
            const char *name = a + 2;
            size_t slot = i;
            if (streq(a, "-l")) {
                if (i+1 >= args.len) return linker_flag_fail(result, error, "expected parameter after -l");
                i++;
                name = args.data[i];
            }
            const char *archive = "";
            if (whole && !bundled_runtime_library_name(t, name)) {
                for (size_t d = 0; d < dirs.len; d++) {
                    char *path = path_join(dirs.data[d], str_fmt("lib%s.a", name));
                    if (is_file(path)) { archive = path; break; }
                }
            }
            /* The separate -l value advances i; its pieces land in that later slot. */
            slot = i;
            if (*archive) linker_flag_emit2(&pieces[slot], "-force_load", archive);
            else if (needed && !bundled_runtime_library_name(t, name)) linker_flag_emit1(&pieces[slot], str_fmt("-needed-l%s", name));
            else { sv_push(&pieces[slot], "-l"); sv_push(&pieces[slot], name); }
            continue;
        }
        if (apple && whole && (ends_with(a, ".a") || ends_with(a, ".o")) && !starts_with(a, "-")) {
            linker_flag_emit2(&pieces[i], "-force_load", a);
            continue;
        }
        sv_push(&pieces[i], a);
        if (takes_value(a) && i+1 < args.len) { i++; sv_push(&pieces[i], args.data[i]); }
    }
    int gc = 0; /* last setting wins; Mach-O has no --no-dead-strip option */
    for (size_t i = 0; i < tokens.len; i++) {
        Linker_Flag_Token token = tokens.data[i];
        const char *a = token.value, *value = "";
        if (linker_flag_operand(a)) {
            if (i+1 >= tokens.len) return linker_flag_fail(result, error, str_fmt("expected parameter after %s", a));
            i++;
            value = tokens.data[i].value;
        }
        StrVec *out = &pieces[token.slot];
        if (LINKER_FLAG_IN(a, "--gc-sections", "-dead_strip")) gc = 1;
        else if (streq(a, "--no-gc-sections")) gc = -1;
        else if (LINKER_FLAG_IN(a, "--start-group", "--end-group", "--sort-common", "-w", "--major-os-version", "--minor-os-version")) {
            /* Accepted by Zig but not forwarded to any format. */
        } else if (streq(a, "-O")) {
            /* Deprecated linker optimization is ignored by Zig. */
        } else if (LINKER_FLAG_IN(a, "--version-script", "-version-script")) {
            if (elf) linker_flag_emit2(out, "--version-script", value);
        } else if (streq(a, "--hash-style")) {
            if (!streq(value, "sysv") && !streq(value, "gnu") && !streq(value, "both")) return linker_flag_fail(result, error, str_fmt("invalid hash style: %s", value));
            if (elf) linker_flag_emit1(out, str_fmt("--hash-style=%s", value));
        } else if (streq(a, "--compress-debug-sections")) {
            if (!streq(value, "none") && !streq(value, "zlib") && !streq(value, "zstd")) return linker_flag_fail(result, error, str_fmt("expected [none|zlib|zstd] after --compress-debug-sections, found %s", linker_flag_quote(value)));
            if (elf && !streq(value, "none")) linker_flag_emit1(out, str_fmt("--compress-debug-sections=%s", value));
        } else if (streq(a, "--subsystem")) {
            if (!msvc_link_subsystem(value)) return linker_flag_fail(result, error, str_fmt("invalid: --subsystem: %s", linker_flag_quote(value)));
            if (coff) linker_flag_emit2(out, a, value);
        } else if (LINKER_FLAG_IN(a, "--stack", "-stack_size")) {
            char *msg = NULL;
            uint64_t number = objcopy_parse_uint(value, 0, UINT64_MAX, &msg);
            if (msg) return linker_flag_fail(result, error, str_fmt("unable to parse stack size %s: %s", linker_flag_quote(value), msg));
            if (!o.shared) {
                if (coff) linker_flag_emit2(out, "--stack", str_fmt("%llu", (unsigned long long)number));
                else if (wasm || elf) linker_flag_emit2(out, "-z", str_fmt("stack-size=%llu", (unsigned long long)number));
                else if (apple) linker_flag_emit2(out, "-stack_size", str_fmt("0x%llx", (unsigned long long)number));
            }
        } else if (streq(a, "-z")) {
            const char *z = value;
            if (LINKER_FLAG_IN(z, "nodelete", "notext", "defs", "undefs", "origin", "nocopyreloc", "noexecstack", "now", "lazy", "relro", "norelro")) {
                if (elf) linker_flag_emit2(out, "-z", z);
            } else if (starts_with(z, "stack-size=")) {
                char *msg = NULL;
                uint64_t number = objcopy_parse_uint(z + strlen("stack-size="), 0, UINT64_MAX, &msg);
                if (msg) return linker_flag_fail(result, error, str_fmt("unable to parse stack size %s: %s", linker_flag_quote(z), msg));
                if (!o.shared) {
                    if (elf || wasm) linker_flag_emit2(out, "-z", z);
                    else if (coff) linker_flag_emit2(out, "--stack", str_fmt("%llu", (unsigned long long)number));
                    else linker_flag_emit2(out, "-stack_size", str_fmt("0x%llx", (unsigned long long)number));
                }
            } else if (starts_with(z, "common-page-size=") || starts_with(z, "max-page-size=")) {
                char *msg = NULL;
                (void)objcopy_parse_uint(strchr(z, '=') + 1, 0, UINT64_MAX, &msg);
                if (msg) return linker_flag_fail(result, error, str_fmt("unable to parse -z %s: %s", z, msg));
                if (elf) linker_flag_emit2(out, "-z", z);
            } else return linker_flag_fail(result, error, str_fmt("unsupported linker extension flag: -z %s", z));
        } else if (streq(a, "--no-undefined")) {
            if (elf) linker_flag_emit2(out, "-z", "defs");
        } else if (LINKER_FLAG_IN(a, "--export-dynamic", "-export-dynamic", "-E", "--export-all-symbols")) {
            if (elf || wasm) linker_flag_emit1(out, "--export-dynamic");
            else if (coff) linker_flag_emit1(out, "--export-all-symbols");
            else linker_flag_emit1(out, "-export_dynamic");
        } else if (LINKER_FLAG_IN(a, "--print-gc-sections", "--print-icf-sections", "--eh-frame-hdr", "--no-eh-frame-hdr", "-Bsymbolic",
                                  "--disable-new-dtags", "--enable-new-dtags", "--undefined-version", "--no-undefined-version")) {
            if (elf) linker_flag_emit1(out, a);
        } else if (LINKER_FLAG_IN(a, "--allow-shlib-undefined", "-allow-shlib-undefined", "--no-allow-shlib-undefined", "-no-allow-shlib-undefined")) {
            if (elf) linker_flag_emit1(out, a);
            else if (apple && o.shared && (streq(a, "--allow-shlib-undefined") || streq(a, "-allow-shlib-undefined"))) linker_flag_emit2(out, "-undefined", "dynamic_lookup");
        } else if (streq(a, "--build-id")) {
            if (elf || coff) linker_flag_emit1(out, a);
        } else if (LINKER_FLAG_IN(a, "-soname", "--soname")) {
            if (elf) linker_flag_emit2(out, "-soname", value);
            else if (apple && o.shared) linker_flag_emit2(out, "-install_name", value);
        } else if (LINKER_FLAG_IN(a, "-rpath", "--rpath", "-R")) {
            if (elf || apple) linker_flag_emit2(out, "-rpath", value);
        } else if (LINKER_FLAG_IN(a, "-rpath-link", "--rpath-link")) {
            /* Zig warns and ignores these. */
        } else if (streq(a, "-headerpad_max_install_names")) {
            if (apple) linker_flag_emit1(out, a);
        } else if (LINKER_FLAG_IN(a, "--strip-all", "--strip-debug", "-s", "-S")) {
            result.strip = true;
            /* The driver handles debug/strip policy for each target. */
            sv_push(out, "-s");
        } else if (LINKER_FLAG_IN(a, "-x", "--discard-all")) {
            if (elf || apple) linker_flag_emit1(out, "-x");
        } else if (streq(a, "--no-entry")) {
            if (wasm) linker_flag_emit1(out, a);
            else if (coff) {
                if (!o.shared) return linker_flag_fail(result, error, "/noentry must be specified with /dll");
                linker_flag_emit2(out, "--entry", "0");
            }
        } else if (streq(a, "--sort-section")) {
            if (!streq(value, "name") && !streq(value, "alignment")) return linker_flag_fail(result, error, str_fmt("expected [name|alignment] after --sort-section, found %s", linker_flag_quote(value)));
            if (elf) linker_flag_emit2(out, a, value);
        } else if (LINKER_FLAG_IN(a, "--enable-auto-import", "--icf", "--threads", "--dynamic-list", "--exclude-libs", "--push-state", "--pop-state")) {
            return linker_flag_fail(result, error, str_fmt("unsupported linker arg: %s", a));
        } else if (linker_flag_operand(a)) {
            linker_flag_emit2(out, a, value);
        } else if (starts_with(a, "--build-id=")) {
            const char *style = a + strlen("--build-id=");
            if (!streq(style, "none") && !streq(style, "fast") && !streq(style, "uuid") && !streq(style, "sha1") && !streq(style, "md5") && !starts_with(style, "0x"))
                return linker_flag_fail(result, error, str_fmt("unable to parse --build-id style %s", linker_flag_quote(style)));
            if (elf || coff) linker_flag_emit1(out, a);
        } else if (starts_with(a, "-O")) {
            /* -O1 and friends are parsed but ignored, as above. */
        } else if (starts_with(a, "-z")) {
            return linker_flag_fail(result, error, str_fmt("unsupported linker extension flag: -z %s", a + 2));
        } else if (starts_with(a, "-l") || ends_with(a, ".a") || ends_with(a, ".o") || ends_with(a, ".obj")) {
            /* Only driver inputs participate in library/whole-archive state. */
            return linker_flag_fail(result, error, str_fmt("unsupported linker arg: %s", a));
        } else if (!needs_link(o)) {
            return linker_flag_fail(result, error, str_fmt("unsupported linker arg: %s", a));
        } else linker_flag_emit1(out, a);
    }
    if (needs_link(o)) {
        if (gc != 0) {
            if (apple) { if (gc > 0) linker_flag_emit1(&pieces[args.len], "-dead_strip"); }
            else linker_flag_emit1(&pieces[args.len], gc > 0 ? "--gc-sections" : "--no-gc-sections");
        }
        if (whole && !apple) linker_flag_emit1(&pieces[args.len], "--no-whole-archive");
        if (needed && elf) linker_flag_emit1(&pieces[args.len], "--as-needed");
    } else {
        /* All recognized settings were validated above, even for -c/-E/-S.
           Consume the validated settings, without blanket suppression of other
           unused compiler arguments. */
        for (size_t p = 0; p <= args.len; p++) {
            StrVec kept = {0};
            for (size_t j = 0; j < pieces[p].len; j++) {
                if (streq(pieces[p].data[j], "-Xlinker")) { j++; continue; }
                if (streq(pieces[p].data[j], "-s")) continue;
                sv_push(&kept, pieces[p].data[j]);
            }
            pieces[p] = kept;
        }
    }
    StrVec forwarded = {0};
    /* Global settings go before --, so literal filenames remain literal. */
    for (size_t i = 0; i < tail; i++) sv_extend(&forwarded, pieces[i]);
    sv_extend(&forwarded, pieces[args.len]);
    if (tail < args.len) sv_extend(&forwarded, pieces[tail]);
    result.forwarded = forwarded;
    return result;
}
