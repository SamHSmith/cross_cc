/* `cross_cc objcopy`, ported from Zig 0.17.1 lib/compiler/objcopy.zig. Zig's objcopy is its own small Zig program, not
   llvm-objcopy: it converts a linked ELF to raw binary or Intel HEX (-O binary / ihex, --only-section, --pad-to) and
   rejects ELF-to-ELF work (--strip-*, --only-keep-debug, --add-section, ...) just as the pinned Zig does. Messages and
   exit codes follow Zig ("error: ..." on stderr, status 1). Entry point: run_objcopy(args) with args after "objcopy". */
#include "cross.h"
#include "resources.h"
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#define objcopy_open(path, flags, mode) _wopen(windows_wide(path), flags, _S_IREAD | _S_IWRITE)
#define objcopy_close _close
#define objcopy_write _write
#define objcopy_seek(fd, off) _lseeki64(fd, (__int64)(off), SEEK_SET)
#define objcopy_truncate(fd, size) _chsize_s(fd, (__int64)(size))
#define OBJCOPY_FLAGS (_O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY)
#else
#include <unistd.h>
#define objcopy_open open
#define objcopy_close close
#define objcopy_write write
#define objcopy_seek(fd, off) lseek(fd, (off_t)(off), SEEK_SET)
#define objcopy_truncate(fd, size) ftruncate(fd, (off_t)(size))
#define OBJCOPY_FLAGS (O_WRONLY | O_CREAT | O_TRUNC)
#endif

typedef struct Objcopy_Segment Objcopy_Segment;
typedef struct Objcopy_Section {
    uint64_t elf_offset, binary_offset, file_size;
    const char *name;
    bool has_name;
    Objcopy_Segment *segment;
} Objcopy_Section;

struct Objcopy_Segment {
    uint64_t physical_address, elf_offset, binary_offset, file_size;
    Objcopy_Section *first_section;
};

typedef enum { OBJCOPY_ELF, OBJCOPY_RAW, OBJCOPY_HEX, OBJCOPY_OTHER } Objcopy_Format;

static const char *objcopy_other_name = "";

static _Noreturn void objcopy_fatal(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    fputs("error: ", stderr);
    vfprintf(stderr, format, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static const char objcopy_usage[] =
    "Usage: cross_cc objcopy [options] input output\n"
    "\n"
    "Options:\n"
    "  -h, --help                              Print this help and exit\n"
    "  --output-target=<value>                 Format of the output file\n"
    "  -O <value>                              Alias for --output-target\n"
    "  --only-section=<section>                Remove all but <section>\n"
    "  -j <value>                              Alias for --only-section\n"
    "  --pad-to <addr>                         Pad the last section up to address <addr>\n"
    "  --strip-debug, -g                       Remove all debug sections from the output.\n"
    "  --strip-all, -S                         Remove all debug sections and symbol table from the output.\n"
    "  --only-keep-debug                       Strip a file, removing contents of any sections that would not be stripped by --strip-debug and leaving the debugging sections intact.\n"
    "  --add-gnu-debuglink=<file>              Creates a .gnu_debuglink section which contains a reference to <file> and adds it to the output file.\n"
    "  --extract-to <file>                     Extract the removed sections into <file>, and add a .gnu-debuglink section.\n"
    "  --compress-debug-sections               Compress DWARF debug sections with zlib\n"
    "  --set-section-alignment <name>=<align>  Set alignment of section <name> to <align> bytes. Must be a power of two.\n"
    "  --set-section-flags <name>=<file>       Set flags of section <name> to <flags> represented as a comma separated set of flags.\n"
    "  --add-section <name>=<file>             Add file content from <file> with the a new section named <name>.\n";

/* std.fmt.parseInt semantics for u64 with base 0 (prefixes 0x 0o 0b, '_' separators) or a fixed base. */
uint64_t objcopy_parse_uint(const char *text, int base, uint64_t max, char **error) {
    const char *s = nz(text);
    if (error) *error = NULL;
    if (s[0] == '+') s++;
    uint64_t b = (uint64_t)base;
    if (base == 0) {
        b = 10;
        if (strlen(s) >= 2 && s[0] == '0') {
            switch (s[1]) {
            case 'x': b = 16; s += 2; break;
            case 'o': b = 8; s += 2; break;
            case 'b': b = 2; s += 2; break;
            default: break;
            }
        }
    }
    size_t n = strlen(s);
    if (n == 0 || s[0] == '_' || s[n-1] == '_') { if (error) *error = "InvalidCharacter"; return 0; }
    bool overflow = false;
    uint64_t value = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '_' && base == 0) continue;
        uint64_t d;
        if (c >= '0' && c <= '9') d = (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'z') d = (uint64_t)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'Z') d = (uint64_t)(c - 'A') + 10;
        else { if (error) *error = "InvalidCharacter"; return 0; }
        if (d >= b) { if (error) *error = "InvalidCharacter"; return 0; }
        if (value > (max - d) / b) overflow = true; else value = value * b + d;
    }
    if (overflow) { if (error) *error = "Overflow"; return 0; }
    return value;
}

