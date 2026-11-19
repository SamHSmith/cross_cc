#include "platforms.h"

/* ABI decoder and CRT recipes translated from Zig 0.17.1 src/libs/glibc.zig.
 * Copyright (c) Zig contributors. MIT License (Expat):
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
static const Glibc_Lib GLIBC_LIBS[] = {{"m", 6},      {"c", 6},       {"ld", 2},
                                       {"resolv", 2}, {"pthread", 0}, {"dl", 2},
                                       {"rt", 1},     {"util", 1}};
#define GV(a, b, c) ((Glibc_Version){{a, b, c}})
unsigned char glibc_byte(Glibc_Reader *r) {
  if (r->pos >= r->len) {
    r->bad = true;
    return 0;
  }
  return r->data[r->pos++];
}
int glibc_u16(Glibc_Reader *r) {
  int a = glibc_byte(r);
  return a | ((int)glibc_byte(r) << 8);
}
char *glibc_zstr(Glibc_Reader *r) {
  size_t start = r->pos;
  while (r->pos < r->len && r->data[r->pos])
    r->pos++;
  size_t end = r->pos;
  glibc_byte(r);
  return str_ncopy((const char *)r->data + start, end - start);
}
uint64_t glibc_uleb(Glibc_Reader *r) {
  uint64_t v = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    unsigned b = glibc_byte(r);
    if (shift == 63 && b > 1) {
      r->bad = true;
      return 0;
    }
    v |= (uint64_t)(b & 127) << shift;
    if (!(b & 128))
      return v;
  }
  r->bad = true;
  return 0;
}
Glibc_Version glibc_version(const char *s, bool *ok) {
  Glibc_Version v = {{0}};
  *ok = false;
  StrVec parts = str_split(nz(s), ".");
  if (parts.len < 2 || parts.len > 3)
    return v;
  for (size_t i = 0; i < parts.len; i++) {
    char *end = NULL;
    errno = 0;
    long n = strtol(parts.data[i], &end, 10);
    if (errno || end == parts.data[i] || *end || n < 0 || n > 255)
      return v;
    v.data[i] = (int)n;
  }
  *ok = true;
  return v;
}
int glibc_ver_cmp(Glibc_Version a, Glibc_Version b) {
  for (int i = 0; i < 3; i++) {
    if (a.data[i] < b.data[i])
      return -1;
    if (a.data[i] > b.data[i])
      return 1;
  }
  return 0;
}
char *glibc_ver_name(Glibc_Version v) {
  if (!v.data[2])
    return str_fmt("%d.%d", v.data[0], v.data[1]);
  return str_fmt("%d.%d.%d", v.data[0], v.data[1], v.data[2]);
}
char *glibc_write(const char *path, const char *text) {
  if (!write_file(path, nz(text), strlen(nz(text))))
    return str_fmt("cannot write %s: %s", path, strerror(errno));
  return NULL;
}

/* Decode every inclusion even for unrelated targets: malformed metadata must
   never silently redirect a target link to the host's libc. */
