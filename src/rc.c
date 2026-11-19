/* Derived from Zig 0.17.1 lib/compiler/resinator (MIT).
   Copyright (c) Zig contributors. See the permission notice in ../LICENSE.
   Windows resource compiler. The resource serialization is implemented here;
   the statically linked Clang frontend only preprocesses source text.
   Unsupported resource grammars are errors, never silently discarded resources. */
#include "cross.h"
#include "resources.h"
#include "rc_case.h"
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

typedef struct { char *text; size_t len; uint16_t ordinal; bool named; } RC_Name;
typedef struct { uint16_t language, flags; uint32_t version, characteristics; } RC_Meta;
typedef struct { char *text; size_t len; int line; bool quoted, wide; int input_cp, output_cp; } RC_Token;
typedef struct { uint32_t value; bool lng; } RC_Number;
typedef struct {
    char *input, *output, *input_format, *output_format, *target, *depfile;
    StrVec includes, defines, undefines;
    uint16_t language;
    int codepage;
    bool no_preprocess, preprocess_only, ignore_include, null_strings, no_auto_includes;
    char *auto_includes, *msvc_libc_file;
    bool msvc_system, msvc_accept_license;
    int max_string;
} RC_Options;
typedef struct { uint16_t id; RC_Meta meta; Bytes strings[16]; bool present[16]; } RC_String_Block;
typedef struct { uint16_t id; unsigned char header[148]; } RC_Font;
typedef struct {
    RC_Font *fonts; size_t fonts_len, fonts_cap;
    RC_String_Block *string_blocks; size_t blocks_len, blocks_cap;
    RC_Token *tokens; size_t tokens_len, tokens_cap;
    size_t pos;
    char *input, *error;
    size_t error_len;
    RC_Options options;
    RC_Meta meta;
    Bytes output;
    StrVec dependencies;
    uint16_t icon_id;
} RC_Parser;

/* ---- byte buffers ---- */

void bytes_push(Bytes *b, unsigned char c) {
    if (b->len == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->data = xrealloc(b->data, b->cap);
    }
    b->data[b->len++] = c;
}

void bytes_append(Bytes *b, const void *data, size_t n) {
    if (n == 0) return;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + n) cap *= 2;
        b->data = xrealloc(b->data, cap);
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
}

/* ---- UTF-8 runes: invalid byte sequences decode to U+FFFD, consuming one byte ---- */

static uint32_t rc_decode(const unsigned char *s, size_t n, size_t *width) {
    *width = 1;
    if (n == 0) return 0xfffd;
    unsigned char c = s[0];
    if (c < 0x80) return c;
    if (c < 0xc2 || c > 0xf4) return 0xfffd;
    if (c < 0xe0) {
        if (n < 2 || (s[1] & 0xc0) != 0x80) return 0xfffd;
        *width = 2;
        return (uint32_t)(c & 0x1f) << 6 | (uint32_t)(s[1] & 0x3f);
    }
    if (c < 0xf0) {
        unsigned char lo = 0x80, hi = 0xbf;
        if (c == 0xe0) lo = 0xa0;
        if (c == 0xed) hi = 0x9f;
        if (n < 3 || s[1] < lo || s[1] > hi || (s[2] & 0xc0) != 0x80) return 0xfffd;
        *width = 3;
        return (uint32_t)(c & 0x0f) << 12 | (uint32_t)(s[1] & 0x3f) << 6 | (uint32_t)(s[2] & 0x3f);
    }
    unsigned char lo = 0x80, hi = 0xbf;
    if (c == 0xf0) lo = 0x90;
    if (c == 0xf4) hi = 0x8f;
    if (n < 4 || s[1] < lo || s[1] > hi || (s[2] & 0xc0) != 0x80 || (s[3] & 0xc0) != 0x80) return 0xfffd;
    *width = 4;
    return (uint32_t)(c & 0x07) << 18 | (uint32_t)(s[1] & 0x3f) << 12 | (uint32_t)(s[2] & 0x3f) << 6 | (uint32_t)(s[3] & 0x3f);
}

/* Encode a rune; surrogates and out-of-range values encode U+FFFD. */
static void rc_encode(Bytes *b, uint32_t r) {
    if ((r >= 0xd800 && r <= 0xdfff) || r > 0x10ffff) r = 0xfffd;
    if (r < 0x80) bytes_push(b, (unsigned char)r);
    else if (r < 0x800) { bytes_push(b, (unsigned char)(0xc0 | (r >> 6))); bytes_push(b, (unsigned char)(0x80 | (r & 63))); }
    else if (r < 0x10000) { bytes_push(b, (unsigned char)(0xe0 | (r >> 12))); bytes_push(b, (unsigned char)(0x80 | ((r >> 6) & 63))); bytes_push(b, (unsigned char)(0x80 | (r & 63))); }
    else { bytes_push(b, (unsigned char)(0xf0 | (r >> 18))); bytes_push(b, (unsigned char)(0x80 | ((r >> 12) & 63))); bytes_push(b, (unsigned char)(0x80 | ((r >> 6) & 63))); bytes_push(b, (unsigned char)(0x80 | (r & 63))); }
}

static uint32_t rc_case_lookup(const uint32_t (*table)[2], size_t count, uint32_t r) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (table[mid][0] == r) return table[mid][1];
        if (table[mid][0] < r) lo = mid + 1; else hi = mid;
    }
    return r;
}

static uint32_t rc_upper_rune(uint32_t r) {
    if (r < 0x80) return r >= 'a' && r <= 'z' ? r - 0x20 : r;
    return rc_case_lookup(rc_upper_table, sizeof rc_upper_table / sizeof rc_upper_table[0], r);
}

static uint32_t rc_lower_rune(uint32_t r) {
    if (r < 0x80) return r >= 'A' && r <= 'Z' ? r + 0x20 : r;
    return rc_case_lookup(rc_lower_table, sizeof rc_lower_table / sizeof rc_lower_table[0], r);
}

/* Rune-wise Unicode case mapping, re-encoded as UTF-8 (NUL-terminated, length in *out_len). */
static char *rc_case(const char *s, size_t n, bool upper, size_t *out_len) {
    Bytes b = {0};
    for (size_t i = 0; i < n;) {
        size_t w;
        uint32_t r = rc_decode((const unsigned char *)s + i, n - i, &w);
        rc_encode(&b, upper ? rc_upper_rune(r) : rc_lower_rune(r));
        i += w;
    }
    bytes_push(&b, 0);
    if (out_len) *out_len = b.len - 1;
    return (char *)b.data;
}

static char *rc_upper(const char *s) { return rc_case(s, strlen(s), true, NULL); }
static char *rc_lower(const char *s) { return rc_case(s, strlen(s), false, NULL); }

/* Unicode White_Space rune width at s (0 when not a space). */
static size_t rc_space_width(const unsigned char *s, size_t n) {
    size_t w;
    uint32_t r = rc_decode(s, n, &w);
    switch (r) {
    case '\t': case '\n': case '\v': case '\f': case '\r': case ' ': case 0x85: case 0xa0:
    case 0x1680: case 0x2028: case 0x2029: case 0x202f: case 0x205f: case 0x3000:
        return w;
    default:
        if (r >= 0x2000 && r <= 0x200a) return w;
        return 0;
    }
}

static size_t rc_trim_left_len(const char *s, size_t n) {
    size_t i = 0, w;
    while (i < n && (w = rc_space_width((const unsigned char *)s + i, n - i)) > 0) i += w;
    return i;
}

/* Trim Unicode white space from both ends. */
static char *rc_trim_space(const char *s, size_t n) {
    size_t start = rc_trim_left_len(s, n);
    size_t end = start, i = start;
    while (i < n) {
        size_t w = rc_space_width((const unsigned char *)s + i, n - i);
        if (w > 0) { i += w; continue; }
        size_t rw;
        rc_decode((const unsigned char *)s + i, n - i, &rw);
        i += rw;
        end = i;
    }
    return str_ncopy(s + start, end - start);
}

/* Integer parse with an explicit base 10: optional sign (only before more text), '_' separators, wrapping. */
static bool rc_parse_int(const char *s, int64_t *out) {
    bool neg = false;
    size_t n = strlen(s);
    if (n > 1) {
        if (s[0] == '-') { neg = true; s++; n--; }
        else if (s[0] == '+') { s++; n--; }
    }
    uint64_t value = 0;
    size_t i = 0;
    while (i < n) {
        size_t w;
        uint32_t r = rc_decode((const unsigned char *)s + i, n - i, &w);
        if (r == '_') { i += w; continue; }
        int v = r >= '0' && r <= '9' ? (int)(r - '0') : 16;
        if (v >= 10) break;
        value = value * 10 + (uint64_t)v;
        i += w;
    }
    if (neg) value = 0 - value;
    *out = (int64_t)value;
    return i == n;
}

static bool rc_text_eq(const char *a, size_t an, const char *b) {
    size_t bn = strlen(b);
    return an == bn && memcmp(a, b, an) == 0;
}

static bool rc_has_prefix(const char *s, size_t n, const char *prefix) {
    size_t pn = strlen(prefix);
    return n >= pn && memcmp(s, prefix, pn) == 0;
}

/* ---- serialization ---- */

/* Messages may quote token text containing NUL bytes, so lengths are explicit. */
static void rc_error_n(RC_Parser *p, const char *message, size_t message_len) {
    if (p->error && *p->error) return;
    int line = 1;
    if (p->pos < p->tokens_len) line = p->tokens[p->pos].line;
    Bytes b = {0};
    char *head = str_fmt("%s:%d: error: ", p->input, line);
    bytes_append(&b, head, strlen(head));
    bytes_append(&b, message, message_len);
    bytes_push(&b, 0);
    p->error = (char *)b.data;
    p->error_len = b.len - 1;
}
static void rc_error(RC_Parser *p, const char *message) { rc_error_n(p, message, strlen(message)); }
/* prefix + text[0:len] + suffix */
static void rc_error_text(RC_Parser *p, const char *prefix, const char *text, size_t len, const char *suffix) {
    Bytes b = {0};
    bytes_append(&b, prefix, strlen(prefix));
    bytes_append(&b, text, len);
    bytes_append(&b, suffix, strlen(suffix));
    rc_error_n(p, b.data ? (const char *)b.data : "", b.len);
}
static void rc_print_error(RC_Parser *p) { fwrite(p->error, 1, p->error_len, stderr); fputc('\n', stderr); }
static bool rc_failed(RC_Parser *p) { return p->error && *p->error; }

void rc_u16(Bytes *b, uint16_t n) { bytes_push(b, (unsigned char)n); bytes_push(b, (unsigned char)(n >> 8)); }
void rc_u32(Bytes *b, uint32_t n) { rc_u16(b, (uint16_t)n); rc_u16(b, (uint16_t)(n >> 16)); }
void rc_set16(unsigned char *b, size_t at, uint16_t n) { b[at] = (unsigned char)n; b[at+1] = (unsigned char)(n >> 8); }
void rc_set32(unsigned char *b, size_t at, uint32_t n) { rc_set16(b, at, (uint16_t)n); rc_set16(b, at+2, (uint16_t)(n >> 16)); }
uint16_t rc_get16(const unsigned char *b, size_t at) { return (uint16_t)(b[at] | b[at+1] << 8); }
uint32_t rc_get32(const unsigned char *b, size_t at) { return (uint32_t)rc_get16(b, at) | (uint32_t)rc_get16(b, at+2) << 16; }
static void rc_align(Bytes *b) { while (b->len % 4 != 0) bytes_push(b, 0); }

static void rc_utf16_n(Bytes *b, const char *s, size_t n, bool nul) {
    for (size_t i = 0; i < n;) {
        size_t w;
        uint32_t r = rc_decode((const unsigned char *)s + i, n - i, &w);
        i += w;
        if (r < 0x10000) rc_u16(b, (uint16_t)r);
        else {
            r -= 0x10000;
            rc_u16(b, (uint16_t)(0xd800 + (r >> 10))); rc_u16(b, (uint16_t)(0xdc00 + (r & 1023)));
        }
    }
    if (nul) rc_u16(b, 0);
}
static void rc_utf16(Bytes *b, const char *s, bool nul) { rc_utf16_n(b, s, strlen(s), nul); }

static void rc_utf8(Bytes *b, uint32_t n) {
    if (n < 0x80) bytes_push(b, (unsigned char)n);
    else if (n < 0x800) { bytes_push(b, (unsigned char)(0xc0 | (n >> 6))); bytes_push(b, (unsigned char)(0x80 | (n & 63))); }
    else if (n < 0x10000) { bytes_push(b, (unsigned char)(0xe0 | (n >> 12))); bytes_push(b, (unsigned char)(0x80 | ((n >> 6) & 63))); bytes_push(b, (unsigned char)(0x80 | (n & 63))); }
    else { bytes_push(b, (unsigned char)(0xf0 | (n >> 18))); bytes_push(b, (unsigned char)(0x80 | ((n >> 12) & 63))); bytes_push(b, (unsigned char)(0x80 | ((n >> 6) & 63))); bytes_push(b, (unsigned char)(0x80 | (n & 63))); }
}

static RC_Name rc_ordinal(uint16_t n) { RC_Name name = {"", 0, n, false}; return name; }

static void rc_write_name(Bytes *b, RC_Name n) {
    if (n.named) rc_utf16_n(b, n.text, n.len, true);
    else { rc_u16(b, 0xffff); rc_u16(b, n.ordinal); }
}

static void rc_resource(RC_Parser *p, RC_Name typ, RC_Name name, RC_Meta m, const unsigned char *data, size_t len) {
    size_t start = p->output.len;
    rc_u32(&p->output, (uint32_t)len); rc_u32(&p->output, 0);
    rc_write_name(&p->output, typ); rc_write_name(&p->output, name); rc_align(&p->output);
    rc_u32(&p->output, 0); rc_u16(&p->output, m.flags); rc_u16(&p->output, m.language);
    rc_u32(&p->output, m.version); rc_u32(&p->output, m.characteristics);
    rc_set32(p->output.data, start+4, (uint32_t)(p->output.len - start));
    bytes_append(&p->output, data, len); rc_align(&p->output);
}

/* ---- lexer ---- */

static void rc_push_token(RC_Parser *p, const char *text, size_t len, int line, bool quoted, bool wide, int input_cp, int output_cp) {
    if (p->tokens_len == p->tokens_cap) {
        p->tokens_cap = p->tokens_cap ? p->tokens_cap * 2 : 256;
        p->tokens = xrealloc(p->tokens, p->tokens_cap * sizeof *p->tokens);
    }
    RC_Token *t = &p->tokens[p->tokens_len++];
    t->text = str_ncopy(text, len);
    if (len) memcpy(t->text, text, len); /* keep embedded NUL bytes */
    t->len = len; t->line = line; t->quoted = quoted; t->wide = wide; t->input_cp = input_cp; t->output_cp = output_cp;
}