static bool objcopy_split_option(const char *option, char **first, char **second) {
    size_t n = strlen(option);
    if (n < 3) return false;
    for (size_t i = 1; i < n - 1; i++) {
        if (option[i] == '=') {
            if (first) *first = str_ncopy(option, i);
            if (second) *second = str_copy(option + i + 1);
            return true;
        }
    }
    return false;
}

static Objcopy_Format objcopy_format_of(const char *name) {
    if (streq(name, "binary") || streq(name, "raw")) return OBJCOPY_RAW;
    if (streq(name, "hex")) return OBJCOPY_HEX;
    if (streq(name, "elf")) return OBJCOPY_ELF;
    /* The remaining std.Target.ObjectFormat tags parse but are unsupported outputs. */
    const char *others[] = {"coff", "macho", "spirv", "wasm", "c", "plan9", "goff", "xcoff"};
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
        if (streq(name, others[i])) { objcopy_other_name = name; return OBJCOPY_OTHER; }
    }
    objcopy_fatal("invalid output format: '%s'", name);
}

static uint64_t objcopy_u16(const unsigned char *d, size_t o, bool big) {
    if (big) return (uint64_t)d[o] << 8 | (uint64_t)d[o+1];
    return (uint64_t)d[o] | (uint64_t)d[o+1] << 8;
}
static uint64_t objcopy_u32(const unsigned char *d, size_t o, bool big) {
    if (big) return objcopy_u16(d, o, big) << 16 | objcopy_u16(d, o+2, big);
    return objcopy_u16(d, o, big) | objcopy_u16(d, o+2, big) << 16;
}
static uint64_t objcopy_u64(const unsigned char *d, size_t o, bool big) {
    if (big) return objcopy_u32(d, o, big) << 32 | objcopy_u32(d, o+4, big);
    return objcopy_u32(d, o, big) | objcopy_u32(d, o+4, big) << 32;
}

static const char *objcopy_next_arg(StrVec args, size_t *i, const char *msg) {
    *i += 1;
    if (*i >= args.len) objcopy_fatal("%s", msg);
    return args.data[*i];
}

typedef struct { uint64_t name, type, flags, offset, size; } Objcopy_Shdr;

static bool objcopy_read_shdr(const unsigned char *data, size_t len, uint64_t at, bool is_64, bool big, Objcopy_Shdr *s) {
    uint64_t size = is_64 ? 64 : 40;
    memset(s, 0, sizeof *s);
    if (at + size > (uint64_t)len || at + size < at) return false;
    size_t o = (size_t)at;
    s->name = objcopy_u32(data, o, big);
    s->type = objcopy_u32(data, o+4, big);
    if (is_64) {
        s->flags = objcopy_u64(data, o+8, big); s->offset = objcopy_u64(data, o+24, big); s->size = objcopy_u64(data, o+32, big);
    } else {
        s->flags = objcopy_u32(data, o+8, big); s->offset = objcopy_u32(data, o+16, big); s->size = objcopy_u32(data, o+20, big);
    }
    return true;
}

static _Noreturn void objcopy_eof(void) { objcopy_fatal("EndOfStream"); }

static void objcopy_write_all(int fd, const void *bytes, size_t n) {
    const char *p = bytes;
    while (n > 0) {
        long w = (long)objcopy_write(fd, p, (unsigned)(n > 0x40000000 ? 0x40000000 : n));
        if (w <= 0) objcopy_fatal("unable to write output: %s", strerror(errno));
        p += w; n -= (size_t)w;
    }
}

static void objcopy_write_section(int fd, const unsigned char *data, size_t len, Objcopy_Section *sec) {
    if (sec->elf_offset + sec->file_size > (uint64_t)len || sec->elf_offset + sec->file_size < sec->elf_offset) objcopy_eof();
    if (sec->file_size == 0) return;
    objcopy_write_all(fd, data + sec->elf_offset, (size_t)sec->file_size);
}

