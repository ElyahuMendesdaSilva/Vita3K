@ Vita3K native AArch32 backend: switch between host and guest.
@
@ uint32_t native_enter(NativeCtx *ctx /*r0*/, NativeHost *host /*r1*/)
@   Saves the host's callee-saved state on the host stack, loads the whole guest state from ctx
@   and jumps to the guest PC. It does not return by itself: the signal handler captures the guest
@   state into ctx and redirects the interrupted context to native_leave, with sp = host->sp and
@   r0 = NativeExit. native_leave then returns to native_enter's caller.
@
@ NativeCtx offsets: r[16]=0, cpsr=64, fpscr=68, tls_hw=72, s[64]=80.

    .syntax unified
    .arch   armv7-a
    .fpu    neon
    .arm
    .text

    .global native_enter
    .type   native_enter, %function
    .p2align 2
native_enter:
    push    {r4-r11, lr}
    vpush   {d8-d15}
    vmrs    r2, fpscr
    push    {r2}                    @ host fpscr
    str     sp, [r1]                @ host->sp
    mov     r4, r0                  @ r4 = ctx

    clrex
    ldr     r0, [r4, #72]
    mcr     p15, 0, r0, c13, c0, 2  @ real TPIDRURW (the guest's TLS, see notes)
    ldr     r0, [r4, #68]
    vmsr    fpscr, r0
    add     r0, r4, #80
    vldmia  r0!, {d0-d15}
    vldmia  r0, {d16-d31}

    ldr     r5, [r4, #60]           @ guest pc
    ldr     r6, [r4, #64]           @ guest cpsr
    tst     r6, #0x20               @ Thumb?
    orrne   r5, r5, #1              @ bit 0 selects the state when loaded into pc
    str     r5, [r4, #60]
    msr     APSR_nzcvqg, r6         @ N Z C V Q GE; ITSTATE must be zero (caller guarantees)

    add     sp, r4, #52             @ sp = &ctx.r[13]
    ldm     r4, {r0-r12}            @ guest r0-r12 (r4 itself is overwritten last)
    ldm     sp, {sp, lr, pc}        @ guest sp, lr and pc (interworking through bit 0)
    .size   native_enter, . - native_enter

@ Entered from the signal handler with sp = host->sp and r0 = NativeExit, in ARM state.
    .global native_leave
    .type   native_leave, %function
    .p2align 2
native_leave:
    pop     {r2}
    vmsr    fpscr, r2
    vpop    {d8-d15}
    pop     {r4-r11, pc}            @ back to native_enter's caller, r0 = reason
    .size   native_leave, . - native_leave

    .section .note.GNU-stack,"",%progbits
