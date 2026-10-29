# clang++ driver targeting the MSVC ABI, linked with lld-link. No cl.exe / clang-cl.
# Override LLVM location with -DLLVM_ROOT=... (default F:/SDKs/LLVM).

if(DEFINED LLVM_ROOT)
  set(_llvm "${LLVM_ROOT}")
elseif(DEFINED ENV{LLVM_ROOT})
  set(_llvm "$ENV{LLVM_ROOT}")
else()
  set(_llvm "F:/SDKs/LLVM")
endif()

set(CMAKE_C_COMPILER   "${_llvm}/bin/clang.exe"    CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "${_llvm}/bin/clang++.exe"  CACHE FILEPATH "")
set(CMAKE_RC_COMPILER  "${_llvm}/bin/llvm-rc.exe"  CACHE FILEPATH "")

set(_wasm_triple "x86_64-pc-windows-msvc")

# dll runtime (/MD, dynamic UCRT) to match Binary Ninja's plugin host.
add_compile_options(
  "--target=${_wasm_triple}"
  "-fms-runtime-lib=dll"
  "-fms-compatibility"
  "-fms-extensions")
add_link_options(
  "--target=${_wasm_triple}"
  "-fuse-ld=lld")