static bool rc_is_ws(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

static bool rc_lex_dependency_known(RC_Parser *p, const char *name) {
    for (size_t i = 0; i < p->dependencies.len; i++) if (streq(p->dependencies.data[i], name)) return true;
    return false;
}

/* Extension of the final path element, including the dot. */
static char *rc_path_ext(const char *path) {
    size_t n = strlen(path);
    for (size_t i = n; i > 0; i--) {
        char c = path[i-1];
#ifdef _WIN32
        if (c == '/' || c == '\\') break;
#else
        if (c == '/') break;
#endif
        if (c == '.') return str_copy(path + i - 1);
    }
    return "";
}

static void rc_lex(RC_Parser *p, const char *source, size_t source_len) {
    size_t i = 0;
    int line = 1;
    bool ignore_contents = false;
    int input_cp = p->options.codepage, output_cp = p->options.codepage;
    bool included = false;
    bool disjoint_possible = true, disjoint = false;
    int pragmas = 0;
    while (i < source_len && !rc_failed(p)) {
        unsigned char ch = (unsigned char)source[i];
        if (ch <= ' ') { if (ch == '\n') line++; i++; continue; }
        if (ch == '#') {
            /* Clang line directives and pragmas are handled separately from RC. */
            size_t end = i;
            while (end < source_len && source[end] != '\n') end++;
            char *directive = rc_trim_space(source + i, end - i);
            size_t dlen = strlen(directive);
            if (rc_has_prefix(directive, dlen, "# ") || rc_has_prefix(directive, dlen, "#line ")) {
                const char *m = directive;
                if (starts_with(m, "#line")) m += 5;
                if (starts_with(m, "#")) m += 1;
                char *marker = rc_trim_space(m, strlen(m));
                size_t mlen = strlen(marker);
                /* First white-space separated field. */
                size_t f0 = rc_trim_left_len(marker, mlen), f1 = f0;
                while (f1 < mlen && rc_space_width((const unsigned char *)marker + f1, mlen - f1) == 0) {
                    size_t w;
                    rc_decode((const unsigned char *)marker + f1, mlen - f1, &w);
                    f1 += w;
                }
                if (f1 > f0) {
                    int64_t n;
                    if (rc_parse_int(str_ncopy(marker + f0, f1 - f0), &n)) line = (int)(n - 1 > 0 ? n - 1 : 0);
                }
                long first = -1, last = -1;
                for (size_t j = 0; j < mlen; j++) {
                    if (marker[j] == '"') { if (first < 0) first = (long)j; else last = (long)j; }
                }
                if (first >= 0 && last > first) {
                    char *filename = str_ncopy(marker + first + 1, (size_t)(last - first - 1));
                    char *ext = rc_lower(rc_path_ext(filename));
                    ignore_contents = streq(ext, ".h") || streq(ext, ".c");
                    if (*filename && filename[0] != '<') {
                        included = !streq(filename, p->input);
                        if (included) disjoint_possible = false;
                    }
                    if (*filename && filename[0] != '<') {
                        if (!rc_lex_dependency_known(p, filename)) sv_push(&p->dependencies, filename);
                    }
                }
            }
            const char *pr = directive;
            if (starts_with(pr, "#pragma")) pr += strlen("#pragma");
            char *pragma = rc_trim_space(pr, strlen(pr));
            if (starts_with(directive, "#pragma") && starts_with(rc_lower(pragma), "code_page") && !included) {
                const char *after = pragma + strlen("code_page");
                const char *v = after + rc_trim_left_len(after, strlen(after));
                size_t vn = strlen(v), k = 0;
                while (k < vn && rc_is_ws(v[k])) k++;
                v += k; vn -= k;
                if (vn == 0 || v[0] != '(') rc_error(p, "invalid #pragma code_page");
                else {
                    v++; vn--;
                    k = 0;
                    while (k < vn && rc_is_ws(v[k])) k++;
                    v += k; vn -= k;
                    k = 0;
                    while (k < vn && v[k] != ')' && !rc_is_ws(v[k])) k++;
                    char *num = str_ncopy(v, k);
                    v += k; vn -= k;
                    k = 0;
                    while (k < vn && rc_is_ws(v[k])) k++;
                    v += k; vn -= k;
                    bool reset = streq(rc_upper(num), "DEFAULT");
                    int n = 0;
                    bool ok = false;
                    if (strlen(num) == 0 || vn == 0 || v[0] != ')') {
                    } else if (reset) { n = p->options.codepage; ok = true; disjoint_possible = false; }
                    else {
                        /* Win32/resinator: digits only, no leading zero, 1..65535. */
                        ok = true;
                        uint64_t x = 0;
                        for (const unsigned char *c = (const unsigned char *)num; *c; c++) {
                            if (*c < '0' || *c > '9' || x > 0xffffffffu) { ok = false; break; }
                            x = x * 10 + (uint64_t)(*c - '0');
                        }
                        if (ok && (x == 0 || x > 0xffff || num[0] == '0')) ok = false;
                        n = (int)x;
                    }
                    if (ok && (n == 1252 || n == 65001)) {
                        if (pragmas == 0) disjoint = disjoint_possible;
                        pragmas++; input_cp = n;
                        output_cp = disjoint && pragmas == 1 ? p->options.codepage : n;
                    } else rc_error(p, "unsupported #pragma code_page");
                }
            }
            i = end;
            continue;
        }
        if (ignore_contents) { i++; continue; }
        disjoint_possible = false;
        if (ch == '/' && i+1 < source_len && source[i+1] == '/') {
            while (i < source_len && source[i] != '\n') i++;
            continue;
        }
        if (ch == '/' && i+1 < source_len && source[i+1] == '*') {
            i += 2;
            while (i+1 < source_len && !(source[i] == '*' && source[i+1] == '/')) { if (source[i] == '\n') line++; i++; }
            if (i+1 >= source_len) { rc_error(p, "unterminated comment"); break; }
            i += 2;
            continue;
        }
        size_t start = i;
        int token_line = line;
        bool wide = ch == 'L' && i+1 < source_len && source[i+1] == '"';
        if (wide) { i++; ch = (unsigned char)source[i]; }
        if (ch == '"') {
            i++;
            bool closed = false, collapsing = false;
            int length = 0, column = 0;
            while (i < source_len) {
                unsigned char c = (unsigned char)source[i];
                if (c == '\n') line++;
                if (c == '\\' && i+1 < source_len) {
                    if (source[i+1] == '"') { rc_error(p, "escaping quotes with \\\" is not allowed (use \"\" instead)"); return; }
                    length += 2; column += 2;
                    i += 2;
                    while (input_cp == 65001 && i < source_len && ((unsigned char)source[i] & 0xC0) == 0x80) i++;
                    continue;
                }
                if (c == '"') {
                    if (i+1 < source_len && source[i+1] == '"') { length++; column++; i += 2; continue; }
                    i++; closed = true; break;
                }
                switch (c) {
                case '\r': column = 0; break;
                case '\n':
                    column = 0;
                    if (!collapsing) { length += 2; collapsing = true; }
                    break;
                case '\t': case ' ': case 0x0b: case 0x0c:
                    if (!collapsing) {
                        int width = 1;
                        if (c == '\t') width = 8 - column % 8;
                        length += width; column += width;
                    }
                    break;
                default:
                    /* UTF-8 input counts whole codepoints, so continuation bytes add nothing. */
                    if (!(input_cp == 65001 && (c & 0xC0) == 0x80)) { collapsing = false; length++; column++; }
                }
                i++;
            }
            if (closed && length > p->options.max_string) { rc_error(p, str_fmt("string literal too long (max length is %d characters)", p->options.max_string)); break; }
            if (!closed) { rc_error(p, "unterminated string literal"); break; }
            rc_push_token(p, source + start, i - start, token_line, true, wide, input_cp, output_cp);
            continue;
        }
        if (strchr("{},()+-|&~", ch)) {
            i++;
        } else {
            while (i < source_len && (unsigned char)source[i] > ' ' && !strchr("\"{},()+-|&~", source[i])) i++;
        }
        if (i == start) i++;
        rc_push_token(p, source + start, i - start, token_line, false, false, input_cp, output_cp);
    }
    rc_push_token(p, "", 0, line, false, false, input_cp, output_cp);
}

/* ---- parser primitives ---- */

static RC_Token rc_peek(RC_Parser *p) {
    size_t last = p->tokens_len - 1;
    return p->tokens[p->pos < last ? p->pos : last];
}
static RC_Token rc_take(RC_Parser *p) { RC_Token t = rc_peek(p); if (p->pos < p->tokens_len - 1) p->pos++; return t; }
static char *rc_token_upper(RC_Token t) { return rc_case(t.text, t.len, true, NULL); }
static bool rc_is(RC_Parser *p, const char *s) {
    RC_Token t = rc_peek(p);
    size_t n;
    char *u = rc_case(t.text, t.len, true, &n);
    return rc_text_eq(u, n, s);
}
static bool rc_eat(RC_Parser *p, const char *s) { if (rc_is(p, s)) { rc_take(p); return true; } return false; }
static void rc_expect(RC_Parser *p, const char *s) { if (!rc_eat(p, s)) { RC_Token t = rc_peek(p); rc_error_text(p, str_fmt("expected '%s', found '", s), t.text, t.len, "'"); } }
static void rc_begin(RC_Parser *p) { if (!rc_eat(p, "BEGIN") && !rc_eat(p, "{")) rc_error(p, "expected BEGIN or '{'"); }
static bool rc_end(RC_Parser *p) { return rc_eat(p, "END") || rc_eat(p, "}"); }
static bool rc_peek_empty(RC_Parser *p) { return rc_peek(p).len == 0; }

static int rc_digit(unsigned char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static RC_Number rc_number(RC_Parser *p);

static RC_Number rc_atom(RC_Parser *p) {
    if (rc_eat(p, "(")) { RC_Number n = rc_number(p); rc_expect(p, ")"); return n; }
    bool neg = rc_eat(p, "-");
    rc_eat(p, "+");
    RC_Token t = rc_take(p);
    const char *s = t.text;
    size_t sn = t.len;
    RC_Number n = {0, false};
    if (sn == 0) { rc_error(p, "expected integer expression"); return n; }
    while (sn > 0 && (s[sn-1] == 'l' || s[sn-1] == 'L' || s[sn-1] == 'u' || s[sn-1] == 'U')) {
        n.lng = n.lng || s[sn-1] == 'l' || s[sn-1] == 'L';
        sn--;
    }
    uint32_t base = 10;
    if (sn >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; sn -= 2; }
    if (sn == 0) { rc_error(p, "expected integer expression"); return n; }
    for (size_t i = 0; i < sn;) {
        size_t w;
        uint32_t r = rc_decode((const unsigned char *)s + i, sn - i, &w);
        i += w;
        int d = rc_digit((unsigned char)r);
        if (d < 0 || (uint32_t)d >= base) { rc_error_text(p, "expected integer, found '", t.text, t.len, "'"); return n; }
        n.value = n.value * base + (uint32_t)d;
    }
    if (neg) n.value = 0 - n.value;
    return n;
}

static RC_Number rc_number(RC_Parser *p) {
    /* RC deliberately gives +, -, | and & the same precedence, unlike C. */
    RC_Number n = rc_atom(p);
    while (!rc_failed(p) && (rc_is(p, "+") || rc_is(p, "-") || rc_is(p, "|") || rc_is(p, "&"))) {
        char *op = rc_take(p).text;
        RC_Number r = rc_atom(p);
        n.lng = n.lng || r.lng;
        if (streq(op, "+")) n.value += r.value;
        else if (streq(op, "-")) n.value -= r.value;
        else if (streq(op, "|")) n.value |= r.value;
        else if (streq(op, "&")) n.value &= r.value;
    }
    return n;
}

static const uint32_t RC_CP1252[32] = {0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,0x90,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178};

static Bytes rc_string(RC_Parser *p, RC_Token t, bool wide) {
    Bytes b = {0};
    if (!t.quoted) { rc_error(p, "expected quoted string"); return b; }
    const char *s = t.text + 1;
    size_t sn = t.len - 2;
    if (t.wide) { s++; sn--; }
    /* Decode source bytes before applying RC's numeric escapes. */
    uint32_t *chars = xmalloc((sn + 1) * sizeof *chars);
    size_t count = 0;
    if (t.input_cp == 65001) {
        for (size_t i = 0; i < sn;) { size_t w; chars[count++] = rc_decode((const unsigned char *)s + i, sn - i, &w); i += w; }
    } else {
        for (size_t i = 0; i < sn; i++) {
            unsigned char ch = (unsigned char)s[i];
            uint32_t r = ch;
            if (ch >= 0x80 && ch < 0xa0) r = RC_CP1252[ch - 0x80];
            chars[count++] = r;
        }
    }
    for (size_t i = 0; i < count; i++) {
        uint32_t r = chars[i];
        bool integer_escape = false;
        if (r == '"' && i+1 < count && chars[i+1] == '"') i++;
        if (r == '\\' && i+1 < count) {
            i++; r = chars[i];
            switch (r) {
            case 'a': r = 7; break;
            case 'b': r = 8; break;
            case 'f': r = 12; break;
            case 'n': r = 10; break;
            case 'r': r = 13; break;
            case 't': r = 9; break;
            case 'v': r = 11; break;
            case '\\': case '"': break;
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case 'x': case 'X': {
                int base = 8, digits = 0, value = 0;
                int limit = t.wide ? 7 : 3;
                if (r == 'x' || r == 'X') { base = 16; limit = t.wide ? 4 : 2; }
                else { value = (int)(r - '0'); digits = 1; }
                while (digits < limit && i+1 < count) {
                    int d = rc_digit((unsigned char)chars[i+1]);
                    if (d < 0 || d >= base) break;
                    i++; digits++; value = value * base + d;
                }
                r = (uint32_t)(value & 0xffff);
                integer_escape = true;
                if ((wide || t.output_cp == 65001) && !t.wide) {
                    r = (uint32_t)(value & 255);
                    if (t.output_cp == 65001 && r >= 0x80) r = 0xfffd;
                    else if (r >= 0x80 && r < 0xa0) r = RC_CP1252[r - 0x80];
                }
                break;
            }
            default: /* Narrow RC strings retain unknown escapes; wide strings omit them. */
                if (t.wide) continue;
                if (wide) rc_u16(&b, '\\'); else bytes_push(&b, '\\');
            }
        }
        if (wide) {
            if (r < 0x10000) rc_u16(&b, (uint16_t)r);
            else { uint32_t n = r - 0x10000; rc_u16(&b, (uint16_t)(0xd800 + (n >> 10))); rc_u16(&b, (uint16_t)(0xdc00 + (n & 1023))); }
        } else {
            if (t.output_cp == 65001) { rc_utf8(&b, r); continue; }
            if (integer_escape || r <= 255) bytes_push(&b, (unsigned char)r);
            else {
                bool found = false;
                for (int j = 0; j < 32; j++) if (r == RC_CP1252[j]) { bytes_push(&b, (unsigned char)(j + 0x80)); found = true; break; }
                if (!found) { bytes_push(&b, '?'); if (r >= 0x10000) bytes_push(&b, '?'); }
            }
        }
    }
    free(chars);
    return b;
}

static RC_Name rc_name(RC_Parser *p, RC_Token t) {
    if (t.len > 0 && t.text[0] >= '0' && t.text[0] <= '9') {
        p->pos--;
        RC_Number n = rc_atom(p);
        return rc_ordinal((uint16_t)n.value);
    }
    const char *s = t.text;
    size_t sn = t.len;
    if (t.quoted) { Bytes b = rc_string(p, t, false); s = (const char *)b.data; sn = b.len; }
    RC_Name name = {0};
    name.text = rc_case(s ? s : "", sn, true, &name.len);
    name.named = true;
    return name;
}

static uint16_t rc_language(RC_Parser *p) {
    RC_Number a = rc_number(p); rc_expect(p, ","); RC_Number b = rc_number(p);
    return (uint16_t)((a.value & 1023) | (b.value & 63) << 10);
}

static void rc_flags(RC_Parser *p, RC_Meta *m) {
    for (;;) {
        char *k = rc_token_upper(rc_peek(p));
        if (streq(k, "MOVEABLE")) m->flags |= 0x10;
        else if (streq(k, "FIXED")) m->flags &= (uint16_t)~0x1010u;
        else if (streq(k, "PURE") || streq(k, "SHARED")) m->flags |= 0x20;
        else if (streq(k, "IMPURE") || streq(k, "NONSHARED")) m->flags &= (uint16_t)~0x1020u;
        else if (streq(k, "PRELOAD")) m->flags |= 0x40;
        else if (streq(k, "LOADONCALL")) m->flags &= (uint16_t)~0x40u;
        else if (streq(k, "DISCARDABLE")) m->flags |= 0x1030;
        else return;
        rc_take(p);
    }
}

static void rc_optional(RC_Parser *p, RC_Meta *m) {
    while (!rc_failed(p)) {
        if (rc_eat(p, "LANGUAGE")) m->language = rc_language(p);
        else if (rc_eat(p, "VERSION")) m->version = rc_number(p).value;
        else if (rc_eat(p, "CHARACTERISTICS")) m->characteristics = rc_number(p).value;
        else break;
    }
}

/* Whole-file read; directories and unreadable paths fail. */
static bool rc_read_entire_file(const char *path, Bytes *out) {
    if (!file_exists(path) || is_dir(path)) return false;
    FILE *f = open_file(path, "rb");
    if (!f) return false;
    Bytes b = {0};
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) bytes_append(&b, buf, n);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) return false;
    *out = b;
    return true;
}

static bool rc_file(RC_Parser *p, RC_Token t, Bytes *out) {
    char *filename = str_ncopy(t.text, t.len);
    size_t filename_len = t.len;
    if (t.quoted) { Bytes b = rc_string(p, t, false); filename_len = b.len; bytes_push(&b, 0); filename = (char *)b.data; }
#ifndef _WIN32
    for (size_t k = 0; k < filename_len; k++) if (filename[k] == '\\') filename[k] = '/';
#endif
    StrVec candidates = {0};
    sv_push(&candidates, filename);
    sv_push(&candidates, path_join(path_dir(p->input), filename));
    for (size_t i = 0; i < p->options.includes.len; i++) sv_push(&candidates, path_join(p->options.includes.data[i], filename));
    for (size_t i = 0; i < candidates.len; i++) {
        /* A path with an embedded NUL byte can never be opened. */
        if (memchr(filename, 0, filename_len)) break;
        if (rc_read_entire_file(candidates.data[i], out)) { sv_push(&p->dependencies, candidates.data[i]); return true; }
    }
    rc_error_text(p, "unable to open resource file '", filename, filename_len, "'");
    Bytes none = {0};
    *out = none;
    return false;
}

