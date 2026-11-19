/* cross_cc UBSan runtime: a semantic port of pinned Zig 0.17.1 lib/ubsan_rt.zig (MIT).
 *
 * One standalone C11 file for every supported target. It includes no headers:
 * all types come from Clang's predefined macros and all external functions are
 * declared here, so it builds with -nostdinc -ffreestanding for any target.
 * The public C ABI uses only pointers and uintptr_t value handles.
 *
 * Variants, selected by the runtime preparer:
 *   CROSS_CC_UBSAN_LINUX_SYMBOLS   bundled libbacktrace stack traces (Linux LP64)
 *   CROSS_CC_UBSAN_DARWIN_SYMBOLS  bundled libbacktrace stack traces (macOS)
 *   CROSS_CC_UBSAN_WINDOWS_SYMBOLS DbgHelp stack traces (loaded on the fatal path)
 * Byte order, pointer width, the stderr writer and the native thread ID are
 * chosen from the target's predefined macros.
 *
 * Every message is built in a bounded stack buffer: no allocation, no global
 * initialization, no thread-local storage. */

typedef __UINT8_TYPE__ u8;
typedef __UINT16_TYPE__ u16;
typedef __UINT32_TYPE__ u32;
typedef __UINT64_TYPE__ u64;
typedef __INT32_TYPE__ i32;
typedef __INT64_TYPE__ i64;
typedef __UINTPTR_TYPE__ uptr;
typedef __INTPTR_TYPE__ iptr;
typedef __SIZE_TYPE__ usize;

#define PTR_BITS (__SIZEOF_POINTER__ * 8)

/* Clang's supported 32-bit C targets have at most 64-bit integer types. */
#if __SIZEOF_POINTER__ == 8 && defined(__SIZEOF_INT128__)
typedef unsigned __int128 wide_u;
typedef __int128 wide_i;
#define WIDE_BITS 128
#else
typedef u64 wide_u;
typedef i64 wide_i;
#define WIDE_BITS 64
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define UBSAN_BIG_ENDIAN 1
#else
#define UBSAN_BIG_ENDIAN 0
#endif

#if defined(_WIN32)
#define UBSAN_EXPORT __attribute__((weak, used))
#else
#define UBSAN_EXPORT __attribute__((weak, used, visibility("hidden")))
#endif

#if defined(_WIN32) && defined(__i386__)
#define WINAPI __attribute__((stdcall))
#else
#define WINAPI
#endif

_Noreturn void abort(void);

/* ---- Platform shims: stderr writer and native thread ID ---------------- */

#if defined(_WIN32)
/* Windows _write takes a 32-bit count and returns int. */
int _write(int fd, const void *data, unsigned int count);
static void ubsan_write(const void *data, uptr count) { (void)_write(2, data, (unsigned int)count); }
#else
/* POSIX write uses pointer-sized ssize_t/size_t. */
iptr write(int fd, const void *data, usize count);
static void ubsan_write(const void *data, uptr count) { (void)write(2, data, (usize)count); }
#endif

/* Pinned std.Thread.getCurrentId: Linux gettid (not getpid/pthread_self) via
 * libc's long syscall(long, ...); numbers from pinned lib/std/os/linux/syscalls.zig. */
#if defined(__linux__)
#if defined(__x86_64__)
#define UBSAN_GETTID 186
#elif defined(__i386__) || defined(__arm__)
#define UBSAN_GETTID 224
#elif defined(__aarch64__) || defined(__riscv) || defined(__loongarch64)
#define UBSAN_GETTID 178
#elif defined(__mips__) && defined(_MIPS_SIM) && defined(_ABIO32) && _MIPS_SIM == _ABIO32
#define UBSAN_GETTID 4222
#elif defined(__mips__) && defined(_MIPS_SIM) && defined(_ABI64) && _MIPS_SIM == _ABI64
#define UBSAN_GETTID 5178
#elif defined(__powerpc64__)
#define UBSAN_GETTID 207
#elif defined(__s390x__)
#define UBSAN_GETTID 236
#endif
#endif

#if defined(UBSAN_GETTID)
long syscall(long number, ...);
static uptr ubsan_thread_id(void) { return (uptr)syscall(UBSAN_GETTID); }
#elif defined(_WIN32)
/* kernel32 GetCurrentThreadId, a u32 (__stdcall, _GetCurrentThreadId@0, on x86). */
WINAPI unsigned long GetCurrentThreadId(void);
static uptr ubsan_thread_id(void) { return (uptr)(u32)GetCurrentThreadId(); }
#elif defined(__APPLE__)
/* pthread_threadid_np(null, &u64), not pthread_self. Failure leaves 0 (bare header). */
int pthread_threadid_np(void *thread, u64 *id);
static uptr ubsan_thread_id(void) {
    u64 tid = 0;
    (void)pthread_threadid_np(0, &tid);
    return (uptr)tid;
}
#else
/* No validated native thread-ID API: keep the bare panic header. */
static uptr ubsan_thread_id(void) { return 0; }
#endif

/* ---- Message buffer ------------------------------------------------------ */

/* Pinned std.debug.panicExtra reserves 4096 message bytes; the panic header,
 * truncation marker and newline do not consume that budget. */
typedef struct { u8 data[4096]; int count; int truncated; } Buffer;
typedef struct { const char *file; u32 line, column; } Location;
typedef struct { u16 kind, info; } Type;
typedef struct { Location loc; const Type *td; } Overflow;
typedef struct { Location loc; const Type *lhs, *rhs; } Shift;
typedef struct { Location loc; const Type *array, *index; } Bounds;
typedef struct { Location loc; const Type *td; u8 alignment, kind; } Mismatch;
typedef struct { Location loc, assumption; const Type *td; } Alignment;
typedef struct { Location loc, attribute; i32 index; } Nonnull_Arg;
typedef struct { Location loc; u8 kind; } Invalid_Builtin;
typedef struct { Location loc; const Type *from, *to; } Float_Cast;
typedef struct { const Type *from, *to; } Float_Cast_V1;

static void bytes(Buffer *b, const void *data, int n) {
    const u8 *p = (const u8 *)data;
    for (int i = 0; i < n; i++) {
        if (b->count == (int)sizeof(b->data)) { b->truncated = 1; return; }
        b->data[b->count++] = p[i];
    }
}

static void text(Buffer *b, const char *s) {
    for (int i = 0; s[i] != 0; i++) {
        if (b->count == (int)sizeof(b->data)) { b->truncated = 1; return; }
        b->data[b->count++] = (u8)s[i];
    }
}

static void name(Buffer *b, const Type *td) {
    if (b->truncated) return;
    const u8 *p = (const u8 *)td + sizeof(Type);
    /* Inspect at most the remaining message capacity plus one terminator byte.
     * An exactly full message is not truncated unless another byte follows. */
    for (int i = 0; p[i] != 0; i++) {
        if (b->count == (int)sizeof(b->data)) { b->truncated = 1; return; }
        b->data[b->count++] = p[i];
    }
}

static void decimal(Buffer *b, wide_u n) {
    /* Decimal double-and-add avoids target-specific 128-bit division builtins. */
    u8 digits[40] = {0};
    for (int bit = WIDE_BITS - 1; bit >= 0; bit--) {
        u8 carry = (u8)((n >> bit) & 1);
        for (int i = (int)sizeof(digits) - 1; i >= 0; i--) {
            u8 digit = (u8)(digits[i] * 2 + carry);
            carry = digit >= 10 ? 1 : 0;
            digits[i] = digit >= 10 ? (u8)(digit - 10) : digit;
        }
    }
    int first = 0;
    while (first < (int)sizeof(digits) - 1 && digits[first] == 0) first++;
    for (int i = first; i < (int)sizeof(digits); i++) digits[i] = (u8)(digits[i] + '0');
    bytes(b, digits + first, (int)sizeof(digits) - first);
}

