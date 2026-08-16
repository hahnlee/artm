#define _GNU_SOURCE 1
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

__attribute__((visibility("default"))) int
central_fd_dup_epoll_fixture(int input_fd, int target_fd, int socket_fd) {
  int first = dup(input_fd);
  if (first < 0)
    return 10;
  int second = dup3(input_fd, target_fd, O_CLOEXEC);
  if (second < 0)
    return 11;
  int third = fcntl(input_fd, F_DUPFD_CLOEXEC, 64);
  if (third < 0)
    return 12;
  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0)
    return 13;
  struct epoll_event registration = {.events = EPOLLIN, .data.u64 = 0x1234};
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &registration) != 0)
    return 14;
  struct epoll_event ready[2];
  int count = epoll_wait(epoll_fd, ready, 2, 0);
  return close(first) | close(second) | close(third) | close(epoll_fd) |
         (count < 0 ? 1 : 0);
}
