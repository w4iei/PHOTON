# Host-built unit tests (macOS/Linux, no Pico SDK). Run with:
#   cmake -B build -G Ninja && ninja -C build && ctest --test-dir build
cmake_minimum_required(VERSION 3.13)
project(photon_host_tests C)
set(CMAKE_C_STANDARD 11)
enable_testing()

set(FW ${CMAKE_CURRENT_LIST_DIR}/../../src)

add_library(shims INTERFACE)
target_include_directories(shims INTERFACE ${CMAKE_CURRENT_LIST_DIR}/shims ${FW})
target_compile_options(shims INTERFACE -Wall -Wextra -g -fsanitize=address,undefined)
target_link_options(shims INTERFACE -fsanitize=address,undefined)

add_executable(test_frame test_frame.c ${FW}/comms/frame.c ${FW}/util/crc32.c)
target_link_libraries(test_frame shims)
add_test(NAME frame COMMAND test_frame)

add_executable(test_events test_events.c ${FW}/core1/events.c)
target_link_libraries(test_events shims)
add_test(NAME events COMMAND test_events)
