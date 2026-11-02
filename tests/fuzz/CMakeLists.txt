cmake_minimum_required(VERSION 3.20)
project(ipc_fuzz LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The IPC fuzz target requires Clang and libFuzzer")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# A sanitizer finding must fail smoke tests and fuzz campaigns immediately.
add_compile_options(-fno-sanitize-recover=undefined)

add_library(ipc_fuzz_codec STATIC
    ../../src/shared/protocol/zpp_codec.cpp
    ../../src/shared/transport/ctrl_channel.cpp
    ../../src/shared/transport/poll.cpp)
target_include_directories(ipc_fuzz_codec PUBLIC ../../src)
target_include_directories(ipc_fuzz_codec SYSTEM PUBLIC ../../thirdparty)
target_compile_options(ipc_fuzz_codec PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)

add_executable(fuzz_ipc_codec codec.cpp)
target_compile_options(fuzz_ipc_codec PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
target_link_options(fuzz_ipc_codec PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_libraries(fuzz_ipc_codec PRIVATE ipc_fuzz_codec)

add_executable(ipc_fuzz_seeds codec.cpp)
target_compile_definitions(ipc_fuzz_seeds PRIVATE IPC_FUZZ_SEEDS)
target_compile_options(ipc_fuzz_seeds PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(ipc_fuzz_seeds PRIVATE -fsanitize=address,undefined)
target_link_libraries(ipc_fuzz_seeds PRIVATE ipc_fuzz_codec)

enable_testing()
add_executable(test_ipc_fuzz_codec codec_test.cpp codec.cpp)
target_compile_options(test_ipc_fuzz_codec PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(test_ipc_fuzz_codec PRIVATE -fsanitize=fuzzer-no-link,address,undefined)
target_link_libraries(test_ipc_fuzz_codec PRIVATE ipc_fuzz_codec)
add_test(NAME ipc_fuzz_codec COMMAND test_ipc_fuzz_codec)

add_executable(fuzz_ipc_framing framing.cpp)
target_compile_options(fuzz_ipc_framing PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
target_link_options(fuzz_ipc_framing PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_libraries(fuzz_ipc_framing PRIVATE ipc_fuzz_codec)

add_executable(test_ipc_fuzz_framing framing_test.cpp framing.cpp)
target_compile_options(test_ipc_fuzz_framing PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(test_ipc_fuzz_framing PRIVATE -fsanitize=fuzzer-no-link,address,undefined)
target_link_libraries(test_ipc_fuzz_framing PRIVATE ipc_fuzz_codec)
add_test(NAME ipc_fuzz_framing COMMAND test_ipc_fuzz_framing)
