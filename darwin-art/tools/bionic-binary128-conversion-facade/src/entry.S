.text
.p2align 2

/* Android: long double strtold(const char*, char**). Darwin never observes a
 * long-double type. The core writes IEEE binary128 to our aligned spill and
 * this entry returns those raw bits in q0 as required by AAPCS64. */
.globl _darwin_art_bionic_strtold
_darwin_art_bionic_strtold:
  stp x29, x30, [sp, #-32]!
  mov x29, sp
  add x2, sp, #16
  bl _darwin_art_bionic_strtold_raw
  ldr q0, [sp, #16]
  ldp x29, x30, [sp], #32
  ret

.globl _darwin_art_bionic_strtold_l
_darwin_art_bionic_strtold_l:
  stp x29, x30, [sp, #-32]!
  mov x29, sp
  add x3, sp, #16
  bl _darwin_art_bionic_strtold_l_raw
  ldr q0, [sp, #16]
  ldp x29, x30, [sp], #32
  ret

.globl _darwin_art_bionic_wcstold
_darwin_art_bionic_wcstold:
  stp x29, x30, [sp, #-32]!
  mov x29, sp
  add x2, sp, #16
  bl _darwin_art_bionic_wcstold_raw
  ldr q0, [sp, #16]
  ldp x29, x30, [sp], #32
  ret
