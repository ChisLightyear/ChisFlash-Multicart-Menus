/* 游戏交接使用 BIOS 清理 RAM 与硬件状态；卡带映射和 SRAM 存档保持不变。 */
.syntax unified
.arm
.section .iwram, "ax", %progbits
.align 2
.global BootGameReset
.type BootGameReset, %function
BootGameReset:
    mov r3, #0x04000000
    add r1, r3, #0x200
    mov r2, #0
    strh r2, [r1, #8]
    strh r2, [r3, #4]
    strh r2, [r1]
    mvn r2, #0
    strh r2, [r1, #2]

    /* 清 RAM 前停止 DMA 与定时器，避免后台传输继续使用菜单数据。 */
    mov r2, #0
    strh r2, [r3, #0xBA]
    strh r2, [r3, #0xC6]
    strh r2, [r3, #0xD2]
    strh r2, [r3, #0xDE]
    add r1, r3, #0x100
    strh r2, [r1, #2]
    strh r2, [r1, #6]
    strh r2, [r1, #10]
    strh r2, [r1, #14]

    /* SWI 1 保留 IWRAM 最后 0x200 字节，从该区域清理后直接复位到 ROM。 */
    ldr r1, =0x03007FFA
    strb r2, [r1]
    ldr r1, =0x03007E00
    ldr r2, =0xDF0120FF  /* Thumb: movs r0, #0xFF; swi 1 */
    str r2, [r1]
    ldr r2, =0xE7FEDF00  /* Thumb: swi 0; b . */
    str r2, [r1, #4]
    add r1, r1, #1
    /* BIOS 调用也会使用当前栈，先把栈移到不会被清零的保留区域。 */
    ldr sp, =0x03007F00
    bx r1
    .ltorg
.size BootGameReset, .-BootGameReset
