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

Build with **cross_cc**, not a system C/C++ compiler or Make:

```sh
./build.sh                         # build bin/cross_cc and its tool aliases
./build.sh --profile=debug         # same pipeline, different optimization flags
./build.sh setup --reference       # prepare the optional test-reference compiler
./build.sh test
./build.sh help
```

On Windows, use `build.cmd` in place of `./build.sh`. The small shell/PowerShell
entry point selects a compiler and compiles the C build tools; all dependency,
compile, link, test and packaging logic lives in `tools/`.

Compiler selection, in order:

1. `--compiler=/path/to/cross_cc` (or `CROSS_CC_COMPILER`).
2. `cross_cc` installed on `PATH`.
3. The matching checked-in [v1 backstop](backstop/v1/README.md), verified against
   `backstop/v1/SHA256SUMS` and extracted under `.cache/bootstrap/`.

Use the **host** compiler package and keep its executable and resources together.
Symlinks are resolved before selection. Set `CROSS_CC_SEED_ROOT` only for an
unusual installation whose resource root cannot be found beside or above the
executable. An invalid explicit compiler fails rather than silently falling back.

Normal builds and releases share the staged build in `tools/release.c`:

```text
selected cross_cc → fresh current cross_cc → final cross_cc
                                              ↓
                           rebuild with final compiler and compare SHA-256
```

The bridge, version objects, driver and final link are rebuilt in each stage.
A native build checks compiler convergence and same-path object/executable
repeatability before publishing. A cross build cannot execute its target, so it
checks repeated compilation with the freshly built host compiler instead; that
is recorded as repeatability, not native convergence. Receipts record the seed
hash and the verified output hashes. Debug and release use the same stages.

`./build.sh release` uses this pipeline and packages the result. On Windows,
pass `--host-sdk=DIR` (or `CROSS_CC_HOST_SDK`) for an MSVC + Windows SDK in xwin
splat layout (`crt/`, `sdk/`) whose license you already accepted. On macOS,
`--host-sdk=DIR`, `SDKROOT`, or `xcrun` locates the Apple SDK; pinned zstd is built
automatically. Intel Mac also needs a prepared LLVM 22.1.8 `LLVM_PREFIX`, since
upstream publishes no matching Intel Mac archive.

Cross releases also need the target LLVM SDK, platform SDK and literal
bridge/link recipes. `--host-sdk` always selects the build host's SDK; release
`--sdk` selects the target's. See `build/tools/release --help` for `--target`,
`--matrix`, `--llvm-prefix`, `--bridge-args`, `--link-args`, dependency notices
and the authenticated `--msvc-helper` required in Mac release bundles.
`./build.sh package` can package an already built compiler.

The host needs curl and tar; Linux backstop extraction also needs unzip and
sha256sum (or shasum), and macOS uses its built-in tar and shasum. Windows uses
PowerShell's ZIP and SHA-256 support. No Make, configure, system compiler or system
linker is used in the normal/release pipeline. Apple and Microsoft targets still
need the applicable SDK inputs and license acceptance; the compiler backstop is
**not a complete offline release-rebuild kit**. The optional Intel Mac LLVM SDK
source-build utility has its own CMake/Ninja prerequisites; ordinary builds use
a prepared, matching static LLVM SDK.

Dependencies and compatibility inputs are pinned in the root `*.lock.json` files.
Bulk headers/runtime sources and curated musl/WASI supplements are reconstructed
under ignored `lib/` and `vendor/` paths. `./build.sh resources` prepares them
from pinned archives and selection manifests. Cold builds need the pinned archives
from the network or a preseeded cache, including LLVM development libraries and
support inputs—not just the compiler ZIP. Completed caches can be reused offline.
All scratch stays under the checkout, not `/tmp`. Release bundles still include
the complete resource tree, so ordinary compilation needs no source downloads.
See [lib/README.md](lib/README.md) for resource cache recovery and refresh.

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
