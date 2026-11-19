#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#endif
#include "windows_runtime.h"
#include <time.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define IS_TTY(fd) _isatty(fd)
#else
#include <unistd.h>
#define IS_TTY(fd) isatty(fd)
#endif

/* Embedded with the driver: installed resources cannot silently follow a mutable
   Visual Studio channel or a lock file edited beside the executable.
   tests/windows_test.c verifies this text equals msvc.lock.json. */
const char MSVC_LOCK_TEXT[] =
    "{\n"
    "  \"schema\": 1,\n"
    "  \"resource_version\": \"vs17.14.37906.3-crt14.44.17.14-sdk10.0.26100-v1\",\n"
    "  \"crt_version\": \"14.44.17.14\",\n"
    "  \"sdk_version\": \"10.0.26100\",\n"
    "  \"sdk_package_version\": \"10.0.26100.9457\",\n"
    "  \"license_version\": \"build-tools-march-2024+WIN10SDK.RTM.AUG_2018_en-US\",\n"
    "  \"license_urls\": [\n"
    "    \"https://visualstudio.microsoft.com/wp-content/uploads/2024/03/Visual-Studio-2022-Diagnostic-Build-Tools-Agent-License_Update-March-2024_EN.docx\",\n"
    "    \"https://aka.ms/WinSDKLicenseURL\"\n"
    "  ],\n"
    "  \"license_hashes\": {\n"
    "    \"build_tools_docx_sha256\": \"2f66b86a00e8d9833789897ce23d05a4a2dbea370cf39c8c1098dbc17d0e7bdc\",\n"
    "    \"sdk_installer_rtf_sha256\": \"0f4a26ac9dc50066f8a1bfeaaf3f092d1b9e4791df5487b9f3c31e7c3dc4d7f5\"\n"
    "  },\n"
    "  \"manifest\": {\n"
    "    \"channelItems\": [{\n"
    "      \"id\": \"Microsoft.VisualStudio.Manifests.VisualStudio\",\n"
    "      \"version\": \"17.14.37906.3\",\n"
    "      \"type\": \"Manifest\",\n"
    "      \"payloads\": [{\n"
    "        \"fileName\": \"VisualStudio.vsman\",\n"
    "        \"sha256\": \"da49c304df0fff4e75c20849171840a06840e845ce814104d709dfb5cd461668\",\n"
    "        \"size\": 30447538,\n"
    "        \"url\": \"https://download.visualstudio.microsoft.com/download/pr/95995f57-8355-4ff5-8d90-a7e1669fb3b3/da49c304df0fff4e75c20849171840a06840e845ce814104d709dfb5cd461668/VisualStudio.vsman\"\n"
    "      }]\n"
    "    }]\n"
    "  },\n"
    "  \"helper_version\": \"xwin-0.10.0\",\n"
    "  \"hosts\": {\n"
    "    \"linux-amd64\": {\n"
    "      \"url\": \"https://github.com/Jake-Shadle/xwin/releases/download/0.10.0/xwin-0.10.0-x86_64-unknown-linux-musl.tar.gz\",\n"
    "      \"sha256\": \"d870eb4b2f390878af6da1ccd3cf321d22fcb72720984853b4be732ae597fc88\",\n"
    "      \"member\": \"xwin-0.10.0-x86_64-unknown-linux-musl/xwin\"\n"
    "    },\n"
    "    \"windows-amd64\": {\n"
    "      \"url\": \"\",\n"
    "      \"sha256\": \"\",\n"
    "      \"member\": \"xwin.exe\",\n"
    "      \"binary_sha256\": \"aebd6d7c5724a0f7e1d687a7d69c1a04d654eafc3f4829f844a44502fde247a0\"\n"
    "    },\n"
    "    \"darwin-arm64\": {\n"
    "      \"url\": \"\",\n"
    "      \"sha256\": \"bede9a47eb9d801ef267bb4f6f6348a200132785da7a2684daeac45ba8a34957\",\n"
    "      \"member\": \"xwin/0.10.0/bin/xwin\",\n"
    "      \"binary_sha256\": \"605ee9a0c1476148d143610b9832f8cb8dc1ceca6ec470fe746ae94829cae7af\"\n"
    "    },\n"
    "    \"darwin-amd64\": {\n"
    "      \"url\": \"\",\n"
    "      \"sha256\": \"11f292e964bbe84479c92792428b56bf8c2ec6be893f9fdc5fccadadbeeade43\",\n"
    "      \"member\": \"xwin/0.10.0/bin/xwin\",\n"
    "      \"binary_sha256\": \"a69d4c0a6492d910099fbf1627b6254fc0d3d6baca5534e96ca5873593d66df7\"\n"
    "    }\n"
    "  }\n"
    "}\n";

/* ---- SHA-256 ---- */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t used; } Sha256;
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha256 *s, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(Sha256 *s) {
    static const uint32_t init[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(s->h, init, sizeof(init));
    s->len = 0;
    s->used = 0;
}

static void sha_update(Sha256 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->len += n;
    while (n > 0) {
        size_t take = 64 - s->used;
        if (take > n) take = n;
        memcpy(s->buf + s->used, p, take);
        s->used += take; p += take; n -= take;
        if (s->used == 64) { sha_block(s, s->buf); s->used = 0; }
    }
}

static char *sha_final_hex(Sha256 *s) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha_update(s, &pad, 1);
    pad = 0;
    while (s->used != 56) sha_update(s, &pad, 1);
    uint8_t lenbytes[8];
    for (int i = 0; i < 8; i++) lenbytes[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, lenbytes, 8);
    char *out = xmalloc(65);
    for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", s->h[i]);
    return out;
}

char *msvc_digest(const void *data, size_t size) {
    Sha256 s;
    sha_init(&s);
    sha_update(&s, data, size);
    return sha_final_hex(&s);
}

char *msvc_file_digest(const char *path, char **error) {
    *error = NULL;
    FILE *f = open_file(path, "rb");
    if (!f) { *error = str_fmt("cannot open %s: %s", path, strerror(errno)); return NULL; }
    Sha256 s;
    sha_init(&s);
    size_t cap = 1024 * 1024;
    uint8_t *buf = xmalloc(cap);
    for (;;) {
        size_t n = fread(buf, 1, cap, f);
        if (n > 0) sha_update(&s, buf, n);
        if (n < cap) {
            if (ferror(f)) { *error = str_fmt("cannot hash %s: %s", path, strerror(errno)); fclose(f); free(buf); return NULL; }
            break;
        }
    }
    fclose(f);
    free(buf);
    return sha_final_hex(&s);
}

