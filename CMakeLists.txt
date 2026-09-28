cmake_minimum_required(VERSION 3.14)
project(binary_protocol C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

# POSIX sockets, read/write, and related APIs.
add_compile_definitions(_POSIX_C_SOURCE=200809L)

set(SHARED_SOURCES
    shared/frame.c
    shared/hpack_lite.c
    shared/hexdump.c
)

add_library(proto_shared STATIC ${SHARED_SOURCES})
target_include_directories(proto_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/shared)

add_executable(bserve server/bserve.c)
target_link_libraries(bserve PRIVATE proto_shared)
set_target_properties(bserve PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_executable(bcurl client/bcurl.c)
target_link_libraries(bcurl PRIVATE proto_shared)
set_target_properties(bcurl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}
)

enable_testing()

add_executable(test_frame tests/test_frame.c)
target_link_libraries(test_frame PRIVATE proto_shared)
add_test(NAME test_frame COMMAND test_frame)

add_executable(test_hpack tests/test_hpack.c)
target_link_libraries(test_hpack PRIVATE proto_shared)
add_test(NAME test_hpack COMMAND test_hpack)

# Dummy fixtures live next to the tests so --raw / file-serving checks can find them.
set_tests_properties(test_frame test_hpack PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
