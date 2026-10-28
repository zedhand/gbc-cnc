  .section .crt0,"ax"
  .arm
  .global _start
_start: b start
  .fill 188,1,0        @ header (logo/title filled by gbafix in the real build)
  b start              @ 0xC0 multiboot entry
  .fill 28,1,0
start:
  mov r0,#0x12 ; msr cpsr_c,r0 ; ldr sp,=0x03007FA0
  mov r0,#0x1F ; msr cpsr_c,r0 ; ldr sp,=0x03007F00
  ldr r0,=__iwram_lma ; ldr r1,=__iwram_start ; ldr r2,=__iwram_end
1: cmp r1,r2 ; ldrlt r3,[r0],#4 ; strlt r3,[r1],#4 ; blt 1b
  ldr r1,=__bss_start ; ldr r2,=__bss_end ; mov r3,#0
2: cmp r1,r2 ; strlt r3,[r1],#4 ; blt 2b
  ldr r0,=main ; bx r0
