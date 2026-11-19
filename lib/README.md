# Checked-in headers and runtime sources

`include/`, `libc/`, `libcxx/`, `libcxxabi/`, `libunwind/` and `libtsan/` are
unchanged copies of the same directories under `lib/` in the Zig 0.17.1 source
release:

- repository: https://codeberg.org/ziglang/zig.git
- tag 0.17.1, commit be248cc1fdc3cc654f9ec8ba68514bcb93b204fe
- archive: https://ziglang.org/download/0.17.1/zig-0.17.1.tar.xz
  (SHA-256 1e89bf06610b769a8c77676c6c8d54b093a0db60f4891f2f91014829012f0e24)

They hold the libc headers and sources (glibc, musl, MinGW-w64, wasi-libc,
FreeBSD, NetBSD, Darwin and others), libc++, libc++abi, libunwind and TSan
sources that cross_cc uses to compile and link for any target without a system
sysroot. Each component keeps its own license files in place. Zig's MIT license
is in `deps/llvm-bridge/LICENSE`; see also `COPYING`.

To refresh after a new compatibility baseline, replace these directories with
the ones from the new release and update this file.
