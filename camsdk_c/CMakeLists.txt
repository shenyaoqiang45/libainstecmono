# camsdk_c - layer 3: C ABI shared library over the vendor CamSDK,
# with a mock backend (ACAM_MOCK=1) for hardware-free testing.

add_library(ainstec_cam SHARED
  src/camsdk_c.cpp
  src/ainstec_backend.cpp
  src/mock_backend.cpp
  src/attribute_tree.cpp
)
target_include_directories(ainstec_cam
  PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(ainstec_cam PRIVATE ACAM_BUILDING_DLL)
target_link_libraries(ainstec_cam PRIVATE Ainstec::CamSDK)
ainstec_copy_runtime_dlls(ainstec_cam)

if(ACAM_BUILD_TESTS)
  add_executable(test_camsdk_c tests/test_camsdk_c.cpp)
  target_link_libraries(test_camsdk_c PRIVATE ainstec_cam)
  ainstec_copy_runtime_dlls(test_camsdk_c)
  add_test(NAME camsdk_c_smoke COMMAND test_camsdk_c)

  # No camera / no DLL: exercises GenICam -> flat attribute-tree normalization.
  add_executable(test_attribute_tree
    tests/test_attribute_tree.cpp
    src/attribute_tree.cpp)
  target_include_directories(test_attribute_tree PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  add_test(NAME attribute_tree_flatten COMMAND test_attribute_tree)
endif()
