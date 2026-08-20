#include "runtime_filesystem_probe.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "darwin_art_bionic_fs.h"

bool InstallProbeAndroidSystemRoot() {
  const char* root = std::getenv("DARWIN_ART_ANDROID_SYSTEM_ROOT");
  if (root == nullptr || root[0] == '\0') return true;
  const int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    std::cerr << "ART Android filesystem: cannot open test root " << root << "\n";
    return false;
  }
  constexpr uint8_t kMount[] = {'/', 's', 'y', 's', 't', 'e', 'm'};
  const auto status = darwin_art_bionic_fs_process_install(
      fd, kMount, sizeof(kMount), kMount, sizeof(kMount));
  close(fd);
  std::cerr << "ART Android filesystem: test root status="
            << static_cast<int>(status) << " root=" << root << "\n";
  return status == DARWIN_ART_BIONIC_FS_PROCESS_OWNER_OK ||
         status == DARWIN_ART_BIONIC_FS_PROCESS_OWNER_ALREADY_INSTALLED;
}
