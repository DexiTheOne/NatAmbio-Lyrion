cmake_minimum_required(VERSION 3.16)
project(NatAmbioAlpha LANGUAGES CXX)
add_executable(natambio-alpha
    engine/main.cpp
    engine/core/NaeProcessor.cpp
    engine/core/NatAmbioCoreProcessor.cpp
    engine/core/PartitionedConvolver.cpp
    engine/core/XtcFilterDesigner.cpp
    engine/core/XtcProcessor.cpp)
target_compile_features(natambio-alpha PRIVATE cxx_std_20)
target_compile_definitions(natambio-alpha PRIVATE NATAMBIO_ENABLE_NAE=1 NATAMBIO_ENABLE_XTC=1)
target_include_directories(natambio-alpha PRIVATE engine/core)
target_compile_options(natambio-alpha PRIVATE -Wall -Wextra -Wpedantic)
include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME streaming COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/stream.py" "$<TARGET_FILE:natambio-alpha>")
endif()
