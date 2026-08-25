#include "darwin_art_bionic_process_state.h"

#include <errno.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int darwin_art_bionic_setjmp(void* environment);
extern void darwin_art_bionic_longjmp(void* environment, int value);
extern void darwin_art_bionic_errno_store(int32_t android_errno);

char* darwin_art_bionic_getenv(const char* name) {
  const int saved_host_errno = errno;
  char* result = darwin_art_bionic_process_getenv_core(name);
  errno = saved_host_errno;
  return result;
}

int darwin_art_bionic___system_property_get(const char* name, char* value) {
  const int saved_host_errno = errno;
  const int result = darwin_art_bionic_process_property_get_core(name, value);
  errno = saved_host_errno;
  return result;
}

unsigned long darwin_art_bionic_getauxval(unsigned long type) {
  const int saved_host_errno = errno;
  const unsigned long result = darwin_art_bionic_process_getauxval_core(type);
  errno = saved_host_errno;
  return result;
}

static _Atomic uint64_t gRandState = UINT64_C(1);

void darwin_art_bionic_srand(unsigned seed) {
  atomic_store_explicit(&gRandState, (uint64_t)seed, memory_order_release);
}

int darwin_art_bionic_rand(void) {
  uint64_t observed = atomic_load_explicit(&gRandState, memory_order_acquire);
  uint64_t next;
  do {
    next = observed * UINT64_C(6364136223846793005) + UINT64_C(1);
  } while (!atomic_compare_exchange_weak_explicit(
      &gRandState, &observed, next, memory_order_acq_rel, memory_order_acquire));
  return (int)((next >> 33) & UINT64_C(0x7fffffff));
}

int darwin_art_bionic_getpid(void) { return (int)getpid(); }
unsigned darwin_art_bionic_geteuid(void) { return (unsigned)geteuid(); }
int darwin_art_bionic_getpagesize(void) { return 4096; }
int darwin_art_bionic_sched_yield(void) { return sched_yield(); }
int darwin_art_bionic_sched_get_priority_max(int policy) {
  return sched_get_priority_max(policy);
}
int darwin_art_bionic_nice(int increment) { return nice(increment); }
int darwin_art_bionic_kill(int pid, int signal_number) {
  return kill((pid_t)pid, signal_number);
}
void darwin_art_bionic_exit(int status) { exit(status); }
void darwin_art_bionic__exit(int status) { _exit(status); }

int darwin_art_bionic_prctl(int option, uintptr_t arg2, uintptr_t arg3,
                            uintptr_t arg4, uintptr_t arg5) {
  (void)arg3;
  (void)arg4;
  (void)arg5;
  if (option == 15) {
    if (arg2 == 0) return -1;
    return pthread_setname_np((const char*)arg2);
  }
  if (option == 16) {
    if (arg2 == 0) return -1;
    return pthread_getname_np(pthread_self(), (char*)arg2, 16);
  }
  darwin_art_bionic_errno_store(22);
  return -1;
}

typedef struct AndroidSigaction {
  int flags;
  void (*handler)(int);
  uint64_t mask;
  void (*restorer)(void);
} AndroidSigaction;

typedef struct AndroidStack {
  void* pointer;
  int flags;
  size_t size;
} AndroidStack;

static int HostSignal(int android_signal) {
  static const unsigned char map[32] = {
      0, SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGBUS,
      SIGFPE, SIGKILL, SIGUSR1, SIGSEGV, SIGUSR2, SIGPIPE, SIGALRM, SIGTERM,
      0, SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU, SIGURG,
      SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF, SIGWINCH, SIGINFO, SIGUSR1, SIGUSR2};
  return android_signal > 0 && android_signal < 32 ? map[android_signal] : 0;
}

static void MaskToHost(uint64_t android_mask, sigset_t* host) {
  sigemptyset(host);
  for (int signal_number = 1; signal_number < 32; ++signal_number) {
    if ((android_mask & (UINT64_C(1) << (signal_number - 1))) != 0) {
      int translated = HostSignal(signal_number);
      if (translated != 0) sigaddset(host, translated);
    }
  }
}

static uint64_t MaskFromHost(const sigset_t* host) {
  uint64_t result = 0;
  for (int signal_number = 1; signal_number < 32; ++signal_number) {
    int translated = HostSignal(signal_number);
    if (translated != 0 && sigismember(host, translated) == 1)
      result |= UINT64_C(1) << (signal_number - 1);
  }
  return result;
}

int darwin_art_bionic_sigfillset(uint64_t* set) {
  if (set == NULL) return -1;
  *set = UINT64_MAX;
  return 0;
}

int darwin_art_bionic_sigdelset(uint64_t* set, int signal_number) {
  if (set == NULL || signal_number <= 0 || signal_number > 64) return -1;
  *set &= ~(UINT64_C(1) << (signal_number - 1));
  return 0;
}

