cmake_minimum_required(VERSION 3.15)
project(Disk)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

if(MSVC)
    set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "" FORCE)
endif()

FetchContent_MakeAvailable(glfw)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
)
FetchContent_MakeAvailable(imgui)

add_executable(Disk WIN32
    "Disk/src/main.cpp" 
    "Disk/src/engine.h" 
    "Disk/src/engine.cpp" 
    "Disk/src/ui.h" 
    "Disk/src/ui.cpp"
    "Disk/src/scanner.h"
    "Disk/src/scanner.cpp"
    "Disk/src/mft_scanner.h"
    "Disk/src/mft_scanner.cpp"
)

target_sources(Disk PRIVATE
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(Disk PRIVATE 
    "Disk/src"
    ${imgui_SOURCE_DIR} 
    ${imgui_SOURCE_DIR}/backends
    ${glfw_SOURCE_DIR}/include
)

target_link_libraries(Disk PRIVATE glfw opengl32)

if(MSVC)
    set_target_properties(Disk PROPERTIES 
        LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\""
    )
endif()