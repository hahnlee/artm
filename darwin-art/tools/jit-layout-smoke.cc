#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>

int main() {
  const size_t page = static_cast<size_t>(getpagesize());
  const size_t metadata_size = 32u * 1024u * 1024u;
  void* metadata = mmap(nullptr, metadata_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
  if (metadata == MAP_FAILED) {
    perror("metadata mmap");
    return 1;
  }
  const uintptr_t metadata_begin = reinterpret_cast<uintptr_t>(metadata);
  const uintptr_t hint = metadata_begin + metadata_size;
  void* code = mmap(reinterpret_cast<void*>(hint), metadata_size,
                    PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  if (code == MAP_FAILED) {
    perror("MAP_JIT hint");
    munmap(metadata, metadata_size);
    return 2;
  }
  const uintptr_t code_begin = reinterpret_cast<uintptr_t>(code);
  const uintptr_t code_end = code_begin + metadata_size;
  const bool ordered = metadata_begin + metadata_size <= code_begin;
  const bool span_ok = code_end >= metadata_begin &&
                       code_end - metadata_begin <= UINT32_MAX;
  std::printf("JIT layout smoke: metadata=%p code=%p hint=%p page=%zu ordered=%s span=%s\n",
              metadata, code, reinterpret_cast<void*>(hint), page,
              ordered ? "pass" : "fail", span_ok ? "pass" : "fail");
  munmap(code, metadata_size);
  munmap(metadata, metadata_size);
  return ordered && span_ok ? 0 : 3;
}
