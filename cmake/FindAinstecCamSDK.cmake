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
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${AINSTEC_CAMSDK_ROOT}/include"
    IMPORTED_IMPLIB_RELEASE   "${AINSTEC_CAMSDK_ROOT}/lib/win/CameraPro.lib"
    IMPORTED_LOCATION_RELEASE "${AINSTEC_CAMSDK_ROOT}/bin/CameraPro.dll"
    IMPORTED_IMPLIB_DEBUG     "${AINSTEC_CAMSDK_ROOT}/lib/win/CameraProd.lib"
    IMPORTED_LOCATION_DEBUG   "${AINSTEC_CAMSDK_ROOT}/bin/CameraProd.dll"
    MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
    MAP_IMPORTED_CONFIG_MINSIZEREL     Release)
endif()

# Runtime dependencies that must be deployed next to any consumer binary.
# Release set only; Debug additionally needs CameraProd.dll + opencv_world410d.dll.
set(AINSTEC_CAMSDK_RUNTIME_DLLS
  "${AINSTEC_CAMSDK_ROOT}/bin/CameraPro.dll"
  "${AINSTEC_CAMSDK_ROOT}/bin/3rdparty/opencv/opencv_world410.dll"
  "${AINSTEC_CAMSDK_ROOT}/Tools/CameraProAuthority.dll"
  "${AINSTEC_CAMSDK_ROOT}/Tools/msvcp140.dll"
  "${AINSTEC_CAMSDK_ROOT}/Tools/vcruntime140.dll"
  CACHE INTERNAL "Ainstec CamSDK runtime DLLs")

# ainstec_copy_runtime_dlls(<target>) — copy vendor DLLs next to <target>.
function(ainstec_copy_runtime_dlls target)
  foreach(dll IN LISTS AINSTEC_CAMSDK_RUNTIME_DLLS)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:${target}>"
      VERBATIM)
  endforeach()
endfunction()