static void objcopy_pad_file(int fd, bool have, uint64_t size) {
    if (!have) return;
    if (objcopy_truncate(fd, size) != 0) objcopy_fatal("unable to write output: %s", strerror(errno));
    if (objcopy_seek(fd, size) < 0) objcopy_fatal("unable to write output: %s", strerror(errno));
}

static uint64_t objcopy_file_size(int fd) {
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0) return 0;
#else
    struct stat st;
    if (fstat(fd, &st) != 0) return 0;
#endif
    return (uint64_t)st.st_size;
}

static void objcopy_record(Bytes *out, uint16_t address, uint8_t type, const unsigned char *payload, size_t n) {
    uint8_t sum = (uint8_t)((uint8_t)n + (uint8_t)(address >> 8) + (uint8_t)address + type);
    for (size_t i = 0; i < n; i++) sum = (uint8_t)(sum + payload[i]);
    char buf[16];
    snprintf(buf, sizeof buf, ":%02X%04X%02X", (unsigned)n, (unsigned)address, (unsigned)type);
    bytes_append(out, buf, strlen(buf));
    for (size_t i = 0; i < n; i++) { snprintf(buf, sizeof buf, "%02X", (unsigned)payload[i]); bytes_append(out, buf, 2); }
    snprintf(buf, sizeof buf, "%02X\r\n", (unsigned)(uint8_t)((sum ^ 0xFF) + 1));
    bytes_append(out, buf, 4);
}