static void hexadecimal(Buffer *b, uptr n) {
    static const char alphabet[] = "0123456789abcdef";
    char digits[16];
    int i = (int)sizeof(digits);
    uptr v = n;
    for (;;) {
        digits[--i] = alphabet[v & 15];
        v >>= 4;
        if (v == 0) break;
    }
    bytes(b, digits + i, (int)sizeof(digits) - i);
}

/* ---- Value decoding ------------------------------------------------------ */

__attribute__((unused)) static u16 load_u16(uptr p) { u16 v; __builtin_memcpy(&v, (const void *)p, sizeof v); return v; }
__attribute__((unused)) static u32 load_u32(uptr p) { u32 v; __builtin_memcpy(&v, (const void *)p, sizeof v); return v; }
static u64 load_u64(uptr p) { u64 v; __builtin_memcpy(&v, (const void *)p, sizeof v); return v; }
static wide_u load_wide(uptr p) { wide_u v; __builtin_memcpy(&v, (const void *)p, sizeof v); return v; }

/* Shifts at or beyond the operand width yield 0, never undefined behavior. */
static uptr width(const Type *td) {
    unsigned s = (unsigned)(td->info >> 1);
    return s < PTR_BITS ? (uptr)1 << s : 0;
}
static int is_signed(const Type *td) { return td->kind == 0 && (td->info & 1) != 0; }

static wide_u unsigned_value(const Type *td, uptr h) {
    uptr bits = width(td);
    if (bits <= PTR_BITS) return (wide_u)h;
    /* Whole target-endian loads; s390x aligns 128-bit objects to only 8 bytes. */
    if (bits == 64) return (wide_u)load_u64(h);
    return load_wide(h);
}

static wide_i signed_value(const Type *td, uptr h) {
    uptr bits = width(td);
    wide_u v = unsigned_value(td, h);
    if (bits == 0 || bits > WIDE_BITS) return 0;
    unsigned shift = (unsigned)(WIDE_BITS - bits);
    return (wide_i)(v << shift) >> shift;
}

static int negative(const Type *td, uptr h) { return is_signed(td) && signed_value(td, h) < 0; }

static void floating(Buffer *b, const Type *td, uptr h);

static void value(Buffer *b, const Type *td, uptr h) {
    if (td->kind == 0) {
        if (is_signed(td)) {
            wide_i n = signed_value(td, h);
            if (n < 0) { text(b, "-"); decimal(b, (wide_u)(-(n + 1)) + 1); }
            else decimal(b, (wide_u)n);
        } else {
            decimal(b, unsigned_value(td, h));
        }
    } else if (td->kind == 1) {
        floating(b, td, h);
    } else {
        text(b, "(unknown)");
    }
}

/* Exact shortest binary128 decimal conversion using integer rounding intervals
 * (Dragon4). Unlike libc's %g, this preserves the value after Zig's widening to
 * f128. No floating arithmetic, allocation, tables or wide division is needed.
 * The pinned Writer.printFloat uses a 347-byte buffer even for f128. Values
 * outside +/-1150 binary exponents necessarily exceed that bound; rejecting
 * them first bounds every integer below 2^1280 (42 base-2^32 limbs suffice). */
typedef struct { u32 limb[42]; int n; } Float_Int;

static Float_Int fi_small(u32 v) {
    Float_Int a;
    for (int i = 0; i < 42; i++) a.limb[i] = 0;
    a.limb[0] = v;
    a.n = 1;
    return a;
}
static void fi_trim(Float_Int *a) { while (a->n > 1 && a->limb[a->n - 1] == 0) a->n--; }
static int fi_compare(const Float_Int *a, const Float_Int *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) {
        if (a->limb[i] != b->limb[i]) return a->limb[i] < b->limb[i] ? -1 : 1;
    }
    return 0;
}
static void fi_mul(Float_Int *a, u32 m) {
    u64 carry = 0;
    for (int i = 0; i < a->n; i++) {
        u64 v = (u64)a->limb[i] * m + carry;
        a->limb[i] = (u32)v;
        carry = v >> 32;
    }
    if (carry != 0) { a->limb[a->n] = (u32)carry; a->n++; }
}
static void fi_shift(Float_Int *a, int bits) {
    for (int i = 0; i < bits / 16; i++) fi_mul(a, 65536);
    fi_mul(a, (u32)1 << (bits % 16));
}
static Float_Int fi_add(const Float_Int *a, const Float_Int *b) {
    Float_Int r = fi_small(0);
    r.n = a->n > b->n ? a->n : b->n;
    u64 carry = 0;
    for (int i = 0; i < r.n; i++) {
        u64 v = (u64)a->limb[i] + b->limb[i] + carry;
        r.limb[i] = (u32)v;
        carry = v >> 32;
    }
    if (carry != 0) { r.limb[r.n] = (u32)carry; r.n++; }
    return r;
}
static void fi_sub(Float_Int *a, const Float_Int *b) {
    u64 borrow = 0;
    for (int i = 0; i < a->n; i++) {
        u64 sub = (u64)b->limb[i] + borrow;
        u64 v = a->limb[i];
        a->limb[i] = (u32)(v - sub);
        borrow = v < sub ? 1 : 0;
    }
    fi_trim(a);
}

#if !UBSAN_BIG_ENDIAN
/* Byte loads avoid imposing alignment requirements on external descriptors. */
static u32 float_word(const u8 *p, int n) {
    return (u32)p[n] | (u32)p[n + 1] << 8 | (u32)p[n + 2] << 16 | (u32)p[n + 3] << 24;
}
#endif

