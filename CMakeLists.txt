cmake_minimum_required(VERSION 3.20)
project(RmlUIStudio VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# 1. FreeType (static, minimal)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/freetype EXCLUDE_FROM_ALL)
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

# 2. RmlUi Core (static)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_TESTS OFF CACHE BOOL "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
add_subdirectory(third_party/RmlUi EXCLUDE_FROM_ALL)

# 3. RmlUI Studio (Pure C++17 + RmlUi + DirectX 11, zero ImGui)
add_executable(RmlUIStudio WIN32
    src/main.cpp
    src/dx11_renderer.h
    src/dx11_renderer.cpp
    src/rml_doc.h
    src/rml_doc.cpp
    src/rml_engine.h
    src/rml_engine.cpp
    src/studio_icons.h
    src/studio_icons.cpp
    src/studio_ui.h
    src/studio_ui.cpp
)

target_include_directories(RmlUIStudio PRIVATE
    src
    third_party/RmlUi/Include
)

target_compile_definitions(RmlUIStudio PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    RMLUI_STATIC_LIB
)

if(MSVC)
    target_compile_options(RmlUIStudio PRIVATE /utf-8 /W3)
endif()

target_link_libraries(RmlUIStudio PRIVATE
    RmlUi::Core
    d3d11
    dxgi
    d3dcompiler
    windowscodecs
    comdlg32
    ole32
    user32
    shell32
)
