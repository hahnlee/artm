#pragma once
#include <stddef.h>

// Caller owns the mapping; retire all executions before munmap. MAP_FAILED on
// error. Requires the signed host JIT entitlement and thread protection support.
void* DarwinArtMapJitCode(size_t size);
void DarwinArtFlushJitCode(void* code, size_t size);
void DarwinArtJitWriteBegin();
void DarwinArtJitWriteEnd();
// Signal-safe diagnostic for the current thread. A non-zero value means
// MAP_JIT is writable and therefore intentionally non-executable here.
unsigned DarwinArtJitWriteDepth();

// Thread-affine and nestable. Never execute JIT code inside a write scope.
class DarwinArtJitWriteScope {
 public:
  DarwinArtJitWriteScope();
  ~DarwinArtJitWriteScope();
  DarwinArtJitWriteScope(const DarwinArtJitWriteScope&) = delete;
  DarwinArtJitWriteScope& operator=(const DarwinArtJitWriteScope&) = delete;
};
