#include "darwin_jit_memory.h"
#include <cassert>
#include <cerrno>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>

namespace { thread_local unsigned write_depth = 0; }

void* DarwinArtMapJitCode(size_t size) {
  if (size == 0 || !pthread_jit_write_protect_supported_np()) {
    errno = ENOTSUP;
    return MAP_FAILED;
  }
  return mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
              MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
}

void DarwinArtJitWriteBegin() {
  assert(pthread_jit_write_protect_supported_np());
  if (write_depth++ == 0) pthread_jit_write_protect_np(0);
}

void DarwinArtJitWriteEnd() {
  assert(write_depth > 0);
  if (--write_depth == 0) pthread_jit_write_protect_np(1);
}

DarwinArtJitWriteScope::DarwinArtJitWriteScope() {
  DarwinArtJitWriteBegin();
}

DarwinArtJitWriteScope::~DarwinArtJitWriteScope() {
  DarwinArtJitWriteEnd();
}

void DarwinArtFlushJitCode(void* code, size_t size) {
  sys_dcache_flush(code, size);
  sys_icache_invalidate(code, size);
}