static Bytes rc_raw(RC_Parser *p) {
    if (!rc_is(p, "BEGIN") && !rc_is(p, "{")) { Bytes f; rc_file(p, rc_take(p), &f); return f; }
    rc_begin(p);
    Bytes b = {0};
    while (!rc_failed(p) && !rc_end(p)) {
        if (rc_peek_empty(p)) { rc_error(p, "unterminated resource data"); break; }
        if (rc_peek(p).quoted) {
            RC_Token t = rc_take(p);
            Bytes s = rc_string(p, t, t.wide);
            bytes_append(&b, s.data, s.len);
        } else {
            RC_Number n = rc_number(p);
            if (n.lng) rc_u32(&b, n.value); else rc_u16(&b, (uint16_t)n.value);
        }
        rc_eat(p, ",");
    }
    return b;
}

static Bytes rc_trim_wide_nul(Bytes data) {
    for (size_t i = 0; i+1 < data.len; i += 2) if (rc_get16(data.data, i) == 0) { data.len = i; return data; }
    return data;
}

typedef struct { Bytes data; bool number, comma; } RC_Version_Value;

static void rc_version_node(RC_Parser *p, Bytes *b, int depth) {
    if (depth > 64) { rc_error(p, "VERSIONINFO nesting exceeds 64"); return; }
    bool block = rc_eat(p, "BLOCK");
    if (!block) rc_expect(p, "VALUE");
    Bytes key = rc_trim_wide_nul(rc_string(p, rc_take(p), true));
    rc_align(b);
    size_t start = b->len;
    rc_u16(b, 0); rc_u16(b, 0); rc_u16(b, 1);
    bytes_append(b, key.data, key.len); rc_u16(b, 0); rc_align(b);
    RC_Version_Value *values = NULL;
    size_t count = 0, cap = 0;
    bool has_number = false;
    /* BLOCK may have values before BEGIN, just like VALUE. */
    rc_eat(p, ",");
    while (!rc_failed(p) && !rc_is(p, "BLOCK") && !rc_is(p, "VALUE") && !rc_is(p, "BEGIN") && !rc_is(p, "{") && !rc_is(p, "END") && !rc_is(p, "}") && !rc_peek_empty(p)) {
        RC_Version_Value v = {{0}, false, false};
        if (rc_peek(p).quoted) v.data = rc_trim_wide_nul(rc_string(p, rc_take(p), true));
        else {
            RC_Number n = rc_number(p);
            Bytes d = {0};
            if (n.lng) rc_u32(&d, n.value); else rc_u16(&d, (uint16_t)n.value);
            v.data = d; v.number = true; has_number = true;
        }
        v.comma = rc_eat(p, ",");
        if (count == cap) { cap = cap ? cap * 2 : 8; values = xrealloc(values, cap * sizeof *values); }
        values[count++] = v;
    }
    size_t value_size = 0;
    for (size_t i = 0; i < count; i++) {
        RC_Version_Value v = values[i];
        bytes_append(b, v.data.data, v.data.len);
        if (has_number || v.number) value_size += v.data.len; else value_size += v.data.len / 2;
        if (!v.number && (v.data.len != 0 || count != 1) && (i == count-1 || v.comma)) {
            rc_u16(b, 0);
            value_size += has_number ? 2 : 1;
        }
    }
    if (has_number) rc_set16(b->data, start+4, 0);
    if (value_size > 0xffff) { rc_error(p, "VERSIONINFO value exceeds 65535 units"); return; }
    rc_set16(b->data, start+2, (uint16_t)value_size);
    if (block) {
        rc_begin(p);
        while (!rc_failed(p) && !rc_end(p)) {
            if (rc_peek_empty(p)) { rc_error(p, "unterminated VERSIONINFO block"); break; }
            rc_version_node(p, b, depth+1);
        }
    }
    if (b->len - start > 0xffff) { rc_error(p, "VERSIONINFO node exceeds 65535 bytes"); return; }
    rc_set16(b->data, start, (uint16_t)(b->len - start));
}

static Bytes rc_version(RC_Parser *p) {
    uint32_t fixed[13] = {0};
    fixed[0] = 0xfeef04bd; fixed[1] = 0x10000;
    while (!rc_failed(p) && !rc_is(p, "BEGIN") && !rc_is(p, "{")) {
        RC_Token key_token = rc_take(p);
        size_t key_len;
        char *key = rc_case(key_token.text, key_token.len, true, &key_len);
        if (streq(key, "FILEVERSION") || streq(key, "PRODUCTVERSION")) {
            uint16_t parts[4] = {0};
            for (int i = 0; i < 4; i++) { parts[i] = (uint16_t)rc_number(p).value; if (i == 3 || !rc_eat(p, ",")) break; }
            int index = streq(key, "PRODUCTVERSION") ? 4 : 2;
            fixed[index] = (uint32_t)parts[0] << 16 | parts[1]; fixed[index+1] = (uint32_t)parts[2] << 16 | parts[3];
        } else {
            int index = -1;
            if (streq(key, "FILEFLAGSMASK")) index = 6;
            else if (streq(key, "FILEFLAGS")) index = 7;
            else if (streq(key, "FILEOS")) index = 8;
            else if (streq(key, "FILETYPE")) index = 9;
            else if (streq(key, "FILESUBTYPE")) index = 10;
            if (index < 0) { rc_error_text(p, "unsupported VERSIONINFO statement '", key, key_len, "'"); break; }
            fixed[index] = rc_number(p).value;
        }
    }
    Bytes b = {0};
    rc_u16(&b, 0); rc_u16(&b, 52); rc_u16(&b, 0); rc_utf16(&b, "VS_VERSION_INFO", true); rc_align(&b);
    for (int i = 0; i < 13; i++) rc_u32(&b, fixed[i]);
    rc_begin(p);
    while (!rc_failed(p) && !rc_end(p)) {
        if (rc_peek_empty(p)) { rc_error(p, "unterminated VERSIONINFO"); break; }
        rc_version_node(p, &b, 0);
    }
    if (b.len > 0xffff) rc_error(p, "VERSIONINFO exceeds 65535 bytes");
    else rc_set16(b.data, 0, (uint16_t)b.len);
    return b;
}

static void rc_icon(RC_Parser *p, RC_Name name, bool cursor) {
    RC_Meta group = p->meta; group.flags = 0x1030;
    RC_Meta image = p->meta; image.flags = 0x1010;
    size_t start_flags = p->pos;
    rc_flags(p, &image);
    size_t end_flags = p->pos;
    p->pos = start_flags; rc_flags(p, &group);
    /* GROUP_ICON/CURSOR interpret PRELOAD/LOADONCALL as SHARED modifiers too. */
    for (size_t k = start_flags; k < end_flags; k++) {
        char *u = rc_token_upper(p->tokens[k]);
        if (streq(u, "PRELOAD")) group.flags &= (uint16_t)~0x20u;
        if (streq(u, "LOADONCALL")) group.flags |= 0x20;
    }
    rc_optional(p, &group);
    Bytes file;
    rc_file(p, rc_take(p), &file);
    if (rc_failed(p)) return;
    const unsigned char *f = file.data;
    size_t flen = file.len;
    if (flen >= 12 && memcmp(f, "RIFF", 4) == 0 && memcmp(f + 8, "ACON", 4) == 0) {
        uint16_t typ = cursor ? 21 : 22;
        image.flags = 0x1010;
        rc_resource(p, rc_ordinal(typ), name, image, f, flen);
        return;
    }
    if (flen < 6 || rc_get16(f, 0) != 0) { rc_error(p, "invalid icon directory header"); return; }
    uint16_t file_type = rc_get16(f, 2);
    uint16_t expected_type = cursor ? 2 : 1;
    if (file_type != expected_type) { rc_error(p, "icon/cursor directory type mismatch"); return; }
    size_t count = rc_get16(f, 4);
    if (count == 0 || count > (flen - 6) / 16) { rc_error(p, "truncated icon directory"); return; }
    Bytes group_data = {0};
    rc_u16(&group_data, 0); rc_u16(&group_data, file_type); rc_u16(&group_data, (uint16_t)count);
    for (size_t i = 0; i < count; i++) {
        size_t at = 6 + i*16;
        uint64_t size = rc_get32(f, at+8), offset = rc_get32(f, at+12);
        if (size < 16 || offset < 6 + count*16 || offset > flen || size > flen - offset) { rc_error(p, "invalid icon image bounds"); return; }
        const unsigned char *data = f + offset;
        size_t dlen = (size_t)size;
        uint16_t width = f[at], height = f[at+1];
        uint16_t planes = rc_get16(f, at+4), bits = rc_get16(f, at+6);
        bool is_png = memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0;
        if (is_png) {
            if (!cursor) {
                planes = 1;
                switch (f[at+2]) { case 2: bits = 1; break; case 8: bits = 3; break; case 16: bits = 4; break; default: break; }
            }
        } else {
            uint32_t header_size = rc_get32(data, 0);
            if (header_size != 40 && header_size != 108 && header_size != 124) { rc_error(p, "unsupported icon bitmap header"); return; }
            if (dlen < header_size) { rc_error(p, "truncated icon bitmap header"); return; }
            if (cursor) { width = (uint16_t)rc_get32(data, 4); height = (uint16_t)rc_get32(data, 8); }
            if (!cursor) { planes = rc_get16(data, 12); bits = rc_get16(data, 14); }
        }
        uint16_t image_type = 3;
        const unsigned char *image_data = data;
        size_t image_len = dlen;
        if (cursor) {
            image_type = 1;
            Bytes b = {0};
            rc_u16(&b, planes); rc_u16(&b, bits); bytes_append(&b, data, dlen);
            image_data = b.data; image_len = b.len;
            if (!is_png) { planes = rc_get16(data, 12); bits = rc_get16(data, 14); }
        }
        if (p->icon_id == 0xffff) { rc_error(p, "icon image ids exhausted"); return; }
        rc_resource(p, rc_ordinal(image_type), rc_ordinal(p->icon_id), image, image_data, image_len);
        if (cursor) {
            rc_u16(&group_data, width); rc_u16(&group_data, height); rc_u16(&group_data, planes); rc_u16(&group_data, bits);
        } else {
            bytes_push(&group_data, (unsigned char)width); bytes_push(&group_data, (unsigned char)height);
            bytes_push(&group_data, f[at+2]); bytes_push(&group_data, f[at+3]);
            rc_u16(&group_data, planes); rc_u16(&group_data, bits);
        }
        rc_u32(&group_data, (uint32_t)image_len); rc_u16(&group_data, p->icon_id);
        p->icon_id++;
    }
    rc_resource(p, rc_ordinal(cursor ? 12 : 14), name, group, group_data.data, group_data.len);
}

static void rc_stringtable(RC_Parser *p) {
    RC_Meta m = p->meta; m.flags = 0x1030;
    rc_flags(p, &m); rc_optional(p, &m); rc_begin(p);
    while (!rc_failed(p) && !rc_end(p)) {
        if (rc_peek_empty(p)) { rc_error(p, "unterminated STRINGTABLE"); break; }
        uint16_t id = (uint16_t)rc_number(p).value;
        rc_eat(p, ",");
        Bytes data = rc_string(p, rc_take(p), true);
        for (size_t i = 0; i+3 < data.len; i += 2) if (rc_get16(data.data, i) == 0 && rc_get16(data.data, i+2) == 0) { data.len = i; break; }
        while (data.len >= 2 && rc_get16(data.data, data.len-2) == 0) data.len -= 2;
        if (data.len / 2 > 65534) { rc_error(p, "string table string exceeds 65534 UTF-16 units"); break; }
        uint16_t block_id = (uint16_t)(id / 16 + 1);
        long index = -1;
        for (size_t i = 0; i < p->blocks_len; i++) {
            if (p->string_blocks[i].id == block_id && p->string_blocks[i].meta.language == m.language) { index = (long)i; break; }
        }
        if (index < 0) {
            if (p->blocks_len == p->blocks_cap) {
                p->blocks_cap = p->blocks_cap ? p->blocks_cap * 2 : 16;
                p->string_blocks = xrealloc(p->string_blocks, p->blocks_cap * sizeof *p->string_blocks);
            }
            index = (long)p->blocks_len++;
            memset(&p->string_blocks[index], 0, sizeof p->string_blocks[index]);
            p->string_blocks[index].id = block_id;
            p->string_blocks[index].meta = m;
        }
        RC_String_Block *block = &p->string_blocks[index];
        if (block->present[id % 16]) { rc_error(p, "duplicate STRINGTABLE id for the same language"); break; }
        block->meta = m; block->strings[id % 16] = data; block->present[id % 16] = true;
        rc_eat(p, ",");
    }
}

static void rc_write_stringtables(RC_Parser *p) {
    for (size_t i = 0; i < p->blocks_len; i++) {
        RC_String_Block first = p->string_blocks[i];
        bool seen = false;
        for (size_t k = 0; k < i; k++) if (p->string_blocks[k].meta.language == first.meta.language) { seen = true; break; }
        if (seen) continue;
        for (size_t k = 0; k < p->blocks_len; k++) {
            RC_String_Block block = p->string_blocks[k];
            if (block.meta.language != first.meta.language) continue;
            Bytes b = {0};
            for (int j = 0; j < 16; j++) {
                Bytes data = block.strings[j];
                size_t units = data.len / 2;
                if (p->options.null_strings && block.present[j]) units++;
                rc_u16(&b, (uint16_t)units); bytes_append(&b, data.data, data.len);
                if (p->options.null_strings && block.present[j]) rc_u16(&b, 0);
            }
            rc_resource(p, rc_ordinal(6), rc_ordinal(block.id), block.meta, b.data, b.len);
        }
    }
}

static bool rc_starts_number(RC_Parser *p) {
    RC_Token t = rc_peek(p);
    const char *s = t.text;
    return t.len > 0 && ((s[0] >= '0' && s[0] <= '9') || rc_text_eq(s, t.len, "-") || rc_text_eq(s, t.len, "+") || rc_text_eq(s, t.len, "("));
}

static void rc_wide_string(RC_Parser *p, Bytes *b) {
    Bytes s = rc_trim_wide_nul(rc_string(p, rc_take(p), true));
    bytes_append(b, s.data, s.len);
    rc_u16(b, 0);
}

static uint16_t rc_menu_options(RC_Parser *p) {
    uint16_t flags = 0;
    while (!rc_failed(p)) {
        while (rc_eat(p, ",")) {}
        char *k = rc_token_upper(rc_peek(p));
        if (streq(k, "CHECKED")) flags |= 8;
        else if (streq(k, "GRAYED")) flags |= 1;
        else if (streq(k, "HELP")) flags |= 0x4000;
        else if (streq(k, "INACTIVE")) flags |= 2;
        else if (streq(k, "MENUBARBREAK")) flags |= 0x20;
        else if (streq(k, "MENUBREAK")) flags |= 0x40;
        else return flags;
        rc_take(p);
    }
    return flags;
}

static void rc_menu_items(RC_Parser *p, Bytes *b, bool extended, int depth) {
    if (depth > 512) { rc_error(p, "MENU nesting exceeds 512"); return; }
    rc_begin(p);
    size_t last = 0;
    int count = 0;
    while (!rc_failed(p) && !rc_end(p)) {
        bool popup = rc_eat(p, "POPUP");
        if (!popup) rc_expect(p, "MENUITEM");
        if (rc_failed(p)) break;
        count++;
        if (!extended && !popup && rc_eat(p, "SEPARATOR")) {
            last = b->len;
            rc_u16(b, 0); rc_u16(b, 0); rc_u16(b, 0);
            while (rc_eat(p, ",")) {}
            continue;
        }
        Bytes text = rc_trim_wide_nul(rc_string(p, rc_take(p), true));
        if (extended) {
            uint32_t fields[4] = {0};
            for (int i = 0; i < (popup ? 4 : 3); i++) {
                if (!rc_eat(p, ",")) break;
                if (rc_starts_number(p)) fields[i] = rc_number(p).value;
            }
            rc_eat(p, ",");
            rc_u32(b, fields[1]); rc_u32(b, fields[2]); rc_u32(b, fields[0]);
            last = b->len;
            rc_u16(b, popup ? 1 : 0); bytes_append(b, text.data, text.len); rc_u16(b, 0); rc_align(b);
            if (popup) { rc_u32(b, fields[3]); rc_menu_items(p, b, true, depth+1); }
        } else {
            uint16_t id = 0;
            if (!popup) {
                while (rc_eat(p, ",")) {}
                id = (uint16_t)rc_number(p).value;
            }
            uint16_t flags = rc_menu_options(p);
            if (popup) flags |= 0x10;
            last = b->len;
            rc_u16(b, flags);
            if (!popup) rc_u16(b, id);
            bytes_append(b, text.data, text.len); rc_u16(b, 0);
            if (popup) rc_menu_items(p, b, false, depth+1);
        }
    }
    if (count == 0) rc_error(p, "empty MENU or POPUP is not allowed");
    if (!rc_failed(p)) rc_set16(b->data, last, (uint16_t)(rc_get16(b->data, last) | 0x80));
}

