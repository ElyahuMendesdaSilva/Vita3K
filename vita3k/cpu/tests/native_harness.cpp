// M1 harness: runs hand-assembled ARM code through NativeCPU without the rest of Vita3K.
// Needs a 32-bit ARM host (armeabi-v7a) and the free window at NATIVE_GUEST_BASE.
#if defined(__arm__)
#include <cpu/impl/native_cpu.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstring>

struct FakeBoundary : GuestBoundary {
    NativeCPU *cpu = nullptr;
    uint32_t last_nid = 0;
    int imports = 0;
    GuestResume checkpoint(GuestStop stop) override {
        return stop.returned ? GuestResume::leave : GuestResume::run;
    }
    void call_import(uint32_t nid) override {
        last_nid = nid;
        imports++;
        cpu->set_reg(0, cpu->get_reg(0) * 10); // the "HLE function": r0 *= 10
    }
    void dispatch_abort(Address) override {}
};

int main() {
    const uintptr_t W = NATIVE_GUEST_BASE;
    void *p = mmap(reinterpret_cast<void *>(W), 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != reinterpret_cast<void *>(W)) {
        std::puts("FAIL: cannot map the guest window");
        return 2;
    }
    auto *w = reinterpret_cast<uint32_t *>(W);
    // import stub at +0: udf; mov pc, lr; .word nid
    w[0] = NATIVE_IMPORT_UDF;
    w[1] = 0xe1a0f00e;
    w[2] = 0x1234;
    // halt (return_pc) at +0x100, Thumb udf
    *reinterpret_cast<uint16_t *>(W + 0x100) = NATIVE_HALT_UDF_THUMB;
    // function at +0x200: r0 += r1; call the stub; return
    w[0x200 / 4 + 0] = 0xe0800001; // add r0, r0, r1
    w[0x200 / 4 + 1] = 0xe92d4000; // push {lr}
    w[0x200 / 4 + 2] = 0xebffff7c; // bl W+0
    w[0x200 / 4 + 3] = 0xe8bd4000; // pop {lr}
    w[0x200 / 4 + 4] = 0xe12fff1e; // bx lr
    __builtin___clear_cache(reinterpret_cast<char *>(W), reinterpret_cast<char *>(W + 0x1000));

    MemState mem;
    NativeCPU cpu(mem, 1, 0);
    set_current_cpu_state(&cpu); // the signal handler finds the CPU through this
    FakeBoundary b;
    b.cpu = &cpu;
    cpu.return_pc = (W + 0x100) | 1;
    cpu.set_sp(W + 0xF000);
    cpu.set_reg(0, 2);
    cpu.set_reg(1, 3);
    cpu.set_lr(cpu.return_pc);
    cpu.set_pc(W + 0x200);

    const auto r = cpu.execute(b, false);
    const bool ok = r && *r == 50 && b.imports == 1 && b.last_nid == 0x1234;
    std::printf("%s: r0=%d imports=%d nid=0x%x\n", ok ? "PASS" : "FAIL", r ? static_cast<int>(*r) : -1, b.imports, b.last_nid);
    return ok ? 0 : 1;
}
#else
int main() { return 0; }
#endif
