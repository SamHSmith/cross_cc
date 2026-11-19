# Downloaded headers and runtime sources

`make resources` (also run by `make`, `make setup`, `make test`, and packaging)
populates the ignored `include/`, `libc/`, `libcxx/`, `libcxxabi/`, `libunwind/`
and `libtsan/` directories from the Zig 0.17.1 source release:

- repository: https://codeberg.org/ziglang/zig.git
- tag 0.17.1, commit be248cc1fdc3cc654f9ec8ba68514bcb93b204fe
- archive: https://ziglang.org/download/0.17.1/zig-0.17.1.tar.xz
  (SHA-256 1e89bf06610b769a8c77676c6c8d54b093a0db60f4891f2f91014829012f0e24)

They hold the libc headers and sources (glibc, musl, MinGW-w64, wasi-libc,
FreeBSD, NetBSD, Darwin and others), libc++, libc++abi, libunwind and TSan
sources that cross_cc uses to compile and link for any target without a system
sysroot. Each component keeps its own license files in place. Zig's MIT license
is in `deps/llvm-bridge/LICENSE`; see also `COPYING`.

The archive URL, SHA-256 and directory selection live in
`upstream.lock.json:resources`. Extraction is staged under `.cache/` on disk;
`.cache/downloads/` keeps the verified archive, and `lib/.resources-receipt`
binds the installed directories to the archive and extraction recipe. Cached
resources are reused without rehashing every extracted file. To check or repair
interior edits, move all six generated directories and the receipt aside and run
`make resources` again; the original archive is verified before extraction.

A failed acquisition leaves `.cache/resources-stage` for inspection. Only one
acquisition may own that stage; after confirming no acquisition is active, move
it aside before retrying. If publication was interrupted or the pin changed,
also move the six generated directories and receipt aside. Existing unrecognized
directories are never overwritten, so preserve any local patches before refresh.
All build/download scratch stays under the checkout, not `/tmp`.

To refresh the compatibility baseline, update the resource pin and this file,
regenerate the directories, and verify the runtime recipes and package tests.
The small supplemental sources in `vendor/` and our modified LLVM bridge remain
tracked separately; they are not interchangeable with these archive directories.
