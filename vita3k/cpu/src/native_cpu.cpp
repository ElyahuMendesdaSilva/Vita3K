// Vita3K emulator project - native AArch32 CPU backend (experimental, M1)
// GPL-2.0-or-later, same as Vita3K.

#if defined(__arm__)

#include <cpu/impl/native_cpu.h>

#include <mem/functions.h>
#include <mem/ptr.h>
#include <util/log.h>

#include <sys/mman.h>
#include <ucontext.h>

#include <bit>
#include <cstring>
#include <mutex>

// ITSTATE bits of the CPSR. They cannot be restored from user mode, so guest code is only
// stopped or resumed outside IT blocks.
static constexpr uint32_t CPSR_IT_MASK = 0x0600FC00u;
static constexpr uint32_t CPSR_THUMB = 0x20u;

// bionic's ucontext_t for ARM exposes the extra register state (VFP) as uc_regspace.
// VERIFY in <sys/ucontext.h> of the NDK in use; adapt this macro if the name differs.
#define NATIVE_UC_REGSPACE(uc) (reinterpret_cast<uint8_t *>((uc)->uc_regspace))

struct NativeAuxHead {
    uint32_t magic;
    uint32_t size;
};
static constexpr uint32_t VFP_MAGIC = 0x56465001;
struct NativeVfpFrame {
    NativeAuxHead head;
    uint64_t fpregs[32];
    uint32_t fpscr;
};

static bool in_guest_window(uintptr_t pc) {
    return pc >= NATIVE_GUEST_BASE && pc < NATIVE_GUEST_END;
}

// ---------------------------------------------------------------------------------------------
// Signal handling. One mechanism ends every guest run: imports, return to the halt instruction,
// faults and stop requests all arrive as signals while the PC is inside the guest window, the
// handler captures the guest state and redirects the context to native_leave.

struct NativeSignals {
    static struct sigaction old_segv, old_bus, old_ill;

    static void capture_vfp(ucontext_t *uc, NativeCtx &ctx) {
        uint8_t *p = NATIVE_UC_REGSPACE(uc);
        for (;;) {
            const auto *head = reinterpret_cast<const NativeAuxHead *>(p);
            if (head->magic == VFP_MAGIC) {
                const auto *vfp = reinterpret_cast<const NativeVfpFrame *>(p);
                std::memcpy(ctx.s, vfp->fpregs, sizeof(ctx.s));
                ctx.fpscr = vfp->fpscr;
                return;
            }
            if (head->magic == 0 || head->size == 0)
                return;
            p += head->size;
        }
    }

    static void leave(ucontext_t *uc, NativeCPU &cpu, NativeExit why) {
        cpu.capture(uc);
        cpu.in_guest_.store(false, std::memory_order_release);
        auto &m = uc->uc_mcontext;
        m.arm_pc = reinterpret_cast<uintptr_t>(&native_leave);
        m.arm_sp = cpu.host_.sp;
        m.arm_r0 = why;
        m.arm_cpsr &= ~(CPSR_THUMB | CPSR_IT_MASK); // native_leave is ARM code
    }

