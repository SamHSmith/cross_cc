#include "runtime.h"

/* Final non-PIE static SPARC64 links only. Validate complete canonical TLS
 * sequences before publishing a private copy. No symbol/index/member sizes
 * change; LLD still owns the final TLS layout. */
typedef struct {
  uint32_t kind, link, info;
  uint64_t offset, size, entsize;
} Sparc_TLS_Section;
static uint64_t sparc_tls_u(const unsigned char *b, size_t p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++)
    v = (v << 8) | b[p + (size_t)i];
  return v;
}
static void sparc_tls_put(unsigned char *b, size_t p, int n, uint64_t value) {
  for (int i = n - 1; i >= 0; i--) {
    b[p + (size_t)i] = (unsigned char)(value & 255);
    value >>= 8;
  }
}
static bool sparc_tls_range(size_t len, uint64_t p, uint64_t n) {
  return p <= len && n <= (uint64_t)len - p;
}
static uint32_t sparc_tls_instruction(const unsigned char *b, size_t p) {
  return (uint32_t)sparc_tls_u(b, p, 4);
}
static uint32_t sparc_tls_rd(uint32_t w) { return (w >> 25) & 31; }
static uint32_t sparc_tls_rs1(uint32_t w) { return (w >> 14) & 31; }
static uint32_t sparc_tls_rs2(uint32_t w) { return w & 31; }
static bool sparc_tls_op(uint32_t w, uint32_t op, uint32_t op3,
                         bool immediate) {
  return w >> 30 == op && ((w >> 19) & 63) == op3 &&
         (((w >> 13) & 1) != 0) == immediate;
}
#define TLS_FAIL(s)                                                            \
  do {                                                                         \
    if (error)                                                                 \
      *error = (s);                                                            \
    return false;                                                              \
  } while (0)
