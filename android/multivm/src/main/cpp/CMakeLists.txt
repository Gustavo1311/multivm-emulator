cmake_minimum_required(VERSION 3.18)
project(multivm_jni C)

# Nucleo portavel do emulador (android/multivm/src/main/cpp -> core)
set(MVM_CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../../../core)
add_subdirectory(${MVM_CORE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/core)

add_library(multivm SHARED jni_bridge.c audio_aaudio.c)
target_link_libraries(multivm mvmcore log dl)
target_compile_options(multivm PRIVATE -Wall -Wextra -Wno-unused-parameter -ffp-contract=off)
# Mantem apenas os simbolos JNI exportados
target_link_options(multivm PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)