/* ---- Minimal JSON tree: parse, and compact serialization in document order. ---- */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct Json { JType type; char *text; struct Json **items; char **keys; size_t len, cap; } Json;

typedef struct { const char *p, *end; bool bad; int depth; } JParser;

static Json *j_new(JType t) { Json *j = xmalloc(sizeof(Json)); memset(j, 0, sizeof(Json)); j->type = t; return j; }
static void j_add(Json *j, const char *key, Json *v) {
    if (j->len == j->cap) {
        j->cap = j->cap ? j->cap * 2 : 8;
        j->items = xrealloc(j->items, j->cap * sizeof(Json *));
        j->keys = xrealloc(j->keys, j->cap * sizeof(char *));
    }
    j->items[j->len] = v;
    j->keys[j->len] = key ? str_copy(key) : NULL;
    j->len++;
}
static void j_ws(JParser *p) { while (p->p < p->end && (*p->p == ' ' || *p->p == '\t' || *p->p == '\n' || *p->p == '\r')) p->p++; }

static void put_utf8(char **out, size_t *n, size_t *cap, uint32_t cp) {
    if (*n + 5 > *cap) { *cap = *cap * 2 + 8; *out = xrealloc(*out, *cap); }
    char *o = *out + *n;
    if (cp < 0x80) { o[0] = (char)cp; *n += 1; }
    else if (cp < 0x800) { o[0] = (char)(0xc0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3f)); *n += 2; }
    else if (cp < 0x10000) { o[0] = (char)(0xe0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[2] = (char)(0x80 | (cp & 0x3f)); *n += 3; }
    else { o[0] = (char)(0xf0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 0x3f)); o[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[3] = (char)(0x80 | (cp & 0x3f)); *n += 4; }
}

static int hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static char *j_string(JParser *p) {
    /* Caller consumed nothing: *p->p == '"'. */
    p->p++;
    size_t n = 0, cap = 32;
    char *out = xmalloc(cap);
    while (p->p < p->end && *p->p != '"') {
        unsigned char c = (unsigned char)*p->p;
        if (c < 0x20) { p->bad = true; return NULL; }
        if (c == '\\') {
            p->p++;
            if (p->p >= p->end) { p->bad = true; return NULL; }
            char e = *p->p++;
            uint32_t cp;
            switch (e) {
            case '"': cp = '"'; break; case '\\': cp = '\\'; break; case '/': cp = '/'; break;
            case 'b': cp = '\b'; break; case 'f': cp = '\f'; break; case 'n': cp = '\n'; break;
            case 'r': cp = '\r'; break; case 't': cp = '\t'; break;
            case 'u': {
                if (p->end - p->p < 4) { p->bad = true; return NULL; }
                int v = hex4(p->p);
                if (v < 0) { p->bad = true; return NULL; }
                p->p += 4;
                cp = (uint32_t)v;
                if (cp >= 0xd800 && cp < 0xdc00 && p->end - p->p >= 6 && p->p[0] == '\\' && p->p[1] == 'u') {
                    int lo = hex4(p->p + 2);
                    if (lo >= 0xdc00 && lo < 0xe000) { cp = 0x10000 + ((cp - 0xd800) << 10) + (uint32_t)(lo - 0xdc00); p->p += 6; }
                }
                break;
            }
            default: p->bad = true; return NULL;
            }
            put_utf8(&out, &n, &cap, cp);
        } else {
            if (n + 2 > cap) { cap = cap * 2; out = xrealloc(out, cap); }
            out[n++] = (char)c;
            p->p++;
        }
    }
    if (p->p >= p->end) { p->bad = true; return NULL; }
    p->p++;
    out[n] = 0;
    return out;
}

static Json *j_value(JParser *p) {
    j_ws(p);
    if (p->p >= p->end || ++p->depth > 64) { p->bad = true; return NULL; }
    Json *r = NULL;
    char c = *p->p;
    if (c == '"') {
        char *s = j_string(p);
        if (s) { r = j_new(J_STR); r->text = s; }
    } else if (c == '{' || c == '[') {
        bool obj = c == '{';
        r = j_new(obj ? J_OBJ : J_ARR);
        p->p++;
        j_ws(p);
        if (p->p < p->end && *p->p == (obj ? '}' : ']')) { p->p++; }
        else {
            for (;;) {
                char *key = NULL;
                if (obj) {
                    j_ws(p);
                    if (p->p >= p->end || *p->p != '"') { p->bad = true; break; }
                    key = j_string(p);
                    if (!key) break;
                    j_ws(p);
                    if (p->p >= p->end || *p->p != ':') { p->bad = true; break; }
                    p->p++;
                }
                Json *v = j_value(p);
                if (!v) break;
                j_add(r, key, v);
                j_ws(p);
                if (p->p < p->end && *p->p == ',') { p->p++; continue; }
                if (p->p < p->end && *p->p == (obj ? '}' : ']')) { p->p++; break; }
                p->bad = true;
                break;
            }
        }
    } else if (p->end - p->p >= 4 && !strncmp(p->p, "true", 4)) { r = j_new(J_BOOL); r->text = "true"; p->p += 4; }
    else if (p->end - p->p >= 5 && !strncmp(p->p, "false", 5)) { r = j_new(J_BOOL); r->text = "false"; p->p += 5; }
    else if (p->end - p->p >= 4 && !strncmp(p->p, "null", 4)) { r = j_new(J_NULL); p->p += 4; }
    else if (c == '-' || (c >= '0' && c <= '9')) {
        const char *start = p->p;
        p->p++;
        while (p->p < p->end && (isdigit((unsigned char)*p->p) || strchr("+-.eE", *p->p))) p->p++;
        r = j_new(J_NUM);
        r->text = str_ncopy(start, (size_t)(p->p - start));
    } else p->bad = true;
    p->depth--;
    if (p->bad) return NULL;
    return r;
}