static void rc_menu(RC_Parser *p, RC_Name name, bool extended) {
    RC_Meta m = p->meta; m.flags = 0x1030; rc_flags(p, &m);
    uint32_t help = 0;
    if (extended && rc_starts_number(p)) help = rc_number(p).value;
    rc_optional(p, &m);
    Bytes b = {0};
    rc_u16(&b, extended ? 1 : 0); rc_u16(&b, extended ? 4 : 0);
    if (extended) rc_u32(&b, help);
    rc_menu_items(p, &b, extended, 1);
    if (!rc_failed(p)) rc_resource(p, rc_ordinal(4), name, m, b.data, b.len);
}

static uint16_t rc_accelerator_key(RC_Parser *p, RC_Token t, uint16_t flags) {
    /* Narrow integer escapes are output-codepage bytes, not Unicode scalars. */
    Bytes data = rc_string(p, t, true);
    uint32_t *chars = xmalloc((data.len / 2 + 1) * sizeof *chars);
    size_t count = 0;
    for (size_t i = 0; i+1 < data.len; i += 2) {
        uint32_t n = rc_get16(data.data, i);
        if (n == 0) break;
        if (n >= 0xd800 && n <= 0xdbff && i+3 < data.len) {
            uint32_t low = rc_get16(data.data, i+2);
            if (low >= 0xdc00 && low <= 0xdfff) { n = 0x10000 + ((n - 0xd800) << 10) + low - 0xdc00; i += 2; }
        }
        chars[count++] = n;
    }
    if (count == 0) { rc_error(p, "empty accelerator key"); return 0; }
    uint32_t first = chars[0];
    if (first == '^') {
        if (count != 2) { rc_error(p, "invalid control accelerator key"); return 0; }
        uint32_t second = chars[1];
        if (second == '^') return '^';
        if (second >= 'a' && second <= 'z') second -= 0x20;
        if (second < 'A' || second > 'Z') { rc_error(p, "control accelerator key must be A-Z"); return 0; }
        return (uint16_t)(second - 0x40);
    }
    if (count > 2 || (first >= 0x10000 && count > 1) || (count == 2 && chars[1] >= 0x10000)) {
        rc_error(p, "accelerator key is too long"); return 0;
    }
    if (first >= 0x10000) return (uint16_t)(0xdc00 + (first & 1023) + ((first - 0x10000) / 1024) * 256);
    if (count == 2) return (uint16_t)((first << 8) + chars[1]);
    if ((flags & 1) != 0 && first >= 'a' && first <= 'z') first -= 0x20;
    return (uint16_t)first;
}

static void rc_accelerators(RC_Parser *p, RC_Name name) {
    RC_Meta m = p->meta; m.flags = 0x30; rc_flags(p, &m); rc_optional(p, &m); rc_begin(p);
    Bytes b = {0};
    while (!rc_failed(p) && !rc_end(p)) {
        bool quoted = rc_peek(p).quoted;
        RC_Token token = {0};
        uint16_t key = 0;
        if (quoted) token = rc_take(p); else key = (uint16_t)rc_number(p).value;
        rc_expect(p, ",");
        uint16_t id = (uint16_t)rc_number(p).value;
        uint16_t flags = 0;
        bool explicit_type = false;
        while (rc_eat(p, ",") && !rc_failed(p)) {
            char *option = rc_token_upper(rc_take(p));
            if (streq(option, "ASCII")) explicit_type = true;
            else if (streq(option, "VIRTKEY")) { flags |= 1; explicit_type = true; }
            else if (streq(option, "NOINVERT")) flags |= 2;
            else if (streq(option, "SHIFT")) flags |= 4;
            else if (streq(option, "CONTROL")) flags |= 8;
            else if (streq(option, "ALT")) flags |= 16;
            else rc_error(p, "invalid accelerator option");
        }
        if (!quoted && !explicit_type) rc_error(p, "numeric accelerator key requires ASCII or VIRTKEY");
        if (quoted) key = rc_accelerator_key(p, token, flags);
        rc_u16(&b, flags); rc_u16(&b, key); rc_u16(&b, id); rc_u16(&b, 0);
    }
    if (b.len > 0) b.data[b.len-8] |= 0x80;
    if (!rc_failed(p)) rc_resource(p, rc_ordinal(9), name, m, b.data, b.len);
}

typedef struct { uint32_t value, mask; } RC_Style_Number;
typedef struct { uint32_t initial; bool used; } RC_Style_Context;

static RC_Style_Number rc_style_expression(RC_Parser *p, RC_Style_Context *c, int depth);

static RC_Style_Number rc_style_atom(RC_Parser *p, RC_Style_Context *c, int depth) {
    RC_Style_Number zero = {0, 0};
    if (depth > 128) { rc_error(p, "STYLE expression nesting exceeds 128"); return zero; }
    if (rc_eat(p, "(")) { RC_Style_Number n = rc_style_expression(p, c, depth+1); rc_expect(p, ")"); return n; }
    bool negative = rc_eat(p, "NOT");
    uint32_t value = rc_atom(p).value;
    RC_Style_Number n;
    if (!c->used) {
        c->used = true;
        n.value = negative ? c->initial & ~value : c->initial | value;
        n.mask = 0xffffffff;
        return n;
    }
    if (negative) { n.value = 0; n.mask = ~value; return n; }
    n.value = value; n.mask = 0xffffffff;
    return n;
}

static RC_Style_Number rc_style_expression(RC_Parser *p, RC_Style_Context *c, int depth) {
    RC_Style_Number n = rc_style_atom(p, c, depth);
    while (!rc_failed(p) && (rc_is(p, "+") || rc_is(p, "-") || rc_is(p, "|") || rc_is(p, "&"))) {
        char *op = rc_take(p).text;
        RC_Style_Number r = rc_style_atom(p, c, depth);
        if (streq(op, "+")) n.value += r.value;
        else if (streq(op, "-")) n.value -= r.value;
        else if (streq(op, "|")) n.value |= r.value;
        else if (streq(op, "&")) n.value &= r.value;
        n.value &= n.mask & r.mask; n.mask = 0xffffffff;
    }
    return n;
}

static uint32_t rc_style(RC_Parser *p, uint32_t initial) {
    RC_Style_Context c = {initial, false};
    return rc_style_expression(p, &c, 0).value;
}

static void rc_control_defaults(const char *kind, uint16_t *cls, uint32_t *style, bool *text) {
    *cls = 0; *style = 0x50000000; *text = true;
    if (streq(kind, "AUTO3STATE")) { *cls = 0x80; *style |= 0x10006; }
    else if (streq(kind, "AUTOCHECKBOX")) { *cls = 0x80; *style |= 0x10003; }
    else if (streq(kind, "AUTORADIOBUTTON")) { *cls = 0x80; *style |= 9; }
    else if (streq(kind, "CHECKBOX")) { *cls = 0x80; *style |= 0x10002; }
    else if (streq(kind, "DEFPUSHBUTTON")) { *cls = 0x80; *style |= 0x10001; }
    else if (streq(kind, "GROUPBOX")) { *cls = 0x80; *style |= 7; }
    else if (streq(kind, "PUSHBOX")) { *cls = 0x80; *style |= 0x1000a; }
    else if (streq(kind, "PUSHBUTTON")) { *cls = 0x80; *style |= 0x10000; }
    else if (streq(kind, "RADIOBUTTON")) { *cls = 0x80; *style |= 4; }
    else if (streq(kind, "STATE3")) { *cls = 0x80; *style |= 0x10005; }
    else if (streq(kind, "USERBUTTON")) { *cls = 0x80; *style |= 0x10008; }
    else if (streq(kind, "COMBOBOX")) { *cls = 0x85; *text = false; }
    else if (streq(kind, "CTEXT")) { *cls = 0x82; *style |= 0x20001; }
    else if (streq(kind, "LTEXT")) { *cls = 0x82; *style |= 0x20000; }
    else if (streq(kind, "RTEXT")) { *cls = 0x82; *style |= 0x20002; }
    else if (streq(kind, "ICON")) { *cls = 0x82; *style |= 3; }
    else if (streq(kind, "EDITTEXT") || streq(kind, "HEDIT") || streq(kind, "IEDIT")) { *cls = 0x81; *style |= 0x810000; *text = false; }
    else if (streq(kind, "LISTBOX")) { *cls = 0x83; *style |= 0x800001; *text = false; }
    else if (streq(kind, "SCROLLBAR")) { *cls = 0x84; *text = false; }
    else if (streq(kind, "CONTROL")) { *cls = 0; }
    else *style = 0;
}

static uint16_t rc_class_ordinal(const char *name, size_t len) {
    size_t n;
    char *u = rc_case(name, len, true, &n);
    if (rc_text_eq(u, n, "BUTTON")) return 0x80;
    if (rc_text_eq(u, n, "EDIT")) return 0x81;
    if (rc_text_eq(u, n, "STATIC")) return 0x82;
    if (rc_text_eq(u, n, "LISTBOX")) return 0x83;
    if (rc_text_eq(u, n, "SCROLLBAR")) return 0x84;
    if (rc_text_eq(u, n, "COMBOBOX")) return 0x85;
    return 0;
}

/* *ok is false only for an unknown unquoted control class (the error case). */
static Bytes rc_dialog_name(RC_Parser *p, bool menu, bool control_class, bool *ok) {
    Bytes b = {0};
    *ok = true;
    if (rc_starts_number(p)) { rc_u16(&b, 0xffff); rc_u16(&b, (uint16_t)rc_number(p).value); return b; }
    RC_Token t = rc_take(p);
    if (control_class) {
        const char *s = t.text;
        size_t sn = t.len;
        if (t.quoted) { Bytes q = rc_string(p, t, false); s = q.data ? (const char *)q.data : ""; sn = q.len; }
        uint16_t ordinal = rc_class_ordinal(s, sn);
        if (ordinal != 0) { rc_u16(&b, 0xffff); rc_u16(&b, ordinal); return b; }
        if (!t.quoted) { rc_error(p, "unknown control class"); *ok = false; Bytes none = {0}; return none; }
    }
    Bytes data;
    if (t.quoted) data = rc_trim_wide_nul(rc_string(p, t, true));
    else {
        const char *name = t.text;
        size_t nn = t.len;
        if (menu) name = rc_case(t.text, t.len, true, &nn);
        Bytes w = {0};
        rc_utf16_n(&w, name, nn, false);
        data = w;
    }
    /* Dialog MENU and CLASS names made entirely of decimal digits are ordinals. */
    if (!control_class) {
        bool numeric = data.len > 0;
        uint16_t value = 0;
        for (size_t i = 0; i+1 < data.len; i += 2) {
            uint16_t ch = rc_get16(data.data, i);
            if (ch < '0' || ch > '9') { numeric = false; break; }
            value = (uint16_t)(value * 10 + ch - '0');
        }
        if (numeric && value != 0) { rc_u16(&b, 0xffff); rc_u16(&b, value); return b; }
    }
    bytes_append(&b, data.data, data.len); rc_u16(&b, 0);
    return b;
}

static void rc_dialog_control(RC_Parser *p, Bytes *b, bool extended) {
    RC_Token kind_token = rc_take(p);
    size_t kind_len;
    char *kind = rc_case(kind_token.text, kind_token.len, true, &kind_len);
    uint16_t cls; uint32_t style; bool has_text;
    rc_control_defaults(kind, &cls, &style, &has_text);
    if (style == 0) { rc_error_text(p, "unknown dialog control '", kind, kind_len, "'"); return; }
    bool control = streq(kind, "CONTROL");
    while (rc_eat(p, ",")) {}
    Bytes text = {0};
    if (has_text) {
        if (rc_peek(p).quoted) rc_wide_string(p, &text);
        else { rc_u16(&text, 0xffff); rc_u16(&text, (uint16_t)rc_atom(p).value); }
        while (rc_eat(p, ",")) {}
    } else rc_u16(&text, 0);
    uint32_t id = rc_number(p).value;
    while (rc_eat(p, ",")) {}
    Bytes class_name = {0};
    if (control) {
        bool ok;
        class_name = rc_dialog_name(p, false, true, &ok);
        while (rc_eat(p, ",")) {}
        style = rc_style(p, style);
        while (rc_eat(p, ",")) {}
    }
    uint16_t coords[4] = {0};
    for (int i = 0; i < 4; i++) { coords[i] = (uint16_t)rc_number(p).value; if (i < 3) rc_eat(p, ","); }
    uint32_t exstyle = 0, help = 0;
    /* Optional trailing fields are positional; omitted values retain defaults. */
    int fields = control ? (extended ? 2 : 1) : (extended ? 3 : 2);
    for (int i = 0; i < fields; i++) {
        if (!rc_eat(p, ",")) break;
        if (!rc_starts_number(p) && !rc_is(p, "NOT")) continue;
        int slot = control ? i + 1 : i;
        switch (slot) {
        case 0: style = rc_style(p, style); break;
        case 1: exstyle = rc_style(p, 0); break;
        case 2: help = rc_number(p).value; break;
        default: break;
        }
    }
    Bytes extra = {0};
    if (extended && (rc_is(p, "BEGIN") || rc_is(p, "{"))) extra = rc_raw(p);
    if (extra.len > 65535) { rc_error(p, "dialog control extra data exceeds 65535 bytes"); return; }
    rc_align(b);
    if (extended) { rc_u32(b, help); rc_u32(b, exstyle); rc_u32(b, style); } else { rc_u32(b, style); rc_u32(b, exstyle); }
    for (int i = 0; i < 4; i++) rc_u16(b, coords[i]);
    if (extended) rc_u32(b, id); else rc_u16(b, (uint16_t)id);
    if (control) bytes_append(b, class_name.data, class_name.len); else { rc_u16(b, 0xffff); rc_u16(b, cls); }
    bytes_append(b, text.data, text.len); rc_u16(b, (uint16_t)extra.len); bytes_append(b, extra.data, extra.len);
}

static void rc_dialog(RC_Parser *p, RC_Name name, bool extended) {
    RC_Meta m = p->meta; m.flags = 0x1030; rc_flags(p, &m);
    uint16_t coords[4] = {0};
    for (int i = 0; i < 4; i++) { coords[i] = (uint16_t)rc_number(p).value; if (i < 3) rc_eat(p, ","); }
    uint32_t help = 0;
    if (rc_eat(p, ",") && rc_starts_number(p)) help = rc_number(p).value;
    uint32_t style = 0x80880000, exstyle = 0;
    Bytes menu = {0}, cls = {0}, caption = {0}, font = {0};
    bool has_menu = false, has_class = false, has_caption = false, has_font = false;
    uint16_t font_weight = 0;
    bool font_italic = false;
    while (!rc_failed(p) && !rc_is(p, "BEGIN") && !rc_is(p, "{")) {
        if (rc_eat(p, "STYLE")) style = rc_style(p, 0);
        else if (rc_eat(p, "EXSTYLE")) exstyle = rc_style(p, 0);
        else if (rc_eat(p, "CAPTION")) { Bytes b = {0}; rc_wide_string(p, &b); caption = b; has_caption = true; }
        else if (rc_eat(p, "MENU")) { bool ok; menu = rc_dialog_name(p, true, false, &ok); has_menu = ok; }
        else if (rc_eat(p, "CLASS")) { bool ok; cls = rc_dialog_name(p, false, false, &ok); has_class = ok; }
        else if (rc_eat(p, "FONT")) {
            uint16_t size = (uint16_t)rc_number(p).value;
            while (rc_eat(p, ",")) {}
            RC_Token typeface = rc_take(p);
            uint8_t charset = 1;
            if (extended) {
                if (rc_eat(p, ",") && rc_starts_number(p)) font_weight = (uint16_t)rc_number(p).value;
                if (rc_eat(p, ",") && rc_starts_number(p)) font_italic = (uint16_t)rc_number(p).value != 0;
                if (rc_eat(p, ",") && rc_starts_number(p)) charset = (uint8_t)rc_number(p).value;
            }
            Bytes b = {0};
            rc_u16(&b, size);
            if (extended) { rc_u16(&b, font_weight); bytes_push(&b, font_italic ? 1 : 0); bytes_push(&b, charset); }
            Bytes face = rc_trim_wide_nul(rc_string(p, typeface, true));
            bytes_append(&b, face.data, face.len); rc_u16(&b, 0);
            font = b; has_font = true;
        } else {
            size_t before = p->pos;
            rc_optional(p, &m);
            if (before == p->pos) rc_error(p, "unknown dialog optional statement");
        }
    }
    if (has_font) style |= 0x40; else style &= ~(uint32_t)0x40;
    if (has_caption) style |= 0xc00000;
    Bytes b = {0};
    if (extended) { rc_u16(&b, 1); rc_u16(&b, 0xffff); rc_u32(&b, help); rc_u32(&b, exstyle); rc_u32(&b, style); }
    else { rc_u32(&b, style); rc_u32(&b, exstyle); }
    size_t count_at = b.len; rc_u16(&b, 0);
    for (int i = 0; i < 4; i++) rc_u16(&b, coords[i]);
    if (has_menu) bytes_append(&b, menu.data, menu.len); else rc_u16(&b, 0);
    if (has_class) bytes_append(&b, cls.data, cls.len); else rc_u16(&b, 0);
    if (has_caption) bytes_append(&b, caption.data, caption.len); else rc_u16(&b, 0);
    bytes_append(&b, font.data, font.len);
    rc_begin(p);
    int count = 0;
    while (!rc_failed(p) && !rc_end(p)) {
        if (count >= 65535) { rc_error(p, "dialog control count exceeds 65535"); break; }
        rc_dialog_control(p, &b, extended); count++;
    }
    rc_set16(b.data, count_at, (uint16_t)count);
    if (!rc_failed(p)) rc_resource(p, rc_ordinal(5), name, m, b.data, b.len);
}