    static void chain(const struct sigaction &old, int sig, siginfo_t *si, void *uc) {
        if (old.sa_flags & SA_SIGINFO) {
            if (old.sa_sigaction)
                old.sa_sigaction(sig, si, uc);
        } else if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN && old.sa_handler) {
            old.sa_handler(sig);
        } else {
            signal(sig, SIG_DFL);
            raise(sig);
        }
    }

    static void handler(int sig, siginfo_t *si, void *uct) {
        auto *uc = static_cast<ucontext_t *>(uct);
        auto &m = uc->uc_mcontext;
        // The thread's CPU, set by ThreadState::run_loop. Only NativeCPU exists in this build.
        auto *cpu = static_cast<NativeCPU *>(get_current_cpu_state());
        const bool guest_pc = cpu && cpu->in_guest_.load(std::memory_order_acquire) && in_guest_window(m.arm_pc);

        if (sig == NATIVE_STOP_SIGNAL) {
            // Outside the guest (or inside an IT block) the request stays pending: the loop reaches
            // a checkpoint on its own at the next import.
            if (guest_pc && !(m.arm_cpsr & CPSR_IT_MASK))
                leave(uc, *cpu, NATIVE_EXIT_STOPPED);
            return;
        }

        if (!guest_pc) {
            chain(sig == SIGILL ? old_ill : sig == SIGBUS ? old_bus
                                                          : old_segv,
                sig, si, uct);
            return;
        }

        if (sig == SIGILL) {
            const bool thumb = m.arm_cpsr & CPSR_THUMB;
            if ((m.arm_pc & ~1u) == (cpu->return_pc & ~1u)) {
                leave(uc, *cpu, NATIVE_EXIT_RETURNED);
            } else if (!thumb && *reinterpret_cast<const uint32_t *>(m.arm_pc) == NATIVE_IMPORT_UDF) {
                // Stub layout: udf; mov pc, lr; .word nid
                cpu->pending_nid_ = *reinterpret_cast<const uint32_t *>(m.arm_pc + 8);
                // capture() stores arm_pc, so move it past the udf first: the guest resumes at
                // the `mov pc, lr`, like the Dynarmic backend does after CallSVC.
                m.arm_pc += 4;
                leave(uc, *cpu, NATIVE_EXIT_IMPORT);
            } else {
                leave(uc, *cpu, NATIVE_EXIT_UNDEFINED);
            }
            return;
        }

        // SIGSEGV / SIGBUS inside guest code
        const uintptr_t addr = reinterpret_cast<uintptr_t>(si->si_addr);
        // VERIFY on device: on an arm64 kernel (compat task) error_code carries the ESR, whose
        // bit 6 is WnR; on a native arm32 kernel it is the FSR, whose bit 11 is WnR.
        const bool write = m.error_code & (1u << 6);
        if (handle_access_violation(cpu->mem, reinterpret_cast<uint8_t *>(addr), write)) {
            // A protect callback (kubridge) may have noted an abort: tell the thread. Otherwise
            // the page is unprotected now and the instruction is retried.
            if (cpu->abort_pending.load())
                leave(uc, *cpu, NATIVE_EXIT_FAULT);
            return;
        }
        cpu->unhandled_fault_ = true;
        cpu->abort_fault_addr.store(static_cast<uint32_t>(addr));
        cpu->abort_pending.store(true);
        leave(uc, *cpu, NATIVE_EXIT_FAULT);
    }

    static void install() {
        static std::once_flag once;
        std::call_once(once, [] {
            struct sigaction sa {};
            sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
            sa.sa_sigaction = &handler;
            sigemptyset(&sa.sa_mask);
            sigaddset(&sa.sa_mask, NATIVE_STOP_SIGNAL);
            sigaction(SIGSEGV, &sa, &old_segv);
            sigaction(SIGBUS, &sa, &old_bus);
            sigaction(SIGILL, &sa, &old_ill);
            sigaction(NATIVE_STOP_SIGNAL, &sa, nullptr);
        });
    }

    // Per host thread: an alternate stack (the guest stack must not host signal frames) and the
    // stop signal unblocked.
    static void prepare_thread() {
        static thread_local bool ready = false;
        if (ready)
            return;
        constexpr size_t size = 64 * 1024;
        void *stack = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack != MAP_FAILED) {
            stack_t ss{};
            ss.ss_sp = stack;
            ss.ss_size = size;
            sigaltstack(&ss, nullptr);
        }
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, NATIVE_STOP_SIGNAL);
        pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
        ready = true;
    }
};
struct sigaction NativeSignals::old_segv;
struct sigaction NativeSignals::old_bus;
struct sigaction NativeSignals::old_ill;

void NativeCPU::capture(ucontext_t *uc) {
    const auto &m = uc->uc_mcontext;
    const unsigned long regs[16] = { m.arm_r0, m.arm_r1, m.arm_r2, m.arm_r3, m.arm_r4, m.arm_r5, m.arm_r6, m.arm_r7,
        m.arm_r8, m.arm_r9, m.arm_r10, m.arm_fp, m.arm_ip, m.arm_sp, m.arm_lr, m.arm_pc };
    for (int i = 0; i < 16; i++)
        ctx_.r[i] = static_cast<uint32_t>(regs[i]);
    ctx_.cpsr = static_cast<uint32_t>(m.arm_cpsr);
    NativeSignals::capture_vfp(uc, ctx_);
}

// ---------------------------------------------------------------------------------------------

NativeCPU::NativeCPU(MemState &mem, SceUID thread_id, std::size_t processor_id)
    : CPUState(mem, thread_id, processor_id) {
    ctx_.cpsr = 0x10; // user mode, ARM state
}

NativeCPU::~NativeCPU() = default;

uint32_t NativeCPU::run_guest() {
    NativeSignals::install();
    NativeSignals::prepare_thread();
    tid_.store(pthread_self());

    ctx_.cpsr &= ~CPSR_IT_MASK;
    ctx_.tls_hw = tpidruro_; // M0 trick: the guest's `mrc c13,c0,3` is patched to read c13,c0,2
    in_guest_.store(true, std::memory_order_release);
    // A request that arrived before in_guest_ was visible would otherwise be lost
    if (stop_requested_.exchange(false)) {
        in_guest_.store(false, std::memory_order_release);
        return NATIVE_EXIT_STOPPED;
    }
    const uint32_t why = native_enter(&ctx_, &host_);
    in_guest_.store(false, std::memory_order_release);
    return why;
}

