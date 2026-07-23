if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "INPUT and OUTPUT are required")
endif()

get_filename_component(input_name "${INPUT}" NAME)
string(REGEX REPLACE "[^A-Za-z0-9_]" "_" symbol "${input_name}")
file(READ "${INPUT}" binary HEX)
string(LENGTH "${binary}" hex_length)
math(EXPR byte_length "${hex_length} / 2")

set(header "unsigned char ${symbol}[] = {\n")
set(offset 0)
while(offset LESS hex_length)
    string(SUBSTRING "${binary}" ${offset} 2 byte)
    math(EXPR byte_index "${offset} / 2")
    math(EXPR line_index "${byte_index} % 12")

    if(line_index EQUAL 0)
        string(APPEND header "  ")
    endif()

    string(APPEND header "0x${byte}")
    math(EXPR next_offset "${offset} + 2")
    if(next_offset LESS hex_length)
        string(APPEND header ", ")
    endif()

    math(EXPR next_line_index "(${byte_index} + 1) % 12")
    if(next_line_index EQUAL 0 OR next_offset GREATER_EQUAL hex_length)
        string(APPEND header "\n")
    endif()

    set(offset ${next_offset})
endwhile()

string(APPEND header "};\nunsigned int ${symbol}_len = ${byte_length};\n")
file(WRITE "${OUTPUT}" "${header}")
