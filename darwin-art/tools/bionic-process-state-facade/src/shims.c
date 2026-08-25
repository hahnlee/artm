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
#include <sys/resource.h>
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

long darwin_art_bionic_random(void) { return darwin_art_bionic_rand(); }
void darwin_art_bionic_srandom(unsigned seed) { darwin_art_bionic_srand(seed); }

int darwin_art_bionic_rand_r(unsigned* seed) {
  if (seed == NULL) return 0;
  *seed = *seed * UINT32_C(1103515245) + UINT32_C(12345);
  return (int)((*seed >> 1) & UINT32_C(0x7fffffff));
}

static uint64_t Advance48(unsigned short state[3]) {
  uint64_t value = (uint64_t)state[0] | ((uint64_t)state[1] << 16) |
                   ((uint64_t)state[2] << 32);
  value = (value * UINT64_C(0x5deece66d) + UINT64_C(0xb)) &
          UINT64_C(0xffffffffffff);
  state[0] = (unsigned short)value;
  state[1] = (unsigned short)(value >> 16);
  state[2] = (unsigned short)(value >> 32);
  return value;
}

double darwin_art_bionic_erand48(unsigned short state[3]) {
  return (double)Advance48(state) / 281474976710656.0;
}

long darwin_art_bionic_nrand48(unsigned short state[3]) {
  return (long)(Advance48(state) >> 17);
}

long darwin_art_bionic_jrand48(unsigned short state[3]) {
  return (long)(int32_t)(Advance48(state) >> 16);
}

uint32_t darwin_art_bionic_arc4random(void) {
  const uint32_t high = (uint32_t)darwin_art_bionic_rand();
  const uint32_t low = (uint32_t)darwin_art_bionic_rand();
  return (high << 1) ^ low;
}

int darwin_art_bionic_getpid(void) { return (int)getpid(); }
unsigned darwin_art_bionic_geteuid(void) { return (unsigned)geteuid(); }
int darwin_art_bionic_getpagesize(void) { return 4096; }
int darwin_art_bionic_daemon(int nochdir, int noclose) {
  (void)nochdir;
  (void)noclose;
  darwin_art_bionic_errno_store(38);
  return -1;
}
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

static int AndroidSignal(int host_signal) {
  for (int signal_number = 1; signal_number < 32; ++signal_number)
    if (HostSignal(signal_number) == host_signal) return signal_number;
  return 0;
}

int darwin_art_bionic_sigemptyset(uint64_t* set) {
  if (set == NULL) return -1;
  *set = 0;
  return 0;
}

int darwin_art_bionic_sigaddset(uint64_t* set, int signal_number) {
  if (set == NULL || signal_number <= 0 || signal_number > 64) return -1;
  *set |= UINT64_C(1) << (signal_number - 1);
  return 0;
}

int darwin_art_bionic_sigismember(const uint64_t* set, int signal_number) {
  if (set == NULL || signal_number <= 0 || signal_number > 64) return -1;
  return (*set & (UINT64_C(1) << (signal_number - 1))) != 0;
}

int darwin_art_bionic_sigpending(uint64_t* set) {
  if (set == NULL) return -1;
  sigset_t host;
  if (sigpending(&host) != 0) return -1;
  *set = MaskFromHost(&host);
  return 0;
}

int darwin_art_bionic_sigwait(const uint64_t* set, int* signal_number) {
  if (set == NULL || signal_number == NULL) return 22;
  sigset_t host;
  MaskToHost(*set, &host);
  int delivered = 0;
  const int result = sigwait(&host, &delivered);
  if (result == 0) *signal_number = AndroidSignal(delivered);
  return result;
}

int darwin_art_bionic_raise(int signal_number) {
  const int translated = HostSignal(signal_number);
  if (translated == 0) return -1;
  return raise(translated);
}

void (*darwin_art_bionic_signal(int signal_number, void (*handler)(int)))(int) {
  const int translated = HostSignal(signal_number);
  if (translated == 0) return SIG_ERR;
  return signal(translated, handler);
}

