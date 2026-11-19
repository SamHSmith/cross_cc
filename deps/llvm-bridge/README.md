# LLVM bridge sources

These files expose Clang, LLD and llvm-ar as plain C entry points called by
`src/backend.c`: `BridgeClang_main`, `BridgeLlvmAr_main`, and
`BridgeLLDLink{ELF,COFF,Wasm,MachO,MinGW}`. `cross_cc_dsymutil_main` wraps LLVM's
generated C++ dsymutil driver. LLVM's C++ API stays inside this bridge.

The bridge comes from Zig 0.17.1 (commit
be248cc1fdc3cc654f9ec8ba68514bcb93b204fe): `src/zig_clang_driver.cpp`,
`zig_clang_cc1_main.cpp`, `zig_clang_cc1as_main.cpp`, `zig_llvm.cpp`,
`zig_llvm-ar.cpp`, and `zig_llvm.h`. Local changes rename files and symbol prefixes
(`Zig`/`ZIG_` to `Bridge`/`BRIDGE_`), rename the header guard and tracing environment
variables (`CROSS_CC_LLVM_TIME_TRACE_*`), change the LLVM option program-name
prefix, add C-compatible type declarations, and provide the additional C ABI wrappers above. `LICENSE` is Zig's MIT
license, which covers these files.

SHA-256 (`sha256sum *.cpp *.h`):

```
7705c2a5e60d067e9858bb9f72ede2b6012ede6c020b13cc7871a894246ec50f  clang_cc1_main.cpp
41ab8f70668e50cd58977c4072eb3cd73fdace21603207fc26c2ea8d33cb997c  clang_cc1as_main.cpp
a053256ca447dd7c29daa94012da2887255f32b21e23c1be3c6151f26de0010d  clang_driver.cpp
72eaaeffd9c61962d1d89083cb1fd7feef5666e9a330a41ee67bfb093b8657c7  llvm_ar.cpp
ca987f2998a48e9db8a4084273f836b6aa3c5fbf1940fad4ab588c0f13b48ced  llvm_bridge.cpp
1b087380c2f9b96d1f239598d7033baa701a8fde4402ae32e7e875679c3decdd  llvm_bridge.h
```