static void floating(Buffer *b, const Type *td, uptr h) {
    Float_Int m = fi_small(0);
    int exponent, fraction, bias, tag, special;
    int is_negative = 0;
    if (td->info <= PTR_BITS) {
        /* Intentional: pinned getFloat bitcasts to the pointer-sized FLOAT, not
         * the descriptor's float. In particular inline f32 on 64-bit is
         * interpreted as f64, NOT numerically converted from f32. */
#if __SIZEOF_POINTER__ == 8
        u64 v = (u64)h;
        m.limb[0] = (u32)v; m.limb[1] = (u32)(v >> 32) & 0xfffff; m.n = 2;
        tag = (int)((v >> 52) & 2047); is_negative = (v >> 63) != 0; fraction = 52; bias = 1023; special = 2047;
#else
        u32 v = (u32)h;
        m.limb[0] = v & 0x7fffff;
        tag = (int)((v >> 23) & 255); is_negative = (v >> 31) != 0; fraction = 23; bias = 127; special = 255;
#endif
    } else {
#if UBSAN_BIG_ENDIAN
        /* Pinned getFloat (ubsan_rt.zig:98-103) dereferences *const f64/f80/f128
         * whatever the C type was, so IBM double-double is read as binary128 too. */
        if (td->info == 64) {
            u64 v = load_u64(h);
            m.limb[0] = (u32)v; m.limb[1] = (u32)(v >> 32) & 0xfffff; m.n = 2;
            tag = (int)((v >> 52) & 2047); is_negative = (v >> 63) != 0; fraction = 52; bias = 1023; special = 2047;
        } else if (td->info == 80) {
            u16 se = load_u16(h);
            u64 v = load_u64(h + 2);
            m.limb[0] = (u32)v; m.limb[1] = (u32)(v >> 32) & 0x7fffffff; m.n = 2;
            tag = se & 32767; is_negative = (se >> 15) != 0; fraction = 63; bias = 16383; special = 32767;
        } else if (td->info == 128) {
            /* Two target-endian halves, most significant first. */
            u64 hi = load_u64(h), lo = load_u64(h + 8);
            m.limb[0] = (u32)lo; m.limb[1] = (u32)(lo >> 32); m.limb[2] = (u32)hi; m.limb[3] = (u32)(hi >> 32) & 0xffff; m.n = 4;
            tag = (int)((hi >> 48) & 32767); is_negative = (hi >> 63) != 0; fraction = 112; bias = 16383; special = 32767;
        } else { text(b, "(unknown)"); return; }
#else
        const u8 *p = (const u8 *)h;
        if (td->info == 64) {
            u32 hi = float_word(p, 4);
            m.limb[0] = float_word(p, 0); m.limb[1] = hi & 0xfffff; m.n = 2;
            tag = (int)((hi >> 20) & 2047); is_negative = (hi >> 31) != 0; fraction = 52; bias = 1023; special = 2047;
        } else if (td->info == 80) {
            /* The explicit integer bit is not part of NaN's payload. */
            m.limb[0] = float_word(p, 0); m.limb[1] = float_word(p, 4) & 0x7fffffff; m.n = 2;
            tag = (int)((u32)p[8] | (u32)(p[9] & 127) << 8); is_negative = (p[9] & 128) != 0;
            fraction = 63; bias = 16383; special = 32767;
        } else if (td->info == 128) {
            for (int i = 0; i < 4; i++) m.limb[i] = float_word(p, 4 * i);
            m.n = 4;
            u32 hi = m.limb[3];
            m.limb[3] &= 0xffff;
            tag = (int)((hi >> 16) & 32767); is_negative = (hi >> 31) != 0; fraction = 112; bias = 16383; special = 32767;
        } else { text(b, "(unknown)"); return; }
#endif
    }
    fi_trim(&m);
    int zero = m.n == 1 && m.limb[0] == 0;
    if (tag == special) {
        if (is_negative) text(b, "-");
        text(b, zero ? "inf" : "nan");
        return;
    }
    if (tag == 0 && zero) {
        if (is_negative) text(b, "-");
        text(b, "0");
        return;
    }
    exponent = (tag > 1 ? tag : 1) - bias - fraction;
    if (tag != 0) {
        m.limb[fraction / 32] |= (u32)1 << (fraction % 32);
        m.n = fraction / 32 + 1;
    }
    /* Widen exactly to a normalized 113-bit significand, including subnormals. */
    while (m.n < 4 || m.limb[3] < 0x10000) { fi_mul(&m, 2); exponent--; }
    if (exponent + 112 < -1150 || exponent + 112 > 1150) { text(b, "(float)"); return; }
    int even = (m.limb[0] & 1) == 0;
    int power_two = m.limb[3] == 0x10000 && m.limb[2] == 0 && m.limb[1] == 0 && m.limb[0] == 0;
    Float_Int r = m;
    fi_mul(&r, 4);
    Float_Int s = fi_small(1), lower = fi_small(power_two ? 1 : 2), upper = fi_small(2);
    if (exponent >= 2) {
        fi_shift(&r, exponent - 2); fi_shift(&lower, exponent - 2); fi_shift(&upper, exponent - 2);
    } else {
        fi_shift(&s, 2 - exponent);
    }
    /* Scale to [1,10) without approximation to log10 or powers of five. */
    int k = 0;
    if (fi_compare(&r, &s) >= 0) {
        for (;;) {
            Float_Int next = s;
            fi_mul(&next, 10);
            if (fi_compare(&r, &next) < 0) break;
            s = next;
            k++;
        }
    } else {
        while (fi_compare(&r, &s) < 0) { fi_mul(&r, 10); fi_mul(&lower, 10); fi_mul(&upper, 10); k--; }
    }
    u8 digits[40];
    int n = 0;
    for (;;) {
        u8 digit = 0;
        while (fi_compare(&r, &s) >= 0) { fi_sub(&r, &s); digit++; }
        int lc = fi_compare(&r, &lower);
        int lo = lc < 0 || (even && lc == 0);
        Float_Int sum = fi_add(&r, &upper);
        int hc = fi_compare(&sum, &s);
        int hi = hc > 0 || (even && hc == 0);
        digits[n++] = digit;
        if (lo || hi) {
            Float_Int twice = r;
            fi_mul(&twice, 2);
            int cmp = fi_compare(&twice, &s);
            if (hi && (!lo || cmp > 0 || (cmp == 0 && (digit & 1) != 0))) {
                int i = n - 1;
                while (i >= 0 && digits[i] == 9) { digits[i] = 0; i--; }
                if (i >= 0) digits[i]++;
                else { digits[0] = 1; n = 1; k++; }
            }
            break;
        }
        fi_mul(&r, 10); fi_mul(&lower, 10); fi_mul(&upper, 10);
    }
    while (n > 1 && digits[n - 1] == 0) n--;
    int e10 = k + 1 - n;
    int abs_e10 = e10 < 0 ? -e10 : e10;
    /* Mirror std.fmt.float.formatDecimal's conservative capacity check exactly. */
    if (2 + abs_e10 + n > 347) { text(b, "(float)"); return; }
    if (is_negative) text(b, "-");
    int point = k + 1;
    if (point <= 0) {
        text(b, "0.");
        for (int i = 0; i < -point; i++) text(b, "0");
    }
    for (int i = 0; i < n; i++) {
        if (point > 0 && i == point) text(b, ".");
        u8 c = (u8)(digits[i] + '0');
        bytes(b, &c, 1);
    }
    for (int i = n; i < point; i++) text(b, "0");
}

/* ---- Stack traces: Linux/Darwin libbacktrace ---------------------------- */

#if defined(CROSS_CC_UBSAN_LINUX_SYMBOLS) || defined(CROSS_CC_UBSAN_DARWIN_SYMBOLS)
/* O_RDONLY | O_NONBLOCK | O_CLOEXEC: Linux 0x80800; Darwin sys/fcntl.h 0x4|0x01000000. */
#if defined(CROSS_CC_UBSAN_DARWIN_SYMBOLS)
#define OPEN_FLAGS 0x1000004
#else
#define OPEN_FLAGS 0x80800
#endif
struct backtrace_state;
typedef void (*backtrace_error_callback)(void *data, const char *msg, int errnum);
typedef int (*backtrace_full_callback)(void *data, uptr pc, const char *filename, int lineno, const char *function);
typedef int (*backtrace_simple_callback)(void *data, uptr pc);
typedef void (*backtrace_syminfo_callback)(void *data, uptr pc, const char *symname, uptr symval, uptr symsize);
struct backtrace_state *backtrace_create_state(const char *filename, int threaded, backtrace_error_callback error_callback, void *data);
int backtrace_simple(struct backtrace_state *state, int skip, backtrace_simple_callback callback, backtrace_error_callback error_callback, void *data);
int backtrace_pcinfo(struct backtrace_state *state, uptr pc, backtrace_full_callback callback, backtrace_error_callback error_callback, void *data);
int backtrace_syminfo(struct backtrace_state *state, uptr addr, backtrace_syminfo_callback callback, backtrace_error_callback error_callback, void *data);
int open(const char *path, int flags, ...);
iptr read(int fd, void *data, usize count);
i64 lseek(int fd, i64 offset, int whence);
int close(int fd);

static void stack_cstring(Buffer *b, const char *s, const char *fallback) {
    if (s == 0) { text(b, fallback); return; }
    for (int i = 0; i < 4096 && s[i] != 0; i++) bytes(b, s + i, 1);
}

/* Never consult a UBSan descriptor Location: stripped programs have no
 * trustworthy source location. Only a real DWARF callback may read a file.
 * Best-effort excerpt: read at most 64 KiB from a small seekable file;
 * missing/large files and late lines are omitted, with no diagnostic noise. */