int run_objcopy(StrVec args) {
    bool have_fmt = false;
    Objcopy_Format opt_out_fmt = OBJCOPY_ELF;
    const char *input = "", *output = "", *only_section = "";
    bool have_input = false, have_output = false, have_only = false, have_pad = false;
    uint64_t pad_to = 0;
    bool strip_any = false, have_extract = false, have_add = false, have_align = false, have_flags = false;

    for (size_t i = 0; i < args.len; i++) {
        const char *arg = args.data[i];
        char *expect = str_fmt("expected another argument after '%s'", arg);
        if (!starts_with(arg, "-")) {
            if (!have_input) { input = arg; have_input = true; }
            else if (!have_output) { output = arg; have_output = true; }
            else objcopy_fatal("unexpected positional argument: '%s'", arg);
        } else if (streq(arg, "-h") || streq(arg, "--help")) {
            fputs(objcopy_usage, stdout);
            fflush(stdout);
            exit(0);
        } else if (streq(arg, "-O") || streq(arg, "--output-target")) {
            opt_out_fmt = objcopy_format_of(objcopy_next_arg(args, &i, expect)); have_fmt = true;
        } else if (starts_with(arg, "--output-target=")) {
            opt_out_fmt = objcopy_format_of(arg + strlen("--output-target=")); have_fmt = true;
        } else if (streq(arg, "-j") || streq(arg, "--only-section")) {
            only_section = objcopy_next_arg(args, &i, expect); have_only = true;
        } else if (starts_with(arg, "--only-section=")) {
            only_section = arg + strlen("--only-section="); have_only = true;
        } else if (streq(arg, "--pad-to")) {
            const char *text = objcopy_next_arg(args, &i, expect);
            char *e = NULL;
            uint64_t v = objcopy_parse_uint(text, 0, UINT64_MAX, &e);
            if (e) objcopy_fatal("unable to parse: '%s': %s", text, e);
            pad_to = v; have_pad = true;
        } else if (streq(arg, "-g") || streq(arg, "--strip-debug") || streq(arg, "-S") || streq(arg, "--strip-all") || streq(arg, "--only-keep-debug")) {
            strip_any = true;
        } else if (streq(arg, "--compress-debug-sections")) {
        } else if (starts_with(arg, "--add-gnu-debuglink=")) {
        } else if (streq(arg, "--add-gnu-debuglink")) {
            objcopy_next_arg(args, &i, expect);
        } else if (starts_with(arg, "--extract-to=")) {
            have_extract = true;
        } else if (streq(arg, "--extract-to")) {
            objcopy_next_arg(args, &i, expect); have_extract = true;
        } else if (streq(arg, "--set-section-alignment")) {
            const char *text = objcopy_next_arg(args, &i, str_fmt("expected section name and alignment arguments after '%s'", arg));
            char *second = NULL;
            if (!objcopy_split_option(text, NULL, &second)) objcopy_fatal("unrecognized argument: '%s', expecting <name>=<alignment>", text);
            char *e = NULL;
            uint64_t v = objcopy_parse_uint(second, 10, UINT32_MAX, &e);
            if (e) objcopy_fatal("unable to parse alignment number: '%s': %s", second, e);
            if (v == 0 || (v & (v-1)) != 0) objcopy_fatal("alignment must be a power of two");
            have_align = true;
        } else if (streq(arg, "--set-section-flags")) {
            const char *text = objcopy_next_arg(args, &i, str_fmt("expected section name and filename arguments after '%s'", arg));
            if (!objcopy_split_option(text, NULL, NULL)) objcopy_fatal("unrecognized argument: '%s', expecting <name>=<flags>", text);
            have_flags = true;
        } else if (streq(arg, "--add-section")) {
            const char *text = objcopy_next_arg(args, &i, str_fmt("expected section name and filename arguments after '%s'", arg));
            if (!objcopy_split_option(text, NULL, NULL)) objcopy_fatal("unrecognized argument: '%s', expecting <name>=<file>", text);
            have_add = true;
        } else {
            objcopy_fatal("unrecognized argument: '%s'", arg);
        }
    }
    if (!have_input) objcopy_fatal("expected input parameter");
    if (!have_output) objcopy_fatal("expected output parameter");

    size_t len = 0;
    errno = 0;
    unsigned char *data = (unsigned char *)read_file(input, &len);
    if (!data) objcopy_fatal("failed to open %s: %s", input, strerror(errno ? errno : ENOENT));

    /* std.elf.Header.read */
    if (len < 64) objcopy_fatal("invalid elf file: EndOfStream");
    if (memcmp(data, "\x7f" "ELF", 4) != 0) objcopy_fatal("invalid elf file: InvalidElfMagic");
    if (data[6] != 1) objcopy_fatal("invalid elf file: InvalidElfVersion");
    bool is_64 = false, big = false;
    switch (data[4]) {
    case 1: is_64 = false; break;
    case 2: is_64 = true; break;
    default: objcopy_fatal("invalid elf file: InvalidElfClass");
    }
    switch (data[5]) {
    case 1: big = false; break;
    case 2: big = true; break;
    default: objcopy_fatal("invalid elf file: InvalidElfEndian");
    }
    uint64_t phoff, shoff, phnum, shnum, shstrndx;
    if (is_64) {
        phoff = objcopy_u64(data, 32, big); shoff = objcopy_u64(data, 40, big);
        phnum = objcopy_u16(data, 56, big); shnum = objcopy_u16(data, 60, big); shstrndx = objcopy_u16(data, 62, big);
    } else {
        phoff = objcopy_u32(data, 28, big); shoff = objcopy_u32(data, 32, big);
        phnum = objcopy_u16(data, 44, big); shnum = objcopy_u16(data, 48, big); shstrndx = objcopy_u16(data, 50, big);
    }

    Objcopy_Format out_fmt;
    if (have_fmt) out_fmt = opt_out_fmt;
    else if (ends_with(output, ".hex") || ends_with(output, ".ihex")) out_fmt = OBJCOPY_HEX;
    else if (ends_with(output, ".bin")) out_fmt = OBJCOPY_RAW;
    else out_fmt = OBJCOPY_ELF;

    /* Zig creates (truncates) the output before it checks the operation. */
    int fd = objcopy_open(output, OBJCOPY_FLAGS, 0666);
    if (fd < 0) objcopy_fatal("unable to create %s: %s", output, strerror(errno));

    switch (out_fmt) {
    case OBJCOPY_HEX: case OBJCOPY_RAW:
        if (strip_any) objcopy_fatal("cross_cc objcopy: ELF to RAW or HEX copying does not support --strip");
        if (have_extract) objcopy_fatal("cross_cc objcopy: ELF to RAW or HEX copying does not support --extract-to");
        if (have_add) objcopy_fatal("cross_cc objcopy: ELF to RAW or HEX copying does not support --add-section");
        if (have_align) objcopy_fatal("cross_cc objcopy: ELF to RAW or HEX copying does not support --set_section_alignment");
        if (have_flags) objcopy_fatal("cross_cc objcopy: ELF to RAW or HEX copying does not support --set_section_flags");
        break;
    case OBJCOPY_ELF:
        if (big) objcopy_fatal("cross_cc objcopy: ELF to ELF copying only supports native endian");
        if (phoff == 0) objcopy_fatal("cross_cc objcopy: ELF to ELF copying only supports programs");
        if (have_only) objcopy_fatal("cross_cc objcopy: ELF to ELF copying does not support --only-section");
        if (have_pad) objcopy_fatal("cross_cc objcopy: ELF to ELF copying does not support --pad-to");
        objcopy_fatal("unimplemented");
    case OBJCOPY_OTHER:
        objcopy_fatal("unsupported output object format: %s", objcopy_other_name);
    }

    /* BinaryElfOutput.parse */
    uint64_t shdr_size = is_64 ? 64 : 40;
    uint64_t phdr_size = is_64 ? 56 : 32;
    const unsigned char *shstrtab = NULL;
    size_t shstrtab_len = 0;
    bool have_shstrtab = false;
    if (shstrndx < shnum) {
        Objcopy_Shdr s;
        bool ok = objcopy_read_shdr(data, len, shoff + shdr_size * shstrndx, is_64, big, &s);
        if (!ok || s.offset + s.size > (uint64_t)len || s.offset + s.size < s.offset) objcopy_eof();
        shstrtab = data + s.offset; shstrtab_len = (size_t)s.size;
        have_shstrtab = true;
    }

    Objcopy_Section **sections = NULL;
    size_t nsections = 0;
    Objcopy_Segment **segments = NULL;
    size_t nsegments = 0;
    sections = xmalloc(sizeof *sections * (size_t)(shnum + 1));
    segments = xmalloc(sizeof *segments * (size_t)(phnum + 1));
    for (uint64_t n = 0; n < shnum; n++) {
        Objcopy_Shdr s;
        if (!objcopy_read_shdr(data, len, shoff + shdr_size * n, is_64, big, &s)) objcopy_eof();
        if (s.type != 8 && (s.flags & 2) == 2) { /* not SHT_NOBITS, SHF_ALLOC */
            Objcopy_Section *sec = xmalloc(sizeof *sec);
            memset(sec, 0, sizeof *sec);
            sec->elf_offset = s.offset;
            sec->file_size = s.size;
            sec->name = "";
            if (have_shstrtab && s.name < (uint64_t)shstrtab_len) {
                const unsigned char *rest = shstrtab + s.name;
                size_t rest_len = shstrtab_len - (size_t)s.name, end = 0;
                while (end < rest_len && rest[end] != 0) end++;
                sec->name = str_ncopy((const char *)rest, end);
                sec->has_name = true;
            }
            sections[nsections++] = sec;
        }
    }

    for (uint64_t n = 0; n < phnum; n++) {
        uint64_t at = phoff + phdr_size * n;
        if (at + phdr_size > (uint64_t)len || at + phdr_size < at) objcopy_eof();
        size_t o = (size_t)at;
        uint64_t ptype = objcopy_u32(data, o, big);
        uint64_t p_offset, p_paddr, p_filesz;
        if (is_64) {
            p_offset = objcopy_u64(data, o+8, big); p_paddr = objcopy_u64(data, o+24, big); p_filesz = objcopy_u64(data, o+32, big);
        } else {
            p_offset = objcopy_u32(data, o+4, big); p_paddr = objcopy_u32(data, o+12, big); p_filesz = objcopy_u32(data, o+16, big);
        }
        if (ptype != 1) continue; /* PT_LOAD */
        Objcopy_Segment *seg = xmalloc(sizeof *seg);
        memset(seg, 0, sizeof *seg);
        seg->physical_address = p_paddr;
        seg->file_size = p_filesz;
        seg->elf_offset = p_offset;
        for (size_t k = 0; k < nsections; k++) {
            Objcopy_Section *sec = sections[k];
            if (p_offset <= sec->elf_offset && p_offset + p_filesz >= sec->elf_offset + sec->file_size) {
                if (sec->segment) {
                    if (sec->segment->elf_offset > seg->elf_offset) sec->segment = seg;
                } else {
                    sec->segment = seg;
                }
                if (!seg->first_section) seg->first_section = sec;
            }
        }
        segments[nsegments++] = seg;
    }

    /* Stable insertion sorts, matching the original stable ordering. */
    for (size_t a = 1; a < nsegments; a++) {
        Objcopy_Segment *v = segments[a];
        size_t b = a;
        while (b > 0 && v->physical_address < segments[b-1]->physical_address) { segments[b] = segments[b-1]; b--; }
        segments[b] = v;
    }
    for (size_t i = 0; i < nsegments; i++) {
        Objcopy_Segment *first = segments[i];
        if (first->first_section) {
            uint64_t diff = first->first_section->elf_offset - first->elf_offset;
            first->elf_offset += diff;
            first->file_size += diff;
            first->physical_address += diff;
            uint64_t base = first->physical_address;
            for (size_t k = i + 1; k < nsegments; k++) segments[k]->binary_offset = segments[k]->physical_address - base;
            break;
        }
    }
    for (size_t k = 0; k < nsections; k++) {
        Objcopy_Section *sec = sections[k];
        if (sec->segment) sec->binary_offset = sec->segment->binary_offset + (sec->elf_offset - sec->segment->elf_offset);
    }
    for (size_t a = 1; a < nsections; a++) {
        Objcopy_Section *v = sections[a];
        size_t b = a;
        while (b > 0 && v->binary_offset < sections[b-1]->binary_offset) { sections[b] = sections[b-1]; b--; }
        sections[b] = v;
    }

    if (have_only) {
        if (out_fmt == OBJCOPY_HEX) objcopy_fatal("cross_cc objcopy: hex format with sections is not implemented yet");
        for (size_t k = 0; k < nsections; k++) {
            Objcopy_Section *sec = sections[k];
            if (!sec->has_name || !streq(sec->name, only_section)) continue;
            objcopy_write_section(fd, data, len, sec);
            objcopy_pad_file(fd, have_pad, pad_to);
            objcopy_close(fd);
            exit(0);
        }
        objcopy_fatal("SectionNotFound");
    }

    if (out_fmt == OBJCOPY_RAW) {
        for (size_t k = 0; k < nsections; k++) {
            Objcopy_Section *sec = sections[k];
            if (objcopy_seek(fd, sec->binary_offset) < 0) objcopy_fatal("unable to write output: %s", strerror(errno));
            objcopy_write_section(fd, data, len, sec);
            /* Zig's writer leaves the file ending at the seek target even when the section is empty. */
            if (objcopy_file_size(fd) < sec->binary_offset) (void)!objcopy_truncate(fd, sec->binary_offset);
        }
        objcopy_pad_file(fd, have_pad, pad_to);
    } else if (nsegments > 0) {
        for (size_t k = 0; k < nsegments; k++) {
            Objcopy_Segment *seg = segments[k];
            if (seg->file_size > UINT32_MAX || seg->physical_address > (uint64_t)UINT32_MAX - seg->file_size) objcopy_fatal("InvalidHexfileAddressRange");
        }
        Bytes out = {0};
        uint64_t prev_addr = 0;
        bool have_prev = false;
        for (size_t k = 0; k < nsegments; k++) {
            Objcopy_Segment *seg = segments[k];
            uint64_t done = 0;
            while (done < seg->file_size) {
                uint32_t row_address = (uint32_t)(seg->physical_address + done);
                uint64_t n = seg->file_size - done < 16 ? seg->file_size - done : 16;
                uint64_t start = seg->elf_offset + done;
                if (start + n > (uint64_t)len || start + n < start) objcopy_eof();
                const unsigned char *row = data + start;
                uint16_t addr16 = (uint16_t)(row_address % 0x10000);
                if (row_address > 0xFFFF && (!have_prev || (uint64_t)addr16 != prev_addr)) {
                    uint16_t segment = (uint16_t)(row_address / 0x10000);
                    unsigned char parts[2];
                    if (row_address > 0xFFFFF) {
                        parts[0] = (unsigned char)(segment >> 8); parts[1] = (unsigned char)segment;
                        objcopy_record(&out, 0, 4, parts, 2);
                    } else {
                        uint16_t shifted = (uint16_t)(segment << 12);
                        parts[0] = (unsigned char)(shifted >> 8); parts[1] = (unsigned char)shifted;
                        objcopy_record(&out, 0, 2, parts, 2);
                    }
                }
                objcopy_record(&out, addr16, 0, row, (size_t)n);
                prev_addr = (uint64_t)addr16 + n;
                have_prev = true;
                done += n;
            }
        }
        if (have_pad) {
            if (out.len) (void)!objcopy_write(fd, out.data, (unsigned)out.len);
            objcopy_fatal("InvalidArgument");
        }
        objcopy_record(&out, 0, 1, NULL, 0);
        objcopy_write_all(fd, out.data, out.len);
    }
    objcopy_close(fd);
    exit(0);
}