char *glibc_emit(const unsigned char *data, size_t size, Target t,
                 Glibc_Version version, const char *dir) {
  Glibc_Reader r = {data, size, 0, false};
  int nlibs = glibc_byte(&r);
  if (nlibs != 8)
    return "invalid glibc abilists library count";
  for (int i = 0; i < 8; i++)
    if (!streq(glibc_zstr(&r), GLIBC_LIBS[i].name))
      return "invalid glibc abilists library order";
  int nv = glibc_byte(&r);
  if (!nv || nv > 128)
    return "invalid glibc abilists version count";
  Glibc_Version versions[128] = {0};
  int vi = -1;
  for (int i = 0; i < nv; i++) {
    for (int j = 0; j < 3; j++)
      versions[i].data[j] = glibc_byte(&r);
    if (!glibc_ver_cmp(versions[i], version))
      vi = i;
  }
  if (vi < 0 && glibc_ver_cmp(version, versions[nv - 1]) > 0)
    vi = nv - 1;
  if (vi < 0)
    return str_fmt("unsupported glibc version %s", glibc_ver_name(version));
  int nt = glibc_byte(&r), ti = -1;
  char *target = str_fmt("%s-linux-%s", t.arch, t.abi);
  for (int i = 0; i < nt; i++)
    if (streq(glibc_zstr(&r), target))
      ti = i;
  if (ti < 0 || ti >= 64)
    return str_fmt("glibc ABI metadata has no target %s", target);
  if (r.bad)
    return "truncated glibc ABI metadata";
  size_t inclusions = r.pos;
  Platform_Text map = {0};
  for (int i = 0; i <= vi; i++)
    pt_printf(&map, "GLIBC_%s { };\n", glibc_ver_name(versions[i]));
  char *error = glibc_write(path_join(dir, "all.map"), map.data);
  free(map.data);
  if (error)
    return error;
  for (int li = 0; li < 8; li++) {
    if (li >= 4 && glibc_ver_cmp(version, GV(2, 34, 0)) >= 0)
      continue;
    r.pos = inclusions;
    Platform_Text a = {0};
    pt_append(&a, ".text\n");
    for (int kind = 0; kind < 2; kind++) {
      if (kind == 1) {
        pt_append(&a, ".section .rodata\n");
        if (li == 1) {
          if (streq(t.arch, "sparc64"))
            pt_append(&a, ".balign 8\n.globl _IO_stdin_used\n.long 0\n.reloc "
                          ".-4, R_SPARC_GOT22, _IO_stdin_used\n");
          else
            pt_printf(&a,
                      ".balign 8\n.globl _IO_stdin_used\n%s _IO_stdin_used\n",
                      glibc_word(t));
        }
        pt_append(&a, ".data\n");
      }
      int count = glibc_u16(&r);
      char *name = "";
      bool present[128] = {0};
      uint64_t sizes[128] = {0};
      for (int k = 0; k < count; k++) {
        if (streq(name, "")) {
          name = glibc_zstr(&r);
          memset(present, 0, sizeof(present));
          memset(sizes, 0, sizeof(sizes));
        }
        uint64_t targets = glibc_uleb(&r), sz = 0;
        if (kind == 1) {
          sz = glibc_uleb(&r);
          if (sz > 65535)
            r.bad = true;
        }
        unsigned lb = glibc_byte(&r);
        bool applies = (int)(lb & 127) == li && (targets & (UINT64_C(1) << ti));
        for (;;) {
          unsigned b = glibc_byte(&r);
          int idx = b & 127;
          if (idx >= nv)
            r.bad = true;
          else if (applies && idx <= vi) {
            present[idx] = true;
            sizes[idx] = sz;
          }
          if ((b & 128) || r.bad)
            break;
        }
        if (r.bad) {
          free(a.data);
          return "malformed glibc ABI inclusions";
        }
        if (!(lb & 128))
          continue;
        int default_vi = -1;
        for (int i = 0; i < nv; i++)
          if (present[i])
            default_vi = i;
        for (int i = 0; i < nv; i++) {
          if (!present[i])
            continue;
          char *ver = glibc_ver_name(versions[i]),
               *local = str_fmt("%s_%d", name, i);
          const char *at = i == default_vi ? "@@" : "@",
                     *typ = kind == 1 ? "object" : "function";
          pt_printf(&a, ".balign 8\n.globl %s\n.type %s, %%%s\n", local, local,
                    typ);
          if (kind == 1)
            pt_printf(&a, ".size %s, %" PRIu64 "\n", local, sizes[i]);
          pt_printf(&a, ".symver %s, %s%sGLIBC_%s, remove\n%s: ", local, name,
                    at, ver, local);
          if (kind == 0)
            pt_printf(&a, "%s 0\n", glibc_word(t));
          else
            pt_printf(&a, ".fill %" PRIu64 ", 1, 0\n", sizes[i]);
        }
        name = "";
      }
      if (!streq(name, "")) {
        free(a.data);
        return "unterminated glibc ABI symbol";
      }
    }
    pt_append(&a, ".section .note.GNU-stack,\"\",%progbits\n");
    error = glibc_write(path_join(dir, str_fmt("%s.s", GLIBC_LIBS[li].name)),
                        a.data);
    free(a.data);
    if (error)
      return error;
    if (r.pos + 2 == size && data[r.pos] == 0 && data[r.pos + 1] == 0)
      r.pos += 2;
    if (r.bad || r.pos != size)
      return "invalid glibc ABI data length";
  }
  return NULL;
}
const char *glibc_source_arch(const char *arch) {
  if (streq(arch, "armeb"))
    return "arm";
  if (streq(arch, "aarch64_be"))
    return "aarch64";
  if (streq(arch, "sparc64"))
    return "sparc/sparc64";
  if (streq(arch, "mips64") || streq(arch, "mips64el"))
    return "mips";
  if (streq(arch, "x86"))
    return "i386";
  if (streq(arch, "riscv32") || streq(arch, "riscv64"))
    return "riscv";
  if (streq(arch, "loongarch64"))
    return "loongarch";
  if (streq(arch, "s390x"))
    return "s390";
  if (streq(arch, "powerpc64le"))
    return "powerpc/powerpc64";
  return arch;
}
char *glibc_default_version(Target t) {
  if (streq(t.arch, "x86") || streq(t.arch, "arm") || streq(t.arch, "armeb"))
    return "2.34";
  if (streq(t.arch, "powerpc64le"))
    return "2.32";
  if (streq(t.arch, "riscv32"))
    return "2.33";
  if (streq(t.arch, "loongarch64"))
    return "2.36";
  return "2.31";
}
const char *glibc_word(Target t) {
  return streq(t.abi, "gnux32") || streq(t.arch, "x86") ||
                 streq(t.arch, "arm") || streq(t.arch, "armeb") ||
                 streq(t.arch, "riscv32")
             ? ".long"
             : ".quad";
}
void glibc_add_arch_includes(StrVec *a, const char *base, const char *arch,
                             const char *suffix, const char *abi) {
  if (streq(arch, "x86_64") && streq(abi, "gnux32") && streq(suffix, ""))
    sv_extend(a, SV("-I", path_join(base, "x86_64/x32")));
  const char *parent = NULL, *sub = NULL;
  if (streq(arch, "powerpc64le")) {
    parent = "powerpc";
    sub = "powerpc/powerpc64";
  } else if (streq(arch, "sparc64")) {
    parent = "sparc";
    sub = "sparc/sparc64";
  } else if (streq(arch, "mips64") || streq(arch, "mips64el")) {
    parent = "mips";
    sub = "mips/mips64";
  }
  if (parent) {
    if (!streq(suffix, ""))
      sv_extend(a, SV("-I", path_join(base, parent, suffix)));
    else
      sv_extend(a,
                SV("-I", path_join(base, sub), "-I", path_join(base, parent)));
    return;
  }
  if (streq(arch, "loongarch64")) {
    sv_extend(a, SV("-I", path_join(base, "loongarch")));
    return;
  }
  sv_extend(a, SV("-I", path_join(base, glibc_source_arch(arch), suffix)));
  if (streq(arch, "x86_64") || streq(arch, "x86"))
    sv_extend(a, SV("-I", path_join(base, "x86", suffix)));
}
StrVec glibc_includes(Target t, Toolchain tc) {
  StrVec a = {0};
  char *g = path_join(tc.lib_dir, "libc/glibc"), *s = path_join(g, "sysdeps"),
       *l = path_join(s, "unix/sysv/linux");
  sv_extend(&a, SV("-I", path_join(g, "include")));
  glibc_add_arch_includes(&a, l, t.arch, "", t.abi);
  glibc_add_arch_includes(&a, s, t.arch, "nptl", "");
  sv_extend(&a, SV("-I", path_join(l, "generic"), "-I", path_join(l, "include"),
                   "-I", l, "-I", path_join(s, "nptl"), "-I",
                   path_join(s, "pthread"), "-I", path_join(s, "unix/sysv")));
  glibc_add_arch_includes(&a, path_join(s, "unix"), t.arch, "", t.abi);
  sv_extend(&a, SV("-I", path_join(s, "unix")));
  glibc_add_arch_includes(&a, s, t.arch, "", t.abi);
  sv_extend(&a, SV("-I", path_join(s, "generic"), "-I", g));
  char *inc = path_join(tc.lib_dir, "libc/include");
  const char *ha = t.arch;
  if (streq(ha, "armeb"))
    ha = "arm";
  if (streq(ha, "aarch64_be"))
    ha = "aarch64";
  if (streq(ha, "x86_64"))
    ha = "x86";
  if (streq(ha, "riscv32") || streq(ha, "riscv64"))
    ha = "riscv";
  if (streq(ha, "powerpc64le"))
    ha = "powerpc";
  if (streq(ha, "loongarch64"))
    ha = "loongarch";
  if (streq(ha, "sparc64"))
    ha = "sparc";
  if (streq(ha, "mips64") || streq(ha, "mips64el"))
    ha = "mips";
  sv_extend(&a, SV("-I", path_join(inc, str_fmt("%s-linux-gnu", ha)), "-I",
                   path_join(inc, "generic-glibc"), "-I",
                   path_join(inc, str_fmt("%s-linux-any", ha)), "-I",
                   path_join(inc, "any-linux-any")));
  bool ok = false;
  Glibc_Version v = glibc_version(
      streq(t.glibc_version, "") ? glibc_default_version(t) : t.glibc_version,
      &ok);
  sv_push(&a, str_fmt("-D__GLIBC_MINOR__=%d", ok ? v.data[1] : 31));
  return a;
}
char *glibc_compile(Options o, Target t, Toolchain tc, const char *src,
                    const char *out, StrVec flags) {
  StrVec a = SV(tc.clang, str_fmt("--target=%s", t.clang), "-c", "-O2", "-fPIC",
                "-fno-stack-protector", "-Qunused-arguments", "-nostdinc",
                "-isystem", path_join(tc.lib_dir, "include"));
  char *error = NULL;
  StrVec cpu = cpu_flags(t, "baseline", (StrVec){0}, &error);
  if (error)
    return error;
  sv_extend(&a, cpu);
  sv_extend(&a, glibc_includes(t, tc));
  sv_extend(&a, flags);
  sv_extend(&a, SV(src, "-o", out));
  if (run_command(a, o.verbose))
    return str_fmt("compiling glibc runtime failed: %s", src);
  return NULL;
}