static void stack_source(const char *file, int line) {
    if (file == 0 || line <= 0) return;
    int fd = open(file, OPEN_FLAGS);
    if (fd < 0) return;
    i64 size = lseek(fd, 0, 2);
    if (size <= 0 || size > 1048576 || lseek(fd, 0, 0) < 0) { close(fd); return; }
    u8 chunk[512];
    u8 source[512];
    int count = 0;
    int current = 1;
    for (int scanned = 0; scanned < 65536;) {
        int want = 65536 - scanned < (int)sizeof(chunk) ? 65536 - scanned : (int)sizeof(chunk);
        iptr n = read(fd, chunk, (usize)want);
        if (n <= 0) break;
        scanned += (int)n;
        for (iptr i = 0; i < n; i++) {
            u8 c = chunk[i];
            if (current == line) {
                if (c == '\n') {
                    ubsan_write("    ", 4);
                    ubsan_write(source, (uptr)count);
                    ubsan_write("\n", 1);
                    close(fd);
                    return;
                }
                /* Do not copy terminal control bytes from a source file. */
                if (count < (int)sizeof(source)) source[count++] = c >= 32 || c == '\t' ? c : (u8)' ';
            }
            if (c == '\n') current++;
        }
    }
    close(fd);
}

static void stack_error(void *data, const char *msg, int err) { (void)data; (void)msg; (void)err; }

static int stack_frame(void *data, uptr pc, const char *file, int line, const char *fn) {
    u32 *count = (u32 *)data;
    if (pc == 0 || pc == ~(uptr)0) return 1;
    Buffer b;
    b.count = 0; b.truncated = 0;
    text(&b, "  #"); decimal(&b, (wide_u)*count); text(&b, " 0x"); hexadecimal(&b, pc);
    text(&b, " in "); stack_cstring(&b, fn, "??");
    if (file != 0 && line > 0) {
        text(&b, " at "); stack_cstring(&b, file, "??"); text(&b, ":"); decimal(&b, (wide_u)line);
    }
    text(&b, "\n");
    ubsan_write(b.data, (uptr)b.count);
    stack_source(file, line);
    *count += 1;
    return *count >= 64 ? 1 : 0;
}

typedef struct { u32 count, depth; struct backtrace_state *state; } Stack_Walk;

static void stack_symbol(void *data, uptr pc, const char *symname, uptr value_, uptr size) {
    (void)value_; (void)size;
    (void)stack_frame(data, pc, 0, 0, symname);
}

static int stack_pc(void *data, uptr pc) {
    Stack_Walk *walk = (Stack_Walk *)data;
    if (pc == 0 || pc == ~(uptr)0 || walk->count >= 64 || walk->depth >= 64) return 1;
    walk->depth++;
    u32 before = walk->count;
    if (walk->state != 0) {
        /* Keep all DWARF inline callbacks, but do not let missing debug info
         * suppress a real unwound PC (macho_nodebug calls only stack_error). */
        (void)backtrace_pcinfo(walk->state, pc, stack_frame, stack_error, &walk->count);
        if (walk->count == before) (void)backtrace_syminfo(walk->state, pc, stack_symbol, stack_error, &walk->count);
    }
    if (walk->count == before) (void)stack_frame(&walk->count, pc, 0, 0, 0);
    return walk->count >= 64 || walk->depth >= 64 ? 1 : 0;
}

__attribute__((noinline)) static void posix_backtrace(void) {
    /* Fatal-only, fresh state: no global initializer, lazy-state race or
     * allocator. libbacktrace uses its mmap allocator and atomic operations. */
    Stack_Walk walk;
    walk.count = 0; walk.depth = 0;
    walk.state = backtrace_create_state(0, 1, stack_error, 0);
    /* simple.c walks independently of symbol/DWARF availability, even if state
     * creation failed. Symbolization cannot stop the physical walk. */
    (void)backtrace_simple(walk.state, 1, stack_pc, stack_error, &walk);
}
#endif

/* ---- Stack traces: Windows DbgHelp -------------------------------------- */

#if defined(CROSS_CC_UBSAN_WINDOWS_SYMBOLS)
/* Native Windows layouts preserve WinSDK's pack(8), including on x86 where
 * the MSVC/MinGW ABIs already align 64-bit members to 8 bytes.
 * There are no dbghelp imports: only the OS copy is loaded, on the fatal path. */
typedef struct {
    u32 size, type_index;
    u64 reserved[2];
    u32 index, symbol_size;
    u64 mod_base;
    u32 flags;
    u64 value, address;
    u32 register_, scope, tag, name_len, max_name_len;
    u16 name[1];
} Win_Symbol;
typedef struct {
    u32 size;
    void *key;
    u32 line;
    u16 *file;
    u64 address;
} Win_Line;
_Static_assert(sizeof(Win_Symbol) == 88 && __builtin_offsetof(Win_Symbol, name) == 84, "SYMBOL_INFOW layout");
_Static_assert(__builtin_offsetof(Win_Symbol, value) == 48, "SYMBOL_INFOW layout");
#if __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(Win_Line) == 24 && __builtin_offsetof(Win_Line, address) == 16, "IMAGEHLP_LINEW64 layout");
#else
_Static_assert(sizeof(Win_Line) == 40 && __builtin_offsetof(Win_Line, address) == 32, "IMAGEHLP_LINEW64 layout");
#endif
typedef struct {
    u32 size;
    u64 base;
    u32 image_size, timestamp, checksum, num_syms, sym_type;
    u16 module_name[32];
    u16 image_name[256], loaded_image_name[256], loaded_pdb_name[256];
    u32 cv_sig;
    u16 cv_data[780];
    u32 pdb_sig;
    u8 pdb_guid[16];
    u32 pdb_age;
    i32 pdb_unmatched, dbg_unmatched, line_numbers, global_symbols;
    i32 type_info, source_indexed, publics;
    u32 machine, reserved;
} Win_Module;
_Static_assert(sizeof(Win_Module) == 3264 && __builtin_offsetof(Win_Module, pdb_guid) == 3204 && __builtin_offsetof(Win_Module, pdb_age) == 3220, "IMAGEHLP_MODULEW64 layout");
/* Pinned mingw-w64 winnt.h MEMORY_BASIC_INFORMATION (native pointer width). */
typedef struct {
    void *base, *allocation_base;
    u32 allocation_protect;
#if __SIZEOF_POINTER__ == 8
    u16 partition_id;
#endif
    uptr region_size;
    u32 state, protect, kind;
} Win_Memory;
#if __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(Win_Memory) == 28 && __builtin_offsetof(Win_Memory, region_size) == 12, "MEMORY_BASIC_INFORMATION layout");
_Static_assert(__builtin_offsetof(Win_Memory, state) == 16 && __builtin_offsetof(Win_Memory, protect) == 20 && __builtin_offsetof(Win_Memory, kind) == 24, "MEMORY_BASIC_INFORMATION layout");
#else
_Static_assert(sizeof(Win_Memory) == 48 && __builtin_offsetof(Win_Memory, region_size) == 24, "MEMORY_BASIC_INFORMATION layout");
_Static_assert(__builtin_offsetof(Win_Memory, state) == 32 && __builtin_offsetof(Win_Memory, protect) == 36 && __builtin_offsetof(Win_Memory, kind) == 40, "MEMORY_BASIC_INFORMATION layout");
#endif

