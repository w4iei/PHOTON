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

add_executable(test_knee test_knee.c ${FW}/cal/knee.c)
target_link_libraries(test_knee shims)
add_test(NAME knee COMMAND test_knee)

add_executable(test_calcap test_calcap.c ${FW}/core1/calcap.c ${FW}/core1/events.c)
target_link_libraries(test_calcap shims)
add_test(NAME calcap COMMAND test_calcap)

add_executable(test_cal_session test_cal_session.c ${FW}/cal/cal_session.c ${FW}/cal/knee.c
               ${FW}/core1/calcap.c ${FW}/core1/events.c)
target_link_libraries(test_cal_session shims)
add_test(NAME cal_session COMMAND test_cal_session)

# microSD recorder: the real recorder + SMF encoder + FatFs on a RAM disk,
# driven by a fake clock, formatted FAT16 / FAT32 / exFAT in turn.
set(FATFS ${CMAKE_CURRENT_LIST_DIR}/../../lib/fatfs)
add_executable(test_recorder test_recorder.c ${FW}/bridge/recorder.c ${FW}/bridge/smf.c
               ${FATFS}/ff.c ${FATFS}/ffunicode.c)
target_include_directories(test_recorder PRIVATE ${FATFS})
target_compile_definitions(test_recorder PRIVATE PHOTON_HOST_TEST=1)
target_link_libraries(test_recorder shims)
add_test(NAME recorder COMMAND test_recorder)

# SETUP.TXT text (the bus side, bridge/setup_log.c, is checked on hardware).
add_executable(test_setup_text test_setup_text.c ${FW}/bridge/setup_text.c)
target_link_libraries(test_setup_text shims)
add_test(NAME setup_text COMMAND test_setup_text)