std::optional<uint32_t> NativeCPU::execute(GuestBoundary &boundary, bool from_kernel_call) {
    GuestResume resume = boundary.checkpoint({ .after_kernel_call = from_kernel_call });
    while (resume != GuestResume::leave) {
        // GuestResume::step is not supported by this backend: it runs instead
        unhandled_fault_ = false;
        const NativeExit why = static_cast<NativeExit>(run_guest());

        bool kernel_call = false;
        bool returned = false;
        switch (why) {
        case NATIVE_EXIT_IMPORT:
            kernel_call = true;
            boundary.call_import(pending_nid_);
            break;
        case NATIVE_EXIT_RETURNED:
            returned = true;
            break;
        case NATIVE_EXIT_UNDEFINED:
            LOG_CRITICAL("Undefined instruction at 0x{:x} (thread {})\n{}", ctx_.r[15], thread_id, save_context().description());
            returned = true; // like a NoExecuteFault in the Dynarmic backend: the thread stops
            break;
        case NATIVE_EXIT_FAULT:
            if (unhandled_fault_) {
                // No guest handler can fix this access. Dynarmic logs it and goes on with 0 or a
                // dropped write; here the instruction would fault forever, so a repeated fault at
                // the same PC ends the thread.
                LOG_ERROR("Invalid access at 0x{:x} by thread {} (pc 0x{:x})", abort_fault_addr.load(), thread_id, ctx_.r[15]);
                fault_repeat_ = (ctx_.r[15] == last_fault_pc_) ? fault_repeat_ + 1 : 0;
                last_fault_pc_ = ctx_.r[15];
            }
            break;
        case NATIVE_EXIT_STOPPED:
            break;
        }

        if (abort_pending.exchange(false))
            boundary.dispatch_abort(abort_fault_addr.load());
        if (fault_repeat_ >= 3) {
            LOG_CRITICAL("Thread {} faults forever at 0x{:x}, stopping it", thread_id, last_fault_pc_);
            fault_repeat_ = 0;
            returned = true;
        }

        resume = boundary.checkpoint({ .after_kernel_call = kernel_call, .debugger_stop = false, .returned = returned });
        if (returned)
            return get_reg(0);
    }
    return std::nullopt;
}

void NativeCPU::request_stop() {
    // Signal safe: an atomic, plus pthread_kill if the thread is running guest code
    stop_requested_.store(true, std::memory_order_release);
    if (in_guest_.load(std::memory_order_acquire))
        pthread_kill(tid_.load(), NATIVE_STOP_SIGNAL);
}

uint32_t NativeCPU::get_reg(uint8_t idx) { return ctx_.r[idx]; }
void NativeCPU::set_reg(uint8_t idx, uint32_t val) { ctx_.r[idx] = val; }
uint32_t NativeCPU::get_cpsr() { return ctx_.cpsr; }
void NativeCPU::set_cpsr(uint32_t val) { ctx_.cpsr = val; }
uint32_t NativeCPU::get_tpidruro() { return tpidruro_; }
void NativeCPU::set_tpidruro(uint32_t val) { tpidruro_ = val; }
uint32_t NativeCPU::get_tpidrurw() { return tpidrurw_; }
void NativeCPU::set_tpidrurw(uint32_t val) { tpidrurw_ = val; }
float NativeCPU::get_float_reg(uint8_t idx) { return std::bit_cast<float>(ctx_.s[idx]); }
void NativeCPU::set_float_reg(uint8_t idx, float val) { ctx_.s[idx] = std::bit_cast<uint32_t>(val); }
uint32_t NativeCPU::get_fpscr() { return ctx_.fpscr; }
void NativeCPU::set_fpscr(uint32_t val) { ctx_.fpscr = val; }

CPUContext NativeCPU::save_context() {
    CPUContext c;
    std::memcpy(c.cpu_registers.data(), ctx_.r, sizeof(ctx_.r));
    std::memcpy(c.fpu_registers.data(), ctx_.s, sizeof(ctx_.s));
    c.cpsr = ctx_.cpsr;
    c.fpscr = ctx_.fpscr;
    return c;
}

void NativeCPU::load_context(const CPUContext &c) {
    std::memcpy(ctx_.r, c.cpu_registers.data(), sizeof(ctx_.r));
    std::memcpy(ctx_.s, c.fpu_registers.data(), sizeof(ctx_.s));
    ctx_.cpsr = c.cpsr;
    ctx_.fpscr = c.fpscr;
}

void NativeCPU::invalidate_jit_cache(Address start, size_t length) {
    // Guest code is real code now: after loading, relocating or patching it, the I-cache must see it
    char *const p = reinterpret_cast<char *>(Ptr<uint8_t>(start).get(mem));
    __builtin___clear_cache(p, p + length);
}

bool NativeCPU::hit_breakpoint() { return false; }
void NativeCPU::set_log_code(bool log) {
    if (log)
        LOG_WARN("Code tracing is not supported by the native CPU backend");
}
void NativeCPU::set_log_mem(bool log) { log_mem_ = log; }
bool NativeCPU::get_log_mem() { return log_mem_; }

#endif // __arm__
