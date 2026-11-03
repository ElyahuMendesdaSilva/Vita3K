// Vita3K emulator project - native AArch32 CPU backend (experimental, M1)
// GPL-2.0-or-later, same as Vita3K.
//
// Runs the guest's ARMv7 code directly on a 32-bit ARM host. Guest address == host address
// (identity window), so this only works once MemState maps guest memory at its own addresses.
#pragma once

#if defined(__arm__)

#include <cpu/state.h>

#include <atomic>
#include <csignal>
#include <ucontext.h>
#include <cstddef>
#include <cstdint>
#include <pthread.h>

// Guest window that must be mapped 1:1 in the host (see mem/ changes). Faults and traps are
// only treated as guest events when the interrupted PC is inside it.
#ifndef NATIVE_GUEST_BASE
#define NATIVE_GUEST_BASE 0x80000000u
#endif
#ifndef NATIVE_GUEST_END
#define NATIVE_GUEST_END 0xC0000000u
#endif
#ifndef NATIVE_STOP_SIGNAL
#define NATIVE_STOP_SIGNAL SIGUSR2
#endif

// ARM `udf #0`. Replaces the guest import stub's `svc #0` (a real svc would be a Linux syscall).
inline constexpr uint32_t NATIVE_IMPORT_UDF = 0xe7f000f0;
// Thumb `udf #0`, used as the halt instruction the guest returns to (return_pc).
inline constexpr uint16_t NATIVE_HALT_UDF_THUMB = 0xde00;

// Layout shared with native_enter_arm.S. Do not reorder.
struct alignas(8) NativeCtx {
    uint32_t r[16]; // 0: r0-r12, sp, lr, pc (pc without the Thumb bit)
    uint32_t cpsr; // 64
    uint32_t fpscr; // 68
    uint32_t tls_hw; // 72: value written to the real TPIDRURW on entry
    uint32_t reserved; // 76
    uint32_t s[64]; // 80: s0-s63 == d0-d31
};
static_assert(offsetof(NativeCtx, cpsr) == 64 && offsetof(NativeCtx, fpscr) == 68);
static_assert(offsetof(NativeCtx, tls_hw) == 72 && offsetof(NativeCtx, s) == 80);

struct NativeHost {
    uint32_t sp = 0; // host sp after native_enter's pushes, where native_leave resumes
};

enum NativeExit : uint32_t {
    NATIVE_EXIT_IMPORT = 1,
    NATIVE_EXIT_RETURNED = 2,
    NATIVE_EXIT_FAULT = 3,
    NATIVE_EXIT_STOPPED = 4,
    NATIVE_EXIT_UNDEFINED = 5,
};

extern "C" uint32_t native_enter(NativeCtx *ctx, NativeHost *host);
extern "C" void native_leave();

class NativeCPU : public CPUState {
    friend struct NativeSignals;

    NativeCtx ctx_{};
    NativeHost host_{};
    uint32_t tpidruro_ = 0;
    uint32_t tpidrurw_ = 0;

    std::atomic<bool> in_guest_{ false };
    std::atomic<bool> stop_requested_{ false };
    std::atomic<pthread_t> tid_{};
    uint32_t pending_nid_ = 0;
    bool unhandled_fault_ = false;
    uint32_t last_fault_pc_ = 0;
    int fault_repeat_ = 0;
    bool log_mem_ = false;

    uint32_t run_guest();
    void capture(ucontext_t *uc);

public:
    NativeCPU(MemState &mem, SceUID thread_id, std::size_t processor_id);
    ~NativeCPU() override;

    std::optional<uint32_t> execute(GuestBoundary &boundary, bool from_kernel_call) override;
    void request_stop() override;

    uint32_t get_reg(uint8_t idx) override;
    void set_reg(uint8_t idx, uint32_t val) override;
    uint32_t get_cpsr() override;
    void set_cpsr(uint32_t val) override;
    uint32_t get_tpidruro() override;
    void set_tpidruro(uint32_t val) override;
    uint32_t get_tpidrurw() override;
    void set_tpidrurw(uint32_t val) override;
    float get_float_reg(uint8_t idx) override;
    void set_float_reg(uint8_t idx, float val) override;
    uint32_t get_fpscr() override;
    void set_fpscr(uint32_t val) override;
    CPUContext save_context() override;
    void load_context(const CPUContext &ctx) override;
    void invalidate_jit_cache(Address start, size_t length) override;
    bool hit_breakpoint() override;
    void set_log_code(bool log) override;
    void set_log_mem(bool log) override;
    bool get_log_mem() override;
};

#endif // __arm__
