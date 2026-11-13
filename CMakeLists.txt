cmake_minimum_required(VERSION 3.16)
project(cuimoji C)

# CUI is vendored under third_party/cui (v1.2.0 plus cuimoji's changes), so a
# build needs no network access and the picker can be tuned in-tree.
set(CUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cui)
set(CUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CUI_DIR} EXCLUDE_FROM_ALL)

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
set(EMOJIBASE_SHORTCODES_URL "https://cdn.jsdelivr.net/npm/emojibase-data@18.0.0/en/shortcodes/emojibase.json"
    CACHE STRING "Emojibase shortcodes URL")
set(EMOJIBASE_JSON "${CMAKE_CURRENT_BINARY_DIR}/emojibase-compact.json")
set(EMOJIBASE_SHORTCODES_JSON "${CMAKE_CURRENT_BINARY_DIR}/emojibase-shortcodes.json")
if(NOT EXISTS "${EMOJIBASE_JSON}")
    file(DOWNLOAD "${EMOJIBASE_URL}" "${EMOJIBASE_JSON}" STATUS emojibase_status)
    if(NOT emojibase_status MATCHES "^0;")
        message(STATUS "cuimoji: Emojibase download failed (${emojibase_status})")
        file(REMOVE "${EMOJIBASE_JSON}")
    endif()
endif()
if(EXISTS "${EMOJIBASE_JSON}" AND NOT EXISTS "${EMOJIBASE_SHORTCODES_JSON}")
    file(DOWNLOAD "${EMOJIBASE_SHORTCODES_URL}" "${EMOJIBASE_SHORTCODES_JSON}" STATUS shortcodes_status)
    if(NOT shortcodes_status MATCHES "^0;")
        message(STATUS "cuimoji: Emojibase shortcodes download failed (${shortcodes_status})")
        file(WRITE "${EMOJIBASE_SHORTCODES_JSON}" "{}")
    endif()
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(EXISTS "${EMOJIBASE_JSON}" AND Python3_Interpreter_FOUND)
    set(EMOJIBASE_C "${CMAKE_CURRENT_BINARY_DIR}/emojibase_aliases.c")
    add_custom_command(OUTPUT "${EMOJIBASE_C}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_aliases.py"
                "${EMOJIBASE_JSON}" "${EMOJIBASE_SHORTCODES_JSON}" "${EMOJIBASE_C}"
        DEPENDS "${EMOJIBASE_JSON}" "${EMOJIBASE_SHORTCODES_JSON}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_aliases.py"
        COMMENT "Generating Emojibase search aliases")
    list(APPEND CUIMOJI_SOURCES "${EMOJIBASE_C}")
    add_compile_definitions(CUIMOJI_HAVE_EMOJIBASE=1)
    message(STATUS "cuimoji: Emojibase search aliases enabled")
else()
    message(STATUS "cuimoji: Emojibase search aliases not built")
endif()

add_executable(cuimoji ${CUIMOJI_SOURCES})
target_include_directories(cuimoji PRIVATE ${CUI_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}
                                           ${CMAKE_CURRENT_SOURCE_DIR}/vulkan ${CUIMOJI_INCLUDE_DIRS})
target_link_libraries(cuimoji PRIVATE ${CUIMOJI_LIBS} ${CMAKE_DL_LIBS})
# Release builds are stripped by default (~2.8 MB -> ~950 KB).
target_link_options(cuimoji PRIVATE $<$<CONFIG:Release>:-s>)
# The merged Lucide subset used by the group tabs at runtime.
add_custom_command(TARGET cuimoji POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/lucide-cuimoji.ttf"
            "$<TARGET_FILE_DIR:cuimoji>/lucide-cuimoji.ttf")
