cmake_minimum_required(VERSION 3.10)
project(multivm_core C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(MVM_CORE_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/src/space.c
  ${CMAKE_CURRENT_LIST_DIR}/src/util.c
  ${CMAKE_CURRENT_LIST_DIR}/src/img/img_util.c
  ${CMAKE_CURRENT_LIST_DIR}/src/img/qcow2.c
  ${CMAKE_CURRENT_LIST_DIR}/src/img/vdi.c
  ${CMAKE_CURRENT_LIST_DIR}/src/img/vmdk.c
  ${CMAKE_CURRENT_LIST_DIR}/src/img/vhd.c
  ${CMAKE_CURRENT_LIST_DIR}/src/img/vhdx.c
  ${CMAKE_CURRENT_LIST_DIR}/src/img/mvd.c
  ${CMAKE_CURRENT_LIST_DIR}/src/img/lz4.c
  ${CMAKE_CURRENT_LIST_DIR}/src/img/convert.c
  ${CMAKE_CURRENT_LIST_DIR}/src/inflate.c
  ${CMAKE_CURRENT_LIST_DIR}/src/vm.c
  ${CMAKE_CURRENT_LIST_DIR}/src/loader.c
  ${CMAKE_CURRENT_LIST_DIR}/src/cpu/arm_common.c
  ${CMAKE_CURRENT_LIST_DIR}/src/cpu/cpu_arm64.c
  ${CMAKE_CURRENT_LIST_DIR}/src/cpu/a64_simd.c
  ${CMAKE_CURRENT_LIST_DIR}/src/cpu/cpu_arm32.c
  ${CMAKE_CURRENT_LIST_DIR}/src/cpu/a32_thumb.c
  ${CMAKE_CURRENT_LIST_DIR}/src/cpu/a32_vfp.c
  ${CMAKE_CURRENT_LIST_DIR}/src/cpu/cpu_x86.c
  ${CMAKE_CURRENT_LIST_DIR}/src/cpu/x86_exec.c
  ${CMAKE_CURRENT_LIST_DIR}/src/cpu/x86_fpu.c
  ${CMAKE_CURRENT_LIST_DIR}/src/cpu/x86_sse.c
  ${CMAKE_CURRENT_LIST_DIR}/src/cpu/x86_jit.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/virtio.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/arm_devices.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/pc_devices.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/pci.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/fw_cfg.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/vga.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/ide.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/i8237.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/fdc.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/eeprom93.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/virtio_net.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/virtio_snd.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/rtl8139.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/e1000.c
  ${CMAKE_CURRENT_LIST_DIR}/src/net/net.c
  ${CMAKE_CURRENT_LIST_DIR}/src/net/tcp.c
  ${CMAKE_CURRENT_LIST_DIR}/src/audio.c
  ${CMAKE_CURRENT_LIST_DIR}/src/vnc.c
  ${CMAKE_CURRENT_LIST_DIR}/src/des.c
  ${CMAKE_CURRENT_LIST_DIR}/src/zlib_dl.c
  ${CMAKE_CURRENT_LIST_DIR}/src/jpeg_enc.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/ac97.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/hda.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/apic.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/acpi.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/hpet.c
  ${CMAKE_CURRENT_LIST_DIR}/src/dev/ahci.c
  ${CMAKE_CURRENT_LIST_DIR}/src/machine/machine_virt.c
  ${CMAKE_CURRENT_LIST_DIR}/src/machine/machine_pc.c
)

add_library(mvmcore STATIC ${MVM_CORE_SOURCES})
target_include_directories(mvmcore PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_options(mvmcore PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -ffp-contract=off)
set_target_properties(mvmcore PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)
target_link_libraries(mvmcore PUBLIC Threads::Threads m ${CMAKE_DL_LIBS})

if(NOT ANDROID)
  add_executable(mvm-cli ${CMAKE_CURRENT_LIST_DIR}/tools/mvm_cli.c)
  target_link_libraries(mvm-cli mvmcore)
  add_executable(mvm-img ${CMAKE_CURRENT_LIST_DIR}/tools/mvm_img.c)
  target_link_libraries(mvm-img mvmcore)
endif()