static void glibc_frame(Platform_Text *frame, Target t, const char *section,
                        bool begin) {
  const char *arch = t.arch;
  if (begin) {
    pt_printf(frame, ".globl _%s\n.hidden _%s\n.type _%s, %%function\n_%s:\n",
              section, section, section, section);
    if (streq(arch, "x86_64"))
      pt_append(frame, "subq $8, %rsp\n");
    else if (streq(arch, "x86"))
      pt_append(frame, "pushl %ebx\nsubl $8, %esp\n");
    else if (streq(arch, "aarch64") || streq(arch, "aarch64_be"))
      pt_append(frame, "stp x29, x30, [sp, #-16]!\nmov x29, sp\n");
    else if (streq(arch, "sparc64"))
      pt_append(frame, "save %sp, -176, %sp\n");
    else if (streq(arch, "mips64") || streq(arch, "mips64el"))
      pt_printf(frame,
                ".set noreorder\ndaddiu $sp, $sp, -32\nsd $ra, 24($sp)\nsd "
                "$gp, 16($sp)\nlui $gp, %%hi(%%neg(%%gp_rel(_%s)))\ndaddu $gp, "
                "$gp, $25\ndaddiu $gp, $gp, %%lo(%%neg(%%gp_rel(_%s)))\n",
                section, section);
    else if (streq(arch, "riscv32"))
      pt_append(frame, "addi sp, sp, -16\nsw ra, 12(sp)\n");
    else if (streq(arch, "riscv64"))
      pt_append(frame, "addi sp, sp, -16\nsd ra, 8(sp)\n");
    else if (streq(arch, "loongarch64"))
      pt_append(frame, "addi.d $sp, $sp, -16\nst.d $ra, $sp, 8\n");
    else if (streq(arch, "arm") || streq(arch, "armeb"))
      pt_append(frame, ".syntax unified\n.arm\npush {r3, lr}\n");
    else if (streq(arch, "powerpc64le"))
      pt_printf(
          frame,
          "0: addis 2, 12, .TOC.-0b@ha\naddi 2, 2, .TOC.-0b@l\n.localentry "
          "_%s, .-_%s\nmflr 0\nstd 0, 16(1)\nstdu 1, -32(1)\n",
          section, section);
    else if (streq(arch, "s390x"))
      pt_append(frame, "stmg %r6, %r15, 48(%r15)\nlgr %r1, %r15\naghi %r15, "
                       "-160\nstg %r1, 0(%r15)\n");
  } else {
    if (streq(arch, "x86_64"))
      pt_append(frame, "addq $8, %rsp\nret\n");
    else if (streq(arch, "x86"))
      pt_append(frame, "addl $8, %esp\npopl %ebx\nret\n");
    else if (streq(arch, "aarch64") || streq(arch, "aarch64_be"))
      pt_append(frame, "ldp x29, x30, [sp], #16\nret\n");
    else if (streq(arch, "sparc64"))
      pt_append(frame, "ret\nrestore\n");
    else if (streq(arch, "mips64") || streq(arch, "mips64el"))
      pt_append(frame, ".set noreorder\nld $gp, 16($sp)\nld $ra, 24($sp)\njr "
                       "$ra\ndaddiu $sp, $sp, 32\n");
    else if (streq(arch, "riscv32"))
      pt_append(frame, "lw ra, 12(sp)\naddi sp, sp, 16\nret\n");
    else if (streq(arch, "riscv64"))
      pt_append(frame, "ld ra, 8(sp)\naddi sp, sp, 16\nret\n");
    else if (streq(arch, "loongarch64"))
      pt_append(frame, "ld.d $ra, $sp, 8\naddi.d $sp, $sp, 16\njr $ra\n");
    else if (streq(arch, "arm") || streq(arch, "armeb"))
      pt_append(frame, ".syntax unified\n.arm\npop {r3, pc}\n");
    else if (streq(arch, "powerpc64le"))
      pt_append(frame, "addi 1, 1, 32\nld 0, 16(1)\nmtlr 0\nblr\n");
    else if (streq(arch, "s390x"))
      pt_append(frame, "lmg %r6, %r15, 208(%r15)\nbr %r14\n");
  }
}
char *glibc_crt(Options o, Target t, Toolchain tc, Glibc_Version version,
                const char *dir) {
  char *g = path_join(tc.lib_dir, "libc/glibc");
  StrVec common = SV(
      "-w", "-D_LIBC_REENTRANT", "-include",
      path_join(g, "include/libc-modules.h"), "-DMODULE_NAME=libc", "-include",
      path_join(g, "include/libc-symbols.h"), "-DPIC", "-DTOP_NAMESPACE=glibc");
  StrVec assembly = sv_copy(common);
  sv_extend(&assembly, SV("-DSHARED", "-DASSEMBLER", "-Wa,--noexecstack"));
  const char *start =
      glibc_ver_cmp(version, GV(2, 33, 0)) <= 0 ? "start-2.33.S" : "start.S";
  char *start_src = path_join(g, "sysdeps", glibc_source_arch(t.arch), start),
       *error = NULL;
  if (streq(t.arch, "sparc64")) {
    size_t size = 0;
    char *bytes = read_file(start_src, &size);
    if (!bytes)
      return "cannot read SPARC glibc startup source";
    char *text = str_replace(bytes, "%gdop_hix22", "%hi");
    text = str_replace(text, "%gdop_lox10", "%lo");
    text = str_replace(text, "\txor\t", "\tor\t");
    const char *symbols[] = {"main", "__libc_csu_init", "__libc_csu_fini"};
    for (int i = 0; i < 3; i++)
      text = str_replace(text, str_fmt(", %%gdop(%s)", symbols[i]), "");
    start_src = path_join(dir, "start.S");
    error = glibc_write(start_src, text);
    free(bytes);
    if (error)
      return error;
  }
  error =
      glibc_compile(o, t, tc, start_src, path_join(dir, "start.o"), assembly);
  if (error)
    return error;
  for (int i = 0; i < 2; i++) {
    const char *name = i == 0 ? "crti" : "crtn";
    Platform_Text frame = {0};
    const char *sections[] = {"init", "fini"};
    for (int j = 0; j < 2; j++) {
      pt_printf(&frame, ".section .%s,\"ax\",%%progbits\n", sections[j]);
      glibc_frame(&frame, t, sections[j], i == 0);
    }
    pt_append(&frame, ".section .note.GNU-stack,\"\",%progbits\n");
    char *src = path_join(dir, str_fmt("%s.S", name));
    error = glibc_write(src, frame.data);
    free(frame.data);
    if (error)
      return error;
    error = glibc_compile(o, t, tc, src, path_join(dir, str_fmt("%s.o", name)),
                          assembly);
    if (error)
      return error;
  }
  StrVec note_flags = SV("-w", "-I", path_join(g, "csu"), "-D_LIBC_REENTRANT",
                         "-DMODULE_NAME=libc", "-DTOP_NAMESPACE=glibc",
                         "-DASSEMBLER", "-Wa,--noexecstack");
  error = glibc_compile(o, t, tc, path_join(g, "csu/abi-note.S"),
                        path_join(dir, "abi-note.o"), note_flags);
  if (error)
    return error;
  error = glibc_compile(o, t, tc, path_join(g, "csu/init.c"),
                        path_join(dir, "init.o"), SV("-w"));
  if (error)
    return error;
  char *crt1 = path_join(dir, "Scrt1.o");
  if (run_command(SV(tc.lld, "-r", "-o", crt1, path_join(dir, "start.o"),
                     path_join(dir, "abi-note.o"), path_join(dir, "init.o")),
                  o.verbose))
    return "linking glibc startup failed";
  size_t size = 0;
  char *bytes = read_file(crt1, &size);
  if (!bytes)
    return "cannot read glibc startup object";
  bool written = write_file(path_join(dir, "crt1.o"), bytes, size);
  free(bytes);
  if (!written)
    return "cannot write glibc crt1.o";
  StrVec deps =
      SV("stdlib/atexit.c", "stdlib/at_quick_exit.c",
         "sysdeps/pthread/pthread_atfork.c", "debug/stack_chk_fail_local.c");
  if (glibc_ver_cmp(version, GV(2, 32, 0)) <= 0) {
    StrVec names = SV("stat", "fstat", "lstat", "stat64", "fstat64", "lstat64",
                      "fstatat", "fstatat64", "mknodat", "mknod");
    for (size_t i = 0; i < names.len; i++)
      sv_push(&deps, str_fmt("io/%s-2.32.c", names.data[i]));
  }
  if (glibc_ver_cmp(version, GV(2, 33, 0)) <= 0)
    sv_push(&deps, "csu/elf-init-2.33.c");
  StrVec flags = sv_copy(common);
  sv_extend(&flags, SV("-std=gnu11", "-fgnu89-inline", "-fmerge-all-constants",
                       "-frounding-math", "-fno-common", "-fmath-errno",
                       "-ftls-model=initial-exec", "-Qunused-arguments",
                       "-DNO_INITFINI", "-DLIBC_NONSHARED=1"));
  StrVec ar = SV(tc.ar, "rcs", path_join(dir, "libc_nonshared.a"));
  for (size_t i = 0; i < deps.len; i++) {
    char *obj = path_join(dir, str_fmt("nonshared-%zu.o", i));
    error = glibc_compile(o, t, tc, path_join(g, deps.data[i]), obj, flags);
    if (error)
      return error;
    sv_push(&ar, obj);
  }
  if (run_command(ar, o.verbose))
    return "archiving glibc libc_nonshared failed";
  return NULL;
}
Runtime prepare_glibc(Options o, Target t, Toolchain tc, char **error) {
  Runtime rt = {0};
  *error = NULL;
  if (!needs_link(o) || o.relocatable || o.no_stdlib)
    return rt;
  if (o.static_link) {
    *error = "static GNU libc is not supplied by the bundled ABI stubs; "
             "provide ZIG_LIBC";
    return rt;
  }
  bool supported =
      ((streq(t.abi, "gnu") ||
        (streq(t.abi, "gnux32") && streq(t.arch, "x86_64"))) &&
       in_target_inventory("x86 x86_64 aarch64 aarch64_be riscv32 riscv64 "
                           "loongarch64 sparc64 s390x powerpc64le",
                           t.arch)) ||
      ((streq(t.arch, "arm") || streq(t.arch, "armeb")) &&
       (streq(t.abi, "gnueabi") || streq(t.abi, "gnueabihf"))) ||
      ((streq(t.arch, "mips64") || streq(t.arch, "mips64el")) &&
       streq(t.abi, "gnuabi64"));
  if (!streq(t.os, "linux") || !supported) {
    *error =
        "bundled GNU libc supports "
        "x86/x86_64(-gnux32)/aarch64/aarch64_be/riscv32/riscv64/loongarch64/"
        "sparc64/s390x/powerpc64le-linux-gnu arm/armeb-linux-gnueabi/gnueabihf "
        "and mips64/mips64el-linux-gnuabi64 only";
    return rt;
  }
  const char *vs =
      streq(t.glibc_version, "") ? glibc_default_version(t) : t.glibc_version;
  bool ok = false;
  Glibc_Version version = glibc_version(vs, &ok);
  if (!ok) {
    *error = str_fmt("invalid GNU libc version: %s", vs);
    return rt;
  }
  Glibc_Version minimum = GV(2, 2, 5);
  if (in_target_inventory("sparc64 mips64 mips64el x86", t.arch))
    minimum = GV(2, 0, 0);
  else if (streq(t.arch, "arm") || streq(t.arch, "armeb"))
    minimum = GV(2, 4, 0);
  else if (streq(t.arch, "aarch64") || streq(t.arch, "aarch64_be"))
    minimum = GV(2, 17, 0);
  else if (streq(t.arch, "riscv32"))
    minimum = GV(2, 33, 0);
  else if (streq(t.arch, "loongarch64"))
    minimum = GV(2, 36, 0);
  else if (streq(t.arch, "riscv64"))
    minimum = GV(2, 27, 0);
  else if (streq(t.arch, "s390x"))
    minimum = GV(2, 2, 0);
  else if (streq(t.arch, "powerpc64le"))
    minimum = GV(2, 19, 0);
  if (glibc_ver_cmp(version, minimum) < 0) {
    *error = str_fmt("%s needs glibc >= %s", t.arch, glibc_ver_name(minimum));
    return rt;
  }
  size_t size = 0;
  unsigned char *data = (unsigned char *)read_file(
      path_join(tc.lib_dir, "libc/glibc/abilists"), &size);
  if (!data) {
    *error = str_fmt("cannot read glibc abilists: %s", strerror(errno));
    return rt;
  }
  if (size > 800 * 1024) {
    free(data);
    *error = "oversized glibc abilists";
    return rt;
  }
  char *identity =
      str_fmt("glibc-recipe-9|%s|%s|%s|%s|%s|%s", stable_compiler_identity(tc),
              tc.lib_dir, t.clang, t.arch, vs, "O2-PIC-no-stack-protector");
  uint64_t hash = runtime_hash(identity);
  for (size_t i = 0; i < size; i++)
    hash = (hash ^ data[i]) * UINT64_C(1099511628211);
  char *dir = path_join(tc.cache_dir, str_fmt("glibc-%016" PRIx64, hash));
  if (!file_exists(dir))
    ensure_dir(dir);
  char *marker = path_join(dir, "complete");
  if (!file_exists(marker)) {
    *error = glibc_emit(data, size, t, version, dir);
    if (*error)
      goto done;
    for (int i = 0; i < 8; i++) {
      const Glibc_Lib lib = GLIBC_LIBS[i];
      if (i >= 4 && glibc_ver_cmp(version, GV(2, 34, 0)) >= 0)
        continue;
      char *obj = path_join(dir, str_fmt("%s.o", lib.name));
      *error =
          glibc_compile(o, t, tc, path_join(dir, str_fmt("%s.s", lib.name)),
                        obj, (StrVec){0});
      if (*error)
        goto done;
      char *filename = str_fmt("lib%s.so.%d", lib.name, lib.sover),
           *soname =
               streq(lib.name, "ld") ? path_base(t.dynamic_linker) : filename;
      if (run_command(
              SV(tc.lld, "-shared", "-z", "notext", "--no-undefined-version",
                 str_fmt("--version-script=%s", path_join(dir, "all.map")),
                 "-soname", soname, "-o", path_join(dir, filename), obj),
              o.verbose)) {
        *error = "linking glibc symbol stubs failed";
        goto done;
      }
    }
    *error = glibc_crt(o, t, tc, version, dir);
    if (*error)
      goto done;
    for (int i = 0; i < 8; i++) {
      const Glibc_Lib lib = GLIBC_LIBS[i];
      const char *filename = str_fmt("lib%s.so.%d", lib.name, lib.sover);
      if (i >= 4 && glibc_ver_cmp(version, GV(2, 34, 0)) >= 0)
        filename = "libc.so.6";
      char *script =
          streq(lib.name, "c")
              ? str_fmt("GROUP ( \"%s\" \"%s\" )\n", path_join(dir, filename),
                        path_join(dir, "libc_nonshared.a"))
              : str_fmt("INPUT ( \"%s\" )\n", path_join(dir, filename));
      *error =
          glibc_write(path_join(dir, str_fmt("lib%s.so", lib.name)), script);
      if (*error)
        goto done;
    }
    *error = glibc_write(marker, identity);
    if (*error)
      goto done;
  }
  sv_extend(&rt.link_flags, SV("-nostdlib", "-L", dir));
  if (!o.no_startfiles) {
    if (!o.shared)
      sv_push(&rt.link_flags, path_join(dir, o.pie < 0 ? "crt1.o" : "Scrt1.o"));
    sv_push(&rt.link_flags, path_join(dir, "crti.o"));
  }
  if (!o.no_defaultlibs) {
    sv_push(&rt.link_flags,
            "-Wl,--push-state,-Bdynamic,--as-needed,--start-group");
    for (int i = 0; i < 8; i++) {
      const Glibc_Lib lib = GLIBC_LIBS[i];
      if (i >= 4 && glibc_ver_cmp(version, GV(2, 34, 0)) >= 0)
        continue;
      sv_push(&rt.link_flags,
              path_join(dir, str_fmt("lib%s.so.%d", lib.name, lib.sover)));
    }
    sv_push(&rt.link_flags, path_join(dir, "libc_nonshared.a"));
    if (!streq(tc.builtins, ""))
      sv_push(&rt.link_flags, tc.builtins);
    sv_push(&rt.link_flags, "-Wl,--end-group,--pop-state");
  }
  if (!o.no_startfiles)
    sv_push(&rt.link_flags, path_join(dir, "crtn.o"));
  if (!o.shared && !streq(t.dynamic_linker, ""))
    sv_push(&rt.link_flags,
            str_fmt("-Wl,--dynamic-linker=%s", t.dynamic_linker));
done:
  free(data);
  return rt;
}
