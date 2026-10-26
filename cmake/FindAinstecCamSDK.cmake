# FindAinstecCamSDK.cmake — locate the vendor Ainstec CamSDK (C++).
#
# Provides:
#   Ainstec::CamSDK               imported shared library target
#   AINSTEC_CAMSDK_ROOT           SDK root (cache var, overridable)
#   AINSTEC_CAMSDK_RUNTIME_DLLS   DLLs that must sit next to consumer binaries
#   ainstec_copy_runtime_dlls(t)  post-build copy helper
#
# Override the default location with -DAINSTEC_CAMSDK_ROOT=<path>.

set(AINSTEC_CAMSDK_ROOT "C:/3DCameraViewer/CamSDK/CamSDK_CPP"
    CACHE PATH "Root of the vendor Ainstec CamSDK_CPP distribution")

if(NOT EXISTS "${AINSTEC_CAMSDK_ROOT}/include/Camera.h")
  message(FATAL_ERROR
    "Ainstec CamSDK not found at '${AINSTEC_CAMSDK_ROOT}'. "
    "Pass -DAINSTEC_CAMSDK_ROOT=<path to CamSDK_CPP>.")
endif()

if(NOT TARGET Ainstec::CamSDK)
  add_library(Ainstec::CamSDK SHARED IMPORTED GLOBAL)
  set_target_properties(Ainstec::CamSDK PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES        "${AINSTEC_CAMSDK_ROOT}/include"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${AINSTEC_CAMSDK_ROOT}/include")
  if(WIN32)
    set_target_properties(Ainstec::CamSDK PROPERTIES
      IMPORTED_IMPLIB_RELEASE   "${AINSTEC_CAMSDK_ROOT}/lib/win/CameraPro.lib"
      IMPORTED_LOCATION_RELEASE "${AINSTEC_CAMSDK_ROOT}/bin/CameraPro.dll"
      IMPORTED_IMPLIB_DEBUG     "${AINSTEC_CAMSDK_ROOT}/lib/win/CameraProd.lib"
      IMPORTED_LOCATION_DEBUG   "${AINSTEC_CAMSDK_ROOT}/bin/CameraProd.dll"
      MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
      MAP_IMPORTED_CONFIG_MINSIZEREL     Release)
  else()
    # The vendor SDK ships Linux builds for x86_64 and arm64 (Jetson).
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
      set(_ainstec_libdir "linux_arm64")
    else()
      set(_ainstec_libdir "linux_x86_64")
    endif()
    set_target_properties(Ainstec::CamSDK PROPERTIES
      IMPORTED_LOCATION "${AINSTEC_CAMSDK_ROOT}/lib/${_ainstec_libdir}/libCameraPro.so")
  endif()
endif()

# Runtime dependencies that must be deployed next to any consumer binary.
# Windows release set (Debug additionally needs CameraProd.dll +
# opencv_world410d.dll); Linux set (libCameraPro.so + bundled OpenCV).
if(WIN32)
  set(AINSTEC_CAMSDK_RUNTIME_DLLS
    "${AINSTEC_CAMSDK_ROOT}/bin/CameraPro.dll"
    "${AINSTEC_CAMSDK_ROOT}/bin/3rdparty/opencv/opencv_world410.dll"
    "${AINSTEC_CAMSDK_ROOT}/Tools/CameraProAuthority.dll"
    "${AINSTEC_CAMSDK_ROOT}/Tools/msvcp140.dll"
    "${AINSTEC_CAMSDK_ROOT}/Tools/vcruntime140.dll"
    CACHE INTERNAL "Ainstec CamSDK runtime libraries")
else()
  set(AINSTEC_CAMSDK_RUNTIME_DLLS
    "${AINSTEC_CAMSDK_ROOT}/lib/${_ainstec_libdir}/libCameraPro.so"
    "${AINSTEC_CAMSDK_ROOT}/lib/${_ainstec_libdir}/3rdparty/opencv/libopencv_core.so.4.1"
    "${AINSTEC_CAMSDK_ROOT}/lib/${_ainstec_libdir}/3rdparty/opencv/libopencv_imgcodecs.so.4.1"
    "${AINSTEC_CAMSDK_ROOT}/lib/${_ainstec_libdir}/3rdparty/opencv/libopencv_imgproc.so.4.1"
    CACHE INTERNAL "Ainstec CamSDK runtime libraries")
endif()

# ainstec_copy_runtime_dlls(<target>) — copy vendor runtime libraries next to
# <target> after build. On Linux also set $ORIGIN rpath so the loader finds
# the co-located .so files.
function(ainstec_copy_runtime_dlls target)
  foreach(dll IN LISTS AINSTEC_CAMSDK_RUNTIME_DLLS)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:${target}>"
      VERBATIM)
  endforeach()
  if(NOT WIN32)
    set_target_properties(${target} PROPERTIES
      BUILD_RPATH "$ORIGIN"
      INSTALL_RPATH "$ORIGIN")
  endif()
endfunction()
