cmake_minimum_required(VERSION 3.24)
project(libainstecmono VERSION 0.1.0 LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  # /utf-8: our sources are UTF-8; vendor headers are GBK -> silence C4819.
  add_compile_options(/utf-8 /wd4819 /MP /EHsc)
endif()

option(ACAM_BUILD_TESTS  "Build camsdk_c tests"            ON)
option(ACAM_BUILD_PYTHON "Build pyainstec Python bindings" ON)

find_package(AinstecCamSDK REQUIRED)
message(STATUS "Ainstec CamSDK root: ${AINSTEC_CAMSDK_ROOT}")
