.text
.p2align 2

// Android arm64's direct JNI CallVoidMethod entry is variadic.  Darwin and
// Android do not share a C va_list ABI, so capture the AAPCS64 argument banks
// before entering Darwin C and build the Android va_list there.
.globl _darwin_art_jni_proxy_call_void_method
_darwin_art_jni_proxy_call_void_method:
  sub sp, sp, #224
  stp x0, x1, [sp, #0]
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  stp q0, q1, [sp, #64]
  stp q2, q3, [sp, #96]
  stp q4, q5, [sp, #128]
  stp q6, q7, [sp, #160]
  str x30, [sp, #192]
  ldp x0, x1, [sp, #0]
  ldr x2, [sp, #16]
  add x3, sp, #0
  add x4, sp, #64
  add x5, sp, #224
  bl _darwin_art_jni_proxy_call_void_method_captured
  ldr x30, [sp, #192]
  add sp, sp, #224
  ret

.globl _darwin_art_jni_proxy_call_int_method
_darwin_art_jni_proxy_call_int_method:
  sub sp, sp, #224
  stp x0, x1, [sp, #0]
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  stp q0, q1, [sp, #64]
  stp q2, q3, [sp, #96]
  stp q4, q5, [sp, #128]
  stp q6, q7, [sp, #160]
  str x30, [sp, #192]
  ldp x0, x1, [sp, #0]
  ldr x2, [sp, #16]
  add x3, sp, #0
  add x4, sp, #64
  add x5, sp, #224
  bl _darwin_art_jni_proxy_call_int_method_captured
  ldr x30, [sp, #192]
  add sp, sp, #224
  ret

.globl _darwin_art_jni_proxy_call_object_method
_darwin_art_jni_proxy_call_object_method:
  sub sp, sp, #224
  stp x0, x1, [sp, #0]
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  stp q0, q1, [sp, #64]
  stp q2, q3, [sp, #96]
  stp q4, q5, [sp, #128]
  stp q6, q7, [sp, #160]
  str x30, [sp, #192]
  ldp x0, x1, [sp, #0]
  ldr x2, [sp, #16]
  add x3, sp, #0
  add x4, sp, #64
  add x5, sp, #224
  bl _darwin_art_jni_proxy_call_object_method_captured
  ldr x30, [sp, #192]
  add sp, sp, #224
  ret

.globl _darwin_art_jni_proxy_new_object
_darwin_art_jni_proxy_new_object:
  sub sp, sp, #224
  stp x0, x1, [sp, #0]
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  stp q0, q1, [sp, #64]
  stp q2, q3, [sp, #96]
  stp q4, q5, [sp, #128]
  stp q6, q7, [sp, #160]
  str x30, [sp, #192]
  ldp x0, x1, [sp, #0]
  ldr x2, [sp, #16]
  add x3, sp, #0
  add x4, sp, #64
  add x5, sp, #224
  bl _darwin_art_jni_proxy_new_object_captured
  ldr x30, [sp, #192]
  add sp, sp, #224
  ret

// Static direct-varargs JNI has the same AAPCS64 register layout (JNIEnv*,
// jclass, jmethodID, ...), but must dispatch with the static-call flag.
.globl _darwin_art_jni_proxy_call_static_void_method
_darwin_art_jni_proxy_call_static_void_method:
  sub sp, sp, #224
  stp x0, x1, [sp, #0]
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  stp q0, q1, [sp, #64]
  stp q2, q3, [sp, #96]
  stp q4, q5, [sp, #128]
  stp q6, q7, [sp, #160]
  str x30, [sp, #192]
  ldp x0, x1, [sp, #0]
  ldr x2, [sp, #16]
  add x3, sp, #0
  add x4, sp, #64
  add x5, sp, #224
  bl _darwin_art_jni_proxy_call_static_void_method_captured
  ldr x30, [sp, #192]
  add sp, sp, #224
  ret

// Same capture for jint-returning static calls. The Darwin helper returns the
// backend's 32-bit result in w0, which is also the Android AAPCS64 jint ABI.
.globl _darwin_art_jni_proxy_call_static_int_method
_darwin_art_jni_proxy_call_static_int_method:
  sub sp, sp, #224
  stp x0, x1, [sp, #0]
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  stp q0, q1, [sp, #64]
  stp q2, q3, [sp, #96]
  stp q4, q5, [sp, #128]
  stp q6, q7, [sp, #160]
  str x30, [sp, #192]
  ldp x0, x1, [sp, #0]
  ldr x2, [sp, #16]
  add x3, sp, #0
  add x4, sp, #64
  add x5, sp, #224
  bl _darwin_art_jni_proxy_call_static_int_method_captured
  ldr x30, [sp, #192]
  add sp, sp, #224
  ret

.globl _darwin_art_jni_proxy_call_static_object_method
_darwin_art_jni_proxy_call_static_object_method:
  sub sp, sp, #224
  stp x0, x1, [sp, #0]
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  stp q0, q1, [sp, #64]
  stp q2, q3, [sp, #96]
  stp q4, q5, [sp, #128]
  stp q6, q7, [sp, #160]
  str x30, [sp, #192]
  ldp x0, x1, [sp, #0]
  ldr x2, [sp, #16]
  add x3, sp, #0
  add x4, sp, #64
  add x5, sp, #224
  bl _darwin_art_jni_proxy_call_static_object_method_captured
  ldr x30, [sp, #192]
  add sp, sp, #224
  ret
