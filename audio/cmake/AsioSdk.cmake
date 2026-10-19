# SPDX-License-Identifier: GPL-3.0-only
# Optional Windows ASIO host API. GPLv3 alternative of the official SDK.
option(BEATBENCH_ENABLE_ASIO "Build Windows ASIO output using the GPLv3 SDK" OFF)
set(BEATBENCH_ASIO_SDK_ROOT "" CACHE PATH "External ASIO SDK root (common/asio.h); empty downloads the pinned official SDK")

if(BEATBENCH_ENABLE_ASIO)
  if(NOT WIN32)
    message(FATAL_ERROR "BEATBENCH_ENABLE_ASIO is supported only on Windows")
  endif()
  if(BEATBENCH_ASIO_SDK_ROOT)
    get_filename_component(_bb_asio_root "${BEATBENCH_ASIO_SDK_ROOT}" ABSOLUTE)
  else()
    # Stable official package, not the floating /asiosdk redirect or a mirror.
    FetchContent_Declare(beatbench_asiosdk
      URL https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip
      URL_HASH SHA256=d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca
      DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(beatbench_asiosdk)
    set(_bb_asio_root "${beatbench_asiosdk_SOURCE_DIR}")
  endif()
  foreach(_file IN ITEMS common/asio.h common/asio.cpp common/asiosys.h
      common/iasiodrv.h common/asiodrvr.h common/combase.h host/asiodrivers.cpp host/asiodrivers.h host/ginclude.h
      host/pc/asiolist.cpp host/pc/asiolist.h LICENSE.txt common/LICENSE.txt README.md)
    if(NOT EXISTS "${_bb_asio_root}/${_file}")
      message(FATAL_ERROR "Incomplete ASIO SDK: missing ${_file} under ${_bb_asio_root}. Use the documented GPLv3 SDK root.")
    endif()
  endforeach()
  # Override stale PortAudio cache entries, including a previous PA_USE_ASIO=OFF.
  set(ASIOSDK_ROOT_DIR "${_bb_asio_root}" CACHE PATH "ASIO SDK root" FORCE)
  set(ASIOSDK_INCLUDE_DIR "${_bb_asio_root}/common" CACHE PATH "ASIO SDK includes" FORCE)
  set(PA_USE_ASIO ON CACHE BOOL "Enable ASIO in PortAudio" FORCE)
  # Keep exact embedded notices and corresponding SDK sources alongside binaries.
  # Packaging must carry this directory when distributing an ASIO-enabled build.
  set(BEATBENCH_AUDIO_NOTICES_DIR "${CMAKE_CURRENT_BINARY_DIR}/third-party" CACHE INTERNAL "Audio third-party source/license materials" FORCE)
  file(MAKE_DIRECTORY "${BEATBENCH_AUDIO_NOTICES_DIR}/asiosdk")
  file(COPY "${_bb_asio_root}/LICENSE.txt" "${_bb_asio_root}/README.md"
       "${_bb_asio_root}/common" "${_bb_asio_root}/host"
       DESTINATION "${BEATBENCH_AUDIO_NOTICES_DIR}/asiosdk")
else()
  # Explicit OFF must also disable an SDK auto-detected by upstream.
  set(PA_USE_ASIO OFF CACHE BOOL "Enable ASIO in PortAudio" FORCE)
  unset(BEATBENCH_AUDIO_NOTICES_DIR CACHE)
endif()