bool sparc_tls_relax_elf(unsigned char *b, size_t len, char **error) {
  if (error)
    *error = NULL;
  bool changed = false;
  if (len < 4 || memcmp(b, "\177ELF", 4))
    return false;
  if (len < 20)
    TLS_FAIL("truncated ELF header");
  if (b[4] != 2 || b[5] != 2 || sparc_tls_u(b, 18, 2) != 43 ||
      sparc_tls_u(b, 16, 2) != 1)
    return false;
  if (len < 64)
    TLS_FAIL("truncated SPARC64 ELF header");
  uint64_t shoff = sparc_tls_u(b, 40, 8), shentsize = sparc_tls_u(b, 58, 2),
           shnum = sparc_tls_u(b, 60, 2);
  if (!shnum || shentsize != 64 || !sparc_tls_range(len, shoff, shnum * 64))
    TLS_FAIL("unsupported SPARC64 section table");
  Sparc_TLS_Section *sections = xmalloc((size_t)shnum * sizeof(*sections));
  for (size_t i = 0; i < shnum; i++) {
    size_t p = (size_t)shoff + i * 64;
    Sparc_TLS_Section s = {(uint32_t)sparc_tls_u(b, p + 4, 4),
                           (uint32_t)sparc_tls_u(b, p + 40, 4),
                           (uint32_t)sparc_tls_u(b, p + 44, 4),
                           sparc_tls_u(b, p + 24, 8),
                           sparc_tls_u(b, p + 32, 8),
                           sparc_tls_u(b, p + 56, 8)};
    if (s.kind != 8 && !sparc_tls_range(len, s.offset, s.size))
      TLS_FAIL("SPARC64 section exceeds input");
    sections[i] = s;
  }
  for (size_t section = 0; section < shnum; section++) {
    Sparc_TLS_Section rel = sections[section];
    if (rel.kind != 4)
      continue;
    if (rel.entsize != 24 || rel.size % 24 || rel.link >= shnum ||
        rel.info >= shnum)
      TLS_FAIL("unsupported SPARC64 RELA table");
    Sparc_TLS_Section symtab = sections[rel.link], target = sections[rel.info];
    if (symtab.kind != 2 || symtab.entsize != 24 || symtab.size % 24)
      TLS_FAIL("unsupported SPARC64 symbol table");
    size_t count = (size_t)(rel.size / 24);
    for (size_t i = 0; i < count; i++) {
      size_t p = (size_t)rel.offset + i * 24;
      uint64_t ri = sparc_tls_u(b, p + 8, 8);
      uint32_t typ = (uint32_t)ri;
      if (typ < 56 || typ > 79 || typ == 72 || typ == 73)
        continue;
      if (typ != 56 && typ != 64 && typ != 67)
        TLS_FAIL(str_fmt("unsupported SPARC64 TLS relocation %u (only "
                         "validated GD/LD/IE sequences can relax)",
                         typ));
      if (i + 3 >= count)
        TLS_FAIL("incomplete SPARC64 TLS sequence");
      uint64_t symbol = ri >> 32;
      if (symbol >= symtab.size / 24)
        TLS_FAIL("SPARC64 TLS symbol index exceeds table");
      size_t sp = (size_t)(symtab.offset + symbol * 24);
      if ((b[sp + 4] & 15) != 6)
        TLS_FAIL("SPARC64 TLS relocation does not name STT_TLS");
      if (b[sp + 4] >> 4 == 2 && sparc_tls_u(b, sp + 6, 2) == 0)
        TLS_FAIL("undefined weak SPARC64 TLS cannot be relaxed safely");
      uint64_t off = sparc_tls_u(b, p, 8);
      if (typ == 64) {
        if (i + 6 >= count || target.kind != 1 || off % 4 ||
            off > target.size || 32 > target.size - off)
          TLS_FAIL("incomplete SPARC64 LD sequence");
        bool o0_order = (uint32_t)sparc_tls_u(b, p + 24 + 8, 8) == 65;
        bool copy_got = o0_order && sparc_tls_u(b, p + 5 * 24, 8) == off + 24;
        uint32_t types[7] = {64, 60, 61, 62, 65, 63, 66};
        uint64_t offsets[7] = {0, 4, 8, 12, 16, 20, 28};
        int mhi_index = 1, mlo_index = 2, madd_index = 3, lo_index = 4;
        if (o0_order) {
          uint32_t order[7] = {64, 65, 60, 61, 62, 63, 66};
          memcpy(types, order, sizeof(types));
          mhi_index = 2;
          mlo_index = 3;
          madd_index = 4;
          lo_index = 1;
          if (copy_got) {
            if (36 > target.size - off)
              TLS_FAIL("SPARC64 LD GOT-copy sequence exceeds text");
            offsets[4] = 20;
            offsets[5] = 24;
            offsets[6] = 32;
          }
        }
        for (int j = 0; j < 7; j++) {
          size_t rp = p + (size_t)j * 24;
          if (sparc_tls_u(b, rp, 8) != off + offsets[j] ||
              sparc_tls_u(b, rp + 8, 8) != ((symbol << 32) | types[j]) ||
              sparc_tls_u(b, rp + 16, 8))
            TLS_FAIL("noncanonical SPARC64 LD relocation sequence");
        }
        size_t ip = (size_t)(target.offset + off);
        uint32_t hi = sparc_tls_instruction(b, ip),
                 mhi =
                     sparc_tls_instruction(b, ip + (size_t)offsets[mhi_index]),
                 mlo =
                     sparc_tls_instruction(b, ip + (size_t)offsets[mlo_index]),
                 madd =
                     sparc_tls_instruction(b, ip + (size_t)offsets[madd_index]),
                 lo = sparc_tls_instruction(b, ip + (size_t)offsets[lo_index]),
                 call = sparc_tls_instruction(b, ip + (size_t)offsets[5]),
                 last = sparc_tls_instruction(b, ip + (size_t)offsets[6]);
        uint32_t r = sparc_tls_rd(hi), mr = sparc_tls_rd(mhi),
                 lr = sparc_tls_rd(lo), mlr = sparc_tls_rd(mlo);
        if (hi >> 30 != 0 || ((hi >> 22) & 7) != 4 || (hi & 0x3fffff) != 0 ||
            mhi >> 30 != 0 || ((mhi >> 22) & 7) != 4 || (mhi & 0x3fffff) != 0 ||
            r == 0 || r == 7 || r == 8 || mr == 0 || mr == 7 || mr == 8 ||
            mr == r || mr == lr)
          TLS_FAIL("noncanonical SPARC64 LD sethi instructions");
        if (!sparc_tls_op(mlo, 2, 0, true) || mlr == 0 || mlr == 7 ||
            mlr == 8 || mlr == r || mlr == lr || sparc_tls_rs1(mlo) != mr ||
            (mlo & 0x1fff) != 0 || !sparc_tls_op(lo, 2, 3, true) || lr == 0 ||
            lr == 7 || lr == 8 || sparc_tls_rs1(lo) != r || (lo & 0x1fff) != 0)
          TLS_FAIL("noncanonical SPARC64 LD immediate instructions");
        if (copy_got) {
          uint32_t move = sparc_tls_instruction(b, ip + 16),
                   source = sparc_tls_rs2(move);
          if (mlr == mr || !sparc_tls_op(move, 2, 2, false) ||
              sparc_tls_rs1(move) != 0 || sparc_tls_rd(move) != mr ||
              (move & 0x1fe0) != 0 || source == mr || source == mlr ||
              source == r || source == lr || source == 7 ||
              sparc_tls_rs1(madd) != mr)
            TLS_FAIL("noncanonical SPARC64 LD GOT-register copy");
        } else if (sparc_tls_rs1(madd) == mr || sparc_tls_rs1(madd) == mlr ||
                   (o0_order && sparc_tls_rs1(madd) == lr))
          TLS_FAIL("SPARC64 LD GOT register aliases a TLS displacement");
        if (!sparc_tls_op(madd, 2, 0, false) || sparc_tls_rd(madd) != 8 ||
            sparc_tls_rs2(madd) != mlr || (madd & 0x1fe0) != 0 ||
            call != 0x40000000 ||
            sparc_tls_instruction(b, ip + (size_t)offsets[5] + 4) !=
                0x01000000 ||
            !sparc_tls_op(last, 2, 0, false) || sparc_tls_rs1(last) != 8 ||
            sparc_tls_rs2(last) != lr || (last & 0x1fe0) != 0)
          TLS_FAIL("noncanonical SPARC64 LD add/call/delay instructions");
        int remove_indices[3] = {mhi_index, mlo_index, madd_index};
        for (int j = 0; j < 3; j++)
          sparc_tls_put(b, ip + (size_t)offsets[remove_indices[j]], 4,
                        0x01000000);
        sparc_tls_put(b, ip + (size_t)offsets[5], 4, 0x90100007);
        for (int j = 0; j < 7; j++) {
          uint64_t info = 0;
          if (j == 0)
            info = (symbol << 32) | 72;
          if (j == lo_index)
            info = (symbol << 32) | 73;
          sparc_tls_put(b, p + (size_t)j * 24 + 8, 8, info);
        }
        changed = true;
        i += 6;
        continue;
      }
      bool gd = typ == 56;
      uint32_t expected[4] = {67, 68, 70, 71};
      uint64_t bytes_needed = 16;
      if (gd) {
        uint32_t order[4] = {56, 57, 58, 59};
        memcpy(expected, order, sizeof(expected));
        bytes_needed = 20;
      }
      if (target.kind != 1 || off % 4 || off > target.size ||
          bytes_needed > target.size - off)
        TLS_FAIL("SPARC64 TLS instruction range exceeds text");
      uint64_t lastoff = sparc_tls_u(b, p + 72, 8);
      if (lastoff % 4 || lastoff < off + 12 || lastoff > target.size - 4)
        TLS_FAIL("SPARC64 TLS final instruction exceeds text");
      uint64_t thirdoff = off + 8;
      bool gd_move = gd && lastoff == off + 16;
      if (gd) {
        if (lastoff != off + 12 && !gd_move)
          TLS_FAIL("noncanonical SPARC64 GD instruction spacing");
        if (target.size - lastoff < 8)
          TLS_FAIL("SPARC64 GD delay slot exceeds text");
        if (gd_move)
          thirdoff = off + 12;
      }
      for (int j = 0; j < 4; j++) {
        size_t rp = p + (size_t)j * 24;
        uint64_t expectedoff = off + (uint64_t)j * 4;
        if (j == 2)
          expectedoff = thirdoff;
        if (j == 3)
          expectedoff = lastoff;
        if (sparc_tls_u(b, rp, 8) != expectedoff ||
            sparc_tls_u(b, rp + 8, 8) != ((symbol << 32) | expected[j]) ||
            sparc_tls_u(b, rp + 16, 8))
          TLS_FAIL("noncanonical SPARC64 TLS relocation sequence");
      }
      size_t ip = (size_t)(target.offset + off);
      uint32_t hi = sparc_tls_instruction(b, ip),
               lo = sparc_tls_instruction(b, ip + 4),
               third =
                   sparc_tls_instruction(b, (size_t)(target.offset + thirdoff)),
               fourth =
                   sparc_tls_instruction(b, (size_t)(target.offset + lastoff)),
               reg = sparc_tls_rd(hi), lowreg = sparc_tls_rd(lo);
      if (hi >> 30 != 0 || ((hi >> 22) & 7) != 4 || (hi & 0x3fffff) != 0 ||
          reg == 0 || reg == 7 || lowreg == 0 || lowreg == 7 ||
          (!gd && lowreg != reg) || !sparc_tls_op(lo, 2, 0, true) ||
          sparc_tls_rs1(lo) != reg || (lo & 0x1fff) != 0)
        TLS_FAIL("noncanonical SPARC64 TLS sethi/add instructions");
      if (gd) {
        if (gd_move) {
          uint32_t move = sparc_tls_instruction(b, ip + 8),
                   source = sparc_tls_rs2(move);
          if (lowreg == reg || !sparc_tls_op(move, 2, 2, false) ||
              sparc_tls_rs1(move) != 0 || sparc_tls_rd(move) != reg ||
              (move & 0x1fe0) != 0 || source == reg || source == lowreg ||
              source == 7 || sparc_tls_rs1(third) != reg)
            TLS_FAIL("noncanonical SPARC64 GD GOT-register copy");
        }
        if (!sparc_tls_op(third, 2, 0, false) || sparc_tls_rd(third) != 8 ||
            sparc_tls_rs2(third) != lowreg || sparc_tls_rs1(third) == lowreg ||
            (third & 0x1fe0) != 0 || fourth != 0x40000000 ||
            sparc_tls_instruction(b, (size_t)(target.offset + lastoff + 4)) !=
                0x01000000)
          TLS_FAIL("noncanonical SPARC64 GD add/call/delay slot");
        third = (third & ~(uint32_t)(31 << 14)) | (7 << 14);
        fourth = 0x01000000;
      } else {
        uint32_t dest = sparc_tls_rd(third);
        if (!sparc_tls_op(third, 3, 0x0b, false) ||
            sparc_tls_rs2(third) != reg || (third & 0x1fe0) != 0 || dest == 0 ||
            dest == 7 || !sparc_tls_op(fourth, 2, 0, false) ||
            sparc_tls_rs1(fourth) != 7 || sparc_tls_rs2(fourth) != dest ||
            sparc_tls_rd(fourth) != dest || (fourth & 0x1fe0) != 0)
          TLS_FAIL("noncanonical SPARC64 IE load/add instructions");
        third = 0x80100000 | (dest << 25) | reg;
      }
      lo = (lo & ~(uint32_t)(63 << 19)) | (3 << 19);
      sparc_tls_put(b, ip + 4, 4, lo);
      sparc_tls_put(b, (size_t)(target.offset + thirdoff), 4, third);
      sparc_tls_put(b, (size_t)(target.offset + lastoff), 4, fourth);
      sparc_tls_put(b, p + 8, 8, (symbol << 32) | 72);
      sparc_tls_put(b, p + 24 + 8, 8, (symbol << 32) | 73);
      sparc_tls_put(b, p + 48 + 8, 8, 0);
      sparc_tls_put(b, p + 72 + 8, 8, 0);
      changed = true;
      i += 3;
    }
  }
  free(sections);
  return changed;
}
static bool sparc_archive_uint(const char *s, uint64_t *value) {
  if (!*s || *s == '-' || *s == '+')
    return false;
  char *end;
  errno = 0;
  unsigned long long n = strtoull(s, &end, 10);
  if (errno || *end)
    return false;
  *value = (uint64_t)n;
  return true;
}
bool sparc_tls_relax_file(const char *source, const char *destination,
                          char **error) {
  if (error)
    *error = NULL;
  size_t len = 0;
  unsigned char *b = (unsigned char *)read_file(source, &len);
  if (!b)
    TLS_FAIL(str_fmt("cannot read SPARC TLS input %s", source));
  bool changed = false;
  char *message = NULL;
  if (len >= 8 && !memcmp(b, "!<thin>\n", 8))
    TLS_FAIL("thin archives are unsupported for SPARC64 static TLS relaxation");
  if (len >= 8 && !memcmp(b, "!<arch>\n", 8)) {
    size_t p = 8;
    while (p < len) {
      if (len - p < 60 || memcmp(b + p + 58, "`\n", 2))
        TLS_FAIL("malformed SPARC TLS archive header");
      uint64_t n;
      if (!sparc_archive_uint(str_trim(str_ncopy((char *)b + p + 48, 10)),
                              &n) ||
          n > len - p - 60)
        TLS_FAIL("malformed SPARC TLS archive member length");
      size_t start = p + 60;
      char *name = str_trim(str_ncopy((char *)b + p, 16));
      if (starts_with(name, "#1/")) {
        uint64_t namesize;
        if (!sparc_archive_uint(name + 3, &namesize) || namesize > n)
          TLS_FAIL("malformed BSD archive member name");
        start += (size_t)namesize;
      }
      bool member =
          sparc_tls_relax_elf(b + start, p + 60 + (size_t)n - start, &message);
      if (message)
        TLS_FAIL(str_fmt("%s (archive member at byte %zu)", message, p));
      changed |= member;
      p += 60 + (size_t)n;
      if (p % 2)
        p++;
    }
  } else {
    changed = sparc_tls_relax_elf(b, len, &message);
    if (message)
      TLS_FAIL(message);
  }
  if (changed) {
    if (streq(source, destination))
      TLS_FAIL("SPARC TLS destination must be a private copy, not the input");
    if (file_exists(destination))
      TLS_FAIL("SPARC TLS private destination already exists");
    if (!write_file(destination, b, len))
      TLS_FAIL(str_fmt("cannot stage SPARC TLS input: %s", strerror(errno)));
  }
  free(b);
  return changed;
}
#undef TLS_FAIL
