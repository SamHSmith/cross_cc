# cross_cc

**cross_cc is a C/C++ compiler driver and toolchain written in C.** It embeds
pinned, statically linked Clang, LLVM and LLD 22.1.8, providing cross-compilation,
target selection and runtime-library handling in a self-contained package.
Clang compiles your code; LLD links it. The small LLVM API bridge is C++.

cross_cc can be a drop-in replacement for `zig cc`.

## Usage

```sh
cross_cc cc hello.c -o hello
cross_cc c++ hello.cpp -o hello
cross_cc cc -target aarch64-linux-musl hello.c -o hello
```

Also includes `ar`, `ranlib`, `lib`, `dlltool`, `objcopy`, `rc` and `dsymutil`.
Keep the executable and its `lib/` resources together. Target runtimes are built
from pinned sources as needed.

**Microsoft targets:** SDK/CRT resources download only after license consent
(interactive, or `--cross-cc-accept-msvc-license`) and are cached for offline use.
Use `--cross-cc-msvc=system` for an installed SDK. MinGW needs no Microsoft SDK.

## Build

Requires a C11 compiler, a C++17 compiler, curl, tar and make. Windows builds need
an MSVC developer shell. The driver and build tools are C; the LLVM bridge uses
LLVM's C++ API.

```sh
make REFERENCE=1 setup
make
make test
```

See `tools/build.c` and `tools/release.c` (`--help`) for options and releases.
Dependencies and compatibility inputs are pinned in the root `*.lock.json` files.
The bulk headers and runtime sources are not stored in Git. `make` prepares them
under `lib/` from the SHA-256-pinned source archive; `make resources` does only
that step. A cold source build needs network access or a preseeded
`.cache/downloads/zig-0.17.1.tar.xz`. Once prepared, resources are reused offline;
`make resources` also reconstructs them offline from the verified cached archive.
Release bundles still contain the complete resource tree: ordinary compilation
does not download these sources. See [lib/README.md](lib/README.md) for cache
recovery and refresh instructions.

## Reproducibility and limits

Reproducible output requires matching inputs, paths, flags and SDKs. Host release
baselines are Linux x86-64 (glibc 2.34+, x86-64-v2), Windows x86-64 (x86-64-v2),
and macOS 15+ on Apple Silicon or Intel (x86-64-v2). A host baseline is not a claim
that every host/target combination has been tested.

SPARC parity and bundled ASan/MSan are unsupported; TSan requires supported
dynamic Linux targets. UBSan coverage varies. MSVC libc++ support on x86/ARM64
is not verified; DLL builds need the matching Microsoft redistributable.

## License

MIT; preserve dependency notices. See [LICENSE](LICENSE), [COPYING](COPYING)
and [licenses/](licenses/).
