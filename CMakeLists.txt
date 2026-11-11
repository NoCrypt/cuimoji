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
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(WAYLAND_CLIENT QUIET wayland-client)
endif()

set(CUIMOJI_SOURCES main.c uinput.c)
set(CUIMOJI_LIBS cui::cui X11::X11)
set(CUIMOJI_INCLUDE_DIRS "")

# Wayland-native window: SDL2 with wl_shm presentation, plus Vulkan on top of
# it when the headers are there.
if(SDL2_FOUND AND WAYLAND_CLIENT_FOUND)
    list(APPEND CUIMOJI_SOURCES sdl_common.c sdl_backend.c)
    list(APPEND CUIMOJI_LIBS SDL2::SDL2 ${WAYLAND_CLIENT_LIBRARIES})
    list(APPEND CUIMOJI_INCLUDE_DIRS ${WAYLAND_CLIENT_INCLUDE_DIRS})
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

# Emojibase search aliases (label + tags + emoticon), converted to a small C
# lookup table at configure time. Skipped when the download or Python is not
# available; the picker then just uses CUI's own names.
set(EMOJIBASE_URL "https://cdn.jsdelivr.net/npm/emojibase-data@18.0.0/en/compact.json"
    CACHE STRING "Emojibase compact.json URL")
set(EMOJIBASE_JSON "${CMAKE_CURRENT_BINARY_DIR}/emojibase-compact.json")
if(NOT EXISTS "${EMOJIBASE_JSON}")
    file(DOWNLOAD "${EMOJIBASE_URL}" "${EMOJIBASE_JSON}" STATUS emojibase_status)
    if(NOT emojibase_status MATCHES "^0;")
        message(STATUS "cuimoji: Emojibase download failed (${emojibase_status})")
        file(REMOVE "${EMOJIBASE_JSON}")
    endif()
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(EXISTS "${EMOJIBASE_JSON}" AND Python3_Interpreter_FOUND)
    set(EMOJIBASE_C "${CMAKE_CURRENT_BINARY_DIR}/emojibase_aliases.c")
    add_custom_command(OUTPUT "${EMOJIBASE_C}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_aliases.py"
                "${EMOJIBASE_JSON}" "${EMOJIBASE_C}"
        DEPENDS "${EMOJIBASE_JSON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_aliases.py"
        COMMENT "Generating Emojibase search aliases")
    list(APPEND CUIMOJI_SOURCES "${EMOJIBASE_C}")
    add_compile_definitions(CUIMOJI_HAVE_EMOJIBASE=1)
    message(STATUS "cuimoji: Emojibase search aliases enabled")
else()
    message(STATUS "cuimoji: Emojibase search aliases not built")
endif()

add_executable(cuimoji ${CUIMOJI_SOURCES})
target_include_directories(cuimoji PRIVATE ${cui_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}
                                           ${CMAKE_CURRENT_SOURCE_DIR}/vulkan ${CUIMOJI_INCLUDE_DIRS})
target_link_libraries(cuimoji PRIVATE ${CUIMOJI_LIBS})
# Release builds are stripped by default (~2.8 MB -> ~950 KB).
target_link_options(cuimoji PRIVATE $<$<CONFIG:Release>:-s>)
# The merged Lucide subset used by the group tabs at runtime.
add_custom_command(TARGET cuimoji POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/lucide-cuimoji.ttf"
            "$<TARGET_FILE_DIR:cuimoji>/lucide-cuimoji.ttf")
