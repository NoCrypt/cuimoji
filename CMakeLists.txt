cmake_minimum_required(VERSION 3.16)
project(cuimoji C)

include(FetchContent)
# CUI v1.2.0 with a small performance patch: draw more emoji per frame and
# keep the bigger texture, so the grid is complete on the first frame.
FetchContent_Declare(cui
    GIT_REPOSITORY https://github.com/yappologistic/CUI.git
    GIT_TAG v1.2.0
    GIT_SHALLOW TRUE
    PATCH_COMMAND sh -c "patch -p1 -N -r - -i ${CMAKE_CURRENT_SOURCE_DIR}/patches/cui-fast-emoji.patch || true")
set(CUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cui)

find_package(X11 REQUIRED)
find_package(SDL2 QUIET)
find_package(Vulkan QUIET)

set(CUIMOJI_SOURCES main.c uinput.c)
set(CUIMOJI_LIBS cui::cui X11::X11)

# Wayland-native window: SDL2 (software renderer), plus Vulkan on top of it
# when the headers are there.
if(SDL2_FOUND)
    list(APPEND CUIMOJI_SOURCES sdl_common.c sdl_backend.c)
    list(APPEND CUIMOJI_LIBS SDL2::SDL2)
    add_compile_definitions(CUIMOJI_HAVE_SDL2=1)
    message(STATUS "cuimoji: SDL2/Wayland backend enabled")
    if(Vulkan_FOUND)
        list(APPEND CUIMOJI_SOURCES vulkan_backend.c vulkan/cui_vulkan.c)
        list(APPEND CUIMOJI_LIBS Vulkan::Vulkan)
        add_compile_definitions(CUIMOJI_HAVE_VULKAN=1)
        message(STATUS "cuimoji: Vulkan renderer enabled")
    else()
        message(STATUS "cuimoji: Vulkan renderer not built (Vulkan headers needed)")
    endif()
else()
    message(STATUS "cuimoji: SDL2 not found, only the X11 window is built")
endif()

add_executable(cuimoji ${CUIMOJI_SOURCES})
target_include_directories(cuimoji PRIVATE ${cui_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/vulkan)
target_link_libraries(cuimoji PRIVATE ${CUIMOJI_LIBS})
