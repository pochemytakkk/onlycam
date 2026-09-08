cmake_minimum_required(VERSION 3.20)
project(onlycam_filter CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(onlycam_filter SHARED
    src/dll.cpp
    src/filter.cpp
    src/framebuffer.cpp
    src/onlycam_filter.def
)

set_target_properties(onlycam_filter PROPERTIES
    OUTPUT_NAME "OnlyCamFilter"
    PREFIX ""
)

target_compile_definitions(onlycam_filter PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(onlycam_filter PRIVATE strmiids ole32 oleaut32 advapi32 user32)

if (MSVC)
    target_compile_options(onlycam_filter PRIVATE /W3 /EHsc)
endif()
