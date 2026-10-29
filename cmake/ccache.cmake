find_program(CCACHE_EXECUTABLE
  NAMES ccache ccache.exe
  HINTS
    "$ENV{ProgramFiles}/ccache"
    "C:/ProgramData/chocolatey/bin"
    "/usr/local/bin"
    "/usr/bin"
    "/opt/homebrew/bin")

if(CCACHE_EXECUTABLE)
  message(STATUS "ccache: ${CCACHE_EXECUTABLE}")
  set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_EXECUTABLE}" CACHE STRING "C compiler launcher")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_EXECUTABLE}" CACHE STRING "C++ compiler launcher")
else()
  message(STATUS "ccache not found, building without compiler caching")
endif()
