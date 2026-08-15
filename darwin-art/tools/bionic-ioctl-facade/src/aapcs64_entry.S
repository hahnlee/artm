.text
.p2align 2
.globl _darwin_art_bionic_ioctl
_darwin_art_bionic_ioctl:
  sub sp, sp, #80
  stp x0, x1, [sp, #0]
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  str x30, [sp, #64]
  mov x0, sp
  bl _darwin_art_bionic_ioctl_captured
  ldr x30, [sp, #64]
  add sp, sp, #80
  ret