static void rc_font(RC_Parser *p, RC_Name name) {
    if (name.named) { rc_error(p, "FONT resource ID must be an ordinal"); return; }
    RC_Meta m = p->meta; m.flags = 0x1030; rc_flags(p, &m); rc_optional(p, &m);
    Bytes data;
    rc_file(p, rc_take(p), &data);
    if (rc_failed(p)) return;
    /* Resinator skips a duplicate font ID, independent of language. */
    for (size_t i = 0; i < p->fonts_len; i++) if (p->fonts[i].id == name.ordinal) return;
    RC_Font font;
    memset(&font, 0, sizeof font);
    font.id = name.ordinal;
    memcpy(font.header, data.data, data.len < 148 ? data.len : 148);
    if (p->fonts_len == p->fonts_cap) {
        p->fonts_cap = p->fonts_cap ? p->fonts_cap * 2 : 8;
        p->fonts = xrealloc(p->fonts, p->fonts_cap * sizeof *p->fonts);
    }
    p->fonts[p->fonts_len++] = font;
    rc_resource(p, rc_ordinal(8), name, m, data.data, data.len);
}

static void rc_write_fontdir(RC_Parser *p) {
    if (p->fonts_len == 0) return;
    Bytes b = {0};
    rc_u16(&b, (uint16_t)p->fonts_len);
    for (size_t i = 0; i < p->fonts_len; i++) { rc_u16(&b, p->fonts[i].id); bytes_append(&b, p->fonts[i].header, 148); rc_u16(&b, 0); }
    RC_Meta m = p->meta; m.flags = 0x50;
    RC_Name fontdir = {"FONTDIR", 7, 0, true};
    rc_resource(p, rc_ordinal(7), fontdir, m, b.data, b.len);
}

static void rc_toolbar(RC_Parser *p, RC_Name name) {
    RC_Meta m = p->meta; m.flags = 0x30; rc_flags(p, &m);
    uint16_t width = (uint16_t)rc_number(p).value; rc_expect(p, ","); uint16_t height = (uint16_t)rc_number(p).value;
    rc_begin(p);
    Bytes b = {0};
    rc_u16(&b, 1); rc_u16(&b, width); rc_u16(&b, height); rc_u16(&b, 0);
    int count = 0;
    while (!rc_failed(p) && !rc_end(p)) {
        if (count >= 65535) { rc_error(p, "TOOLBAR button count exceeds 65535"); break; }
        if (rc_eat(p, "SEPARATOR")) rc_u16(&b, 0);
        else { rc_expect(p, "BUTTON"); rc_u16(&b, (uint16_t)rc_number(p).value); }
        count++;
    }
    rc_set16(b.data, 6, (uint16_t)count);
    if (!rc_failed(p)) rc_resource(p, rc_ordinal(241), name, m, b.data, b.len);
}

static void rc_compile(RC_Parser *p) {
    RC_Meta zero = {0, 0, 0, 0};
    rc_resource(p, rc_ordinal(0), rc_ordinal(0), zero, NULL, 0);
    while (!rc_peek_empty(p) && !rc_failed(p)) {
        if (rc_eat(p, "LANGUAGE")) { p->meta.language = rc_language(p); continue; }
        if (rc_eat(p, "VERSION")) { p->meta.version = rc_number(p).value; continue; }
        if (rc_eat(p, "CHARACTERISTICS")) { p->meta.characteristics = rc_number(p).value; continue; }
        if (rc_eat(p, "STRINGTABLE")) { rc_stringtable(p); continue; }
        RC_Name name = rc_name(p, rc_take(p));
        /* A dangling literal at end-of-file is only a warning in resinator. */
        if (rc_peek_empty(p)) { fprintf(stderr, "cross_cc rc: warning: dangling literal at end-of-file; this is not a problem, but it is likely a mistake\n"); return; }
        RC_Token t = rc_take(p);
        char *kind = rc_token_upper(t);
        RC_Name typ = rc_ordinal(0);
        /* A numeric type naming a predefined RT_ resource uses that type's grammar (ResourceType.fromRT). */
        if (!t.quoted && t.len > 0 && t.text[0] >= '0' && t.text[0] <= '9') {
            size_t save = p->pos;
            RC_Name alias = rc_name(p, t);
            p->pos = save;
            if (!alias.named) {
                switch (alias.ordinal) {
                case 2: kind = "BITMAP"; break;
                case 4: kind = "MENU"; break;
                case 5: kind = "DIALOG"; break;
                case 6: rc_error(p, "the number for the STRING resource type is invalid as a resource type"); return;
                case 8: kind = "FONT"; break;
                case 9: kind = "ACCELERATORS"; break;
                case 10: kind = "RCDATA"; break;
                case 11: kind = "MESSAGETABLE"; break;
                case 12: kind = "CURSOR"; break;
                case 14: kind = "ICON"; break;
                case 16: kind = "VERSIONINFO"; break;
                case 17: kind = "DLGINCLUDE"; break;
                case 19: kind = "PLUGPLAY"; break;
                case 20: kind = "VXD"; break;
                case 23: kind = "HTML"; break;
                case 240: kind = "DLGINIT"; break;
                case 241: kind = "TOOLBAR"; break;
                default: break;
                }
            }
        }
        if (streq(kind, "RCDATA")) typ.ordinal = 10;
        else if (streq(kind, "HTML")) typ.ordinal = 23;
        else if (streq(kind, "BITMAP")) typ.ordinal = 2;
        else if (streq(kind, "MESSAGETABLE")) typ.ordinal = 11;
        else if (streq(kind, "DLGINIT")) typ.ordinal = 240;
        else if (streq(kind, "DLGINCLUDE")) typ.ordinal = 17;
        else if (streq(kind, "PLUGPLAY")) typ.ordinal = 19;
        else if (streq(kind, "VXD")) typ.ordinal = 20;
        else if (streq(kind, "VERSIONINFO")) typ.ordinal = 16;
        else if (streq(kind, "ICON") || streq(kind, "CURSOR")) { rc_icon(p, name, streq(kind, "CURSOR")); continue; }
        else if (streq(kind, "MENU") || streq(kind, "MENUEX")) { rc_menu(p, name, streq(kind, "MENUEX")); continue; }
        else if (streq(kind, "ACCELERATORS")) { rc_accelerators(p, name); continue; }
        else if (streq(kind, "DIALOG") || streq(kind, "DIALOGEX")) { rc_dialog(p, name, streq(kind, "DIALOGEX")); continue; }
        else if (streq(kind, "FONT")) { rc_font(p, name); continue; }
        else if (streq(kind, "TOOLBAR")) { rc_toolbar(p, name); continue; }
        else typ = rc_name(p, t);
        RC_Meta m = p->meta; m.flags = 0x30;
        if (!typ.named) {
            switch (typ.ordinal) {
            case 1: case 3: case 21: case 22: m.flags = 0x1010; break;
            case 7: m.flags = 0x50; break;
            default: break;
            }
        }
        if (typ.ordinal == 17) m.flags = 0x1030;
        rc_flags(p, &m); rc_optional(p, &m);
        Bytes data = {0};
        if (!streq(kind, "VERSIONINFO") && (rc_is(p, "{") || rc_is(p, "BEGIN"))) {
            const char *display = "";
            if (typ.named || typ.ordinal >= 256) {
                /* user-defined */
            } else {
                switch (typ.ordinal) {
                case 2: display = "bitmap"; break;
                case 11: display = "messagetable"; break;
                case 17: display = "dlginclude"; break;
                case 1: display = "1 (cursor)"; break;
                case 3: display = "3 (icon)"; break;
                case 7: display = "7 (fontdir)"; break;
                case 21: display = "21 (anicursor)"; break;
                case 22: display = "22 (aniicon)"; break;
                default: break;
                }
            }
            if (*display) { { RC_Token t = rc_peek(p); rc_error_text(p, "expected '<filename>', found '", t.text, t.len, str_fmt("' (resource type '%s' can't use raw data)", display)); } return; }
        }
        if (streq(kind, "VERSIONINFO")) data = rc_version(p);
        else if (streq(kind, "DLGINCLUDE")) {
            if (!rc_peek(p).quoted) { { RC_Token t = rc_peek(p); rc_error_text(p, "expected quoted string literal; got '", t.text, t.len, "'"); } return; }
            Bytes sb = rc_string(p, rc_take(p), false);
            Bytes d = {0};
            bytes_append(&d, sb.data, sb.len); bytes_push(&d, 0);
            data = d;
        } else data = rc_raw(p);
        if (typ.ordinal == 2) {
            if (data.len < 14 || data.data[0] != 'B' || data.data[1] != 'M') { rc_error(p, "invalid bitmap file"); return; }
            data.data += 14; data.len -= 14; data.cap = 0;
        }
        if (!rc_failed(p)) rc_resource(p, typ, name, m, data.data, data.len);
    }
    if (!rc_failed(p)) { rc_write_fontdir(p); rc_write_stringtables(p); }
}

/* resinator Arg.looksLikeFilepath: a '/' argument with an .rc/.res/.rcpp extension ends option parsing,
   unless it could be an /fo option and does not exist. */
static bool rc_looks_like_filepath(const char *a) {
    const char *base = a;
    const char *slash = strrchr(a, '/');
    if (slash) base = slash + 1;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) return false;
    char *ext = rc_lower(dot);
    if (!streq(ext, ".rc") && !streq(ext, ".res") && !streq(ext, ".rcpp")) return false;
    bool could_fo = false;
    size_t n = strlen(a);
    for (size_t k = 1; k + 1 < n; k++) {
        if (a[k] == '/') break;
        if ((a[k] == 'f' || a[k] == 'F') && (a[k+1] == 'o' || a[k+1] == 'O')) { could_fo = true; break; }
    }
    if (!could_fo) return true;
    return file_exists(a);
}

static bool rc_msvc_mode_from_env(const char *v, bool *system, char **error) {
    *error = NULL;
    *system = false;
    if (!*v || streq(v, "managed")) return true;
    if (streq(v, "system")) { *system = true; return true; }
    *error = "CROSS_CC_MSVC expects 'managed' or 'system'";
    return false;
}

static bool rc_lang_tag(const char *tag, uint16_t *id);
static bool rc_parse_percent(const char *str, uint32_t *result);

RC_Options rc_parse_options_msvc_env(StrVec args, const char *msvc_env, char **error);

RC_Options rc_parse_options(StrVec args, char **error) {
    return rc_parse_options_msvc_env(args, env("CROSS_CC_MSVC"), error);
}

/* msvc_env is the CROSS_CC_MSVC value; split out so tests need not mutate the process environment. */
RC_Options rc_parse_options_msvc_env(StrVec args, const char *msvc_env, char **error) {
    RC_Options o;
    memset(&o, 0, sizeof o);
    o.input = ""; o.output = ""; o.input_format = ""; o.output_format = ""; o.target = ""; o.depfile = "";
    o.msvc_libc_file = "";
    *error = NULL;
    bool cli_msvc = false;
    o.language = 0x409; o.codepage = 1252; o.max_string = 4097; o.auto_includes = "any";
    bool positional = false;
    for (size_t i = 0; i < args.len; i++) {
        const char *a = args.data[i];
        if (streq(a, "--") && !positional) { positional = true; continue; }
        /* Driver policy flags must precede RC's slash/dash option grammar. */
        if (!positional) {
            if (starts_with(a, "--cross-cc-msvc=")) {
                const char *mode = a + strlen("--cross-cc-msvc=");
                if (!streq(mode, "managed") && !streq(mode, "system")) { *error = str_fmt("invalid Microsoft resource mode: %s (expected managed or system)", mode); return o; }
                o.msvc_system = streq(mode, "system");
                cli_msvc = true;
                continue;
            }
            if (streq(a, "--cross-cc-accept-msvc-license")) { o.msvc_accept_license = true; continue; }
            if (streq(a, "--libc") || starts_with(a, "--libc=")) {
                const char *value = starts_with(a, "--libc=") ? a + strlen("--libc=") : a;
                if (streq(a, "--libc")) { i++; if (i == args.len) { *error = "missing value for '--libc'"; return o; } value = args.data[i]; }
                if (!*value) { *error = "missing value for '--libc'"; return o; }
                o.msvc_libc_file = (char *)value;
                continue;
            }
        }
        if (!positional && starts_with(a, "/") && args.len - i <= 2 && rc_looks_like_filepath(a)) positional = true;
        if (!positional && strlen(a) > 1 && (a[0] == '/' || a[0] == '-')) {
            const char *key = a + 1;
            if (starts_with(key, "-")) key++;
            char *lower = rc_lower(key);
            if (streq(lower, "?") || streq(lower, "h") || streq(lower, "help")) { *error = "help"; return o; }
            if (streq(lower, "nologo") || streq(lower, "r") || streq(lower, "a") || streq(lower, "v") || streq(lower, "y") || streq(lower, "w")) continue;
            if (streq(lower, "x")) { o.ignore_include = true; continue; }
            if (streq(lower, "n")) { o.null_strings = true; continue; }
            if (streq(lower, ":no-preprocess")) { o.no_preprocess = true; continue; }
            if (streq(lower, "p")) { o.preprocess_only = true; continue; }
            static const char *candidates[] = {":output-format", ":input-format", ":auto-includes", ":depfile-fmt", ":depfile", ":target", "fo", "ln", "sl", "d", "u", "i", "l", "c"};
            const char *option = "";
            const char *value = "";
            for (size_t k = 0; k < sizeof candidates / sizeof candidates[0]; k++) {
                if (starts_with(lower, candidates[k])) {
                    option = candidates[k];
                    /* The value is sliced from the original spelling by byte length. */
                    value = strlen(key) >= strlen(option) ? key + strlen(option) : "";
                    break;
                }
            }
            if (!*option) { *error = str_fmt("unknown option '%s'", a); return o; }
            if (value[0] == ':') value++;
            if (!*value) { i++; if (i == args.len) { *error = str_fmt("missing value for '%s'", a); return o; } value = args.data[i]; }
            if (streq(option, "fo")) o.output = (char *)value;
            else if (streq(option, "d")) sv_push(&o.defines, value);
            else if (streq(option, "u")) sv_push(&o.undefines, value);
            else if (streq(option, "i")) sv_push(&o.includes, value);
            else if (streq(option, "l")) {
                /* lang.parseInt: hexadecimal, optional 0x and '-', wraps at 16 bits, stops at the first bad digit. */
                const char *v = value;
                bool neg = false;
                if (v[0] == '-') { neg = true; v++; }
                if (strlen(v) > 2 && v[0] == '0' && v[1] == 'x') v += 2;
                uint16_t n = 0;
                for (size_t k = 0; v[k]; k++) {
                    int d = rc_digit((unsigned char)v[k]);
                    if (d < 0) { if (k == 0) { *error = "invalid language id"; return o; } break; }
                    if (n != 0) n = (uint16_t)(n * 16);
                    n = (uint16_t)(n + d);
                }
                if (neg) n = (uint16_t)(0 - n);
                o.language = n;
            } else if (streq(option, "ln")) {
                uint16_t id;
                if (!rc_lang_tag(value, &id)) { *error = str_fmt("invalid language tag: %s", value); return o; }
                o.language = id;
            } else if (streq(option, "c")) {
                int64_t n;
                bool ok = rc_parse_int(value, &n);
                if (!ok || (n != 1252 && n != 65001)) { *error = "supported code pages are 1252 and 65001"; return o; }
                o.codepage = (int)n;
            } else if (streq(option, "sl")) {
                uint32_t percent;
                if (!rc_parse_percent(value, &percent)) { *error = str_fmt("invalid percent format '%s' (string length percent must be an integer between 1 and 100 (inclusive))", value); return o; }
                if (percent == 0 || percent > 100) { *error = str_fmt("percent out of range: %u (parsed from '%s')", percent, value); return o; }
                volatile float scaled = (float)percent / 100.0f;
                scaled = scaled * 8192.0f;
                o.max_string = (int)scaled;
            } else if (streq(option, ":auto-includes")) {
                if (streq(value, "none")) o.no_auto_includes = true;
                else if (streq(value, "gnu") || streq(value, "any") || streq(value, "msvc")) {}
                else { *error = str_fmt("invalid auto includes setting: %s", value); return o; }
                o.auto_includes = (char *)value;
            } else if (streq(option, ":input-format")) o.input_format = rc_lower(value);
            else if (streq(option, ":output-format")) o.output_format = rc_lower(value);
            else if (streq(option, ":target")) o.target = (char *)value;
            else if (streq(option, ":depfile")) o.depfile = (char *)value;
            else if (streq(option, ":depfile-fmt")) { if (!streq(value, "json")) { *error = "unsupported depfile format"; return o; } }
        } else {
            if (!*o.input) o.input = (char *)a;
            else if (!*o.output) o.output = (char *)a;
            else { *error = "too many input/output paths"; return o; }
        }
    }
    if (!*o.input) { *error = "missing input file"; return o; }
    if (!file_exists(o.input) && !ends_with(rc_lower(o.input), ".rc") && file_exists(str_fmt("%s.rc", o.input))) o.input = str_fmt("%s.rc", o.input);
    if (!*o.input_format) {
        o.input_format = "rc";
        char *ext = rc_lower(rc_path_ext(o.input));
        if (streq(ext, ".rcpp")) o.input_format = "rcpp";
        if (streq(ext, ".res")) o.input_format = "res";
    }
    if (streq(o.input_format, "rcpp")) o.no_preprocess = true;
    if (!*o.output_format) {
        o.output_format = "res";
        char *ext = rc_lower(rc_path_ext(o.output));
        if (streq(ext, ".o") || streq(ext, ".obj") || streq(o.input_format, "res")) o.output_format = "coff";
        if (streq(ext, ".rcpp") || o.preprocess_only) o.output_format = "rcpp";
    }
    if (streq(o.output_format, "rcpp")) o.preprocess_only = true;
    if (!*o.output) {
        const char *extension = streq(o.output_format, "coff") ? "obj" : o.output_format;
        char *ext = rc_path_ext(o.input);
        o.output = str_fmt("%s.%s", str_ncopy(o.input, strlen(o.input) - strlen(ext)), extension);
    }
    if (!streq(o.input_format, "rc") && !streq(o.input_format, "rcpp") && !streq(o.input_format, "res")) { *error = "unsupported input format (rc, rcpp and res are supported)"; return o; }
    if (streq(o.input_format, "res") && !streq(o.output_format, "coff")) { *error = ".res input requires COFF output"; return o; }
    if (!streq(o.output_format, "res") && !streq(o.output_format, "rcpp") && !streq(o.output_format, "coff")) { *error = "unsupported output format (res, rcpp and coff are supported)"; return o; }
    /* A valid --cross-cc-msvc flag overrides a bad variable; unset or empty keeps managed. */
    if (!cli_msvc) {
        bool sys;
        char *msg;
        if (!rc_msvc_mode_from_env(nz(msvc_env), &sys, &msg)) { *error = msg; return o; }
        o.msvc_system = sys;
    }
    return o;
}

