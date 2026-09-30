.text
.align 2

.global ra_frame_wrapper
.hidden ra_frame_wrapper
.type ra_frame_wrapper, %function

.extern ra_frame_tick
.extern g_original_frame_target

/*
 * Entered from NNDDSS's existing DraStic frame trampoline.
 *
 * Preserve all volatile integer and SIMD argument registers, call
 * ra_frame_tick(), restore everything, then continue to DraStic's
 * original target.
 */
ra_frame_wrapper:
    sub sp, sp, #0x120

    stp x0,  x1,  [sp, #0x00]
    stp x2,  x3,  [sp, #0x10]
    stp x4,  x5,  [sp, #0x20]
    stp x6,  x7,  [sp, #0x30]
    stp x8,  x9,  [sp, #0x40]
    stp x10, x11, [sp, #0x50]
    stp x12, x13, [sp, #0x60]
    stp x14, x15, [sp, #0x70]
    stp x16, x17, [sp, #0x80]
    stp x18, x30, [sp, #0x90]

    stp q0, q1, [sp, #0xA0]
    stp q2, q3, [sp, #0xC0]
    stp q4, q5, [sp, #0xE0]
    stp q6, q7, [sp, #0x100]

    bl ra_frame_tick

    ldp q0, q1, [sp, #0xA0]
    ldp q2, q3, [sp, #0xC0]
    ldp q4, q5, [sp, #0xE0]
    ldp q6, q7, [sp, #0x100]

    ldp x0,  x1,  [sp, #0x00]
    ldp x2,  x3,  [sp, #0x10]
    ldp x4,  x5,  [sp, #0x20]
    ldp x6,  x7,  [sp, #0x30]
    ldp x8,  x9,  [sp, #0x40]
    ldp x10, x11, [sp, #0x50]
    ldp x12, x13, [sp, #0x60]
    ldp x14, x15, [sp, #0x70]
    ldp x16, x17, [sp, #0x80]
    ldp x18, x30, [sp, #0x90]

    adrp x16, g_original_frame_target
    add  x16, x16, :lo12:g_original_frame_target
    ldr  x16, [x16]

    add sp, sp, #0x120
    br x16

.size ra_frame_wrapper, .-ra_frame_wrapper

.section .note.GNU-stack,"",%progbits