unsigned darwin_art_bionic_getuid(void) { return (unsigned)getuid(); }
unsigned darwin_art_bionic_getgid(void) { return (unsigned)getgid(); }
unsigned darwin_art_bionic_getegid(void) { return (unsigned)getegid(); }
int darwin_art_bionic_setuid(unsigned uid) { return setuid((uid_t)uid); }
int darwin_art_bionic_gethostname(char* name, size_t length) {
  return gethostname(name, length);
}

typedef struct AndroidUtsname {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
} AndroidUtsname;

int darwin_art_bionic_uname(AndroidUtsname* value) {
  if (value == NULL) return -1;
  memset(value, 0, sizeof(*value));
  strcpy(value->sysname, "Linux");
  if (gethostname(value->nodename, sizeof(value->nodename)) != 0)
    strcpy(value->nodename, "darwin-art");
  strcpy(value->release, "6.12.0-darwin-art");
  strcpy(value->version, "Darwin ART Android compatibility layer");
  strcpy(value->machine, "aarch64");
  return 0;
}

int darwin_art_bionic_process_unsupported(void) {
  darwin_art_bionic_errno_store(38);
  return -1;
}

int darwin_art_bionic__setjmp(void* environment) {
  return darwin_art_bionic_setjmp(environment);
}

int darwin_art_bionic_sigsetjmp(void* environment, int save_mask) {
  (void)save_mask;
  return darwin_art_bionic_setjmp(environment);
}

void darwin_art_bionic__longjmp(void* environment, int value) {
  darwin_art_bionic_longjmp(environment, value);
}

void darwin_art_bionic_siglongjmp(void* environment, int value) {
  darwin_art_bionic_longjmp(environment, value);
}

int darwin_art_bionic_sched_get_priority_min(int policy) {
  return sched_get_priority_min(policy);
}

int darwin_art_bionic_sched_getparam(int pid, struct sched_param* param) {
  (void)pid;
  (void)param;
  darwin_art_bionic_errno_store(38);
  return -1;
}

int darwin_art_bionic_sched_setscheduler(int pid, int policy,
                                         const struct sched_param* param) {
  (void)pid;
  (void)policy;
  (void)param;
  darwin_art_bionic_errno_store(38);
  return -1;
}

int darwin_art_bionic_getpwuid_r_unsupported(
    unsigned uid, void* password, char* buffer, size_t capacity,
    void** result) {
  (void)uid;
  (void)password;
  (void)buffer;
  (void)capacity;
  if (result != NULL) *result = NULL;
  return 2;
}

void* darwin_art_bionic_getservbyport_unsupported(int port,
                                                  const char* protocol) {
  (void)port;
  (void)protocol;
  return NULL;
}

int darwin_art_bionic_setenv(const char* name, const char* value,
                             int overwrite) {
  return setenv(name, value, overwrite);
}