/* COFF conversion uses the pinned resinator/cvtres layout: sorted directory
   entries, original-order 8-byte-aligned data, two .rsrc sections and static
   relocation symbols. It does not require a host cvtres or an LLVM C++ ABI. */
typedef struct { const unsigned char *name; size_t name_len; uint16_t ordinal; bool named; } RC_Coff_Key;
typedef struct { RC_Coff_Key typ, name; uint16_t language; const unsigned char *data; size_t data_len; uint32_t data_offset, entry_offset; } RC_Coff_Resource;
typedef struct { const unsigned char *name; size_t name_len; uint32_t offset; } RC_Coff_String;

static bool rc_bytes_equal(const unsigned char *a, size_t an, const unsigned char *b, size_t bn) {
    return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

bool rc_key_equal(RC_Coff_Key a, RC_Coff_Key b) {
    return a.named == b.named && (a.named ? rc_bytes_equal(a.name, a.name_len, b.name, b.name_len) : a.ordinal == b.ordinal);
}

bool rc_key_less(RC_Coff_Key a, RC_Coff_Key b) {
    if (a.named != b.named) return a.named;
    if (!a.named) return a.ordinal < b.ordinal;
    size_t n = a.name_len < b.name_len ? a.name_len : b.name_len;
    for (size_t i = 0; i < n; i += 2) {
        uint16_t x = rc_get16(a.name, i), y = rc_get16(b.name, i);
        if (x != y) return x < y;
    }
    return a.name_len < b.name_len;
}

static bool rc_coff_less(RC_Coff_Resource a, RC_Coff_Resource b) {
    if (!rc_key_equal(a.typ, b.typ)) return rc_key_less(a.typ, b.typ);
    if (!rc_key_equal(a.name, b.name)) return rc_key_less(a.name, b.name);
    return a.language < b.language;
}

static RC_Coff_Key rc_parse_res_key(RC_Parser *p, const unsigned char *data, size_t len, size_t *pos) {
    RC_Coff_Key none = {NULL, 0, 0, false};
    if (*pos + 2 > len) { rc_error(p, "truncated .res resource name"); return none; }
    uint16_t first = rc_get16(data, *pos); *pos += 2;
    if (first == 0xffff) {
        if (*pos + 2 > len) { rc_error(p, "truncated .res ordinal"); return none; }
        RC_Coff_Key k = {NULL, 0, rc_get16(data, *pos), false};
        *pos += 2;
        return k;
    }
    size_t start = *pos - 2;
    while (first != 0) {
        if (*pos + 2 > len) { rc_error(p, "unterminated .res resource name"); return none; }
        first = rc_get16(data, *pos); *pos += 2;
    }
    RC_Coff_Key k = {data + start, *pos - 2 - start, 0, true};
    return k;
}

static void rc_coff_name(Bytes *b, RC_Coff_Key key, const RC_Coff_String *table, size_t count) {
    if (!key.named) { rc_u32(b, key.ordinal); return; }
    for (size_t i = 0; i < count; i++) {
        if (rc_bytes_equal(table[i].name, table[i].name_len, key.name, key.name_len)) { rc_u32(b, table[i].offset | 0x80000000u); return; }
    }
}

static void rc_coff_directory(Bytes *b, size_t names, size_t ids) {
    rc_u32(b, 0); rc_u32(b, 0); rc_u32(b, 0); rc_u16(b, (uint16_t)names); rc_u16(b, (uint16_t)ids);
}

static void rc_coff_section(Bytes *b, const char *name, uint32_t size, uint32_t data, uint32_t relocations, uint16_t count) {
    bytes_append(b, name, strlen(name)); rc_u32(b, 0); rc_u32(b, 0); rc_u32(b, size); rc_u32(b, data);
    rc_u32(b, relocations); rc_u32(b, 0); rc_u16(b, count); rc_u16(b, 0); rc_u32(b, 0xc0000040);
}

static void rc_coff_symbol(Bytes *b, const char *name, uint32_t value, uint16_t section, uint8_t aux) {
    size_t n = strlen(name);
    for (size_t i = 0; i < 8; i++) bytes_push(b, i < n ? (unsigned char)name[i] : 0);
    rc_u32(b, value); rc_u16(b, section); rc_u16(b, 0); bytes_push(b, 3); bytes_push(b, aux);
}

static void rc_coff_aux(Bytes *b, uint32_t size, uint16_t count) {
    rc_u32(b, size); rc_u16(b, count);
    for (int i = 0; i < 12; i++) bytes_push(b, 0);
}

static void rc_coff_target(const char *target, uint16_t *machine, uint16_t *relocation) {
    char *arch = rc_lower(nz(target));
    char *dash = strchr(arch, '-');
    if (dash) *dash = 0;
    *machine = 0; *relocation = 0;
    if (streq(arch, "") || streq(arch, "x64") || streq(arch, "amd64") || streq(arch, "x86_64")) { *machine = 0x8664; *relocation = 3; }
    else if (streq(arch, "x86") || streq(arch, "i386")) { *machine = 0x14c; *relocation = 7; }
    else if (streq(arch, "arm") || streq(arch, "armnt")) { *machine = 0x1c4; *relocation = 2; }
    else if (streq(arch, "arm64") || streq(arch, "aarch64")) { *machine = 0xaa64; *relocation = 2; }
    else if (streq(arch, "arm64ec")) { *machine = 0xa641; *relocation = 2; }
    else if (streq(arch, "arm64x")) { *machine = 0xa64e; *relocation = 2; }
    else if (streq(arch, "ia64")) { *machine = 0x200; *relocation = 0x10; }
    else if (streq(arch, "ebc")) { *machine = 0xebc; *relocation = 1; }
}

typedef struct { size_t *data; size_t len, cap; } RC_Index_Vec;
static void rc_index_push(RC_Index_Vec *v, size_t x) {
    if (v->len == v->cap) { v->cap = v->cap ? v->cap * 2 : 16; v->data = xrealloc(v->data, v->cap * sizeof *v->data); }
    v->data[v->len++] = x;
}

/* Returns false (with p->error set) on failure. */
static bool rc_convert_coff(RC_Parser *p, const unsigned char *data, size_t len, Bytes *out) {
    uint16_t machine, relocation;
    rc_coff_target(p->options.target, &machine, &relocation);
    if (machine == 0) { rc_error(p, "unsupported COFF target"); return false; }
    RC_Coff_Resource *resources = NULL;
    size_t nres = 0, res_cap = 0;
    RC_Coff_String *string_table = NULL;
    size_t nstr = 0, str_cap = 0;
    uint32_t string_bytes = 0, data_bytes = 0;
    size_t at = 0;
    while (at < len && !rc_failed(p)) {
        if (at + 8 > len) { rc_error(p, "truncated .res header"); break; }
        size_t size = rc_get32(data, at), header = rc_get32(data, at+4);
        if (header < 8 || header > len - at || size > len - at - header) { rc_error(p, "invalid .res resource size"); break; }
        size_t pos = 8;
        const unsigned char *h = data + at;
        RC_Coff_Key typ = rc_parse_res_key(p, h, header, &pos);
        RC_Coff_Key name = rc_parse_res_key(p, h, header, &pos);
        pos = (pos + 3) & ~(size_t)3;
        if (rc_failed(p)) break;
        if (pos + 16 != header) { rc_error(p, "invalid .res header size"); break; }
        uint16_t language = rc_get16(h, pos+6);
        if (at == 0) {
            if (size != 0 || typ.named || typ.ordinal != 0 || name.named || name.ordinal != 0) { rc_error(p, "invalid 32-bit .res preface"); break; }
            for (size_t k = pos; k < header; k++) if (h[k] != 0) { rc_error(p, "invalid 32-bit .res preface"); break; }
        } else if (size != 0 && (typ.named || typ.ordinal != 17)) {
            if (nres >= 65535) { rc_error(p, "COFF relocation count exceeds 65535"); break; }
            for (size_t k = 0; k < nres; k++) {
                if (rc_key_equal(typ, resources[k].typ) && rc_key_equal(name, resources[k].name) && language == resources[k].language) { rc_error(p, "duplicate type/name/language resource"); break; }
            }
            RC_Coff_Resource r = {typ, name, language, data + at + header, size, data_bytes, 0};
            if ((uint64_t)data_bytes + (uint64_t)size + 7 > 0xffffffffu) { rc_error(p, "COFF resource data exceeds 4 GiB"); break; }
            data_bytes += (uint32_t)((size + 7) & ~(size_t)7);
            if (nres == res_cap) { res_cap = res_cap ? res_cap * 2 : 16; resources = xrealloc(resources, res_cap * sizeof *resources); }
            resources[nres++] = r;
            RC_Coff_Key keys[2] = {typ, name};
            for (int k = 0; k < 2; k++) {
                RC_Coff_Key key = keys[k];
                if (!key.named) continue;
                bool found = false;
                for (size_t e = 0; e < nstr; e++) if (rc_bytes_equal(string_table[e].name, string_table[e].name_len, key.name, key.name_len)) { found = true; break; }
                if (!found) {
                    if (nstr == str_cap) { str_cap = str_cap ? str_cap * 2 : 16; string_table = xrealloc(string_table, str_cap * sizeof *string_table); }
                    RC_Coff_String s = {key.name, key.name_len, string_bytes};
                    string_table[nstr++] = s;
                    string_bytes += (uint32_t)(2 + key.name_len);
                }
            }
        }
        at += header + ((size + 3) & ~(size_t)3);
        if (at > len) rc_error(p, "truncated .res data padding");
    }
    if (len == 0) rc_error(p, "missing 32-bit .res preface");
    if (rc_failed(p)) return false;
    size_t *order = xmalloc((nres + 1) * sizeof *order);
    for (size_t i = 0; i < nres; i++) order[i] = i;
    /* Stable insertion ordering also keeps the implementation independent of
       platform sort APIs and preserves ordinal/name comparison in UTF-16 units. */
    for (size_t i = 1; i < nres; i++) {
        size_t j = i;
        while (j > 0 && rc_coff_less(resources[order[j]], resources[order[j-1]])) { size_t t = order[j]; order[j] = order[j-1]; order[j-1] = t; j--; }
    }
    RC_Index_Vec types = {0}, names = {0};
    for (size_t i = 0; i < nres; i++) {
        size_t index = order[i];
        bool new_type = i == 0 || !rc_key_equal(resources[index].typ, resources[order[i-1]].typ);
        if (new_type) rc_index_push(&types, i);
        if (new_type || !rc_key_equal(resources[index].name, resources[order[i-1]].name)) rc_index_push(&names, i);
    }
    uint32_t *type_offsets = xmalloc((types.len + 1) * sizeof *type_offsets);
    uint32_t *name_offsets = xmalloc((names.len + 1) * sizeof *name_offsets);
    uint32_t level2 = (uint32_t)(16 + 8*types.len);
    uint32_t level3 = level2 + (uint32_t)(16*types.len + 8*names.len);
    uint32_t entries_start = level3 + (uint32_t)(16*names.len + 8*nres);
    uint32_t strings_start = entries_start + (uint32_t)(16*nres);
    uint32_t section1 = (strings_start + string_bytes + 3) & ~(uint32_t)3;
    for (size_t e = 0; e < nstr; e++) string_table[e].offset += strings_start;
    for (size_t i = 0; i < types.len; i++) {
        size_t start = types.data[i];
        type_offsets[i] = level2;
        size_t count = 0;
        for (size_t k = 0; k < names.len; k++) if (rc_key_equal(resources[order[names.data[k]]].typ, resources[order[start]].typ)) count++;
        level2 += (uint32_t)(16 + 8*count);
    }
    for (size_t i = 0; i < names.len; i++) {
        size_t start = names.data[i], end = nres;
        if (i+1 < names.len) end = names.data[i+1];
        name_offsets[i] = level3;
        level3 += (uint32_t)(16 + 8*(end - start));
    }
    Bytes b = {0};
    uint64_t symbol_ptr = 100 + (uint64_t)section1 + (uint64_t)(nres*10) + (uint64_t)data_bytes;
    if (symbol_ptr > 0xffffffffu) { rc_error(p, "COFF file exceeds 4 GiB"); return false; }
    rc_u16(&b, machine); rc_u16(&b, 2); rc_u32(&b, 0); rc_u32(&b, (uint32_t)symbol_ptr); rc_u32(&b, (uint32_t)(5 + nres)); rc_u16(&b, 0); rc_u16(&b, 0x100);
    uint32_t reloc_ptr = nres > 0 ? 100 + section1 : 0;
    rc_coff_section(&b, ".rsrc$01", section1, 100, reloc_ptr, (uint16_t)nres);
    rc_coff_section(&b, ".rsrc$02", data_bytes, 100 + section1 + (uint32_t)(nres*10), 0, 0);
    size_t named = 0;
    for (size_t i = 0; i < types.len; i++) if (resources[order[types.data[i]]].typ.named) named++;
    rc_coff_directory(&b, named, types.len - named);
    for (size_t i = 0; i < types.len; i++) { rc_coff_name(&b, resources[order[types.data[i]]].typ, string_table, nstr); rc_u32(&b, type_offsets[i] | 0x80000000u); }
    for (size_t t = 0; t < types.len; t++) {
        RC_Coff_Key typ = resources[order[types.data[t]]].typ;
        size_t tnamed = 0, count = 0;
        for (size_t k = 0; k < names.len; k++) {
            RC_Coff_Resource r = resources[order[names.data[k]]];
            if (rc_key_equal(r.typ, typ)) { count++; if (r.name.named) tnamed++; }
        }
        rc_coff_directory(&b, tnamed, count - tnamed);
        for (size_t k = 0; k < names.len; k++) {
            RC_Coff_Resource r = resources[order[names.data[k]]];
            if (rc_key_equal(r.typ, typ)) { rc_coff_name(&b, r.name, string_table, nstr); rc_u32(&b, name_offsets[k] | 0x80000000u); }
        }
    }
    for (size_t i = 0; i < names.len; i++) {
        size_t start = names.data[i], end = nres;
        if (i+1 < names.len) end = names.data[i+1];
        rc_coff_directory(&b, 0, end - start);
        for (size_t j = start; j < end; j++) {
            RC_Coff_Resource *r = &resources[order[j]];
            r->entry_offset = entries_start + (uint32_t)(j*16);
            rc_u32(&b, r->language); rc_u32(&b, r->entry_offset);
        }
    }
    for (size_t i = 0; i < nres; i++) { RC_Coff_Resource r = resources[order[i]]; rc_u32(&b, 0); rc_u32(&b, (uint32_t)r.data_len); rc_u32(&b, 0); rc_u32(&b, 0); }
    for (size_t e = 0; e < nstr; e++) {
        if (string_table[e].name_len / 2 > 65535) { rc_error(p, "COFF resource name too long"); return false; }
        rc_u16(&b, (uint16_t)(string_table[e].name_len / 2)); bytes_append(&b, string_table[e].name, string_table[e].name_len);
    }
    rc_align(&b);
    for (size_t i = 0; i < nres; i++) { rc_u32(&b, resources[i].entry_offset); rc_u32(&b, (uint32_t)(5 + i)); rc_u16(&b, relocation); }
    for (size_t i = 0; i < nres; i++) {
        bytes_append(&b, resources[i].data, resources[i].data_len);
        while ((b.len - 100 - section1 - nres*10) % 8 != 0) bytes_push(&b, 0);
    }
    rc_coff_symbol(&b, "@feat.00", 0x11, 0xffff, 0);
    rc_coff_symbol(&b, ".rsrc$01", 0, 1, 1); rc_coff_aux(&b, section1, (uint16_t)nres);
    rc_coff_symbol(&b, ".rsrc$02", 0, 2, 1); rc_coff_aux(&b, data_bytes, 0);
    Bytes long_names = {0};
    for (size_t i = 0; i < nres; i++) {
        char *name = str_fmt("$R%06X", (unsigned)resources[i].data_offset);
        if (strlen(name) <= 8) rc_coff_symbol(&b, name, resources[i].data_offset, 2, 0);
        else {
            rc_u32(&b, 0); rc_u32(&b, (uint32_t)(4 + long_names.len)); rc_u32(&b, resources[i].data_offset); rc_u16(&b, 2); rc_u16(&b, 0); bytes_push(&b, 3); bytes_push(&b, 0);
            bytes_append(&long_names, name, strlen(name)); bytes_push(&long_names, 0);
        }
    }
    rc_u32(&b, (uint32_t)(4 + long_names.len)); bytes_append(&b, long_names.data, long_names.len);
    *out = b;
    return true;
}

static bool rc_write_bytes(const char *path, const unsigned char *data, size_t len) {
    return write_file(path, (const char *)(data ? data : (const unsigned char *)""), len);
}

int rc_compile_source(RC_Options o, const char *source, size_t source_len) {
    RC_Parser p;
    memset(&p, 0, sizeof p);
    p.input = o.input; p.options = o; p.meta.language = o.language; p.icon_id = 1;
    sv_push(&p.dependencies, o.input);
    rc_lex(&p, source, source_len);
    if (!rc_failed(&p)) rc_compile(&p);
    if (rc_failed(&p)) { rc_print_error(&p); return 1; }
    Bytes output = p.output;
    if (streq(o.output_format, "coff")) {
        Bytes converted = {0};
        if (rc_convert_coff(&p, p.output.data, p.output.len, &converted)) output = converted;
    }
    if (rc_failed(&p)) { rc_print_error(&p); return 1; }
    if (!rc_write_bytes(o.output, output.data, output.len)) { fprintf(stderr, "cross_cc rc: cannot write output: %s\n", strerror(errno)); return 1; }
    if (*nz(o.depfile)) {
        Bytes b = {0};
        bytes_push(&b, '[');
        for (size_t i = 0; i < p.dependencies.len; i++) {
            if (i > 0) bytes_push(&b, ',');
            bytes_push(&b, '"');
            for (const char *c = p.dependencies.data[i]; *c; c++) { if (*c == '\\' || *c == '"') bytes_push(&b, '\\'); bytes_push(&b, (unsigned char)*c); }
            bytes_push(&b, '"');
        }
        bytes_push(&b, ']'); bytes_push(&b, '\n');
        if (!rc_write_bytes(o.depfile, b.data, b.len)) { fprintf(stderr, "cross_cc rc: cannot write depfile: %s\n", strerror(errno)); return 1; }
    }
    return 0;
}

static int rc_preprocess_and_compile(RC_Options o, const char *temporary) {
    StrVec argv = {0};
    const char *fixed[] = {"cross_cc", "-cc1", "-E", "-triple", "x86_64-pc-windows-msvc", "-fms-extensions", "-fms-compatibility", "-fgnuc-version=4.2.1", "-x", "c", "-DRC_INVOKED", "-D_WIN32"};
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) sv_push(&argv, fixed[i]);
    for (size_t i = 0; i < o.includes.len; i++) { sv_push(&argv, "-I"); sv_push(&argv, o.includes.data[i]); }
    if (!o.no_auto_includes) {
        char *root = project_root();
        char *lib = first_existing(env("CROSS_CC_LIB_DIR"), env("ZIG_LIB_DIR"), path_join(root, "lib"));
        const char *mode = o.auto_includes;
        uint16_t machine, relocation;
        rc_coff_target(o.target, &machine, &relocation);
        if (machine == 0) {
            fprintf(stderr, "cross_cc rc: error: automatic include path detection is not supported for target '%s'\nnote: to disable auto includes, use the option /:auto-includes none\n", o.target);
            return 1;
        }
        bool found = false;
        const char *libc_file = o.msvc_libc_file;
        if (!*nz(libc_file)) libc_file = env("ZIG_LIBC");
        /* Ordinary RC is not an MSVC-target cc invocation: "any" uses the
           same bundled GNU headers on every host, without SDK acquisition. */
        if (streq(mode, "msvc") || (streq(mode, "any") && (*nz(libc_file) || o.msvc_system))) {
            const char *arch = "x86_64";
            switch (machine) { case 0x14c: arch = "x86"; break; case 0x1c4: arch = "thumb"; break; case 0xaa64: case 0xa641: arch = "aarch64"; break; default: break; }
            Target target;
            memset(&target, 0, sizeof target);
            target.arch = (char *)arch; target.os = "windows"; target.abi = "msvc";
            Msvc_Libc libc;
            memset(&libc, 0, sizeof libc);
            char *detect_err = NULL;
            if (*nz(libc_file)) libc = msvc_libc_from_file(libc_file, &detect_err);
            else if (o.msvc_system) {
                /* Installed resources are considered only by explicit opt-out. */
                libc = msvc_libc_detect(target, &detect_err);
            } else {
                Options policy;
                memset(&policy, 0, sizeof policy);
                policy.msvc_accept_license = o.msvc_accept_license;
                Toolchain tc = discover_toolchain(policy, target);
                libc = managed_msvc(policy, target, tc, &detect_err);
            }
            if (!detect_err || !*detect_err) {
                StrVec dirs = msvc_include_dirs(libc);
                for (size_t i = 0; i < dirs.len; i++) { sv_push(&argv, "-isystem"); sv_push(&argv, dirs.data[i]); }
                found = true;
            } else if (streq(mode, "msvc") || *nz(libc_file)) {
                fprintf(stderr, "cross_cc rc: error: %s\nnote: to disable auto includes, use the option /:auto-includes none\n", detect_err);
                return 1;
            }
        }
        if (*nz(lib) && !streq(mode, "msvc") && !found) { sv_push(&argv, "-isystem"); sv_push(&argv, path_join(lib, "libc", "include", "any-windows-any")); }
    }
    if (!o.ignore_include) {
        const char *include = nz(env("INCLUDE"));
        const char *sep = PATH_LIST_SEPARATOR;
        size_t sep_len = strlen(sep);
        for (;;) {
            const char *next = sep_len ? strstr(include, sep) : NULL;
            size_t n = next ? (size_t)(next - include) : strlen(include);
            if (n > 0) { sv_push(&argv, "-isystem"); sv_push(&argv, str_ncopy(include, n)); }
            if (!next) break;
            include = next + sep_len;
        }
    }
    for (size_t i = 0; i < o.defines.len; i++) sv_push(&argv, str_fmt("-D%s", o.defines.data[i]));
    for (size_t i = 0; i < o.undefines.len; i++) sv_push(&argv, str_fmt("-U%s", o.undefines.data[i]));
    sv_push(&argv, "-o"); sv_push(&argv, temporary); sv_push(&argv, o.input);
    char **cargs = xmalloc((argv.len + 1) * sizeof *cargs);
    for (size_t i = 0; i < argv.len; i++) cargs[i] = str_copy(argv.data[i]);
    cargs[argv.len] = NULL;
    if (BridgeClang_main((int)argv.len, cargs) != 0) return 1;
    Bytes data;
    if (!rc_read_entire_file(temporary, &data)) { fprintf(stderr, "cross_cc rc: cannot read preprocessor output: %s\n", strerror(errno)); return 1; }
    if (o.preprocess_only) {
        if (!rc_write_bytes(o.output, data.data, data.len)) { fprintf(stderr, "cross_cc rc: cannot write output: %s\n", strerror(errno)); return 1; }
        return 0;
    }
    return rc_compile_source(o, data.data ? (const char *)data.data : "", data.len);
}

