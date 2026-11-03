# Standalone build of the NativeCPU harness (armeabi-v7a only).
# Compiles only cpu + mem sources of Vita3K, with a tiny util/log.h shim, so the whole
# emulator (SDL3, curl, emuenv, Vulkan, ...) is NOT configured.
cmake_minimum_required(VERSION 3.20)
project(native_harness C CXX ASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "native_harness needs a 32-bit ARM target (ANDROID_ABI=armeabi-v7a)")
endif()

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(V3K "${REPO}/vita3k")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- external deps (submodules of the Vita3K repo) --------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPO}/external/fmt" "${CMAKE_BINARY_DIR}/fmt" EXCLUDE_FROM_ALL)

# option names differ between capstone versions; unknown ones are ignored
set(CAPSTONE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_CSTOOL OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(CAPSTONE_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
add_subdirectory("${REPO}/external/capstone" "${CMAKE_BINARY_DIR}/capstone" EXCLUDE_FROM_ALL)

# ---- sources -----------------------------------------------------------------------------------
add_executable(native_harness
    "${V3K}/cpu/tests/native_harness.cpp"
    "${V3K}/cpu/src/cpu.cpp"
    "${V3K}/cpu/src/disasm.cpp"
    "${V3K}/cpu/src/native_cpu.cpp"
    "${V3K}/cpu/src/native_enter_arm.S"
    "${V3K}/mem/src/mem.cpp"
    "${V3K}/mem/src/allocator.cpp"
)

# the shim directory comes first so <util/log.h> resolves to it
target_include_directories(native_harness PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/shim"
    "${V3K}/cpu/include"
    "${V3K}/mem/include"
    "${V3K}/util/include"
)
target_compile_definitions(native_harness PRIVATE VITA3K_NATIVE_CPU=1)
target_compile_options(native_harness PRIVATE -g -fno-omit-frame-pointer)

# if the harness' .S fails to assemble, these flags are the first thing to check
set_source_files_properties("${V3K}/cpu/src/native_enter_arm.S" PROPERTIES
    COMPILE_FLAGS "-march=armv7-a -mfpu=neon-vfpv4")

target_link_libraries(native_harness PRIVATE fmt capstone log)