typedef i32 (WINAPI *Win_Info)(void *, u64, Win_Module *);
typedef i32 (WINAPI *Win_Init)(void *, const u16 *, i32);
typedef u32 (WINAPI *Win_Options)(u32);
typedef u32 (WINAPI *Win_Get_Options)(void);
typedef u64 (WINAPI *Win_Load)(void *, void *, const u16 *, const u16 *, u64, u32, void *, u32);
typedef i32 (WINAPI *Win_Sym)(void *, u64, u64 *, Win_Symbol *);
typedef i32 (WINAPI *Win_Get_Line)(void *, u64, u32 *, Win_Line *);
typedef i32 (WINAPI *Win_Cleanup)(void *);
typedef void (*Win_Proc)(void);

WINAPI u16 RtlCaptureStackBackTrace(u32, u32, void **, u32 *);
WINAPI uptr VirtualQuery(const void *, Win_Memory *, uptr);
WINAPI u32 GetSystemDirectoryW(u16 *, u32);
WINAPI void *LoadLibraryExW(const u16 *, void *, u32);
WINAPI Win_Proc GetProcAddress(void *, const char *);
WINAPI i32 FreeLibrary(void *);
WINAPI i32 GetModuleHandleExW(u32, const void *, void **);
WINAPI u32 GetModuleFileNameW(void *, u16 *, u32);
WINAPI u32 GetDriveTypeW(const u16 *);
WINAPI u32 GetFileAttributesW(const u16 *);
WINAPI void *CreateFileW(const u16 *, u32, u32, void *, u32, u32, void *);
WINAPI u32 GetFileType(void *);
WINAPI i32 GetFileSizeEx(void *, i64 *);
WINAPI i32 ReadFile(void *, void *, u32, u32 *, void *);
WINAPI i32 CloseHandle(void *);

/* Reject UNC, device paths, alternate streams, mapped network drives and
 * relative paths before opening anything. Only ordinary local disk files. */
