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

# 3. OpenGL & GLFW (system package if available, otherwise FetchContent)
find_package(OpenGL REQUIRED)
find_package(glfw3 3.3 QUIET)
if(NOT glfw3_FOUND AND NOT TARGET glfw)
    include(FetchContent)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glfw)
endif()

# 4. RmlUI Studio (Cross-platform C++17 + RmlUi + OpenGL 3.3 + GLFW)
set(STUDIO_SOURCES
    src/main.cpp
    src/gl_renderer.h
    src/gl_renderer.cpp
    src/platform_utils.h
    src/platform_utils.cpp
    src/rml_doc.h
    src/rml_doc.cpp
    src/rml_engine.h
    src/rml_engine.cpp
    src/studio_icons.h
    src/studio_icons.cpp
    src/studio_ui.h
    src/studio_ui.cpp
    third_party/RmlUi/Backends/RmlUi_Platform_GLFW.cpp
    third_party/RmlUi/Backends/RmlUi_Renderer_GL3.cpp
)

if(WIN32)
    add_executable(RmlUIStudio WIN32 ${STUDIO_SOURCES})
else()
    add_executable(RmlUIStudio ${STUDIO_SOURCES})
endif()

target_include_directories(RmlUIStudio PRIVATE
    src
    third_party/RmlUi/Include
    third_party/RmlUi/Backends
    third_party/stb
)

target_compile_definitions(RmlUIStudio PRIVATE
    RMLUI_STATIC_LIB
)

if(WIN32)
    target_compile_definitions(RmlUIStudio PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

if(MSVC)
    target_compile_options(RmlUIStudio PRIVATE /utf-8 /W3)
else()
    target_compile_options(RmlUIStudio PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

target_link_libraries(RmlUIStudio PRIVATE
    RmlUi::Core
    OpenGL::GL
    glfw
)

if(WIN32)
    target_link_libraries(RmlUIStudio PRIVATE
        comdlg32
        dwmapi
        user32
        shell32
    )
else()
    find_package(Threads REQUIRED)
    target_link_libraries(RmlUIStudio PRIVATE
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
endif()