int darwin_art_bionic_getrusage(int who, struct rusage* usage) {
  return getrusage(who, usage);
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
    {"_longjmp", (DarwinArtBionicProcessFunction)darwin_art_bionic__longjmp},
    {"_setjmp", (DarwinArtBionicProcessFunction)darwin_art_bionic__setjmp},
    {"arc4random", (DarwinArtBionicProcessFunction)darwin_art_bionic_arc4random},
    {"daemon", (DarwinArtBionicProcessFunction)darwin_art_bionic_daemon},
    {"erand48", (DarwinArtBionicProcessFunction)darwin_art_bionic_erand48},
    {"execlp", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
    {"execvp", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
    {"exit", (DarwinArtBionicProcessFunction)darwin_art_bionic_exit},
    {"fork", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
    {"getauxval", (DarwinArtBionicProcessFunction)darwin_art_bionic_getauxval},
    {"getegid", (DarwinArtBionicProcessFunction)darwin_art_bionic_getegid},
    {"getenv", (DarwinArtBionicProcessFunction)darwin_art_bionic_getenv},
    {"geteuid", (DarwinArtBionicProcessFunction)darwin_art_bionic_geteuid},
    {"getgid", (DarwinArtBionicProcessFunction)darwin_art_bionic_getgid},
    {"gethostname", (DarwinArtBionicProcessFunction)darwin_art_bionic_gethostname},
    {"getpagesize", (DarwinArtBionicProcessFunction)darwin_art_bionic_getpagesize},
    {"getpid", (DarwinArtBionicProcessFunction)darwin_art_bionic_getpid},
    {"getpwuid_r", (DarwinArtBionicProcessFunction)darwin_art_bionic_getpwuid_r_unsupported},
    {"getrusage", (DarwinArtBionicProcessFunction)darwin_art_bionic_getrusage},
    {"getservbyport", (DarwinArtBionicProcessFunction)darwin_art_bionic_getservbyport_unsupported},
    {"getuid", (DarwinArtBionicProcessFunction)darwin_art_bionic_getuid},
    {"jrand48", (DarwinArtBionicProcessFunction)darwin_art_bionic_jrand48},
    {"kill", (DarwinArtBionicProcessFunction)darwin_art_bionic_kill},
    {"longjmp", (DarwinArtBionicProcessFunction)darwin_art_bionic_longjmp},
    {"nice", (DarwinArtBionicProcessFunction)darwin_art_bionic_nice},
    {"nrand48", (DarwinArtBionicProcessFunction)darwin_art_bionic_nrand48},
    {"prctl", (DarwinArtBionicProcessFunction)darwin_art_bionic_prctl},
    {"raise", (DarwinArtBionicProcessFunction)darwin_art_bionic_raise},
    {"rand", (DarwinArtBionicProcessFunction)darwin_art_bionic_rand},
    {"rand_r", (DarwinArtBionicProcessFunction)darwin_art_bionic_rand_r},
    {"random", (DarwinArtBionicProcessFunction)darwin_art_bionic_random},
    {"sched_get_priority_max", (DarwinArtBionicProcessFunction)darwin_art_bionic_sched_get_priority_max},
    {"sched_get_priority_min", (DarwinArtBionicProcessFunction)darwin_art_bionic_sched_get_priority_min},
    {"sched_getparam", (DarwinArtBionicProcessFunction)darwin_art_bionic_sched_getparam},
    {"sched_setscheduler", (DarwinArtBionicProcessFunction)darwin_art_bionic_sched_setscheduler},
    {"sched_yield", (DarwinArtBionicProcessFunction)darwin_art_bionic_sched_yield},
    {"setenv", (DarwinArtBionicProcessFunction)darwin_art_bionic_setenv},
    {"setjmp", (DarwinArtBionicProcessFunction)darwin_art_bionic_setjmp},
    {"setuid", (DarwinArtBionicProcessFunction)darwin_art_bionic_setuid},
    {"sigaction", (DarwinArtBionicProcessFunction)darwin_art_bionic_sigaction},
    {"sigaddset", (DarwinArtBionicProcessFunction)darwin_art_bionic_sigaddset},
    {"sigaltstack", (DarwinArtBionicProcessFunction)darwin_art_bionic_sigaltstack},
    {"sigdelset", (DarwinArtBionicProcessFunction)darwin_art_bionic_sigdelset},
    {"sigemptyset", (DarwinArtBionicProcessFunction)darwin_art_bionic_sigemptyset},
    {"sigfillset", (DarwinArtBionicProcessFunction)darwin_art_bionic_sigfillset},
    {"sigismember", (DarwinArtBionicProcessFunction)darwin_art_bionic_sigismember},
    {"siglongjmp", (DarwinArtBionicProcessFunction)darwin_art_bionic_siglongjmp},
    {"signal", (DarwinArtBionicProcessFunction)darwin_art_bionic_signal},
    {"sigpending", (DarwinArtBionicProcessFunction)darwin_art_bionic_sigpending},
    {"sigsetjmp", (DarwinArtBionicProcessFunction)darwin_art_bionic_sigsetjmp},
    {"sigwait", (DarwinArtBionicProcessFunction)darwin_art_bionic_sigwait},
    {"srand", (DarwinArtBionicProcessFunction)darwin_art_bionic_srand},
    {"srandom", (DarwinArtBionicProcessFunction)darwin_art_bionic_srandom},
    {"system", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
    {"uname", (DarwinArtBionicProcessFunction)darwin_art_bionic_uname},
    {"vfork", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
    {"vmsplice", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
    {"waitpid", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
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
