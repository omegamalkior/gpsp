    .section .crt0, "ax"
    .arm
    .global _start
_start:
    b reset
    .fill 156, 1, 0          @ logo (not checked by gpSP)
    .ascii "GBABENCH    "    @ title (12)
    .ascii "GBNE"            @ game code
    .ascii "01"              @ maker
    .byte 0x96, 0, 0
    .fill 7, 1, 0
    .byte 0, 0               @ version, checksum (unchecked)
    .byte 0, 0
reset:
    mov r0, #0x12            @ IRQ mode
    msr cpsr_c, r0
    ldr sp, =0x03007fa0
    mov r0, #0x13            @ SVC
    msr cpsr_c, r0
    ldr sp, =0x03007fe0
    mov r0, #0x1f            @ SYS
    msr cpsr_c, r0
    ldr sp, =0x03007f00
    @ copy .iwram/.data
    ldr r0, =__iwram_lma
    ldr r1, =__iwram_start
    ldr r2, =__iwram_end
1:  cmp r1, r2
    ldrlo r3, [r0], #4
    strlo r3, [r1], #4
    blo 1b
    @ clear bss
    ldr r1, =__bss_start
    ldr r2, =__bss_end
    mov r3, #0
2:  cmp r1, r2
    strlo r3, [r1], #4
    blo 2b
    @ ewram bss
    ldr r1, =__ebss_start
    ldr r2, =__ebss_end
3:  cmp r1, r2
    strlo r3, [r1], #4
    blo 3b
    ldr r0, =irq_handler
    ldr r1, =0x03007ffc
    str r0, [r1]
    ldr r0, =main
    mov lr, pc
    bx r0
4:  b 4b
    .pool

    .section .iwram, "ax"
    .arm
    .global irq_handler
irq_handler:
    @ BIOS has pushed r0-r3,r12,lr; r0 = 0x04000000
    mov r0, #0x04000000
    ldr r1, [r0, #0x200]         @ IE|IF
    and r1, r1, r1, lsr #16      @ IE & IF
    add r2, r0, #0x200
    strh r1, [r2, #2]            @ ack IF
    ldr r2, =0x03007ff8
    ldrh r3, [r2]
    orr r3, r3, r1
    strh r3, [r2]                @ BIOS IF for IntrWait
    ldr r2, =irq_count
    ldr r3, [r2]
    add r3, r3, #1
    str r3, [r2]
    bx lr
    .pool