int darwin_art_bionic_sigaction(int signal_number,
                                const AndroidSigaction* action,
                                AndroidSigaction* old_action) {
  const int host_signal = HostSignal(signal_number);
  if (host_signal == 0) return -1;
  struct sigaction host_action;
  struct sigaction host_old;
  struct sigaction* host_action_pointer = NULL;
  if (action != NULL) {
    memset(&host_action, 0, sizeof(host_action));
    host_action.sa_handler = action->handler;
    MaskToHost(action->mask, &host_action.sa_mask);
    if ((action->flags & 0x08000000) != 0) host_action.sa_flags |= SA_ONSTACK;
    if ((action->flags & 0x10000000) != 0) host_action.sa_flags |= SA_RESTART;
    if ((action->flags & 0x40000000) != 0) host_action.sa_flags |= SA_NODEFER;
    if ((uint32_t)action->flags & UINT32_C(0x80000000)) host_action.sa_flags |= SA_RESETHAND;
    if ((action->flags & 4) != 0) host_action.sa_flags |= SA_SIGINFO;
    host_action_pointer = &host_action;
  }
  if (sigaction(host_signal, host_action_pointer,
                old_action == NULL ? NULL : &host_old) != 0) return -1;
  if (old_action != NULL) {
    memset(old_action, 0, sizeof(*old_action));
    old_action->handler = host_old.sa_handler;
    old_action->mask = MaskFromHost(&host_old.sa_mask);
    if ((host_old.sa_flags & SA_ONSTACK) != 0) old_action->flags |= 0x08000000;
    if ((host_old.sa_flags & SA_RESTART) != 0) old_action->flags |= 0x10000000;
    if ((host_old.sa_flags & SA_NODEFER) != 0) old_action->flags |= 0x40000000;
    if ((host_old.sa_flags & SA_RESETHAND) != 0) old_action->flags |= (int)UINT32_C(0x80000000);
    if ((host_old.sa_flags & SA_SIGINFO) != 0) old_action->flags |= 4;
  }
  return 0;
}

int darwin_art_bionic_sigaltstack(const AndroidStack* stack,
                                  AndroidStack* old_stack) {
  stack_t host_stack;
  stack_t host_old;
  stack_t* host_stack_pointer = NULL;
  if (stack != NULL) {
    host_stack.ss_sp = stack->pointer;
    host_stack.ss_size = stack->size;
    host_stack.ss_flags = stack->flags;
    host_stack_pointer = &host_stack;
  }
  if (sigaltstack(host_stack_pointer, old_stack == NULL ? NULL : &host_old) != 0)
    return -1;
  if (old_stack != NULL) {
    old_stack->pointer = host_old.ss_sp;
    old_stack->flags = host_old.ss_flags;
    old_stack->size = host_old.ss_size;
  }
  return 0;
}

static int NameCompare(const char* left, const char* right) {
  while (*left == *right && *left != '\0') {
    ++left;
    ++right;
  }
  return (unsigned char)*left < (unsigned char)*right
             ? -1
             : ((unsigned char)*left != (unsigned char)*right);
}

typedef struct Binding {
  const char* name;
  DarwinArtBionicProcessFunction address;
} Binding;

static const Binding kBindings[] = {
    {"__system_property_get",
     (DarwinArtBionicProcessFunction)darwin_art_bionic___system_property_get},
    {"_exit", (DarwinArtBionicProcessFunction)darwin_art_bionic__exit},
    {"exit", (DarwinArtBionicProcessFunction)darwin_art_bionic_exit},
    {"getauxval", (DarwinArtBionicProcessFunction)darwin_art_bionic_getauxval},
    {"getenv", (DarwinArtBionicProcessFunction)darwin_art_bionic_getenv},
    {"geteuid", (DarwinArtBionicProcessFunction)darwin_art_bionic_geteuid},
    {"getpagesize", (DarwinArtBionicProcessFunction)darwin_art_bionic_getpagesize},
    {"getpid", (DarwinArtBionicProcessFunction)darwin_art_bionic_getpid},
    {"kill", (DarwinArtBionicProcessFunction)darwin_art_bionic_kill},
    {"longjmp", (DarwinArtBionicProcessFunction)darwin_art_bionic_longjmp},
    {"nice", (DarwinArtBionicProcessFunction)darwin_art_bionic_nice},
    {"prctl", (DarwinArtBionicProcessFunction)darwin_art_bionic_prctl},
    {"rand", (DarwinArtBionicProcessFunction)darwin_art_bionic_rand},
    {"sched_get_priority_max", (DarwinArtBionicProcessFunction)darwin_art_bionic_sched_get_priority_max},
    {"sched_yield", (DarwinArtBionicProcessFunction)darwin_art_bionic_sched_yield},
    {"setjmp", (DarwinArtBionicProcessFunction)darwin_art_bionic_setjmp},
    {"sigaction", (DarwinArtBionicProcessFunction)darwin_art_bionic_sigaction},
    {"sigaltstack", (DarwinArtBionicProcessFunction)darwin_art_bionic_sigaltstack},
    {"sigdelset", (DarwinArtBionicProcessFunction)darwin_art_bionic_sigdelset},
    {"sigfillset", (DarwinArtBionicProcessFunction)darwin_art_bionic_sigfillset},
    {"srand", (DarwinArtBionicProcessFunction)darwin_art_bionic_srand},
};

DarwinArtBionicProcessFunction darwin_art_bionic_process_state_resolve(
    const char* name) {
  if (name == NULL) return NULL;
  size_t low = 0;
  size_t high = sizeof(kBindings) / sizeof(kBindings[0]);
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    const int order = NameCompare(name, kBindings[middle].name);
    if (order == 0) return kBindings[middle].address;
    if (order < 0)
      high = middle;
    else
      low = middle + 1;
  }
  return NULL;
}
