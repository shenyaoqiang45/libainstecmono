# bindings/python - pybind11 bindings for ainstec_cam (module: pyainstec).
# Builds the pyainstec extension module and stages a self-contained
# python/ install dir (module + ainstec_cam.dll + vendor runtime DLLs)
# for later ROS2 consumption.

find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)

include(FetchContent)
FetchContent_Declare(pybind11
  URL https://github.com/pybind/pybind11/archive/refs/tags/v2.13.6.tar.gz)
FetchContent_MakeAvailable(pybind11)

pybind11_add_module(pyainstec pyainstec.cpp)
# Linking ainstec_cam also brings its PUBLIC include dir (the C header).
target_link_libraries(pyainstec PRIVATE ainstec_cam)

# Self-contained staging: extension module, C ABI DLL and vendor runtimes
# side by side in <prefix>/python.
install(TARGETS pyainstec LIBRARY DESTINATION python RUNTIME DESTINATION python)
install(TARGETS ainstec_cam RUNTIME DESTINATION python)
install(FILES ${AINSTEC_CAMSDK_RUNTIME_DLLS} DESTINATION python)