static Json *j_parse(const char *text, size_t size) {
    JParser p = {text, text + size, false, 0};
    Json *v = j_value(&p);
    if (!v || p.bad) return NULL;
    j_ws(&p);
    if (p.p != p.end) return NULL;
    return v;
}

static Json *j_get(const Json *o, const char *key) {
    if (!o || o->type != J_OBJ) return NULL;
    for (size_t i = 0; i < o->len; i++) if (streq(o->keys[i], key)) return o->items[i];
    return NULL;
}
static const char *j_str(const Json *o, const char *key) {
    Json *v = j_get(o, key);
    return v && v->type == J_STR ? v->text : "";
}

typedef struct { char *data; size_t len, cap; } Buf;
static void buf_add(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) { b->cap = (b->len + n + 1) * 2; b->data = xrealloc(b->data, b->cap); }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}
static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

static void j_emit_string(Buf *b, const char *s) {
    buf_str(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char tmp[8];
        if (c == '"') buf_str(b, "\\\"");
        else if (c == '\\') buf_str(b, "\\\\");
        else if (c == '\n') buf_str(b, "\\n");
        else if (c == '\r') buf_str(b, "\\r");
        else if (c == '\t') buf_str(b, "\\t");
        else if (c < 0x20) { snprintf(tmp, sizeof(tmp), "\\u%04x", c); buf_str(b, tmp); }
        else buf_add(b, (const char *)&c, 1);
    }
    buf_str(b, "\"");
}

static void j_emit(Buf *b, const Json *j) {
    switch (j->type) {
    case J_NULL: buf_str(b, "null"); break;
    case J_BOOL: case J_NUM: buf_str(b, j->text); break;
    case J_STR: j_emit_string(b, j->text); break;
    case J_ARR:
        buf_str(b, "[");
        for (size_t i = 0; i < j->len; i++) { if (i) buf_str(b, ","); j_emit(b, j->items[i]); }
        buf_str(b, "]");
        break;
    case J_OBJ:
        buf_str(b, "{");
        for (size_t i = 0; i < j->len; i++) {
            if (i) buf_str(b, ",");
            j_emit_string(b, j->keys[i]);
            buf_str(b, ":");
            j_emit(b, j->items[i]);
        }
        buf_str(b, "}");
        break;
    }
}

/* ---- Lock ---- */
Managed_Msvc_Lock msvc_lock(char **error) {
    Managed_Msvc_Lock l = {0};
    *error = NULL;
    Json *root = j_parse(MSVC_LOCK_TEXT, strlen(MSVC_LOCK_TEXT));
    if (!root || root->type != J_OBJ) { *error = str_copy("invalid embedded Microsoft resource lock"); return l; }
    Json *schema = j_get(root, "schema");
    l.resource_version = (char *)j_str(root, "resource_version");
    l.crt_version = (char *)j_str(root, "crt_version");
    l.sdk_version = (char *)j_str(root, "sdk_version");
    l.sdk_package_version = (char *)j_str(root, "sdk_package_version");
    l.license_version = (char *)j_str(root, "license_version");
    l.helper_version = (char *)j_str(root, "helper_version");
    Json *urls = j_get(root, "license_urls");
    if (urls && urls->type == J_ARR) for (size_t i = 0; i < urls->len; i++) if (urls->items[i]->type == J_STR) sv_push(&l.license_urls, urls->items[i]->text);
    Json *manifest = j_get(root, "manifest");
    if (manifest) { Buf b = {0}; j_emit(&b, manifest); l.manifest_json = b.data ? b.data : str_copy(""); }
    Json *hosts = j_get(root, "hosts");
    if (hosts && hosts->type == J_OBJ) {
        l.host_names = xmalloc(sizeof(char *) * (hosts->len + 1));
        l.hosts = xmalloc(sizeof(Managed_Msvc_Helper) * (hosts->len + 1));
        for (size_t i = 0; i < hosts->len; i++) {
            Json *h = hosts->items[i];
            l.host_names[i] = hosts->keys[i];
            l.hosts[i].url = (char *)j_str(h, "url");
            l.hosts[i].sha256 = (char *)j_str(h, "sha256");
            l.hosts[i].member = (char *)j_str(h, "member");
            l.hosts[i].binary_sha256 = (char *)j_str(h, "binary_sha256");
        }
        l.host_count = hosts->len;
    }
    if (!schema || schema->type != J_NUM || !streq(schema->text, "1")) { *error = str_copy("unsupported Microsoft resource lock schema"); return l; }
    return l;
}

const char *msvc_host(void) {
#if defined(__linux__) && defined(__x86_64__)
    return "linux-amd64";
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "windows-amd64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "darwin-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "darwin-amd64";
#else
    return "unsupported";
#endif
}

char *msvc_resource_dir(Toolchain tc, Target t) {
    return path_join(tc.cache_dir, "msvc", msvc_digest(MSVC_LOCK_TEXT, strlen(MSVC_LOCK_TEXT)), nz(t.arch));
}

Msvc_Libc msvc_managed_layout(const char *root, Target t) {
    const char *arch = nz(t.arch);
    if (streq(arch, "arm") || streq(arch, "thumb")) arch = "aarch";
    Msvc_Libc l;
    l.include_dir = path_join(root, "sdk", "include", "ucrt");
    l.sys_include_dir = path_join(root, "crt", "include");
    l.crt_dir = path_join(root, "sdk", "lib", "ucrt", arch);
    l.msvc_lib_dir = path_join(root, "crt", "lib", arch);
    l.kernel32_lib_dir = path_join(root, "sdk", "lib", "um", arch);
    return l;
}

/* Refuse untrusted receipt paths before joining them to the cache root. */
bool msvc_safe_relative(const char *p) {
    p = nz(p);
    if (!p[0] || strchr(p, '\\') || strchr(p, ':') || p[0] == '/') return false;
    const char *part = p;
    for (;;) {
        const char *slash = strchr(part, '/');
        size_t n = slash ? (size_t)(slash - part) : strlen(part);
        if (n == 0 || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return false;
        if (!slash) break;
        part = slash + 1;
    }
    return true;
}

static bool path_exists(const char *path) {
    return file_exists(path);
}

bool msvc_layout_valid(Msvc_Libc l) {
    char *sdk_root = msvc_dirname(l.include_dir);
    char *paths[] = {
        path_join(l.include_dir, "stdio.h"), path_join(l.sys_include_dir, "vcruntime.h"),
        path_join(sdk_root, "um", "windows.h"), path_join(sdk_root, "shared", "winerror.h"),
        path_join(l.crt_dir, "ucrt.lib"), path_join(l.msvc_lib_dir, "libcmt.lib"),
        path_join(l.kernel32_lib_dir, "kernel32.lib"),
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) if (!path_exists(paths[i]) || is_dir(paths[i])) return false;
    return true;
}

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }

/* Parse only symlink/junction substitute names, with byte offsets relative to
   PathBuffer (not UTF-16 indices). Keep the exact target for receipt comparison. */
char *msvc_reparse_target(const uint8_t *data, size_t size_in, char **error) {
    *error = NULL;
    if (size_in < 8) { *error = str_copy("truncated reparse header"); return NULL; }
    uint32_t tag = le32(data);
    size_t size = (size_t)le16(data + 4) + 8;
    size_t start;
    if (tag == 0xa000000c) start = 20;       /* IO_REPARSE_TAG_SYMLINK */
    else if (tag == 0xa0000003) start = 16;  /* IO_REPARSE_TAG_MOUNT_POINT */
    else { *error = str_fmt("unsupported reparse tag 0x%08x", tag); return NULL; }
    if (size > size_in || size < start) { *error = str_copy("truncated reparse data"); return NULL; }
    size_t offset = le16(data + 8), length = le16(data + 10);
    if (offset % 2 != 0 || length % 2 != 0 || length == 0 || offset > size - start || length > size - start - offset) {
        *error = str_copy("invalid reparse target bounds");
        return NULL;
    }
    size_t count = length / 2;
    uint16_t *words = xmalloc(count * sizeof(uint16_t));
    for (size_t i = 0; i < count; i++) {
        words[i] = (uint16_t)le16(data + start + offset + 2 * i);
        if (words[i] == 0) { free(words); *error = str_copy("NUL in reparse target"); return NULL; }
    }
    size_t n = 0, cap = 3 * count + 1;
    char *out = xmalloc(cap + 4);
    for (size_t i = 0; i < count; i++) {
        uint32_t cp = words[i];
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < count && words[i + 1] >= 0xdc00 && words[i + 1] < 0xe000) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (uint32_t)(words[i + 1] - 0xdc00);
            i++;
        } else if (cp >= 0xd800 && cp < 0xe000) cp = 0xfffd;
        put_utf8(&out, &n, &cap, cp);
    }
    out[n] = 0;
    free(words);
    return out;
}

/* ---- Platform file operations ---- */
#ifdef _WIN32
static wchar_t *w_from(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = xmalloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *w_to(const wchar_t *w, int len) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = xmalloc((size_t)n + 1);
    WideCharToMultiByte(CP_UTF8, 0, w, len, s, n, NULL, NULL);
    s[n] = 0;
    return s;
}

/* Preserve long-path support: absolute, backslashed, \\?\ or \\?\UNC\ prefixed. */
static wchar_t *msvc_windows_name(const char *path, char **error) {
    char *absolute = str_replace(absolute_path(path), "/", "\\");
    char *extended = absolute;
    if (!starts_with(extended, "\\\\?\\")) {
        if (starts_with(extended, "\\\\")) extended = str_fmt("\\\\?\\UNC\\%s", extended + 2);
        else extended = str_fmt("\\\\?\\%s", extended);
    }
    wchar_t *w = w_from(extended);
    if (!w) *error = str_fmt("cannot convert path %s", path);
    return w;
}
#endif

char *msvc_read_link(const char *path, char **error) {
    *error = NULL;
#ifdef _WIN32
    wchar_t *wpath = msvc_windows_name(path, error);
    if (!wpath) return NULL;
    HANDLE h = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) { *error = str_fmt("Win32 open error %lu", (unsigned long)GetLastError()); return NULL; }
    uint8_t buffer[16384];
    DWORD n = 0;
    if (!DeviceIoControl(h, 0x000900a8 /* FSCTL_GET_REPARSE_POINT */, NULL, 0, buffer, sizeof(buffer), &n, NULL)) {
        *error = str_fmt("Win32 reparse error %lu", (unsigned long)GetLastError());
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    if (n > sizeof(buffer)) { *error = str_copy("oversized reparse data"); return NULL; }
    return msvc_reparse_target(buffer, n, error);
#else
    char buf[4096];
    ssize_t n = readlink(path, buf, sizeof(buf));
    if (n < 0) { *error = str_fmt("%s", strerror(errno)); return NULL; }
    if (n == 0) { *error = str_copy("empty symlink target"); return NULL; }
    return str_ncopy(buf, (size_t)n);
#endif
}

/* Resolve the existing object, not just its lexical spelling: intermediate
   symlinks/junctions must not hide an escape. GetFinalPathName also gives root
   and target the same extended DOS/UNC namespace and actual on-disk case. */
char *msvc_final_path(const char *path, char **error) {
    *error = NULL;
#ifdef _WIN32
    wchar_t *wpath = msvc_windows_name(path, error);
    if (!wpath) return NULL;
    HANDLE h = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) { *error = str_fmt("Win32 open error %lu", (unsigned long)GetLastError()); return NULL; }
    DWORD n = GetFinalPathNameByHandleW(h, NULL, 0, 0 /* VOLUME_NAME_DOS */);
    if (n == 0) { *error = str_fmt("Win32 final path error %lu", (unsigned long)GetLastError()); CloseHandle(h); return NULL; }
    wchar_t *buffer = xmalloc(((size_t)n + 1) * sizeof(wchar_t));
    DWORD count = GetFinalPathNameByHandleW(h, buffer, n + 1, 0);
    CloseHandle(h);
    if (count == 0 || count >= n + 1) { *error = str_copy("cannot resolve final path"); return NULL; }
    char *result = w_to(buffer, (int)count);
    if (!result) { *error = str_copy("cannot decode final path"); return NULL; }
    return result;
#else
    char *resolved = realpath(path, NULL);
    if (!resolved) { *error = str_fmt("%s", strerror(errno)); return NULL; }
    return resolved;
