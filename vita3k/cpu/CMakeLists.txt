set(SOURCE_LIST
include/cpu/state.h
include/cpu/common.h
include/cpu/disasm/functions.h
include/cpu/disasm/state.h

src/disasm.cpp
src/cpu.cpp
)

# Native AArch32 backend: only for 32-bit ARM hosts (armeabi-v7a), replaces Dynarmic
if(VITA3K_NATIVE_CPU)
    enable_language(ASM)
    list(APPEND SOURCE_LIST
        include/cpu/impl/native_cpu.h
        src/native_cpu.cpp
        src/native_enter_arm.S
    )
else()
    list(APPEND SOURCE_LIST
        include/cpu/impl/dynarmic_cpu.h
        src/dynarmic_cpu.cpp
    )
endif()

add_library(
cpu
STATIC
${SOURCE_LIST}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCE_LIST})

target_include_directories(cpu PUBLIC include)
target_link_libraries(cpu PUBLIC mem util)
if(VITA3K_NATIVE_CPU)
    target_compile_definitions(cpu PUBLIC VITA3K_NATIVE_CPU=1)
    target_link_libraries(cpu PRIVATE capstone merry::mcl)

    # M1 harness: hand-assembled guest code through NativeCPU, no emulator needed
    add_executable(native_harness tests/native_harness.cpp)
    target_link_libraries(native_harness PRIVATE cpu)
else()
    target_link_libraries(cpu PRIVATE dynarmic capstone merry::mcl)
endif()
