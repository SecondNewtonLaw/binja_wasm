cmake_minimum_required(VERSION 3.28)
project(binja_wasm CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Silences BN vendored rapidjson/jsoncpp warnings; global so CPM dep scans honor it.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_compile_options(
    -Wno-unknown-warning-option
    -Wno-pragmas
    -Wno-macro-redefined)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/CPM.cmake)

option(USE_CCACHE "Use ccache as the compiler launcher when available" ON)
if(USE_CCACHE)
  include(cmake/ccache.cmake)
endif()

set(BN_INSTALL_DIR "C:/Users/Dottik/AppData/Local/Programs/Vector35/BinaryNinja"
    CACHE PATH "Binary Ninja install dir (contains binaryninjacore.dll)")
set(BN_API_PATH "" CACHE PATH "Local binaryninja-api checkout; empty = fetch via CPM")
# Must match <BN_install>/api_REVISION.txt; mismatch makes the core reject the plugin.
set(BN_API_REVISION "775c3b5eed8598217b20e1087afe7aa092a9b4b1"
    CACHE STRING "binaryninja-api commit matching the installed BN")
option(BINJA_WASM_BUILD_TESTS "Build the standalone parser tests" ON)

set(HEADLESS ON CACHE BOOL "" FORCE)
if(BN_API_PATH AND EXISTS "${BN_API_PATH}/CMakeLists.txt")
  CPMAddPackage(
    NAME binaryninjaapi
    SOURCE_DIR "${BN_API_PATH}"
    EXCLUDE_FROM_ALL YES)
else()
  CPMAddPackage(
    NAME binaryninjaapi
    GITHUB_REPOSITORY Vector35/binaryninja-api
    GIT_TAG ${BN_API_REVISION}
    EXCLUDE_FROM_ALL YES)
endif()

# BN-independent WASM core: parser, branch map, stack heights
add_library(wasm_core STATIC
  src/wasm/opcodes.cpp
  src/wasm/decode.cpp
  src/wasm/module.cpp
  src/wasm/analysis.cpp
  src/wasm/pointers.cpp)
target_include_directories(wasm_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(wasm_core PUBLIC cxx_std_23)

add_library(binja_wasm SHARED
  src/plugin.cpp
  src/bn/architecture.cpp
  src/bn/view.cpp
  src/bn/regs.cpp
  src/bn/intrinsics.cpp
  src/bn/state.cpp
  src/bn/lifter.cpp)
target_include_directories(binja_wasm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(binja_wasm PRIVATE cxx_std_23)
target_link_libraries(binja_wasm PRIVATE wasm_core binaryninjaapi)
target_compile_definitions(binja_wasm PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(binja_wasm PROPERTIES PREFIX "" OUTPUT_NAME "binja_wasm")

set(BN_USER_PLUGINS_DIR "$ENV{APPDATA}/Binary Ninja/plugins"
    CACHE PATH "Binary Ninja user plugins directory (auto-copy on build)")
add_custom_command(TARGET binja_wasm POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${BN_USER_PLUGINS_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:binja_wasm>" "${BN_USER_PLUGINS_DIR}/"
  COMMENT "Installing binja_wasm.dll -> ${BN_USER_PLUGINS_DIR}"
  VERBATIM)

# Tests (no BN needed)
if(BINJA_WASM_BUILD_TESTS)
  enable_testing()
  add_executable(wasm_core_test tests/module_test.cpp)
  target_link_libraries(wasm_core_test PRIVATE wasm_core)
  add_test(NAME wasm_core_test COMMAND wasm_core_test)
endif()