static int win_local_path(const u16 *path) {
    if (path == 0) return 0;
    int n = 0;
    while (n < 1024 && path[n] != 0) n++;
    if (n < 3 || n == 1024) return 0;
    if (!((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) || path[1] != ':' || (path[2] != '\\' && path[2] != '/')) return 0;
    for (int i = 2; i < n; i++) {
        u16 c = path[i];
        if (c < 32 || c == ':' || c == '?' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|') return 0;
    }
    u16 root[4] = {path[0], ':', '\\', 0};
    u32 drive = GetDriveTypeW(root);
    if (drive != 2 && drive != 3 && drive != 5 && drive != 6) return 0;
    /* Check every prefix before traversing it: a local-looking path may
     * otherwise enter a junction/symlink targeting a network share/pipe.
     * Reject DOS normalization aliases (dot components/trailing dot/space). */
    u16 prefix[1024];
    for (int i = 0; i < n; i++) prefix[i] = path[i];
    prefix[n] = 0;
    int start = 3;
    for (int i = 3; i <= n; i++) {
        if (i != n && path[i] != '\\' && path[i] != '/') continue;
        if (i == start || path[i - 1] == '.' || path[i - 1] == ' ') return 0;
        u16 saved = prefix[i];
        prefix[i] = 0;
        u32 attributes = GetFileAttributesW(prefix);
        prefix[i] = saved;
        if (attributes == ~(u32)0 || (attributes & 0x440) != 0) return 0; /* REPARSE_POINT | DEVICE */
        start = i + 1;
    }
    return 1;
}

static void *win_open(const u16 *path) {
    if (!win_local_path(path)) return 0;
    /* read / share all / existing / sequential + open reparse point */
    void *h = CreateFileW(path, 0x80000000u, 7, 0, 3, 0x08200000u, 0);
    if ((uptr)h == ~(uptr)0) return 0;
    if (GetFileType(h) != 1) { CloseHandle(h); return 0; }
    return h;
}

static void win_wtext(Buffer *b, const u16 *s, int limit) {
    if (s == 0) { text(b, "??"); return; }
    int bound = limit < 1024 ? limit : 1024;
    for (int i = 0; i < bound && s[i] != 0; i++) {
        u32 c = s[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < bound && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + (u32)s[i + 1] - 0xdc00;
            i++;
        } else if (c >= 0xd800 && c <= 0xdfff) {
            c = 0xfffd;
        }
        u8 out[4] = {0, 0, 0, 0};
        int n = 1;
        if (c < 0x80) out[0] = c >= 32 ? (u8)c : (u8)' ';
        else if (c < 0x800) { out[0] = (u8)(0xc0 | (c >> 6)); out[1] = (u8)(0x80 | (c & 63)); n = 2; }
        else if (c < 0x10000) { out[0] = (u8)(0xe0 | (c >> 12)); out[1] = (u8)(0x80 | ((c >> 6) & 63)); out[2] = (u8)(0x80 | (c & 63)); n = 3; }
        else { out[0] = (u8)(0xf0 | (c >> 18)); out[1] = (u8)(0x80 | ((c >> 12) & 63)); out[2] = (u8)(0x80 | ((c >> 6) & 63)); out[3] = (u8)(0x80 | (c & 63)); n = 4; }
        bytes(b, out, n);
    }
}

static void win_source(const u16 *file, u32 line) {
    if (line == 0 || line > 65536) return;
    void *h = win_open(file);
    if (h == 0) return;
    i64 size = 0;
    if (GetFileSizeEx(h, &size) == 0 || size <= 0 || size > 1048576) { CloseHandle(h); return; }
    u8 chunk[512], source[512];
    int count = 0;
    u32 current = 1;
    int complete = 0;
    for (int scanned = 0; scanned < 65536;) {
        u32 n = 0;
        u32 want = 65536 - scanned < (int)sizeof(chunk) ? (u32)(65536 - scanned) : (u32)sizeof(chunk);
        if (ReadFile(h, chunk, want, &n, 0) == 0) { CloseHandle(h); return; }
        if (n == 0) { complete = current == line; break; }
        scanned += (int)n;
        for (u32 i = 0; i < n; i++) {
            u8 c = chunk[i];
            if (current == line && c != '\n' && count < (int)sizeof(source)) source[count++] = c >= 32 || c == '\t' ? c : (u8)' ';
            if (c == '\n') {
                if (current == line) { complete = 1; break; }
                current++;
            }
        }
        if (complete) break;
    }
    if (complete) {
        ubsan_write("    ", 4);
        ubsan_write(source, (uptr)count);
        ubsan_write("\n", 1);
    }
    CloseHandle(h);
}

/* SizeOfImage is only an arithmetic bound: discardable debug pages may no
 * longer be committed/readable. One query per span; crossing a region is
 * conservatively rejected. No faulting probe. External unmap/protect races
 * cannot be prevented by VirtualQuery; this is best effort, not a lifetime lock. */
static int win_image_readable(void *module, uptr offset, uptr count, uptr limit) {
    uptr base = (uptr)module;
    if (base == 0 || count == 0 || limit > ~(uptr)0 - base || offset >= limit || count > limit - offset) return 0;
    uptr p = base + offset;
    Win_Memory m;
    if (VirtualQuery((const void *)p, &m, sizeof(Win_Memory)) != sizeof(Win_Memory)) return 0;
    /* MEM_COMMIT, MEM_IMAGE, PAGE_GUARD */
    if (m.state != 0x1000 || m.kind != 0x1000000 || m.allocation_base != module || (m.protect & 0x100) != 0) return 0;
    switch (m.protect & 0xff) {
    case 0x02: case 0x04: case 0x08: case 0x20: case 0x40: case 0x80: break; /* readable variants */
    default: return 0;
    }
    uptr region = (uptr)m.base;
    if (m.region_size == 0 || m.region_size > ~(uptr)0 - region || p < region || p - region >= m.region_size) return 0;
    return count <= m.region_size - (p - region);
}

static int win_pdb(void *process, void *module, Win_Load load, Win_Info info) {
    /* Require a genuine mapped RSDS record. In particular an adjacent stale
     * PDB must never invent source locations for a stripped/-g0 executable. */
    uptr base = (uptr)module;
    /* A small header cap applies until the guarded optional header supplies
     * SizeOfImage. Every loaded-image dereference has its own bounded span. */
    if (!win_image_readable(module, 0, 64, 4096) || load_u16(base) != 0x5a4d) return 0;
    uptr pe = (uptr)load_u32(base + 0x3c);
    if (pe > 3072 || !win_image_readable(module, pe, 24, 4096) || load_u32(base + pe) != 0x4550) return 0;
    uptr optional_size = (uptr)load_u16(base + pe + 20);
    uptr optional = base + pe + 24;
    if (optional_size < 2 || !win_image_readable(module, pe + 24, optional_size, 4096)) return 0;
    u16 magic = load_u16(optional);
    if (magic != 0x10b && magic != 0x20b) return 0;
    uptr directory_offset = magic == 0x10b ? 96 : 112;
    if (optional_size < directory_offset + 56) return 0;
    u32 size = load_u32(optional + 56);
    if ((uptr)size < pe + 24 + optional_size || (uptr)size > ~(uptr)0 - base) return 0;
    uptr directories = optional + directory_offset;
    if (load_u32(directories - 4) <= 6) return 0;
    u32 rva = load_u32(directories + 48);
    u32 count = load_u32(directories + 52);
    if (rva == 0 || count < 28 || count > 28 * 128 || rva >= size || count > size - rva) return 0;
    int found = 0;
    u8 guid[16];
    u32 age = 0;
    for (u32 i = 0; i + 28 <= count; i += 28) {
        if (!win_image_readable(module, (uptr)(rva + i), 28, (uptr)size)) return 0;
        uptr d = base + (uptr)(rva + i);
        if (load_u32(d + 12) != 2) continue;
        u32 length = load_u32(d + 16), data = load_u32(d + 20);
        if (data == 0 || data >= size || length < 24 || length > size - data) continue;
        if (!win_image_readable(module, (uptr)data, 24, (uptr)size)) continue;
        uptr rsds = base + (uptr)data;
        if (load_u32(rsds) != 0x53445352) continue;
        /* Snapshot the entire checked identity before any DbgHelp/file call. */
        age = load_u32(rsds + 20);
        for (int j = 0; j < 16; j++) guid[j] = ((const u8 *)(rsds + 4))[j];
        found = 1;
        break;
    }
    if (!found) return 0;
    u16 path[1024];
    int n = (int)GetModuleFileNameW(module, path, 1024);
    if (n <= 4 || n >= 1024 - 4) return 0;
    int dot = n;
    for (int j = n - 1; j >= 0 && path[j] != '\\' && path[j] != '/'; j--) {
        if (path[j] == '.') { dot = j; break; }
    }
    /* Explicit adjacent PDB only. Never hand DbgHelp an executable image,
     * embedded CodeView path, source-server request or default search path. */
    path[dot] = '.'; path[dot + 1] = 'p'; path[dot + 2] = 'd'; path[dot + 3] = 'b'; path[dot + 4] = 0;
    void *h = win_open(path);
    if (h == 0) return 0;
    int ok = 0;
    if (load(process, h, path, 0, (u64)base, size, 0, 0) != 0) {
        Win_Module m;
        __builtin_memset(&m, 0, sizeof m);
        m.size = (u32)sizeof(Win_Module);
        /* A direct PDB load has no image for DbgHelp to compare and therefore
         * sets PdbUnmatched even for the right PDB. Verify its GUID and age
         * ourselves against the loaded image's checked RSDS identity instead. */
        if (info(process, (u64)base, &m) != 0 && m.pdb_age == age) {
            ok = 1;
            for (int i = 0; i < 16; i++) if (m.pdb_guid[i] != guid[i]) ok = 0;
        }
    }
    CloseHandle(h);
    return ok;
}

/* Never wait on another crashing UBSan thread: it still prints real PCs.
 * A private nonzero session token avoids the application's DbgHelp session;
 * invade=false and explicit PDB loading need no real process handle.
 * DbgHelp itself and its option mask remain process-global, not thread safe:
 * external concurrent users can still race (including option save/restore).
 * Local-only IO and bounded loops do not promise a hard timeout in OS APIs. */
static u32 win_symbols_busy;
static u8 win_symbols_token;

__attribute__((noinline)) static void windows_backtrace(void) {
    void *pcs[64];
    int count = (int)RtlCaptureStackBackTrace(1, 64, pcs, 0);
    void *dll = 0;
    void *process = &win_symbols_token;
    Win_Sym from_addr = 0;
    Win_Get_Line get_line = 0;
    Win_Load load = 0;
    Win_Info info = 0;
    Win_Cleanup cleanup = 0;
    Win_Options options = 0;
    u32 previous_options = 0;
    int changed_options = 0;
    int initialized = 0;
    int own = __atomic_exchange_n(&win_symbols_busy, 1u, __ATOMIC_SEQ_CST) == 0;
    if (own) {
        /* Absolute OS path; no cwd/PATH DLL lookup or redistributable DLL. */
        u16 path[1024];
        static const char suffix[] = "\\dbghelp.dll";
        int n = (int)GetSystemDirectoryW(path, 1024);
        int suffix_len = (int)sizeof(suffix) - 1;
        if (n > 0 && n + suffix_len < 1024) {
            for (int i = 0; i <= suffix_len; i++) path[n + i] = (u16)suffix[i];
            dll = LoadLibraryExW(path, 0, 0x00000800); /* SEARCH_SYSTEM32 */
        }
        if (dll != 0) {
            Win_Init init = (Win_Init)GetProcAddress(dll, "SymInitializeW");
            Win_Get_Options get_options;
            options = (Win_Options)GetProcAddress(dll, "SymSetOptions");
            get_options = (Win_Get_Options)GetProcAddress(dll, "SymGetOptions");
            from_addr = (Win_Sym)GetProcAddress(dll, "SymFromAddrW");
            get_line = (Win_Get_Line)GetProcAddress(dll, "SymGetLineFromAddrW64");
            load = (Win_Load)GetProcAddress(dll, "SymLoadModuleExW");
            cleanup = (Win_Cleanup)GetProcAddress(dll, "SymCleanup");
            info = (Win_Info)GetProcAddress(dll, "SymGetModuleInfoW64");
            if (init != 0 && options != 0 && get_options != 0 && from_addr != 0 && get_line != 0 && load != 0 && cleanup != 0 && info != 0) {
                /* LOAD_LINES | UNDNAME | EXACT_SYMBOLS | IGNORE_NT_SYMPATH |
                 * IGNORE_CVREC | NO_IMAGE_SEARCH | NO_PROMPTS |
                 * FAIL_CRITICAL_ERRORS | DISABLE_SYMSRV_AUTODETECT.
                 * No source/symbol server plugins are requested. Do not set
                 * SECURE: DbgHelp makes that bit sticky, so the application's
                 * original options could not be restored after our session. */
                previous_options = get_options(); /* SymSetOptions returns the NEW mask. */
                options(0x020a1692);
                changed_options = 1;
                u16 empty[1] = {0};
                initialized = init(process, empty, 0) != 0;
            }
        }
    }
    void *modules[64];
    u8 verified[64];
    int module_count = 0;
    for (int i = 0; i < count; i++) {
        void *pc = pcs[i];
        Buffer b;
        b.count = 0; b.truncated = 0;
        text(&b, "  #"); decimal(&b, (wide_u)i); text(&b, " 0x"); hexadecimal(&b, (uptr)pc);
        /* Return PCs point just after the call; resolve the calling site. */
        u64 address = (u64)((uptr)pc - 1);
        Win_Line line;
        __builtin_memset(&line, 0, sizeof line);
        line.size = (u32)sizeof(Win_Line);
        int have_line = 0;
        if (initialized) {
            void *module = 0;
            int valid = 0;
            if (GetModuleHandleExW(6, pc, &module) != 0) { /* FROM_ADDRESS | UNCHANGED_REFCOUNT */
                int known = 0;
                for (int j = 0; j < module_count; j++) {
                    if (modules[j] == module) { known = 1; valid = verified[j]; break; }
                }
                if (!known && module_count < 64) {
                    valid = win_pdb(process, module, load, info);
                    modules[module_count] = module;
                    verified[module_count] = (u8)valid;
                    module_count++;
                }
            }
            struct { Win_Symbol info; u16 extra[1024]; } storage;
            __builtin_memset(&storage, 0, sizeof storage);
            storage.info.size = (u32)sizeof(Win_Symbol);
            storage.info.max_name_len = 1024;
            u64 displacement = 0;
            if (valid && from_addr(process, address, &displacement, &storage.info) != 0) {
                text(&b, " in ");
                win_wtext(&b, storage.info.name, (int)(storage.info.name_len < 1023 ? storage.info.name_len : 1023));
            } else {
                text(&b, " in ??");
            }
            u32 line_displacement = 0;
            have_line = valid && get_line(process, address, &line_displacement, &line) != 0 && line.file != 0 && line.line > 0;
            if (have_line) { text(&b, " at "); win_wtext(&b, line.file, 1024); text(&b, ":"); decimal(&b, (wide_u)line.line); }
        } else {
            text(&b, " in ??");
        }
        text(&b, "\n");
        ubsan_write(b.data, (uptr)b.count);
        if (have_line) win_source(line.file, line.line);
    }
    if (initialized) cleanup(process);
    if (changed_options) options(previous_options);
    if (dll != 0) FreeLibrary(dll);
    if (own) __atomic_store_n(&win_symbols_busy, 0u, __ATOMIC_SEQ_CST);
}
#endif

/* ---- Reports ------------------------------------------------------------- */

_Noreturn static void finish(Buffer *b) {
    ubsan_write(b->data, (uptr)b->count);
    if (b->truncated) ubsan_write("(msg truncated)", 15);
    ubsan_write("\n", 1);
#if defined(CROSS_CC_UBSAN_LINUX_SYMBOLS) || defined(CROSS_CC_UBSAN_DARWIN_SYMBOLS)
    posix_backtrace();
#endif
#if defined(CROSS_CC_UBSAN_WINDOWS_SYMBOLS)
    windows_backtrace();
#endif
    abort();
}

static void start(Buffer *b) {
    b->count = 0;
    b->truncated = 0;
    /* A zero ID means the platform shim has no validated native thread-ID API.
     * Keep the bare header rather than inventing a process/pthread identity. */
    uptr tid = ubsan_thread_id();
    if (tid != 0) { text(b, "thread "); decimal(b, (wide_u)tid); text(b, " "); }
    text(b, "panic: ");
    ubsan_write(b->data, (uptr)b->count);
    b->count = 0;
}

_Noreturn static void binary(const Overflow *d, uptr lhs, uptr rhs, const char *op) {
    Buffer b; start(&b);
    text(&b, is_signed(d->td) ? "signed" : "unsigned");
    text(&b, " integer overflow: "); value(&b, d->td, lhs); text(&b, " "); text(&b, op); text(&b, " ");
    value(&b, d->td, rhs); text(&b, " cannot be represented in type "); name(&b, d->td);
    finish(&b);
}

_Noreturn static void negate(const Overflow *d, uptr h) {
    Buffer b; start(&b);
    text(&b, "negation of "); value(&b, d->td, h);
    text(&b, " cannot be represented in type "); name(&b, d->td);
    finish(&b);
}

_Noreturn static void divrem(const Overflow *d, uptr lhs, uptr rhs) {
    Buffer b; start(&b);
    if (is_signed(d->td) && signed_value(d->td, rhs) == -1) {
        text(&b, "division of "); value(&b, d->td, lhs); text(&b, " by -1 cannot be represented in type "); name(&b, d->td);
    } else {
        text(&b, "division by zero");
    }
    finish(&b);
}

_Noreturn static void shift(const Shift *d, uptr lhs, uptr rhs) {
    Buffer b; start(&b);
    if (negative(d->rhs, rhs)) {
        text(&b, "shift exponent "); value(&b, d->rhs, rhs); text(&b, " is negative");
    } else if (unsigned_value(d->rhs, rhs) >= (wide_u)width(d->lhs)) {
        text(&b, "shift exponent "); value(&b, d->rhs, rhs); text(&b, " is too large for ");
        decimal(&b, (wide_u)width(d->lhs)); text(&b, "-bit type "); name(&b, d->lhs);
    } else if (negative(d->lhs, lhs)) {
        text(&b, "left shift of negative value "); value(&b, d->lhs, lhs);
    } else {
        text(&b, "left shift of "); value(&b, d->lhs, lhs); text(&b, " by "); value(&b, d->rhs, rhs);
        text(&b, " places cannot be represented in type "); name(&b, d->lhs);
    }
    finish(&b);
}

_Noreturn static void bounds(const Bounds *d, uptr index) {
    Buffer b; start(&b);
    text(&b, "index "); value(&b, d->index, index); text(&b, " out of bounds for type "); name(&b, d->array);
    finish(&b);
}

static void operation(Buffer *b, u8 kind) {
    switch (kind) {
    case 0: text(b, "load of"); break;
    case 1: text(b, "store of"); break;
    case 2: text(b, "reference binding to"); break;
    case 3: text(b, "member access within"); break;
    case 4: text(b, "member call on"); break;
    case 5: text(b, "constructor call on"); break;
    case 6: case 7: text(b, "downcast of"); break;
    case 8: text(b, "upcast of"); break;
    case 9: text(b, "cast to virtual base of"); break;
    case 10: text(b, "_Nonnull binding to"); break;
    case 11: text(b, "dynamic operation on"); break;
    default: break;
    }
}

_Noreturn static void mismatch(const Mismatch *d, uptr p) {
    Buffer b; start(&b);
    operation(&b, d->kind);
    uptr alignment = d->alignment < PTR_BITS ? (uptr)1 << d->alignment : 0;
    if (p == 0) {
        text(&b, " null pointer of type "); name(&b, d->td);
    } else if ((p & (alignment - 1)) != 0) {
        text(&b, " misaligned address 0x"); hexadecimal(&b, p); text(&b, " for type "); name(&b, d->td);
        text(&b, ", which requires "); decimal(&b, (wide_u)alignment); text(&b, " byte alignment");
    } else {
        text(&b, " address 0x"); hexadecimal(&b, p); text(&b, " with insufficient space for an object of type "); name(&b, d->td);
    }
    finish(&b);
}

_Noreturn static void pointer(uptr base, uptr result) {
    Buffer b; start(&b);
    if (base == 0) {
        if (result == 0) text(&b, "applying zero offset to null pointer");
        else { text(&b, "applying non-zero offset "); decimal(&b, (wide_u)result); text(&b, " to null pointer"); }
    } else if (result == 0) {
        text(&b, "applying non-zero offset to non-null pointer 0x"); hexadecimal(&b, base); text(&b, " produced null pointer");
    } else {
        if (((iptr)base >= 0) == ((iptr)result >= 0)) {
            text(&b, base > result ? "addition" : "subtraction"); text(&b, " of unsigned offset to 0x");
        } else {
            text(&b, "pointer index expression with base 0x");
        }
        hexadecimal(&b, base); text(&b, " overflowed to 0x"); hexadecimal(&b, result);
    }
    finish(&b);
}

_Noreturn static void alignment_failed(const Alignment *d, uptr p, uptr a, uptr offset) {
    Buffer b; start(&b);
    text(&b, "assumption of "); value(&b, d->td, a); text(&b, " byte alignment");
    if (offset != 0) { text(&b, " (with offset of "); decimal(&b, (wide_u)offset); text(&b, " byte)"); }
    text(&b, " for pointer of type "); name(&b, d->td); text(&b, " failed\n");
    text(&b, offset != 0 ? "offset address is " : "address is ");
    uptr real = p - offset;
    uptr actual = 1;
    /* Zig uses ctz; zero has no representable alignment. Avoid undefined shifts. */
    if (real != 0) { while ((real & actual) == 0) actual <<= 1; } else actual = 0;
    decimal(&b, (wide_u)actual); text(&b, " aligned, misalignment offset is ");
    decimal(&b, (wide_u)(real & (a - 1))); text(&b, " bytes");
    finish(&b);
}

_Noreturn static void invalid(const Overflow *d, uptr h) {
    Buffer b; start(&b);
    text(&b, "load of value "); value(&b, d->td, h); text(&b, ", which is not valid for type "); name(&b, d->td);
    finish(&b);
}

_Noreturn static void vla(const Overflow *d, uptr h) {
    Buffer b; start(&b);
    text(&b, "variable length array bound evaluates to non-positive value "); value(&b, d->td, h);
    finish(&b);
}

/* Pinned compat: Zig ubsan_rt.zig:605-606 sniffs the first two bytes of the
 * data struct itself. On 64-bit BE those are the top bytes of a pointer (0),
 * so V1 is always chosen and the misparse crashes silently (observed rc=139
 * under qemu s390x/ppc64 with Zig too). Kept as is. */
_Noreturn static void float_cast(const void *data, uptr h) {
    Buffer b; start(&b);
    const u8 *p = (const u8 *)data;
    const Type *from, *to;
    if ((u16)p[0] + (u16)p[1] < 2 || p[0] == 255 || p[1] == 255) {
        const Float_Cast_V1 *d = (const Float_Cast_V1 *)data; from = d->from; to = d->to;
    } else {
        const Float_Cast *d = (const Float_Cast *)data; from = d->from; to = d->to;
    }
    value(&b, from, h); text(&b, " is outside the range of representable values of type "); name(&b, to);
    finish(&b);
}

_Noreturn static void nonnull_arg(const Nonnull_Arg *d) {
    Buffer b; start(&b);
    text(&b, "null pointer passed as argument "); decimal(&b, (wide_u)(i64)d->index);
    text(&b, ", which is declared to never be null");
    finish(&b);
}

_Noreturn static void invalid_builtin(const Invalid_Builtin *d) {
    Buffer b; start(&b);
    text(&b, "passing zero to "); text(&b, d->kind == 0 ? "ctz" : "clz"); text(&b, "(), which is not a valid argument");
    finish(&b);
}

_Noreturn static void message(const char *s) {
    Buffer b; start(&b);
    text(&b, s);
    finish(&b);
}

/* ---- Public ABI ---------------------------------------------------------- */
/* Both recoverable and abort entry points intentionally terminate, as Zig does. */

#define HANDLER2(name_, call) \
    UBSAN_EXPORT _Noreturn void __ubsan_handle_##name_(void *d, uptr a, uptr b); \
    UBSAN_EXPORT _Noreturn void __ubsan_handle_##name_(void *d, uptr a, uptr b) { call; } \
    UBSAN_EXPORT _Noreturn void __ubsan_handle_##name_##_abort(void *d, uptr a, uptr b); \
    UBSAN_EXPORT _Noreturn void __ubsan_handle_##name_##_abort(void *d, uptr a, uptr b) { call; }
#define HANDLER1(name_, call) \
    UBSAN_EXPORT _Noreturn void __ubsan_handle_##name_(void *d, uptr a); \
    UBSAN_EXPORT _Noreturn void __ubsan_handle_##name_(void *d, uptr a) { call; } \
    UBSAN_EXPORT _Noreturn void __ubsan_handle_##name_##_abort(void *d, uptr a); \
    UBSAN_EXPORT _Noreturn void __ubsan_handle_##name_##_abort(void *d, uptr a) { call; }
#define HANDLER0(name_, call) \
    UBSAN_EXPORT _Noreturn void __ubsan_handle_##name_(void *d); \
    UBSAN_EXPORT _Noreturn void __ubsan_handle_##name_(void *d) { call; } \
    UBSAN_EXPORT _Noreturn void __ubsan_handle_##name_##_abort(void *d); \
    UBSAN_EXPORT _Noreturn void __ubsan_handle_##name_##_abort(void *d) { call; }

HANDLER2(add_overflow, binary((const Overflow *)d, a, b, "+"))
HANDLER2(sub_overflow, binary((const Overflow *)d, a, b, "-"))
HANDLER2(mul_overflow, binary((const Overflow *)d, a, b, "*"))
HANDLER1(negate_overflow, negate((const Overflow *)d, a))
HANDLER2(divrem_overflow, divrem((const Overflow *)d, a, b))
HANDLER2(shift_out_of_bounds, shift((const Shift *)d, a, b))
HANDLER1(out_of_bounds, bounds((const Bounds *)d, a))
HANDLER1(type_mismatch_v1, mismatch((const Mismatch *)d, a))
HANDLER2(pointer_overflow, ((void)d, pointer(a, b)))
HANDLER1(load_invalid_value, invalid((const Overflow *)d, a))
HANDLER1(vla_bound_not_positive, vla((const Overflow *)d, a))
HANDLER1(float_cast_overflow, float_cast(d, a))
HANDLER0(nonnull_arg, nonnull_arg((const Nonnull_Arg *)d))
HANDLER0(invalid_builtin, invalid_builtin((const Invalid_Builtin *)d))

UBSAN_EXPORT _Noreturn void __ubsan_handle_alignment_assumption(void *d, uptr p, uptr a, uptr o);
UBSAN_EXPORT _Noreturn void __ubsan_handle_alignment_assumption(void *d, uptr p, uptr a, uptr o) { alignment_failed((const Alignment *)d, p, a, o); }
UBSAN_EXPORT _Noreturn void __ubsan_handle_alignment_assumption_abort(void *d, uptr p, uptr a, uptr o);
UBSAN_EXPORT _Noreturn void __ubsan_handle_alignment_assumption_abort(void *d, uptr p, uptr a, uptr o) { alignment_failed((const Alignment *)d, p, a, o); }

UBSAN_EXPORT _Noreturn void __ubsan_handle_nonnull_return_v1(void *d, void *l);
UBSAN_EXPORT _Noreturn void __ubsan_handle_nonnull_return_v1(void *d, void *l) { (void)d; (void)l; message("null pointer returned from function declared to never return null"); }
UBSAN_EXPORT _Noreturn void __ubsan_handle_nonnull_return_v1_abort(void *d, void *l);
UBSAN_EXPORT _Noreturn void __ubsan_handle_nonnull_return_v1_abort(void *d, void *l) { (void)d; (void)l; message("null pointer returned from function declared to never return null"); }

UBSAN_EXPORT _Noreturn void __ubsan_handle_builtin_unreachable(void *d);
UBSAN_EXPORT _Noreturn void __ubsan_handle_builtin_unreachable(void *d) { (void)d; message("execution reached an unreachable program point"); }
UBSAN_EXPORT _Noreturn void __ubsan_handle_missing_return(void *d);
UBSAN_EXPORT _Noreturn void __ubsan_handle_missing_return(void *d) { (void)d; message("execution reached the end of a value-returning function without returning a value"); }
