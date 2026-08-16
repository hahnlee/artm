#include "darwin_art_bionic_fd_broker.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kAndroidEbadf = 9;
constexpr uint64_t kBlockedObject = 99;
constexpr uint64_t kBlockedCloseObject = 100;

struct ObjectState {
  std::string input;
  std::string output;
  size_t offset = 0;
  bool closed = false;
};

struct Fake {
  std::mutex mutex;
  std::condition_variable changed;
  std::map<uint64_t, ObjectState> objects;
  std::vector<std::string> events;
  bool blocked_read_entered = false;
  bool release_blocked_read = false;
  std::thread::id blocked_reader;
  bool blocked_close_entered = false;
  bool release_blocked_close = false;
};

intptr_t Read(void *context, uint64_t object, void *bytes, size_t count,
              int *android_errno) {
  Fake *fake = static_cast<Fake *>(context);
  std::unique_lock lock(fake->mutex);
  ObjectState &state = fake->objects.at(object);
  assert(!state.closed);
  fake->events.push_back(std::to_string(object) + ":read-enter");
  if (object == kBlockedObject && !fake->blocked_read_entered) {
    fake->blocked_read_entered = true;
    fake->blocked_reader = std::this_thread::get_id();
    fake->changed.notify_all();
    fake->changed.wait(lock, [&] { return fake->release_blocked_read; });
  } else if (object == kBlockedObject &&
             fake->blocked_reader != std::this_thread::get_id()) {
    *android_errno = 11;
    return -1;
  }
  const size_t available = state.input.size() - state.offset;
  const size_t copied = std::min(count, available);
  if (copied != 0) {
    std::memcpy(bytes, state.input.data() + state.offset, copied);
    state.offset += copied;
  }
  fake->events.push_back(std::to_string(object) + ":read-exit");
  *android_errno = 0;
  return static_cast<intptr_t>(copied);
}

intptr_t Write(void *context, uint64_t object, const void *bytes, size_t count,
               int *android_errno) {
  Fake *fake = static_cast<Fake *>(context);
  std::lock_guard lock(fake->mutex);
  ObjectState &state = fake->objects.at(object);
  assert(!state.closed);
  state.output.append(static_cast<const char *>(bytes), count);
  fake->events.push_back(std::to_string(object) + ":write");
  *android_errno = 0;
  return static_cast<intptr_t>(count);
}

int Poll(void *context, uint64_t object, int16_t events, int16_t *revents,
         int *android_errno) {
  Fake *fake = static_cast<Fake *>(context);
  std::lock_guard lock(fake->mutex);
  assert(!fake->objects.at(object).closed);
  *revents = events;
  *android_errno = 0;
  fake->events.push_back(std::to_string(object) + ":poll");
  return events == 0 ? 0 : 1;
}

int Ioctl(void *context, uint64_t object, uint64_t request, void *argument,
          int *android_errno) {
  Fake *fake = static_cast<Fake *>(context);
  std::lock_guard lock(fake->mutex);
  assert(!fake->objects.at(object).closed);
  if (request != 0x80045200 || argument == nullptr) {
    *android_errno = 25;
    return -1;
  }
  *static_cast<int *>(argument) = 32;
  fake->events.push_back(std::to_string(object) + ":ioctl");
  *android_errno = 0;
  return 0;
}

int Close(void *context, uint64_t object, int *android_errno) {
  Fake *fake = static_cast<Fake *>(context);
  std::unique_lock lock(fake->mutex);
  ObjectState &state = fake->objects.at(object);
  assert(!state.closed);
  if (object == kBlockedCloseObject) {
    fake->blocked_close_entered = true;
    fake->events.push_back(std::to_string(object) + ":close-enter");
    fake->changed.notify_all();
    fake->changed.wait(lock, [&] { return fake->release_blocked_close; });
  }
  state.closed = true;
  fake->events.push_back(std::to_string(object) + ":close");
  *android_errno = 0;
  return 0;
}