static int rc_create_exclusive(const char *path) {
#ifdef _WIN32
    int fd = _wopen(windows_wide(path), _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd >= 0) _close(fd);
#else
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd >= 0) close(fd);
#endif
    return fd;
}

int run_rc(StrVec args) {
    char *err = NULL;
    RC_Options o = rc_parse_options(args, &err);
    if (err && streq(err, "help")) {
        puts("Usage: cross_cc rc [options] [--] <INPUT> [<OUTPUT>]");
        puts("  /fo FILE  /d NAME[=VALUE]  /u NAME  /i DIR  /l LANGID  /c CODEPAGE");
        puts("  /p  /:no-preprocess  /:auto-includes any|gnu|msvc|none (default: bundled GNU)");
        puts("  /:input-format rc|rcpp|res  /:output-format res|rcpp|coff");
        puts("  /:target x86|x64|arm|arm64 (or triple)  --libc FILE (or ZIG_LIBC)");
        puts("  --cross-cc-msvc=managed|system  --cross-cc-accept-msvc-license");
        puts("  Explicit msvc uses managed pinned headers; only system discovers installed SDKs.");
        puts("Supported resources: file resources, RCDATA, BITMAP, HTML, manifests,");
        puts("  ICON, CURSOR, VERSIONINFO, STRINGTABLE, MENU, MENUEX, ACCELERATORS,");
        puts("  DIALOG, DIALOGEX, FONT and TOOLBAR. Other grammars are errors.");
        return 0;
    }
    if (err && *err) { fprintf(stderr, "cross_cc rc: error: %s\n", err); return 1; }
    if (streq(o.input_format, "res")) {
        Bytes data;
        if (!rc_read_entire_file(o.input, &data)) { fprintf(stderr, "cross_cc rc: cannot read input: %s\n", strerror(errno)); return 1; }
        RC_Parser p;
        memset(&p, 0, sizeof p);
        p.input = o.input; p.options = o;
        Bytes output = {0};
        rc_convert_coff(&p, data.data, data.len, &output);
        if (rc_failed(&p)) { rc_print_error(&p); return 1; }
        if (!rc_write_bytes(o.output, output.data, output.len)) { fprintf(stderr, "cross_cc rc: cannot write output: %s\n", strerror(errno)); return 1; }
        return 0;
    }
    if (!o.no_preprocess) {
        /* Use cc1 directly: no Clang driver jobs or external compiler processes.
           Exclusively create our own scratch file; never overwrite another run. */
        char *temporary = path_join(first_existing(env("TMPDIR"), env("TEMP"), path_dir(o.output)), str_fmt("cross-rc-%d.rcpp", (int)process_id()));
        if (rc_create_exclusive(temporary) < 0) { fprintf(stderr, "cross_cc rc: cannot create preprocessor output: %s\n", strerror(errno)); return 1; }
        int status = rc_preprocess_and_compile(o, temporary);
        remove_file(temporary);
        return status;
    }
    Bytes data;
    if (!rc_read_entire_file(o.input, &data)) { fprintf(stderr, "cross_cc rc: cannot read input: %s\n", strerror(errno)); return 1; }
    if (o.preprocess_only) { fprintf(stderr, "cross_cc rc: error: /p conflicts with /:no-preprocess\n"); return 1; }
    return rc_compile_source(o, data.data ? (const char *)data.data : "", data.len);
}

/* Language tags follow resinator's lang.zig (MIT, see Zig's LICENSE): the table is MS-LCID
   Appendix A with tags normalised to lowercase and '_' separators. */
static bool rc_parse_percent(const char *str, uint32_t *result) {
    *result = 0;
    if (!*str) return false;
    const char *b = str;
    bool minus = b[0] == '-';
    if (minus) b++;
    for (size_t i = 0; b[i]; i++) {
        unsigned char c = (unsigned char)b[i];
        if (c < '0' || c > '9') { if (i == 0) return false; break; }
        if (*result != 0) *result *= 10;
        *result += (uint32_t)(c - '0');
    }
    if (minus) *result = 0 - *result;
    return true;
}