#endif
}

bool msvc_path_within(const char *root, const char *target, bool windows) {
    root = nz(root);
    target = nz(target);
    if (windows) { root = str_replace(root, "\\", "/"); target = str_replace(target, "\\", "/"); }
    size_t n = strlen(root);
    while (n > 0 && root[n - 1] == '/') n--;
    root = str_ncopy(root, n);
    /* Do not fold component case: NTFS directories may be case-sensitive.
       Callers supply final paths, so root and target already share disk spelling. */
    return root[0] && (streq(target, root) || starts_with(target, str_fmt("%s/", root)));
}

#ifdef _WIN32
#define HOST_IS_WINDOWS true
#else
#define HOST_IS_WINDOWS false
#endif

/* Size and a stable modification stamp for change detection. */
static bool msvc_stat(const char *path, int64_t *size, char **modified) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    wchar_t *w = w_from(path);
    if (!w || !GetFileAttributesExW(w, GetFileExInfoStandard, &d)) return false;
    *size = (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
    *modified = str_fmt("%llu", (unsigned long long)(((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime));
    return true;
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    long nsec = 0;
#if defined(__APPLE__)
    nsec = st.st_mtimespec.tv_nsec;
#elif defined(__linux__)
    nsec = st.st_mtim.tv_nsec;
#endif
    *size = (int64_t)st.st_size;
    *modified = str_fmt("%lld.%09ld", (long long)st.st_mtime, nsec);
    return true;
#endif
}

/* Hash every installed file once. Subsequent TUs authenticate the receipt and
   check file metadata, not 600+ MB of contents. Any changed/deleted file fails
   closed; reinstall into a new staging directory is the repair operation. */
static void files_push(Managed_Msvc_Files *files, Managed_Msvc_File f) {
    if (files->len == files->cap) {
        files->cap = files->cap ? files->cap * 2 : 256;
        files->data = xrealloc(files->data, files->cap * sizeof(Managed_Msvc_File));
    }
    files->data[files->len++] = f;
}

char *msvc_inventory(const char *root, const char *relative, Managed_Msvc_Files *files) {
    const char *dir = root;
    if (nz(relative)[0]) dir = path_join(root, relative);
    Msvc_Dir_Entries entries;
    if (!msvc_read_dir(dir, &entries)) return str_fmt("cannot inspect Microsoft resources: %s", strerror(errno));
    for (size_t i = 0; i < entries.len; i++) {
        const char *name = entries.data[i].name;
        char *rel = str_copy(name);
        if (nz(relative)[0]) rel = str_fmt("%s/%s", relative, name);
        if (!msvc_safe_relative(rel)) return str_copy("unsafe Microsoft resource path");
        char *path = path_join(root, rel);
        int type = entries.data[i].type;
        if (type == 1) {
            char *error = msvc_inventory(root, rel, files);
            if (error) return error;
        } else if (type == 2) {
            char *le = NULL, *ae = NULL, *be = NULL;
            char *target = msvc_read_link(path, &le);
            if (le) return str_fmt("cannot inspect Microsoft resource symlink \"%s\": %s", path, le);
            char *resolved = msvc_final_path(path, &ae);
            char *base = msvc_final_path(root, &be);
            if (ae || be) return str_fmt("cannot resolve Microsoft resource symlink \"%s\" -> \"%s\": %s %s", path, target, nz(ae), nz(be));
            if (!msvc_path_within(base, resolved, HOST_IS_WINDOWS)) {
                return str_fmt("Microsoft resource symlink escapes the installation: \"%s\" -> \"%s\" (resolved \"%s\"; root \"%s\")", path, target, resolved, base);
            }
            Managed_Msvc_File f = {0};
            f.path = rel;
            f.sha256 = msvc_digest(target, strlen(target));
            f.link = target;
            files_push(files, f);
        } else if (type == 0) {
            int64_t size;
            char *modified;
            if (!msvc_stat(path, &size, &modified)) return str_copy("cannot stat installed Microsoft resource");
            char *herr = NULL;
            char *digest = msvc_file_digest(path, &herr);
            if (herr) return herr;
            Managed_Msvc_File f = {0};
            f.path = rel;
            f.sha256 = digest;
            f.size = size;
            f.modified = modified;
            f.link = str_copy("");
            files_push(files, f);
        }
    }
    return NULL;
}

char *msvc_receipt_json(const char *arch, const Managed_Msvc_Files *files) {
    Buf b = {0};
    buf_str(&b, "{\"lock_sha256\":");
    j_emit_string(&b, msvc_digest(MSVC_LOCK_TEXT, strlen(MSVC_LOCK_TEXT)));
    buf_str(&b, ",\"arch\":");
    j_emit_string(&b, nz(arch));
    buf_str(&b, ",\"files\":[");
    for (size_t i = 0; i < files->len; i++) {
        const Managed_Msvc_File *f = &files->data[i];
        char num[32];
        if (i) buf_str(&b, ",");
        buf_str(&b, "{\"path\":"); j_emit_string(&b, nz(f->path));
        buf_str(&b, ",\"sha256\":"); j_emit_string(&b, nz(f->sha256));
        buf_str(&b, ",\"modified\":"); j_emit_string(&b, nz(f->modified));
        buf_str(&b, ",\"link\":"); j_emit_string(&b, nz(f->link));
        snprintf(num, sizeof(num), "%lld", (long long)f->size);
        buf_str(&b, ",\"size\":"); buf_str(&b, num);
        buf_str(&b, "}");
    }
    buf_str(&b, "]}");
    return b.data;
}

Msvc_Libc msvc_cached(Toolchain tc, Target t, char **error) {
    char *root = msvc_resource_dir(tc, t);
    Msvc_Libc l = msvc_managed_layout(root, t);
    *error = NULL;
    size_t marker_size = 0, bytes_size = 0;
    char *marker = read_file(path_join(root, "complete"), &marker_size);
    if (!marker) { *error = str_copy("managed Microsoft resources are not installed"); return l; }
    char *bytes = read_file(path_join(root, "receipt.json"), &bytes_size);
    char *expected = bytes ? msvc_digest(bytes, bytes_size) : NULL;
    if (!bytes || marker_size != strlen(expected) || memcmp(marker, expected, marker_size) != 0) {
        *error = str_copy("managed Microsoft resource receipt is corrupt");
        return l;
    }
    Json *receipt = j_parse(bytes, bytes_size);
    Json *files = receipt ? j_get(receipt, "files") : NULL;
    if (!receipt || receipt->type != J_OBJ || (files && files->type != J_ARR)) {
        *error = str_copy("managed Microsoft resource receipt is invalid");
        return l;
    }
    if (!streq(j_str(receipt, "lock_sha256"), msvc_digest(MSVC_LOCK_TEXT, strlen(MSVC_LOCK_TEXT))) ||
        !streq(j_str(receipt, "arch"), nz(t.arch)) || !files || files->len == 0) {
        *error = str_copy("managed Microsoft resource receipt does not match this lock/architecture");
        return l;
    }
    for (size_t i = 0; i < files->len; i++) {
        Json *f = files->items[i];
        const char *fpath = j_str(f, "path"), *fsha = j_str(f, "sha256"), *flink = j_str(f, "link"), *fmod = j_str(f, "modified");
        if (!msvc_safe_relative(fpath) || strlen(fsha) != 64) { *error = str_copy("unsafe Microsoft resource receipt"); return l; }
        char *path = path_join(root, fpath);
        if (flink[0]) {
            char *le = NULL, *ae = NULL, *be = NULL;
            char *target = msvc_read_link(path, &le);
            if (le || !streq(target, flink) || !streq(msvc_digest(target, strlen(target)), fsha)) {
                *error = str_copy("managed Microsoft resource symlink changed");
                return l;
            }
            char *resolved = msvc_final_path(path, &ae);
            char *base = msvc_final_path(root, &be);
            if (ae || be || !msvc_path_within(base, resolved, HOST_IS_WINDOWS)) {
                *error = str_fmt("managed Microsoft resource symlink is missing or escapes the installation: \"%s\" -> \"%s\"", path, target);
                return l;
            }
            continue;
        }
        Json *size_json = j_get(f, "size");
        int64_t want = size_json && size_json->type == J_NUM ? (int64_t)strtoll(size_json->text, NULL, 10) : 0;
        int64_t size;
        char *modified;
        if (!msvc_stat(path, &size, &modified) || size != want || !streq(modified, fmod)) {
            *error = str_fmt("managed Microsoft resource is missing or changed: %s", fpath);
            return l;
        }
    }
    if (!msvc_layout_valid(l)) { *error = str_copy("managed Microsoft resource installation is incomplete"); return l; }
    return l;
}

char *msvc_license_receipt(Managed_Msvc_Lock lock) {
    return str_fmt("cross_cc Microsoft license acceptance v1\nlock-sha256=%s\nlicense-version=%s\neligibility-confirmed=yes\n",
        msvc_digest(MSVC_LOCK_TEXT, strlen(MSVC_LOCK_TEXT)), nz(lock.license_version));
}

static char *join_with(StrVec v, const char *sep) {
    Buf b = {0};
    for (size_t i = 0; i < v.len; i++) { if (i) buf_str(&b, sep); buf_str(&b, v.data[i]); }
    return b.data ? b.data : str_copy("");
}

char *msvc_consent(Options o, Toolchain tc, Managed_Msvc_Lock lock) {
    char *root = path_join(tc.cache_dir, "msvc", "licenses");
    char *receipt = msvc_license_receipt(lock);
    char *path = path_join(root, str_fmt("%s.txt", msvc_digest(MSVC_LOCK_TEXT, strlen(MSVC_LOCK_TEXT))));
    size_t prior_size = 0;
    char *prior = read_file(path, &prior_size);
    if (prior && prior_size == strlen(receipt) && memcmp(prior, receipt, prior_size) == 0) return NULL;
    bool accepted = o.msvc_accept_license;
    if (!accepted && IS_TTY(0) && IS_TTY(2) && !o.dry_run) {
        fprintf(stderr, "cross_cc needs Microsoft CRT %s and Windows SDK %s (%s).\n", nz(lock.crt_version), nz(lock.sdk_version), nz(lock.sdk_package_version));
        fprintf(stderr, "License version: %s\n", nz(lock.license_version));
        for (size_t i = 0; i < lock.license_urls.len; i++) fprintf(stderr, "  %s\n", lock.license_urls.data[i]);
        fprintf(stderr, "Acceptance does not grant rights beyond these terms; you must be eligible to use both products.\n");
        fprintf(stderr, "Confirm eligibility, accept both terms, and download into %s? Type yes [no]: ", path_join(tc.cache_dir, "msvc"));
        fflush(stderr);
        char response[65];
        size_t length = 0;
        while (length < 64) {
            int c = fgetc(stdin);
            if (c == EOF || c == '\n') break;
            response[length++] = (char)c;
        }
        response[length] = 0;
        accepted = streq(str_trim(response), "yes");
    }
    if (!accepted || o.dry_run) {
        return str_fmt("Microsoft resources are required; review %s, then rerun with --cross-cc-accept-msvc-license (or accept interactively in a terminal). No download was attempted. Use --cross-cc-msvc=system for explicit system discovery, --libc <file>/ZIG_LIBC, or --sysroot for your own resources", join_with(lock.license_urls, " and "));
    }
    ensure_dir(root);
    if (!is_dir(root)) return str_copy("cannot create Microsoft license acceptance directory");
    /* Unique file + rename prevents partial acceptance from being mistaken for consent. */
    char *terr = NULL;
    char *stage = make_temp_dir(root, "accept-", &terr);
    if (terr) return str_copy("cannot stage Microsoft license acceptance");
    char *name = path_join(stage, "acceptance.txt");
    char *result = NULL;
    if (!write_file(name, receipt, strlen(receipt))) result = str_copy("cannot write Microsoft license acceptance");
    else if (!move_path(name, path)) {
        size_t saved_size = 0;
        char *saved = read_file(path, &saved_size);
        if (!saved || saved_size != strlen(receipt) || memcmp(saved, receipt, saved_size) != 0) result = str_copy("cannot publish Microsoft license acceptance");
    }
    remove_tree(stage);
    return result;
}

static char *msvc_fetch(const char *url, const char *expected, const char *dest, bool verbose) {
    if (!starts_with(url, "https://") || strlen(nz(expected)) != 64) return str_copy("invalid Microsoft helper download pin");
    /* curl is a transport dependency, never a shell command. Reject HTTP on
       initial requests and redirects, including redirects from GitHub releases. */
    StrVec cmd = SV("curl", "-fL", "--proto", "=https", "--proto-redir", "=https", "--connect-timeout", "30", "--max-time", "1800", "-sS", "-o", dest, url);
    if (run_command(cmd, verbose) != 0) return str_copy("cannot download Microsoft resource helper (curl with HTTPS support is required)");
    char *error = NULL;
    char *digest = msvc_file_digest(dest, &error);
    if (error) return error;
    if (!streq(digest, expected)) return str_copy("Microsoft resource helper SHA-256 mismatch");
    return NULL;
}

static char *msvc_install(Options o, Toolchain tc, Target t, Managed_Msvc_Lock lock) {
    const Managed_Msvc_Helper *pin = NULL;
    for (size_t i = 0; i < lock.host_count; i++) if (streq(lock.host_names[i], msvc_host())) pin = &lock.hosts[i];
    if (!pin) return str_copy("no authenticated Microsoft resource extraction helper for this host; use --cross-cc-msvc=system or --libc/--sysroot (managed download is unavailable on this host)");
    const char *arch_name = nz(t.arch);
    if (!streq(arch_name, "x86_64") && !streq(arch_name, "x86") && !streq(arch_name, "aarch64") && !streq(arch_name, "arm") && !streq(arch_name, "thumb")) {
        return str_copy("unsupported managed Microsoft resource architecture");
    }
    char *parent = path_join(tc.cache_dir, "msvc", msvc_digest(MSVC_LOCK_TEXT, strlen(MSVC_LOCK_TEXT)));
    ensure_dir(parent);
    if (!is_dir(parent)) return str_copy("cannot create Microsoft resource cache");
    char *terr = NULL;
    char *stage = make_temp_dir(parent, "stage-", &terr);
    if (terr) return str_copy("cannot stage Microsoft resources");
    char *error = NULL;
    const char *helper = "";
    char *bundled = path_join(tc.root, "libexec", str_fmt("xwin%s", NATIVE_EXE_SUFFIX));
    if (nz(pin->binary_sha256)[0] && path_exists(bundled)) {
        char *got = msvc_file_digest(bundled, &error);
        if (error) goto done;
        if (!streq(got, pin->binary_sha256)) { error = str_copy("bundled Microsoft resource helper SHA-256 mismatch; reinstall the compiler release"); goto done; }
        helper = bundled;
    } else {
        if (!nz(pin->url)[0]) { error = str_fmt("the authenticated Microsoft extraction helper is missing: %s; install the complete compiler release for this host, or use --cross-cc-msvc=system / --libc / --sysroot", bundled); goto done; }
        char *archive = path_join(stage, "helper.tar.gz");
        error = msvc_fetch(pin->url, pin->sha256, archive, o.verbose);
        if (error) goto done;
        /* Extract ONE pinned regular executable. Never pass an untrusted archive's
           member list to tar, and reject links before extraction. */
        if (!msvc_safe_relative(pin->member)) { error = str_copy("unsafe extraction helper member"); goto done; }
        int code = 0;
        char *raw = capture_command(SV("tar", "-tvzf", archive, pin->member), &code);
        char *listing = str_trim(nz(raw));
        if (code != 0 || !listing[0] || listing[0] != '-' || contains(listing, "\n")) { error = str_copy("Microsoft helper archive does not contain the expected regular file"); goto done; }
        if (run_command(SV("tar", "-xzf", archive, "-C", stage, pin->member), o.verbose) != 0) { error = str_copy("cannot extract Microsoft helper (tar is required)"); goto done; }
        helper = path_join(stage, pin->member);
    }
    char *manifest_path = path_join(stage, "channel.json");
    const char *manifest = nz(lock.manifest_json);
    if (!manifest[0]) { error = str_copy("cannot encode pinned Microsoft manifest"); goto done; }
    if (!write_file(manifest_path, manifest, strlen(manifest))) { error = str_copy("cannot write pinned Microsoft manifest"); goto done; }
    char *output = path_join(stage, "resources");
    const char *arch = nz(t.arch);
    if (streq(arch, "arm") || streq(arch, "thumb")) arch = "aarch";
    StrVec cmd = SV(helper, "--accept-license", "--manifest", manifest_path, "--sdk-version", lock.sdk_version,
        "--crt-version", lock.crt_version, "--arch", arch, "--variant", "desktop", "--cache-dir", path_join(stage, "payloads"),
        "splat", "--output", output);
#ifdef _WIN32
    /* Windows' filesystem is case-insensitive and symlink creation may require
       administrator privileges. Extraction must never require elevation. */
    sv_push(&cmd, "--disable-symlinks");
#endif
    /* Only the dedicated, lock-scoped consent above authorizes this argument.
       XWIN_ACCEPT_LICENSE and generic environment consent are never consulted. */
    if (run_command(cmd, o.verbose) != 0) { error = str_copy("managed Microsoft resource acquisition failed; no partial installation was published"); goto done; }
    if (!msvc_layout_valid(msvc_managed_layout(output, t))) { error = str_copy("downloaded Microsoft resource layout is incomplete"); goto done; }
    Managed_Msvc_Files files = {0};
    error = msvc_inventory(output, "", &files);
    if (error) goto done;
    char *receipt = msvc_receipt_json(t.arch, &files);
    if (!write_file(path_join(output, "receipt.json"), receipt, strlen(receipt))) { error = str_copy("cannot write Microsoft resource receipt"); goto done; }
    char *complete = msvc_digest(receipt, strlen(receipt));
    if (!write_file(path_join(output, "complete"), complete, strlen(complete))) { error = str_copy("cannot finish Microsoft resource receipt"); goto done; }
    char *dest = msvc_resource_dir(tc, t);
    /* Never replace or delete an existing cache behind concurrent compiler jobs.
       Corrupt installations are diagnosed with their exact path for explicit repair. */
    if (!move_path(output, dest)) {
        int saved_errno = errno;
        char *cached_error = NULL;
        msvc_cached(tc, t, &cached_error);
        if (cached_error) error = str_fmt("cannot publish Microsoft resources; move aside the incomplete cache %s and rerun: %s", dest, strerror(saved_errno));
    }
done:
    remove_tree(stage);
    return error;
}

Msvc_Libc managed_msvc(Options o, Target t, Toolchain tc, char **error) {
    Msvc_Libc l = msvc_cached(tc, t, error);
    if (!*error) return l;
    /* -### is an inspection operation, including after prior acceptance. */
    if (o.dry_run || o.query) { *error = NULL; return l; }
    char *dir = msvc_resource_dir(tc, t);
    if (path_exists(dir)) {
        *error = str_fmt("%s; move aside %s and rerun to repair", *error, dir);
        return l;
    }
    char *le = NULL;
    Managed_Msvc_Lock lock = msvc_lock(&le);
    if (le) { *error = le; return l; }
    char *ce = msvc_consent(o, tc, lock);
    if (ce) { *error = ce; return l; }
    char *ie = msvc_install(o, tc, t, lock);
    if (ie) { *error = ie; return l; }
    return msvc_cached(tc, t, error);
}

/* Conservative no-SDK fast path, not a C preprocessor. Header-free compilation
   must not prompt or network. Any include/import directive (including a macro
   include or a local header) requests resources; conditionally dead directives
   can therefore request them too. Stdin is never consumed speculatively: use
   the consent flag to provision resources explicitly before compiling stdin.
   Scan preprocessing tokens, not raw lines: phase-2 splicing precedes comment
   recognition. Work on a private buffer; the source passed to Clang is unchanged. */
bool msvc_source_needs_sdk(const uint8_t *source, size_t size) {
    uint8_t *text = xmalloc(size + 1);
    size_t n = 0;
    for (size_t i = 0; i < size; i++) {
        if (source[i] == '\\' && i + 1 < size) {
            if (source[i + 1] == '\n') { i++; continue; }
            if (source[i + 1] == '\r') {
                i++;
                if (i + 1 < size && source[i + 1] == '\n') i++;
                continue;
            }
        }
        text[n++] = source[i];
    }
    /* 0: only whitespace so far, 1: after directive #, 2: ordinary tokens. */
    int stage = 0;
    size_t start = 0;
    bool result = false;
    if (n >= 3 && text[0] == 0xef && text[1] == 0xbb && text[2] == 0xbf) start = 3;
    for (size_t i = start; i < n;) {
        uint8_t c = text[i];
        if (c == '\n' || c == '\r') { stage = 0; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\v' || c == '\f') { i++; continue; }
        if (c == '/' && i + 1 < n) {
            if (text[i + 1] == '/') {
                i += 2;
                while (i < n && text[i] != '\n' && text[i] != '\r') i++;
                continue;
            }
            if (text[i + 1] == '*') {
                i += 2;
                /* A block comment is one space, including its physical newlines. */
                while (i + 1 < n && !(text[i] == '*' && text[i + 1] == '/')) i++;
                i = i + 2 < n ? i + 2 : n;
                continue;
            }
        }
        if (stage == 0 && c == '#') { stage = 1; i++; continue; }
        if (stage == 0 && c == '%' && i + 1 < n && text[i + 1] == ':') { stage = 1; i += 2; continue; }
        if (stage == 1) {
            size_t word_start = i;
            while (i < n && ((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= '0' && text[i] <= '9') || text[i] == '_')) i++;
            size_t wl = i - word_start;
            const uint8_t *w = text + word_start;
            if ((wl == 7 && !memcmp(w, "include", 7)) || (wl == 12 && !memcmp(w, "include_next", 12)) || (wl == 6 && !memcmp(w, "import", 6))) { result = true; break; }
            if (i != word_start) { stage = 2; continue; }
        }
        stage = 2;
        /* C++ raw strings may contain both quotes and apparent directives. */
        if (c == 'R' && i + 1 < n && text[i + 1] == '"') {
            size_t end = i + 2;
            while (end < n && end - (i + 2) <= 16 && text[end] != '(' && text[end] > ' ' && text[end] != '\\' && text[end] != ')') end++;
            if (end < n && end - (i + 2) <= 16 && text[end] == '(') {
                size_t dl = end - (i + 2);
                const uint8_t *delimiter = text + i + 2;
                i = end + 1;
                while (i < n) {
                    if (text[i] == ')' && i + dl + 1 < n && !memcmp(text + i + 1, delimiter, dl) && text[i + 1 + dl] == '"') {
                        i += dl + 2;
                        break;
                    }
                    i++;
                }
                continue;
            }
        }
        if (c == '"' || c == '\'') {
            i++;
            while (i < n && text[i] != '\n' && text[i] != '\r') {
                if (text[i] == '\\') { i = i + 2 < n ? i + 2 : n; continue; }
                if (text[i] == c) { i++; break; }
                i++;
            }
            continue;
        }
        i++;
    }
    free(text);
    return result;
}

bool msvc_without_sdk(Options o, Target t, Toolchain tc, Runtime r) {
    (void)t; (void)tc; (void)r;
    if (o.query || !o.has_input || compiler_plain_assembly(o.forwarded)) return true;
    bool operands = false;
    for (size_t i = 0; i < o.forwarded.len; i++) {
        const char *a = o.forwarded.data[i];
        if (streq(a, "--")) { operands = true; continue; }
        if (streq(a, "-")) return !o.msvc_accept_license;
        if (!operands && starts_with(a, "-")) {
            if (starts_with(a, "-include") || starts_with(a, "-imacros")) return false;
            if (takes_value(a)) i++;
            continue;
        }
        /* Object/archive inputs do not need SDK headers. Missing files are
           Clang's error, not a reason to acquire Microsoft resources. */
        const char *slash = strrchr(a, '/');
        const char *back = strrchr(a, '\\');
        if (back > slash) slash = back;
        const char *ext = strrchr(slash ? slash : a, '.');
        if (ext && (streq(ext, ".o") || streq(ext, ".obj") || streq(ext, ".a") || streq(ext, ".lib"))) continue;
        size_t size = 0;
        char *data = read_file(a, &size);
        if (!data) continue;
        bool needs = msvc_source_needs_sdk((const uint8_t *)data, size);
        if (needs) return false;
    }
    return true;
}
