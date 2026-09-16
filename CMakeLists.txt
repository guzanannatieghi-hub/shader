cmake_minimum_required(VERSION 3.24)
project(Ethereal LANGUAGES CXX)

add_executable(Ethereal WIN32
    src/main.cpp
)

target_compile_features(Ethereal PRIVATE cxx_std_20)

target_compile_definitions(Ethereal PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(Ethereal PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    shlwapi
)

if (MSVC)
    target_compile_options(Ethereal PRIVATE /W4 /permissive- /EHsc)
endif()