DarwinArtFdOwnerV1 Callbacks(Fake *fake) {
  return DarwinArtFdOwnerV1{DARWIN_ART_FD_OWNER_ABI_V1,
                            sizeof(DarwinArtFdOwnerV1),
                            fake,
                            Read,
                            Write,
                            Poll,
                            Ioctl,
                            Close};
}

size_t FindEvent(const std::vector<std::string> &events,
                 const std::string &wanted) {
  for (size_t index = 0; index < events.size(); ++index) {
    if (events[index] == wanted) {
      return index;
    }
  }
  assert(false && "event missing");
  return events.size();
}

void Expect(DarwinArtFdBrokerStatus actual, DarwinArtFdBrokerStatus expected) {
  assert(actual == expected);
}

} // namespace

int main() {
  Fake fake;
  fake.objects.emplace(10, ObjectState{"hello", "", 0, false});
  fake.objects.emplace(20, ObjectState{});
  fake.objects.emplace(30, ObjectState{});
  fake.objects.emplace(40, ObjectState{});
  fake.objects.emplace(50, ObjectState{"other", "", 0, false});
  fake.objects.emplace(kBlockedObject, ObjectState{"blocked", "", 0, false});
  fake.objects.emplace(100, ObjectState{});

  DarwinArtFdBroker *broker = darwin_art_fd_broker_create();
  assert(broker != nullptr);
  DarwinArtFdOwnerV1 callbacks = Callbacks(&fake);
  DarwinArtFdOwnerHandle file_owner = 0;
  DarwinArtFdOwnerHandle random_owner = 0;
  DarwinArtFdOwnerHandle stdio_owner = 0;
  DarwinArtFdOwnerHandle socket_owner = 0;
  DarwinArtFdOwnerHandle second_file_owner = 0;
  Expect(darwin_art_fd_broker_install_owner(broker, DARWIN_ART_FD_FS_FILE,
                                            &callbacks, &file_owner),
         DARWIN_ART_FD_BROKER_OK);
  Expect(darwin_art_fd_broker_install_owner(broker, DARWIN_ART_FD_FS_RANDOM,
                                            &callbacks, &random_owner),
         DARWIN_ART_FD_BROKER_OK);
  Expect(darwin_art_fd_broker_install_owner(broker, DARWIN_ART_FD_STDIO,
                                            &callbacks, &stdio_owner),
         DARWIN_ART_FD_BROKER_OK);
  Expect(darwin_art_fd_broker_install_owner(broker, DARWIN_ART_FD_SOCKET,
                                            &callbacks, &socket_owner),
         DARWIN_ART_FD_BROKER_OK);
  Expect(darwin_art_fd_broker_install_owner(broker, DARWIN_ART_FD_FS_FILE,
                                            &callbacks, &second_file_owner),
         DARWIN_ART_FD_BROKER_OK);

  int file_fd = -1;
  int random_fd = -1;
  int stdio_fd = -1;
  int socket_fd = -1;
  int second_file_fd = -1;
  Expect(darwin_art_fd_broker_publish(broker, file_owner, 10, &file_fd),
         DARWIN_ART_FD_BROKER_OK);
  Expect(darwin_art_fd_broker_publish(broker, random_owner, 20, &random_fd),
         DARWIN_ART_FD_BROKER_OK);
  Expect(darwin_art_fd_broker_publish(broker, stdio_owner, 30, &stdio_fd),
         DARWIN_ART_FD_BROKER_OK);
  Expect(darwin_art_fd_broker_publish(broker, socket_owner, 40, &socket_fd),
         DARWIN_ART_FD_BROKER_OK);
  Expect(darwin_art_fd_broker_publish(broker, second_file_owner, 50,
                                      &second_file_fd),
         DARWIN_ART_FD_BROKER_OK);
  assert(file_fd >= 0x40000000 && random_fd != file_fd && stdio_fd != file_fd &&
         socket_fd != file_fd && second_file_fd != file_fd);

  DarwinArtFdIoResult result{};
  Expect(
      darwin_art_fd_broker_close_owned(broker, file_owner, socket_fd, &result),
      DARWIN_ART_FD_BROKER_WRONG_OWNER);
  int entropy = 0;
  Expect(darwin_art_fd_broker_ioctl(broker, socket_fd, DARWIN_ART_FD_FS_RANDOM,
                                    0x80045200, &entropy, &result),
         DARWIN_ART_FD_BROKER_WRONG_KIND);
  Expect(darwin_art_fd_broker_ioctl(broker, random_fd, DARWIN_ART_FD_FS_RANDOM,
                                    0x80045200, &entropy, &result),
         DARWIN_ART_FD_BROKER_OK);
  assert(result.value == 0 && entropy == 32);

  Expect(darwin_art_fd_broker_sendfile(broker, socket_fd, file_fd, 5, &result),
         DARWIN_ART_FD_BROKER_OK);
  assert(result.value == 5);
  {
    std::lock_guard lock(fake.mutex);
    assert(fake.objects.at(40).output == "hello");
  }
  DarwinArtFdPollEntry poll_entries[] = {
      DarwinArtFdPollEntry{socket_fd, 1, 0},
      DarwinArtFdPollEntry{-1, 1, 7},
      DarwinArtFdPollEntry{123, 1, 0},
  };
  Expect(darwin_art_fd_broker_poll(broker, poll_entries, 3, &result),
         DARWIN_ART_FD_BROKER_OK);
  assert(result.value == 2 && poll_entries[0].revents == 1 &&
         poll_entries[1].revents == 0 && poll_entries[2].revents == 0x20);

  const int stale_file_fd = second_file_fd;
  Expect(darwin_art_fd_broker_close(broker, second_file_fd, &result),
         DARWIN_ART_FD_BROKER_OK);
  assert(result.value == 0);
  Expect(darwin_art_fd_broker_close(broker, stale_file_fd, &result),
         DARWIN_ART_FD_BROKER_STALE);
  Expect(darwin_art_fd_broker_read(broker, stale_file_fd, nullptr, 0, &result),
         DARWIN_ART_FD_BROKER_STALE);
  int reused_fd = -1;
  Expect(
      darwin_art_fd_broker_publish(broker, second_file_owner, 100, &reused_fd),
      DARWIN_ART_FD_BROKER_OK);
  assert(reused_fd != stale_file_fd);
  assert((static_cast<uint32_t>(reused_fd) & 0x3ffU) ==
         (static_cast<uint32_t>(stale_file_fd) & 0x3ffU));

  int blocked_fd = -1;
  Expect(darwin_art_fd_broker_publish(broker, file_owner, kBlockedObject,
                                      &blocked_fd),
         DARWIN_ART_FD_BROKER_OK);
  std::atomic<bool> read_done{false};
  std::atomic<bool> close_done{false};
  std::thread reader([&] {
    char bytes[7]{};
    DarwinArtFdIoResult read_result{};
    Expect(darwin_art_fd_broker_read(broker, blocked_fd, bytes, sizeof(bytes),
                                     &read_result),
           DARWIN_ART_FD_BROKER_OK);
    assert(read_result.value == 7 && std::memcmp(bytes, "blocked", 7) == 0);
    read_done = true;
  });
  {
    std::unique_lock lock(fake.mutex);
    fake.changed.wait(lock, [&] { return fake.blocked_read_entered; });
  }
  std::thread closer([&] {
    DarwinArtFdIoResult close_result{};
    Expect(darwin_art_fd_broker_close(broker, blocked_fd, &close_result),
           DARWIN_ART_FD_BROKER_OK);
    close_done = true;
  });
  DarwinArtFdBrokerStatus during_close = DARWIN_ART_FD_BROKER_OK;
  for (size_t attempt = 0; attempt < 100000; ++attempt) {
    char byte = 0;
    DarwinArtFdIoResult one{};
    during_close =
        darwin_art_fd_broker_read(broker, blocked_fd, &byte, 1, &one);
    if (during_close == DARWIN_ART_FD_BROKER_STALE) {
      break;
    }
    std::this_thread::yield();
  }
  assert(during_close == DARWIN_ART_FD_BROKER_STALE && !close_done.load());
  {
    std::lock_guard lock(fake.mutex);
    fake.release_blocked_read = true;
    fake.changed.notify_all();
  }
  reader.join();
  closer.join();
  assert(read_done.load() && close_done.load());
  {
    std::lock_guard lock(fake.mutex);
    const size_t read_exit = FindEvent(fake.events, "99:read-exit");
    const size_t close = FindEvent(fake.events, "99:close");
    assert(read_exit < close);
  }

  Expect(darwin_art_fd_broker_uninstall_owner(broker, socket_owner),
         DARWIN_ART_FD_BROKER_BUSY);
  int rejected_fd = -1;
  Expect(darwin_art_fd_broker_publish(broker, socket_owner, 40, &rejected_fd),
         DARWIN_ART_FD_BROKER_DRAINING);
  Expect(darwin_art_fd_broker_close(broker, socket_fd, &result),
         DARWIN_ART_FD_BROKER_OK);
  Expect(darwin_art_fd_broker_uninstall_owner(broker, socket_owner),
         DARWIN_ART_FD_BROKER_OK);
  Expect(darwin_art_fd_broker_publish(broker, socket_owner, 40, &rejected_fd),
         DARWIN_ART_FD_BROKER_STALE);

  std::atomic<bool> owner_close_done{false};
  std::thread owner_closer([&] {
    DarwinArtFdIoResult close_result{};
    Expect(darwin_art_fd_broker_close_owned(broker, second_file_owner,
                                            reused_fd, &close_result),
           DARWIN_ART_FD_BROKER_OK);
    owner_close_done = true;
  });
  {
    std::unique_lock lock(fake.mutex);
    fake.changed.wait(lock, [&] { return fake.blocked_close_entered; });
  }
  Expect(darwin_art_fd_broker_uninstall_owner(broker, second_file_owner),
         DARWIN_ART_FD_BROKER_BUSY);
  Expect(
      darwin_art_fd_broker_publish(broker, second_file_owner, 50, &rejected_fd),
      DARWIN_ART_FD_BROKER_DRAINING);
  assert(!owner_close_done.load());
  {
    std::lock_guard lock(fake.mutex);
    fake.release_blocked_close = true;
    fake.changed.notify_all();
  }
  owner_closer.join();
  Expect(darwin_art_fd_broker_uninstall_owner(broker, second_file_owner),
         DARWIN_ART_FD_BROKER_OK);

  assert(darwin_art_fd_broker_destroy(broker) == DARWIN_ART_FD_BROKER_BUSY);
  const std::pair<DarwinArtFdOwnerHandle, int> remaining[] = {
      {file_owner, file_fd},
      {random_owner, random_fd},
      {stdio_owner, stdio_fd},
  };
  for (const auto &[owner, fd] : remaining) {
    Expect(darwin_art_fd_broker_close_owned(broker, owner, fd, &result),
           DARWIN_ART_FD_BROKER_OK);
    Expect(darwin_art_fd_broker_uninstall_owner(broker, owner),
           DARWIN_ART_FD_BROKER_OK);
  }
  Expect(darwin_art_fd_broker_destroy(broker), DARWIN_ART_FD_BROKER_OK);

  std::cout << "bionic-central-fd-broker: PASS kinds=4 owners=5 "
               "namespace=collision-free foreign=reject\n";
  std::cout << "bionic-central-fd-broker: PASS generation=reuse-safe "
               "stale=reject double-close=reject\n";
  std::cout << "bionic-central-fd-broker: PASS close-vs-use=drained "
               "poll+sendfile+ioctl=typed uninstall=quiescent\n";
  return kAndroidEbadf == 9 ? 0 : 1;
}
