# binja_wasm

[![Build](https://github.com/SecondNewtonLaw/binja_wasm/actions/workflows/build.yml/badge.svg)](https://github.com/SecondNewtonLaw/binja_wasm/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/SecondNewtonLaw/binja_wasm?include_prereleases)](https://github.com/SecondNewtonLaw/binja_wasm/releases/latest)

Native [Binary Ninja](https://binary.ninja) plugin for WebAssembly, providing an architecture to work with web assembly binaries, including decode, text and lifting into BNIL.

Only created this because the [python variant](https://github.com/ivision-research/binjawa/) originally made available was insanely slow, lacked BNIL lifting and was last updated years ago, as well as just not worth iterating on due to this when dealing with massive binaries in some web contexts.

### State of Development

Work in progress. The parser, branch map, stack-height analysis, lifter, and view are in place; import stubs and debug info are still pending. Expect lifting gaps on less common opcodes.

The plugin registers as `wasm-native` / `WASM-native` so it can sit next to the Python plugin. Remove the Python one to avoid two views claiming `.wasm`.

### Installing

Grab the zip matching your Binary Ninja build from [Releases](https://github.com/SecondNewtonLaw/binja_wasm/releases) and drop `binja_wasm.dll` into `%APPDATA%\Binary Ninja\plugins`.

Releases are built automatically against every Binary Ninja stable tag (`bn-<version>`), plus a rolling `bn-dev` pre-release built from the `dev` branch of `binaryninja-api`. Windows x64 only for now.

The plugin must be built against the API revision of your Binary Ninja install (`api_REVISION.txt` in the install directory); otherwise the core rejects it. If no release matches your build, build it yourself.

### Building

Requirements: CMake 3.28+, Ninja, LLVM/clang, MSVC build tools (for the Windows SDK and CRT).

```
cmake --preset release
cmake --build --preset release
ctest --test-dir cmake-build-release
```

- `-DLLVM_ROOT=<path>` points at the LLVM install (default `F:/SDKs/LLVM`, or the `LLVM_ROOT` environment variable).
- `-DBN_API_REVISION=<commit|tag>` selects the `binaryninja-api` revision to fetch.
- `-DBN_API_PATH=<checkout>` reuses a local `binaryninja-api` checkout.
- `-DBN_INSTALL_DIR=<path>` points at the Binary Ninja install used for linking.
- `-DBN_ALLOW_STUBS=ON -DBN_INSTALL_DIR=` links against generated stubs instead, no install needed (this is what CI does).
- The DLL is copied to `%APPDATA%\Binary Ninja\plugins` after each build (`-DBN_USER_PLUGINS_DIR=` to change it).

Created using Claude Sonnet 5.5, after a spiteful afternoon reversing a WASM binary with the Python plugin!