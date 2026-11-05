# binja_wasm

[![Build](https://github.com/SecondNewtonLaw/binja_wasm/actions/workflows/build.yml/badge.svg)](https://github.com/SecondNewtonLaw/binja_wasm/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/SecondNewtonLaw/binja_wasm?include_prereleases)](https://github.com/SecondNewtonLaw/binja_wasm/releases/latest)

Native [Binary Ninja](https://binary.ninja) plugin for WebAssembly, providing an architecture to work with web assembly binaries, including decode, text and lifting into BNIL.

Only created this because the [python variant](https://github.com/ivision-research/binjawa/) originally made available was insanely slow, lacked BNIL lifting and was last updated years ago, as well as just not worth iterating on due to this when dealing with massive binaries in some web contexts.

### State of Development

Work in progress. The parser, branch map, stack-height analysis, lifter, and view are in place; import stubs and debug info are still pending.

The native lifter has a headless validation harness (see [tests/README.md](tests/README.md)). It compiles the real `src/bn/lifter.cpp` against a partial IL, so the tests exercise shipping code without a Binary Ninja install or license. The current suite covers:

- a differential numeric matrix (all integer/float/comparison/conversion opcodes) against an independent WASM interpreter,
- memory, locals/globals, calls, multi-value returns, structured control flow, and v128 transport,
- a 531-case opcode coverage matrix that fails on `unimplemented` IL,
- golden IL snapshots for readability review,
- a clang-produced WASM fixture and a corpus probe/report tool.

SIMD and atomic instructions are covered structurally and lifted to `__builtin_*` intrinsics; their runtime semantics are left to the intrinsic implementations. Bulk memory (`memory.copy/fill/init`) is tested semantically. Signatures beyond 16 arguments or 8 results are flagged with `unimplemented` rather than silently truncated.

The plugin registers as `wasm-native` / `WASM-native` so it can sit next to the Python plugin. Remove the Python one to avoid two views claiming `.wasm`.

### Installing

Grab the zip matching your Binary Ninja build from [Releases](https://github.com/SecondNewtonLaw/binja_wasm/releases) and drop `binja_wasm.dll` into `%APPDATA%\Binary Ninja\plugins`.

Releases are built automatically against every Binary Ninja stable tag (`bn-<version>`), plus a rolling `bn-dev` pre-release built from the `dev` branch of `binaryninja-api`. Windows x64 only for now.

The plugin must be built against the API revision of your Binary Ninja install (`api_REVISION.txt` in the install directory); otherwise the core rejects it. If no release matches your build, build it yourself.

### Building

Requirements: CMake 3.28+, Ninja, LLVM/clang, MSVC build tools (for the Windows SDK and CRT).

The presets are host-specific: `release` on Windows, `linux-release` on Linux, and `macos-release` on macOS. The build directory follows the preset (`cmake-build-<preset>`).

```
# Windows
cmake --preset release
cmake --build --preset release
ctest --test-dir cmake-build-release
```

On Linux or macOS, substitute `linux-release` or `macos-release` for `release` in all three commands (and in the `cmake-build-*` directory name).

- `-DLLVM_ROOT=<path>` points at the LLVM install (default `F:/SDKs/LLVM`, or the `LLVM_ROOT` environment variable).
- `-DBN_API_REVISION=<commit|tag>` selects the `binaryninja-api` revision to fetch.
- `-DBN_API_PATH=<checkout>` reuses a local `binaryninja-api` checkout.
- `-DBN_INSTALL_DIR=<path>` points at the Binary Ninja install used for linking.
- `-DBN_ALLOW_STUBS=ON -DBN_INSTALL_DIR=` links against generated stubs instead, no install needed (this is what CI does).
- `-DBINJA_WASM_BUILD_PLUGIN=OFF` skips the plugin target and the Binary Ninja dependency entirely; the lift tests and probe still build. This is the fastest way to work on the lifter.
- The DLL is copied to `%APPDATA%\Binary Ninja\plugins` after each build (`-DBN_USER_PLUGINS_DIR=` to change it).

Headless test/probe build:

```
# Use release (Windows), linux-release (Linux), or macos-release (macOS).
cmake --preset release -DBINJA_WASM_BUILD_PLUGIN=OFF
cmake --build --preset release
ctest --test-dir cmake-build-release
./cmake-build-release/wasm_lift_probe path/to/module.wasm
```

The probe reports unimplemented opcodes, spills, intrinsic usage, native-vs-intrinsic arithmetic, and optionally dumps the lifted IL with `--dump <hex-offset>`; `tools/lift_audit.py` aggregates the `--json` output over a corpus.

`tests/fixtures/lift_issues.wasm` (source: `tests/fixtures/lift_issues.wat`) is a targeted regression module for the lifting fixes. Open it in Binary Ninja and run `tools/bn_check_lift.py` from `Tools > Run Script` to verify the active plugin build, scan for unimplemented IL, and dump the LLIL/HLIL of each case. The script checks the new `gv*`/`tv*` banks as a build marker, so it will tell you if the old manager-installed DLL is still the active one.

Created using Claude Sonnet 5.5, after a spiteful afternoon reversing a WASM binary with the Python plugin!
