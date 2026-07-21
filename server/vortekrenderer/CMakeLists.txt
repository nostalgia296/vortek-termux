cmake_minimum_required(VERSION 3.22.1)

project(VortekRenderer)

option(VORTEK_BUILD_JNI "Build the Android JNI shared library" ON)
option(VORTEK_BUILD_CLI "Build the standalone Termux CLI service" OFF)
option(VORTEK_CLI_X11 "Enable X11 readback presentation backend for the CLI service" ON)

set(WINLATOR_INCLUDE_DIR "../winlator/include" CACHE PATH "Path to Winlator public headers")
set(WINLATOR_SOURCE_DIR "../winlator" CACHE PATH "Path to Winlator source tree")
set(ADRENOTOOLS_INCLUDE_DIR "../libadrenotools/include" CACHE PATH "Path to libadrenotools public headers")
set(WINLATOR_LIBRARY winlator CACHE STRING "Winlator library target or linker name")
set(ADRENOTOOLS_LIBRARY adrenotools CACHE STRING "libadrenotools target or linker name")
option(VORTEK_CLI_BUNDLE_WINLATOR_SOURCES "Compile the small Winlator utility sources needed by the CLI instead of linking libwinlator" ON)
set(VORTEK_CLI_ANDROID_API "26" CACHE STRING "Android API level for Termux CLI clang target")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wno-unused-function -Wimplicit-function-declaration -D_GNU_SOURCE -DVK_USE_PLATFORM_ANDROID_KHR")

set(VORTEK_RENDERER_SOURCES
    src/vk_object.c
    src/request_handler.c
    src/vulkan_helper.c
    src/vk_context.c
    src/resource_memory.c
    src/xwindow_swapchain.c
    src/texture_decoder.c
    src/shader_inspector.c
    src/async_pipeline_creator.c
    src/timeline_semaphore.c)

set(VORTEK_INCLUDE_DIRS
    include
    ${WINLATOR_INCLUDE_DIR}
    ${ADRENOTOOLS_INCLUDE_DIR})

if(VORTEK_BUILD_JNI)
    add_library(vortekrenderer SHARED
                src/main.c
                ${VORTEK_RENDERER_SOURCES})

    target_include_directories(vortekrenderer PRIVATE ${VORTEK_INCLUDE_DIRS})
    target_link_libraries(vortekrenderer
                          ${ADRENOTOOLS_LIBRARY}
                          ${WINLATOR_LIBRARY}
                          log
                          android)
endif()

if(VORTEK_BUILD_CLI)
    add_executable(vortekrenderer-cli
                   src/cli_main.c
                   ${VORTEK_RENDERER_SOURCES})

    target_include_directories(vortekrenderer-cli PRIVATE ${VORTEK_INCLUDE_DIRS})
    if(VORTEK_CLI_ANDROID_API)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)")
            set(VORTEK_CLI_ANDROID_TRIPLE "aarch64-linux-android${VORTEK_CLI_ANDROID_API}")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv7)")
            set(VORTEK_CLI_ANDROID_TRIPLE "armv7a-linux-androideabi${VORTEK_CLI_ANDROID_API}")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)")
            set(VORTEK_CLI_ANDROID_TRIPLE "x86_64-linux-android${VORTEK_CLI_ANDROID_API}")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86)")
            set(VORTEK_CLI_ANDROID_TRIPLE "i686-linux-android${VORTEK_CLI_ANDROID_API}")
        else()
            message(FATAL_ERROR "Unsupported CMAKE_SYSTEM_PROCESSOR for VORTEK_CLI_ANDROID_API: ${CMAKE_SYSTEM_PROCESSOR}")
        endif()

        target_compile_options(vortekrenderer-cli PRIVATE -target ${VORTEK_CLI_ANDROID_TRIPLE})
        target_link_options(vortekrenderer-cli PRIVATE -target ${VORTEK_CLI_ANDROID_TRIPLE})
    endif()
    if(VORTEK_CLI_BUNDLE_WINLATOR_SOURCES)
        target_sources(vortekrenderer-cli PRIVATE
                       ${WINLATOR_SOURCE_DIR}/src/arrays.c
                       ${WINLATOR_SOURCE_DIR}/src/ring_buffer.c
                       ${WINLATOR_SOURCE_DIR}/src/sysvshared_memory.c
                       src/ahardwarebuffer_fd.c)
    else()
        target_link_libraries(vortekrenderer-cli ${WINLATOR_LIBRARY})
    endif()
    target_link_libraries(vortekrenderer-cli
                          ${ADRENOTOOLS_LIBRARY}
                          log
                          android
                          dl)
    if(VORTEK_CLI_X11)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(X11 REQUIRED x11)
        target_compile_definitions(vortekrenderer-cli PRIVATE VORTEK_CLI_X11)
        target_include_directories(vortekrenderer-cli PRIVATE ${X11_INCLUDE_DIRS})
        target_link_directories(vortekrenderer-cli PRIVATE ${X11_LIBRARY_DIRS})
        target_link_libraries(vortekrenderer-cli ${X11_LIBRARIES})
    endif()
endif()
