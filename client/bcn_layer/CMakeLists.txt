cmake_minimum_required(VERSION 3.16)
project(vortek_bcn_layer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(LIB_INSTALL_DIR "lib" CACHE STRING "Library directory relative to the glibc rootfs")
set(LAYER_INSTALL_DIR "share/vulkan/implicit_layer.d" CACHE STRING "Vulkan implicit-layer manifest directory")
set(LAYER_LIBRARY_PATH "${CMAKE_INSTALL_PREFIX}/${LIB_INSTALL_DIR}/libvortek_bcn_layer.so")

find_program(GLSLC_EXECUTABLE NAMES glslc REQUIRED)
find_program(SLANGC_EXECUTABLE NAMES slangc REQUIRED)
find_package(Threads REQUIRED)

set(GENERATED_SRC_DIR "${CMAKE_CURRENT_BINARY_DIR}/src")
set(BINARY_TO_HEADER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/binary_to_header.cmake")
file(MAKE_DIRECTORY "${GENERATED_SRC_DIR}")

function(add_binary_header input output)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}"
                "-DINPUT=${input}"
                "-DOUTPUT=${output}"
                -P "${BINARY_TO_HEADER_SCRIPT}"
        DEPENDS "${input}" "${BINARY_TO_HEADER_SCRIPT}"
        VERBATIM
    )
endfunction()

function(add_glsl_shader name source)
    set(spirv "${GENERATED_SRC_DIR}/${name}.spv")
    set(header "${GENERATED_SRC_DIR}/${name}_spv.h")
    add_custom_command(
        OUTPUT "${spirv}"
        COMMAND "${GLSLC_EXECUTABLE}" -c "${source}" -o "${spirv}"
        DEPENDS "${source}"
        VERBATIM
    )
    add_binary_header("${spirv}" "${header}")
    set(SPIRV_HEADERS "${SPIRV_HEADERS}" "${header}" PARENT_SCOPE)
endfunction()

file(GLOB SLANG_DEPENDENCIES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.slang"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

function(add_slang_shader name source_name)
    set(glsl "${GENERATED_SRC_DIR}/${name}.comp")
    set(spirv "${GENERATED_SRC_DIR}/${name}.spv")
    set(header "${GENERATED_SRC_DIR}/${name}_spv.h")
    add_custom_command(
        OUTPUT "${glsl}"
        COMMAND "${SLANGC_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/${source_name}.slang"
                -target glsl
                -line-directive-mode none
                ${ARGN}
                -entry main
                -o "${glsl}"
        DEPENDS ${SLANG_DEPENDENCIES}
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${spirv}"
        COMMAND "${GLSLC_EXECUTABLE}" -c "${glsl}" -o "${spirv}"
        DEPENDS "${glsl}"
        VERBATIM
    )
    add_binary_header("${spirv}" "${header}")
    set(SPIRV_HEADERS "${SPIRV_HEADERS}" "${header}" PARENT_SCOPE)
endfunction()

set(SPIRV_HEADERS)
foreach(shader s3tc rgtc bc6 bc7 s3tc_iv rgtc_iv bc6_iv bc7_iv watermark)
    add_glsl_shader("${shader}" "${CMAKE_CURRENT_SOURCE_DIR}/src/${shader}.comp")
endforeach()

foreach(shader etc2_encode astc_encoder analyze_astc_parameters)
    add_slang_shader("${shader}" "${shader}")
endforeach()
add_slang_shader(astc_encoder_debug astc_encoder -D ENABLE_DEBUG)

set(LUT2_HEADER "${GENERATED_SRC_DIR}/lut2.h")
set(ASTC_2P_LUT_HEADER "${GENERATED_SRC_DIR}/astc_2p_lut_s2.h")
add_binary_header("${CMAKE_CURRENT_SOURCE_DIR}/src/lut2.bin" "${LUT2_HEADER}")
add_binary_header("${CMAKE_CURRENT_SOURCE_DIR}/src/astc_2p_lut_s2.bin" "${ASTC_2P_LUT_HEADER}")
list(APPEND SPIRV_HEADERS "${LUT2_HEADER}" "${ASTC_2P_LUT_HEADER}")

file(GLOB BCN_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(vortek_bcn_layer SHARED ${BCN_SOURCES} ${SPIRV_HEADERS})
target_include_directories(vortek_bcn_layer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GENERATED_SRC_DIR}"
)
target_link_libraries(vortek_bcn_layer PRIVATE Threads::Threads)
set_target_properties(vortek_bcn_layer PROPERTIES OUTPUT_NAME vortek_bcn_layer)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/vortek_bcn_layer.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/vortek_bcn_layer.json"
    @ONLY
)

install(TARGETS vortek_bcn_layer DESTINATION "${LIB_INSTALL_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vortek_bcn_layer.json" DESTINATION "${LAYER_INSTALL_DIR}")
