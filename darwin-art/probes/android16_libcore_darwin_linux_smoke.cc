#include "libcore_darwin_linux.h"

#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

uint64_t Fnv1a(const unsigned char* bytes, size_t size) {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ULL;
  }
  return hash;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s INPUT\n", argv[0]);
    return 2;
  }
  errno = 0;
  const long processors =
      darwin_art::libcore_darwin::Sysconf(_SC_NPROCESSORS_CONF);
  if (processors <= 0) {
    std::fprintf(stderr, "sysconf(_SC_NPROCESSORS_CONF): result=%ld errno=%s\n",
                 processors, std::strerror(errno));
    return 3;
  }
  // Linux O_PATH has no faithful Darwin file-description equivalent. The
  // boundary must reject it explicitly instead of silently weakening access.
  errno = 0;
  if (darwin_art::libcore_darwin::Open(argv[1], 010000000, 0) != -1 ||
      errno != ENOTSUP) {
    std::fprintf(stderr, "O_PATH did not fail with ENOTSUP\n");
    return 4;
  }
  const int fd = darwin_art::libcore_darwin::Open(argv[1], 0, 0);
  if (fd == -1) {
    std::fprintf(stderr, "open: %s\n", std::strerror(errno));
    return 5;
  }
  struct stat status {};
  if (darwin_art::libcore_darwin::Fstat(fd, &status) == -1 ||
      status.st_size <= 0) {
    std::fprintf(stderr, "fstat: %s\n", std::strerror(errno));
    return 6;
  }
  std::array<unsigned char, 64> prefix {};
  const ssize_t read_count = darwin_art::libcore_darwin::Read(
      fd, prefix.data(), prefix.size());
  if (read_count != static_cast<ssize_t>(prefix.size())) {
    std::fprintf(stderr, "read: count=%zd errno=%s\n", read_count,
                 std::strerror(errno));
    return 7;
  }
  void* mapping = darwin_art::libcore_darwin::Mmap(
      nullptr, static_cast<size_t>(status.st_size), 1, 1, fd, 0);
  if (mapping == MAP_FAILED) {
    std::fprintf(stderr, "mmap: %s\n", std::strerror(errno));
    return 8;
  }
  const uint64_t prefix_hash = Fnv1a(prefix.data(), prefix.size());
  const uint64_t mapped_hash = Fnv1a(
      static_cast<const unsigned char*>(mapping), prefix.size());
  if (darwin_art::libcore_darwin::Munmap(
          mapping, static_cast<size_t>(status.st_size)) == -1) {
    std::fprintf(stderr, "munmap: %s\n", std::strerror(errno));
    return 9;
  }
  if (darwin_art::libcore_darwin::Close(fd) == -1) {
    std::fprintf(stderr, "close: %s\n", std::strerror(errno));
    return 10;
  }
  if (prefix_hash != mapped_hash) {
    std::fprintf(stderr, "read/mmap hash mismatch\n");
    return 11;
  }
  std::printf("libcore-darwin-linux: processors=%ld bytes=%lld prefix-hash=%016llx\n",
              processors,
              static_cast<long long>(status.st_size),
              static_cast<unsigned long long>(prefix_hash));
  return 0;
}
