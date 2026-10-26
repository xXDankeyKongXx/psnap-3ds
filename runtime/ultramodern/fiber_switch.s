@ 3DS port: user-mode context switch for game-thread fibers (fibers.cpp).
@ FiberRegs: r4-r11 (0-28), sp (32), lr (36), d8-d15 (40-103): what the ARM ABI has a callee keep.

    .arch armv6k
    .fpu vfpv2
    .arm
    .text

@ void fiber_switch(FiberRegs* from, const FiberRegs* to)
    .global fiber_switch
    .type fiber_switch, %function
fiber_switch:
    stmia   r0, {r4-r11}
    str     sp, [r0, #32]
    str     lr, [r0, #36]
    add     r2, r0, #40
    vstmia  r2, {d8-d15}
    ldmia   r1, {r4-r11}
    ldr     sp, [r1, #32]
    ldr     lr, [r1, #36]
    add     r2, r1, #40
    vldmia  r2, {d8-d15}
    bx      lr

@ A new fiber's first switch lands here: r4 holds its Fiber*.
    .global fiber_trampoline
    .type fiber_trampoline, %function
fiber_trampoline:
    mov     r0, r4
    bl      fiber_entry
1:  b       1b
