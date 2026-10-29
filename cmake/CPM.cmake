# CPM.cmake bootstrap.
# Downloads the real CPM.cmake package manager on first configure, then includes it.
# Override the version/cache location with -DCPM_DOWNLOAD_VERSION / -DCPM_SOURCE_CACHE.

set(CPM_DOWNLOAD_VERSION 0.40.5)

if(CPM_SOURCE_CACHE)
  set(CPM_DOWNLOAD_LOCATION "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
elseif(DEFINED ENV{CPM_SOURCE_CACHE})
  set(CPM_DOWNLOAD_LOCATION "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
else()
  set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
endif()

get_filename_component(CPM_DOWNLOAD_LOCATION "${CPM_DOWNLOAD_LOCATION}" ABSOLUTE)

function(_bnmcp_download_cpm)
  if(EXISTS "${CPM_DOWNLOAD_LOCATION}")
    return()
  endif()
  message(STATUS "Downloading CPM.cmake v${CPM_DOWNLOAD_VERSION} -> ${CPM_DOWNLOAD_LOCATION}")
  file(DOWNLOAD
    "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake"
    "${CPM_DOWNLOAD_LOCATION}"
    STATUS _cpm_status)
  list(GET _cpm_status 0 _cpm_code)
  if(NOT _cpm_code EQUAL 0)
    list(GET _cpm_status 1 _cpm_msg)
    message(FATAL_ERROR "Failed to download CPM.cmake: ${_cpm_msg}")
  endif()
endfunction()

_bnmcp_download_cpm()
include("${CPM_DOWNLOAD_LOCATION}")
