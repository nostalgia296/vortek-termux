cmake_minimum_required(VERSION 3.22.1)

project(Vortek C)
message("Building ${PROJECT_NAME}")

option(VORTEK_WAYLAND "Enable VK_KHR_wayland_surface presentation" ON)

set(CMAKE_VERBOSE_MAKEFILE on)
set(WARNS "-Wall -Wno-discarded-qualifiers")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 ${WARNS} -DVK_USE_PLATFORM_XLIB_KHR -DVK_USE_PLATFORM_XCB_KHR")

if(VORTEK_WAYLAND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_CLIENT QUIET wayland-client)
    endif()
    if(NOT WAYLAND_CLIENT_FOUND)
        find_path(WAYLAND_CLIENT_INCLUDE_DIR wayland-client.h)
        find_library(WAYLAND_CLIENT_LIBRARY NAMES wayland-client)
        if(NOT WAYLAND_CLIENT_INCLUDE_DIR OR NOT WAYLAND_CLIENT_LIBRARY)
            message(FATAL_ERROR "VORTEK_WAYLAND requires the wayland-client development headers and library")
        endif()
        set(WAYLAND_CLIENT_INCLUDE_DIRS "${WAYLAND_CLIENT_INCLUDE_DIR}")
        set(WAYLAND_CLIENT_LIBRARIES "${WAYLAND_CLIENT_LIBRARY}")
    endif()
    add_compile_definitions(VK_USE_PLATFORM_WAYLAND_KHR VORTEK_WAYLAND_WSI)
endif()

include_directories(include)

set(VORTEK_SOURCES
    src/main.c
    src/vulkan_calls.c
    src/vk_object.c
    src/vk_object_pool.c
    src/arrays.c
    src/descriptor_update_template.c
    src/ring_buffer.c)

if(VORTEK_WAYLAND)
    list(APPEND VORTEK_SOURCES src/wayland_wsi.c)
endif()

add_library(vulkan_vortek SHARED ${VORTEK_SOURCES})
target_compile_definitions(vulkan_vortek PRIVATE VT_CLIENT)
if(VORTEK_WAYLAND)
    target_include_directories(vulkan_vortek PRIVATE ${WAYLAND_CLIENT_INCLUDE_DIRS})
    target_link_directories(vulkan_vortek PRIVATE ${WAYLAND_CLIENT_LIBRARY_DIRS})
    target_link_libraries(vulkan_vortek ${WAYLAND_CLIENT_LIBRARIES})
endif()

install(TARGETS vulkan_vortek LIBRARY DESTINATION lib)
install(FILES vortek_icd.aarch64.json DESTINATION share/vulkan/icd.d)
