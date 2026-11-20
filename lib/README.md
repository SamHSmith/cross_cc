# Downloaded headers and runtime sources

`./build.sh resources` (also run by the build, setup, test and packaging commands;
use `build.cmd` on Windows)
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
`./build.sh resources` again; the original archive is verified before extraction.

A failed acquisition leaves `.cache/resources-stage` for inspection. Only one
acquisition may own that stage; after confirming no acquisition is active, move
it aside before retrying. If publication was interrupted or the pin changed,
also move the six generated directories and receipt aside. Existing unrecognized
directories are never overwritten, so preserve any local patches before refresh.
All build/download scratch stays under the checkout, not `/tmp`.

To refresh the compatibility baseline, update the resource pin and this file,
regenerate the directories, and verify the runtime recipes and package tests.
The curated musl/WASI supplements are reconstructed separately at their existing
`vendor/` paths by the same command. `musl.lock.json` and
`vendor/wasi/dlmalloc.lock.json` pin their archives; `tools/resources/*.files`
select the exact files and pin each SHA-256. Their generated paths are ignored,
while our WASI `uname.c`/`valloc.c`, notices and the modified LLVM bridge remain
tracked. Release bundles include all reconstructed sources.

Supplement caches recheck every selected file and reject extra files, symlinks,
modified contents and stale pins. For a deliberate refresh or repair, preserve
local edits and move `vendor/musl/` (musl) or the three generated WASI directories
(`dlmalloc/`, `libc-bottom-half/`, `libc-top-half/`) aside with the matching
`vendor/.resources-<musl|wasi>-receipt`, then rerun `./build.sh resources`.
Keep the tracked WASI shims, lock and notices in place. Cached archives allow
offline reconstruction; a failed run retains `.cache/supplements-<name>-stage`
for inspection. After confirming no acquisition is active, move that stage aside
before retrying.