static bool rc_alpha(const char *s) { for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))) return false; return true; }
static bool rc_alnum(const char *s) { for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9'))) return false; return true; }
static bool rc_numeric(const char *s) { for (; *s; s++) if (*s < '0' || *s > '9') return false; return true; }
static bool rc_ascii_fold_eq(const char *a, const char *b) {
    size_t n = strlen(a);
    if (n != strlen(b)) return false;
    for (size_t i = 0; i < n; i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

typedef struct { const char *tag; uint16_t id; } RC_Lang_Entry;

static const RC_Lang_Entry RC_LANG_TABLE[] = {
    {"af", 0x0036},
    {"af_za", 0x0436},
    {"sq", 0x001C},
    {"sq_al", 0x041C},
    {"gsw", 0x0084},
    {"gsw_fr", 0x0484},
    {"am", 0x005E},
    {"am_et", 0x045E},
    {"ar", 0x0001},
    {"ar_dz", 0x1401},
    {"ar_bh", 0x3C01},
    {"ar_eg", 0x0c01},
    {"ar_iq", 0x0801},
    {"ar_jo", 0x2C01},
    {"ar_kw", 0x3401},
    {"ar_lb", 0x3001},
    {"ar_ly", 0x1001},
    {"ar_ma", 0x1801},
    {"ar_om", 0x2001},
    {"ar_qa", 0x4001},
    {"ar_sa", 0x0401},
    {"ar_sy", 0x2801},
    {"ar_tn", 0x1C01},
    {"ar_ae", 0x3801},
    {"ar_ye", 0x2401},
    {"hy", 0x002B},
    {"hy_am", 0x042B},
    {"as", 0x004D},
    {"as_in", 0x044D},
    {"az_cyrl", 0x742C},
    {"az_cyrl_az", 0x082C},
    {"az", 0x002C},
    {"az_latn", 0x782C},
    {"az_latn_az", 0x042C},
    {"bn", 0x0045},
    {"bn_bd", 0x0845},
    {"bn_in", 0x0445},
    {"ba", 0x006D},
    {"ba_ru", 0x046D},
    {"eu", 0x002D},
    {"eu_es", 0x042D},
    {"be", 0x0023},
    {"be_by", 0x0423},
    {"bs_cyrl", 0x641A},
    {"bs_cyrl_ba", 0x201A},
    {"bs_latn", 0x681A},
    {"bs", 0x781A},
    {"bs_latn_ba", 0x141A},
    {"br", 0x007E},
    {"br_fr", 0x047E},
    {"bg", 0x0002},
    {"bg_bg", 0x0402},
    {"my", 0x0055},
    {"my_mm", 0x0455},
    {"ca", 0x0003},
    {"ca_es", 0x0403},
    {"tzm_arab_ma", 0x045F},
    {"ku", 0x0092},
    {"ku_arab", 0x7c92},
    {"ku_arab_iq", 0x0492},
    {"chr", 0x005C},
    {"chr_cher", 0x7c5C},
    {"chr_cher_us", 0x045C},
    {"zh_hans", 0x0004},
    {"zh", 0x7804},
    {"zh_cn", 0x0804},
    {"zh_sg", 0x1004},
    {"zh_hant", 0x7C04},
    {"zh_hk", 0x0C04},
    {"zh_mo", 0x1404},
    {"zh_tw", 0x0404},
    {"co", 0x0083},
    {"co_fr", 0x0483},
    {"hr", 0x001A},
    {"hr_hr", 0x041A},
    {"hr_ba", 0x101A},
    {"cs", 0x0005},
    {"cs_cz", 0x0405},
    {"da", 0x0006},
    {"da_dk", 0x0406},
    {"prs", 0x008C},
    {"prs_af", 0x048C},
    {"dv", 0x0065},
    {"dv_mv", 0x0465},
    {"nl", 0x0013},
    {"nl_be", 0x0813},
    {"nl_nl", 0x0413},
    {"dz_bt", 0x0C51},
    {"en", 0x0009},
    {"en_au", 0x0C09},
    {"en_bz", 0x2809},
    {"en_ca", 0x1009},
    {"en_029", 0x2409},
    {"en_hk", 0x3C09},
    {"en_in", 0x4009},
    {"en_ie", 0x1809},
    {"en_jm", 0x2009},
    {"en_my", 0x4409},
    {"en_nz", 0x1409},
    {"en_ph", 0x3409},
    {"en_sg", 0x4809},
    {"en_za", 0x1C09},
    {"en_tt", 0x2c09},
    {"en_ae", 0x4C09},
    {"en_gb", 0x0809},
    {"en_us", 0x0409},
    {"en_zw", 0x3009},
    {"et", 0x0025},
    {"et_ee", 0x0425},
    {"fo", 0x0038},
    {"fo_fo", 0x0438},
    {"fil", 0x0064},
    {"fil_ph", 0x0464},
    {"fi", 0x000B},
    {"fi_fi", 0x040B},
    {"fr", 0x000C},
    {"fr_be", 0x080C},
    {"fr_cm", 0x2c0C},
    {"fr_ca", 0x0c0C},
    {"fr_029", 0x1C0C},
    {"fr_cd", 0x240C},
    {"fr_ci", 0x300C},
    {"fr_fr", 0x040C},
    {"fr_ht", 0x3c0C},
    {"fr_lu", 0x140C},
    {"fr_ml", 0x340C},
    {"fr_ma", 0x380C},
    {"fr_mc", 0x180C},
    {"fr_re", 0x200C},
    {"fr_sn", 0x280C},
    {"fr_ch", 0x100C},
    {"fy", 0x0062},
    {"fy_nl", 0x0462},
    {"ff", 0x0067},
    {"ff_latn", 0x7C67},
    {"ff_ng", 0x0467},
    {"ff_latn_sn", 0x0867},
    {"gl", 0x0056},
    {"gl_es", 0x0456},
    {"ka", 0x0037},
    {"ka_ge", 0x0437},
    {"de", 0x0007},
    {"de_at", 0x0C07},
    {"de_de", 0x0407},
    {"de_li", 0x1407},
    {"de_lu", 0x1007},
    {"de_ch", 0x0807},
    {"el", 0x0008},
    {"el_gr", 0x0408},
    {"kl", 0x006F},
    {"kl_gl", 0x046F},
    {"gn", 0x0074},
    {"gn_py", 0x0474},
    {"gu", 0x0047},
    {"gu_in", 0x0447},
    {"ha", 0x0068},
    {"ha_latn", 0x7C68},
    {"ha_latn_ng", 0x0468},
    {"haw", 0x0075},
    {"haw_us", 0x0475},
    {"he", 0x000D},
    {"he_il", 0x040D},
    {"hi", 0x0039},
    {"hi_in", 0x0439},
    {"hu", 0x000E},
    {"hu_hu", 0x040E},
    {"is", 0x000F},
    {"is_is", 0x040F},
    {"ig", 0x0070},
    {"ig_ng", 0x0470},
    {"id", 0x0021},
    {"id_id", 0x0421},
    {"iu", 0x005D},
    {"iu_latn", 0x7C5D},
    {"iu_latn_ca", 0x085D},
    {"iu_cans", 0x785D},
    {"iu_cans_ca", 0x045d},
    {"ga", 0x003C},
    {"ga_ie", 0x083C},
    {"it", 0x0010},
    {"it_it", 0x0410},
    {"it_ch", 0x0810},
    {"ja", 0x0011},
    {"ja_jp", 0x0411},
    {"kn", 0x004B},
    {"kn_in", 0x044B},
    {"kr_latn_ng", 0x0471},
    {"ks", 0x0060},
    {"ks_arab", 0x0460},
    {"ks_deva_in", 0x0860},
    {"kk", 0x003F},
    {"kk_kz", 0x043F},
    {"km", 0x0053},
    {"km_kh", 0x0453},
    {"quc", 0x0086},
    {"quc_latn_gt", 0x0486},
    {"rw", 0x0087},
    {"rw_rw", 0x0487},
    {"sw", 0x0041},
    {"sw_ke", 0x0441},
    {"kok", 0x0057},
    {"kok_in", 0x0457},
    {"ko", 0x0012},
    {"ko_kr", 0x0412},
    {"ky", 0x0040},
    {"ky_kg", 0x0440},
    {"lo", 0x0054},
    {"lo_la", 0x0454},
    {"la_va", 0x0476},
    {"lv", 0x0026},
    {"lv_lv", 0x0426},
    {"lt", 0x0027},
    {"lt_lt", 0x0427},
    {"dsb", 0x7C2E},
    {"dsb_de", 0x082E},
    {"lb", 0x006E},
    {"lb_lu", 0x046E},
    {"mk", 0x002F},
    {"mk_mk", 0x042F},
    {"ms", 0x003E},
    {"ms_bn", 0x083E},
    {"ms_my", 0x043E},
    {"ml", 0x004C},
    {"ml_in", 0x044C},
    {"mt", 0x003A},
    {"mt_mt", 0x043A},
    {"mi", 0x0081},
    {"mi_nz", 0x0481},
    {"arn", 0x007A},
    {"arn_cl", 0x047A},
    {"mr", 0x004E},
    {"mr_in", 0x044E},
    {"moh", 0x007C},
    {"moh_ca", 0x047C},
    {"mn", 0x0050},
    {"mn_cyrl", 0x7850},
    {"mn_mn", 0x0450},
    {"mn_mong", 0x7C50},
    {"mn_mong_cn", 0x0850},
    {"mn_mong_mn", 0x0C50},
    {"ne", 0x0061},
    {"ne_in", 0x0861},
    {"ne_np", 0x0461},
    {"no", 0x0014},
    {"nb", 0x7C14},
    {"nb_no", 0x0414},
    {"nn", 0x7814},
    {"nn_no", 0x0814},
    {"oc", 0x0082},
    {"oc_fr", 0x0482},
    {"or_in", 0x0448},
    {"om", 0x0072},
    {"om_et", 0x0472},
    {"ps", 0x0063},
    {"ps_af", 0x0463},
    {"fa", 0x0029},
    {"fa_ir", 0x0429},
    {"pl", 0x0015},
    {"pl_pl", 0x0415},
    {"pt", 0x0016},
    {"pt_br", 0x0416},
    {"pt_pt", 0x0816},
    {"qps_ploca", 0x05FE},
    {"qps_ploc", 0x0501},
    {"qps_plocm", 0x09FF},
    {"pa", 0x0046},
    {"pa_arab", 0x7C46},
    {"pa_in", 0x0446},
    {"pa_arab_pk", 0x0846},
    {"quz", 0x006B},
    {"quz_bo", 0x046B},
    {"quz_ec", 0x086B},
    {"quz_pe", 0x0C6B},
    {"ro", 0x0018},
    {"ro_md", 0x0818},
    {"ro_ro", 0x0418},
    {"rm", 0x0017},
    {"rm_ch", 0x0417},
    {"ru", 0x0019},
    {"ru_md", 0x0819},
    {"ru_ru", 0x0419},
    {"sah", 0x0085},
    {"sah_ru", 0x0485},
    {"smn", 0x703B},
    {"smn_fi", 0x243B},
    {"smj", 0x7C3B},
    {"smj_no", 0x103B},
    {"smj_se", 0x143B},
    {"se", 0x003B},
    {"se_fi", 0x0C3B},
    {"se_no", 0x043B},
    {"se_se", 0x083B},
    {"sms", 0x743B},
    {"sms_fi", 0x203B},
    {"sma", 0x783B},
    {"sma_no", 0x183B},
    {"sma_se", 0x1C3B},
    {"sa", 0x004F},
    {"sa_in", 0x044F},
    {"gd", 0x0091},
    {"gd_gb", 0x0491},
    {"sr_cyrl", 0x6C1A},
    {"sr_cyrl_ba", 0x1C1A},
    {"sr_cyrl_me", 0x301A},
    {"sr_cyrl_rs", 0x281A},
    {"sr_cyrl_cs", 0x0C1A},
    {"sr_latn", 0x701A},
    {"sr", 0x7C1A},
    {"sr_latn_ba", 0x181A},
    {"sr_latn_me", 0x2c1A},
    {"sr_latn_rs", 0x241A},
    {"sr_latn_cs", 0x081A},
    {"nso", 0x006C},
    {"nso_za", 0x046C},
    {"tn", 0x0032},
    {"tn_bw", 0x0832},
    {"tn_za", 0x0432},
    {"sd", 0x0059},
    {"sd_arab", 0x7C59},
    {"sd_arab_pk", 0x0859},
    {"si", 0x005B},
    {"si_lk", 0x045B},
    {"sk", 0x001B},
    {"sk_sk", 0x041B},
    {"sl", 0x0024},
    {"sl_si", 0x0424},
    {"so", 0x0077},
    {"so_so", 0x0477},
    {"st", 0x0030},
    {"st_za", 0x0430},
    {"es", 0x000A},
    {"es_ar", 0x2C0A},
    {"es_ve", 0x200A},
    {"es_bo", 0x400A},
    {"es_cl", 0x340A},
    {"es_co", 0x240A},
    {"es_cr", 0x140A},
    {"es_cu", 0x5c0A},
    {"es_do", 0x1c0A},
    {"es_ec", 0x300A},
    {"es_sv", 0x440A},
    {"es_gt", 0x100A},
    {"es_hn", 0x480A},
    {"es_419", 0x580A},
    {"es_mx", 0x080A},
    {"es_ni", 0x4C0A},
    {"es_pa", 0x180A},
    {"es_py", 0x3C0A},
    {"es_pe", 0x280A},
    {"es_pr", 0x500A},
    {"es_es_tradnl", 0x040A},
    {"es_es", 0x0c0A},
    {"es_us", 0x540A},
    {"es_uy", 0x380A},
    {"sv", 0x001D},
    {"sv_fi", 0x081D},
    {"sv_se", 0x041D},
    {"syr", 0x005A},
    {"syr_sy", 0x045A},
    {"tg", 0x0028},
    {"tg_cyrl", 0x7C28},
    {"tg_cyrl_tj", 0x0428},
    {"tzm", 0x005F},
    {"tzm_latn", 0x7C5F},
    {"tzm_latn_dz", 0x085F},
    {"ta", 0x0049},
    {"ta_in", 0x0449},
    {"ta_lk", 0x0849},
    {"tt", 0x0044},
    {"tt_ru", 0x0444},
    {"te", 0x004A},
    {"te_in", 0x044A},
    {"th", 0x001E},
    {"th_th", 0x041E},
    {"bo", 0x0051},
    {"bo_cn", 0x0451},
    {"ti", 0x0073},
    {"ti_er", 0x0873},
    {"ti_et", 0x0473},
    {"ts", 0x0031},
    {"ts_za", 0x0431},
    {"tr", 0x001F},
    {"tr_tr", 0x041F},
    {"tk", 0x0042},
    {"tk_tm", 0x0442},
    {"uk", 0x0022},
    {"uk_ua", 0x0422},
    {"hsb", 0x002E},
    {"hsb_de", 0x042E},
    {"ur", 0x0020},
    {"ur_in", 0x0820},
    {"ur_pk", 0x0420},
    {"ug", 0x0080},
    {"ug_cn", 0x0480},
    {"uz_cyrl", 0x7843},
    {"uz_cyrl_uz", 0x0843},
    {"uz", 0x0043},
    {"uz_latn", 0x7C43},
    {"uz_latn_uz", 0x0443},
    {"ca_es_valencia", 0x0803},
    {"ve", 0x0033},
    {"ve_za", 0x0433},
    {"vi", 0x002A},
    {"vi_vn", 0x042A},
    {"cy", 0x0052},
    {"cy_gb", 0x0452},
    {"wo", 0x0088},
    {"wo_sn", 0x0488},
    {"xh", 0x0034},
    {"xh_za", 0x0434},
    {"ii", 0x0078},
    {"ii_cn", 0x0478},
    {"yi_001", 0x043D},
    {"yo", 0x006A},
    {"yo_ng", 0x046A},
    {"zu", 0x0035},
    {"zu_za", 0x0435},
    {"x_iv_mathan", 0x007F},
};
static const size_t RC_LANG_COUNT = sizeof RC_LANG_TABLE / sizeof RC_LANG_TABLE[0];

static bool rc_lang_tag(const char *tag, uint16_t *id) {
    StrVec parts = {0};
    for (const char *s = tag;;) {
        size_t n = strcspn(s, "-_");
        sv_push(&parts, str_ncopy(s, n));
        if (!s[n]) break;
        s += n + 1;
    }
    const char *lang = parts.data[0];
    *id = 0;
    if (strlen(lang) < 1 || strlen(lang) > 3 || !rc_alpha(lang)) return false;
    const char *script = "", *country = "", *suffix = "";
    bool multiple = false;
    size_t next = 1;
    if (parts.len > 1) {
        const char *part = parts.data[1];
        size_t pn = strlen(part);
        next = 2;
        if (strlen(lang) == 1) {
            if (pn == 2 && rc_alpha(part)) country = part;
            else if (pn > 0 && pn <= 8 && rc_alnum(part)) suffix = part;
            else return false;
        } else if (pn == 4 && rc_alpha(part)) script = part;
        else if (pn == 2 && rc_alpha(part)) country = part;
        else if (pn == 3 && rc_numeric(part)) suffix = part;
        else if (rc_ascii_fold_eq(lang, "qps") && (rc_ascii_fold_eq(part, "ploca") || rc_ascii_fold_eq(part, "plocm"))) suffix = part;
        else return false;
    } else {
        if (strlen(lang) == 1) return false;
        next = parts.len;
    }
    if (parts.len > 1) {
        if (*script) {
            if (parts.len > 2) {
                if (strlen(parts.data[2]) == 2 && rc_alpha(parts.data[2])) { country = parts.data[2]; next = 3; }
                else return false;
            } else next = parts.len;
        }
        for (size_t k = next; k < parts.len; k++) {
            const char *part = parts.data[k];
            if (strlen(part) == 0 || strlen(part) > 8 || !rc_alnum(part)) return false;
            if (!*suffix) suffix = part; else multiple = true;
        }
    }
    uint16_t custom = 0x1000;
    size_t longest = 0;
    for (size_t e = 0; e < RC_LANG_COUNT; e++) if (strlen(RC_LANG_TABLE[e].tag) > longest) longest = strlen(RC_LANG_TABLE[e].tag);
    if (multiple || strlen(tag) > longest) { *id = custom; return true; }
    char *use = str_copy(tag);
    if (*country && *suffix && !*script) {
        static const char *sorts[][3] = {{"de","de","phoneb"},{"hu","hu","tchncl"},{"ka","ge","modern"},{"zh","cn","stroke"},{"zh","sg","stroke"},{"zh","mo","stroke"},{"zh","tw","pronun"},{"zh","tw","radstr"},{"ja","jp","radstr"},{"zh","hk","radstr"},{"zh","mo","radstr"},{"zh","cn","phoneb"},{"zh","sg","phoneb"}};
        for (size_t e = 0; e < sizeof sorts / sizeof sorts[0]; e++) {
            if (rc_ascii_fold_eq(sorts[e][0], lang) && rc_ascii_fold_eq(sorts[e][1], country) && rc_ascii_fold_eq(sorts[e][2], suffix)) {
                use = str_ncopy(tag, strlen(tag) - (strlen(suffix) + 1));
                break;
            }
        }
    }
    char *name = str_copy(use);
    for (char *c = name; *c; c++) *c = *c == '-' ? '_' : (*c >= 'A' && *c <= 'Z' ? (char)(*c + 32) : *c);
    if (streq(name, "ff_latn_ng")) name = "ff_ng";
    for (size_t e = 0; e < RC_LANG_COUNT; e++) if (streq(RC_LANG_TABLE[e].tag, name)) { *id = RC_LANG_TABLE[e].id; return true; }
    *id = custom;
    return true;
}

