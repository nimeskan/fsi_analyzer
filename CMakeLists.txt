cmake_minimum_required(VERSION 3.13)
project(FSIAnalyzer)

set(ANALYZER_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/AnalyzerSDK")

if(NOT EXISTS "${ANALYZER_SDK_PATH}/include/Analyzer.h")
    message(FATAL_ERROR
        "AnalyzerSDK not found at ${ANALYZER_SDK_PATH}.\n"
        "Clone with: git clone --recurse-submodules https://github.com/saleae/SampleAnalyzer\n"
        "Then place AnalyzerSDK alongside this CMakeLists.txt.")
endif()

if(APPLE)
    set(SALEAE_LIB "${ANALYZER_SDK_PATH}/lib/libAnalyzer.dylib")
elseif(WIN32)
    set(SALEAE_LIB "${ANALYZER_SDK_PATH}/lib_x86_64/Analyzer.lib")
    set(SALEAE_DLL "${ANALYZER_SDK_PATH}/lib_x86_64/Analyzer.dll")
else()
    # SDK ships arch-specific lib dirs
    if(EXISTS "${ANALYZER_SDK_PATH}/lib_x86_64/libAnalyzer.so")
        set(SALEAE_LIB "${ANALYZER_SDK_PATH}/lib_x86_64/libAnalyzer.so")
    else()
        set(SALEAE_LIB "${ANALYZER_SDK_PATH}/lib/libAnalyzer.so")
    endif()
endif()

add_definitions(-DLOGIC2)

add_library(FSIAnalyzer SHARED
    src/FSIAnalyzer.cpp
    src/FSIAnalyzerSettings.cpp
    src/FSIAnalyzerResults.cpp
)

target_include_directories(FSIAnalyzer PRIVATE
    "${ANALYZER_SDK_PATH}/include"
    src/
)

target_link_libraries(FSIAnalyzer PRIVATE "${SALEAE_LIB}")

set_target_properties(FSIAnalyzer PROPERTIES
    CXX_STANDARD 14
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Analyzers"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Analyzers"
)

if(WIN32)
    add_custom_command(TARGET FSIAnalyzer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SALEAE_DLL}"
            "$<TARGET_FILE_DIR:FSIAnalyzer>"
    )
endif()
