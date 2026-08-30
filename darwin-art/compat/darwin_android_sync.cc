#include "darwin_art_bionic_socket_broker.h"

#include <cerrno>

// libsync waits on a Linux sync_file with poll(POLLIN).  Darwin ART fence
// descriptors live in the central bionic FD broker, so waiting through the
// broker preserves Android descriptor identity across dup/SCM_RIGHTS and
// works for Metal shared-event-backed descriptors before and after they are
// signaled.
extern "C" int sync_wait(int fd, int timeout_ms) {
  if (fd < 0 || timeout_ms < -1) {
    errno = EINVAL;
    return -1;
  }
  DarwinArtBionicPollFd descriptor{fd, 0x0001, 0};  // POLLIN
  const int ready =
      darwin_art_bionic_socket_broker_poll(&descriptor, 1, timeout_ms);
  if (ready < 0) return -1;
  if (ready == 0) {
    errno = ETIMEDOUT;
    return -1;
  }
  constexpr int16_t kPollError = 0x0008;
  constexpr int16_t kPollHangup = 0x0010;
  constexpr int16_t kPollInvalid = 0x0020;
  if ((descriptor.revents & kPollInvalid) != 0) {
    errno = EINVAL;
    return -1;
  }
  if ((descriptor.revents & (0x0001 | kPollError | kPollHangup)) != 0) {
    return 0;
  }
  errno = EIO;
  return -1;
}
