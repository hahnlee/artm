#include "darwin_art_bionic_abort.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern int darwin_art_bionic_errno_set_from_darwin(int darwin_errno);

_Static_assert(SIGABRT == 6,
               "Darwin SIGABRT must equal pinned Android arm64 SIGABRT");
_Static_assert(sizeof(size_t) == 8, "Android arm64 size_t width drift");

typedef struct MagicAbortMessage {
  uint64_t magic1;
  uint64_t magic2;
  size_t size;
  char message[];
} MagicAbortMessage;

_Static_assert(offsetof(DarwinArtBionicAbortMessage, message) == sizeof(size_t),
               "abort message layout drift");
_Static_assert(offsetof(MagicAbortMessage, size) == 2 * sizeof(uint64_t),
               "magic abort message layout drift");
_Static_assert(sizeof(MagicAbortMessage) == 24,
               "magic abort message size drift");

static pthread_mutex_t gAbortMessageLock = PTHREAD_MUTEX_INITIALIZER;
static DarwinArtBionicAbortMessage* gAbortMessage;

__attribute__((optnone)) static void FillAbortMessageMagic(
    MagicAbortMessage* allocation) {
  allocation->magic1 = UINT64_C(0xb18e40886ac388f0);
  allocation->magic2 = UINT64_C(0xc6dfba755a1de0b5);
}

__attribute__((noreturn)) void darwin_art_bionic_abort(void) {
  sigset_t mask;
  sigfillset(&mask);
  sigdelset(&mask, SIGABRT);

  (void)pthread_sigmask(SIG_SETMASK, &mask, NULL);
  (void)pthread_kill(pthread_self(), SIGABRT);

  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_DFL;
  action.sa_flags = SA_RESTART;
  sigemptyset(&action.sa_mask);
  (void)sigaction(SIGABRT, &action, NULL);

  (void)pthread_sigmask(SIG_SETMASK, &mask, NULL);
  (void)pthread_kill(pthread_self(), SIGABRT);
  _exit(127);
}

void darwin_art_bionic_android_set_abort_message(const char* message) {
  const int saved_errno = errno;
  (void)pthread_mutex_lock(&gAbortMessageLock);
  if (gAbortMessage != NULL) {
    (void)pthread_mutex_unlock(&gAbortMessageLock);
    errno = saved_errno;
    return;
  }
  if (message == NULL) message = "(null)";

  const size_t size = sizeof(MagicAbortMessage) + strlen(message) + 1;
  void* mapping = mmap(NULL, size, PROT_READ | PROT_WRITE,
                       MAP_ANON | MAP_PRIVATE, -1, 0);
  if (mapping == MAP_FAILED) {
    const int map_errno = errno;
    (void)darwin_art_bionic_errno_set_from_darwin(map_errno);
    (void)pthread_mutex_unlock(&gAbortMessageLock);
    errno = saved_errno;
    return;
  }

  MagicAbortMessage* allocation = (MagicAbortMessage*)mapping;
  FillAbortMessageMagic(allocation);
  allocation->size = size;
  memcpy(allocation->message, message, strlen(message) + 1);
  gAbortMessage = (DarwinArtBionicAbortMessage*)&allocation->size;
  (void)pthread_mutex_unlock(&gAbortMessageLock);
  errno = saved_errno;
}

const DarwinArtBionicAbortMessage*
darwin_art_bionic_abort_message_for_test(void) {
  (void)pthread_mutex_lock(&gAbortMessageLock);
  const DarwinArtBionicAbortMessage* result = gAbortMessage;
  (void)pthread_mutex_unlock(&gAbortMessageLock);
  return result;
}

void* darwin_art_bionic_abort_resolve(const char* soname,
                                      const char* symbol,
                                      const char* version) {
  if (soname == NULL || symbol == NULL || version == NULL ||
      strcmp(soname, "libc.so") != 0 || strcmp(version, "LIBC") != 0)
    return NULL;
  if (strcmp(symbol, "abort") == 0)
    return (void*)(uintptr_t)&darwin_art_bionic_abort;
  if (strcmp(symbol, "android_set_abort_message") == 0)
    return (void*)(uintptr_t)&darwin_art_bionic_android_set_abort_message;
  return NULL;
}

int darwin_art_bionic_abort_capability(const char* capability) {
  if (capability == NULL) return 0;
  return strcmp(capability, "SIGABRT=6-raw-forwarding") == 0 ||
         strcmp(capability, "abort-message-first-wins") == 0 ||
         strcmp(capability, "abort-message-process-lifetime") == 0 ||
         strcmp(capability, "abort-message-magic-layout") == 0;
}
