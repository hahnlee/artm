#include "darwin_art_bionic_process_state.h"

#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern int darwin_art_bionic_setjmp(void* environment);
extern void darwin_art_bionic_longjmp(void* environment, int value);
extern void darwin_art_bionic_errno_store(int32_t android_errno);
static int HostSignal(int android_signal);

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

const void* darwin_art_bionic___system_property_find(const char* name) {
  const int saved_host_errno = errno;
  const void* result = darwin_art_bionic_process_property_find_core(name);
  errno = saved_host_errno;
  return result;
}

void darwin_art_bionic___system_property_read_callback(
    const void* property,
    void (*callback)(void*, const char*, const char*, uint32_t), void* cookie) {
  const int saved_host_errno = errno;
  darwin_art_bionic_process_property_read_callback_core(property, callback,
                                                        cookie);
  errno = saved_host_errno;
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

void darwin_art_bionic_arc4random_buf(void* output, size_t length) {
  unsigned char* bytes = (unsigned char*)output;
  while (length != 0) {
    const uint32_t value = darwin_art_bionic_arc4random();
    const size_t count = length < sizeof(value) ? length : sizeof(value);
    for (size_t index = 0; index < count; ++index)
      bytes[index] = (unsigned char)(value >> (index * 8));
    bytes += count;
    length -= count;
  }
}

long darwin_art_bionic_getrandom(void* output, size_t length, unsigned flags) {
  (void)flags;
  if (output == NULL && length != 0) {
    darwin_art_bionic_errno_store(14);
    return -1;
  }
  darwin_art_bionic_arc4random_buf(output, length);
  return (long)length;
}

int darwin_art_bionic_android_get_device_api_level(void) { return 36; }

int darwin_art_bionic_getpid(void) { return (int)getpid(); }
int darwin_art_bionic_getppid(void) { return 1; }
const char* darwin_art_bionic_getprogname(void) { return "chrome"; }
unsigned darwin_art_bionic_geteuid(void) { return (unsigned)geteuid(); }
int darwin_art_bionic_getpagesize(void) { return getpagesize(); }
int darwin_art_bionic_daemon(int nochdir, int noclose) {
  (void)nochdir;
  (void)noclose;
  darwin_art_bionic_errno_store(38);
  return -1;
}

int darwin_art_bionic_posix_spawn(int* process_id, const char* path,
                                  const void* file_actions,
                                  const void* attributes, char* const argv[],
                                  char* const environment[]) {
  (void)process_id;
  (void)path;
  (void)file_actions;
  (void)attributes;
  (void)argv;
  (void)environment;
  // POSIX spawn reports its error number directly instead of using errno.
  // Android child processes are created through ActivityManager/zygote; a
  // native exec of an Android ELF image is not a valid Darwin operation.
  return 38;
}
int darwin_art_bionic_sched_yield(void) { return sched_yield(); }
int darwin_art_bionic_sched_get_priority_max(int policy) {
  return sched_get_priority_max(policy);
}
int darwin_art_bionic_nice(int increment) { return nice(increment); }
int darwin_art_bionic_setpriority(int which, unsigned who, int priority) {
  // Android uses per-thread PRIO_PROCESS nice values as a scheduling hint.
  // Guest thread identifiers do not name Darwin processes, so forwarding this
  // call to Darwin could reprioritize an unrelated host process.  Keep the
  // mutation inside the virtual Android process and acknowledge Linux's
  // supported nice range.  The process broker can attach these hints to guest
  // threads when it grows a scheduler policy implementation.
  if (which != PRIO_PROCESS || priority < -20 || priority > 19) {
    darwin_art_bionic_errno_store(22);
    return -1;
  }
  (void)who;
  return 0;
}
int darwin_art_bionic_getpriority(int which, unsigned who) {
  if (which != PRIO_PROCESS) {
    darwin_art_bionic_errno_store(22);
    return -1;
  }
  (void)who;
  return 0;
}
int darwin_art_bionic_setsid(void) { return darwin_art_bionic_getpid(); }
int darwin_art_bionic_kill(int pid, int signal_number) {
  const int translated = signal_number == 0 ? 0 : HostSignal(signal_number);
  if (signal_number != 0 && translated == 0) return -1;
  return kill((pid_t)pid, translated);
}
void darwin_art_bionic_exit(int status) {
  char message[128];
  int length = snprintf(message, sizeof(message),
                        "DARWIN Bionic exit status=%d caller=%p\n", status,
                        __builtin_return_address(0));
  if (length > 0) (void)write(STDERR_FILENO, message, (size_t)length);
  exit(status);
}
void darwin_art_bionic__exit(int status) {
  char message[128];
  int length = snprintf(message, sizeof(message),
                        "DARWIN Bionic _exit status=%d caller=%p\n", status,
                        __builtin_return_address(0));
  if (length > 0) (void)write(STDERR_FILENO, message, (size_t)length);
  _exit(status);
}

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

int darwin_art_bionic_sigaction(int signal_number,
                                const AndroidSigaction* action,
                                AndroidSigaction* old_action);

typedef struct AndroidStack {
  void* pointer;
  int flags;
  size_t size;
} AndroidStack;

typedef struct AndroidSignalMachineContext {
  uint64_t fault_address;
  uint64_t registers[31];
  uint64_t stack_pointer;
  uint64_t program_counter;
  uint64_t processor_state;
  _Alignas(16) unsigned char reserved[4096];
} AndroidSignalMachineContext;

typedef struct AndroidSignalContext {
  uint64_t flags;
  struct AndroidSignalContext* link;
  AndroidStack stack;
  uint64_t signal_mask;
  unsigned char signal_mask_padding[120];
  AndroidSignalMachineContext machine;
} AndroidSignalContext;

_Static_assert(sizeof(AndroidSignalMachineContext) == 4384,
               "Android arm64 mcontext ABI");
_Static_assert(offsetof(AndroidSignalContext, machine) == 176,
               "Android arm64 ucontext ABI");

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

static _Atomic(uintptr_t) gAndroidSignalHandlers[NSIG];
static _Atomic(int) gAndroidSignalFlags[NSIG];
static _Atomic(uint64_t) gAndroidSignalMasks[NSIG];
static _Atomic(uintptr_t) gAndroidSignalRestorers[NSIG];
static _Atomic(uintptr_t) gJitFaultRecovery;

typedef int (*DarwinArtSigchainOwnsSignal)(int signal_number);
typedef void (*DarwinArtEnsureFrontOfChain)(int signal_number);

static int RecoverAndroidReadableSystemRegister(ucontext_t* context) {
#if defined(__aarch64__)
  if (context == NULL || context->uc_mcontext == NULL) return 0;
  _STRUCT_ARM_THREAD_STATE64* state = &context->uc_mcontext->__ss;
  const uint32_t* instruction_pointer =
      (const uint32_t*)(uintptr_t)state->__pc;
  const uint32_t instruction = *instruction_pointer;
  // Linux exposes CTR_EL0 to EL0. Darwin traps the same architectural read,
  // even though Android libraries use it to select cache-maintenance code.
  // Report 64-byte I/D cache lines with DIC+IDC: translated code and Apple's
  // coherent unified cache do not require guest dc cvau/ic ivau operations.
  if ((instruction & UINT32_C(0xffffffe0)) == UINT32_C(0xd53b0020)) {
    const unsigned destination = instruction & 31;
    const uint64_t ctr_el0 = UINT64_C(0x30040004);
    if (destination < 29) {
      state->__x[destination] = ctr_el0;
    } else if (destination == 29) {
      state->__fp = ctr_el0;
    } else if (destination == 30) {
      state->__lr = ctr_el0;
    }
    state->__pc += sizeof(instruction);
    return 1;
  }
#else
  (void)context;
#endif
  return 0;
}

void darwin_art_bionic_process_state_bind_jit_fault_recovery(
    DarwinArtBionicJitFaultRecovery recovery) {
  atomic_store_explicit(&gJitFaultRecovery, (uintptr_t)recovery,
                        memory_order_release);
}

static void DarwinArtAndroidSignalTrampoline(int host_signal,
                                              siginfo_t* host_info,
                                              void* host_context) {
  if (host_signal <= 0 || host_signal >= NSIG) return;
  const uintptr_t address = atomic_load_explicit(
      &gAndroidSignalHandlers[host_signal], memory_order_acquire);
  if (address == (uintptr_t)SIG_DFL || address == (uintptr_t)SIG_IGN) return;
  const int android_signal = AndroidSignal(host_signal);
  if (android_signal == 4 &&
      RecoverAndroidReadableSystemRegister((ucontext_t*)host_context)) {
    return;
  }
  const int flags = atomic_load_explicit(&gAndroidSignalFlags[host_signal],
                                         memory_order_relaxed);
#if defined(__aarch64__)
  if (android_signal == 7 && host_info != NULL && host_context != NULL) {
    const ucontext_t* recovery_context = (const ucontext_t*)host_context;
    if (recovery_context->uc_mcontext != NULL) {
      const uintptr_t program_counter =
          (uintptr_t)recovery_context->uc_mcontext->__ss.__pc;
      if ((uintptr_t)host_info->si_addr == program_counter) {
        const uintptr_t recovery = atomic_load_explicit(
            &gJitFaultRecovery, memory_order_acquire);
        if (recovery != 0 &&
            ((DarwinArtBionicJitFaultRecovery)recovery)(program_counter) == 1) {
          return;
        }
      }
    }
  }
#endif
  if ((flags & 4) != 0) {
    _Alignas(16) unsigned char android_info[128];
    AndroidSignalContext android_context;
    memset(android_info, 0, sizeof(android_info));
    memset(&android_context, 0, sizeof(android_context));
    memcpy(android_info, &android_signal, sizeof(android_signal));
    if (host_info != NULL) {
      const int android_errno = host_info->si_errno;
      int android_code = host_info->si_code;
      if (android_code == SI_USER || android_code == SI_QUEUE)
        android_code = -6;  // Linux SI_TKILL.
      memcpy(android_info + 4, &android_errno, sizeof(android_errno));
      memcpy(android_info + 8, &android_code, sizeof(android_code));
      if (android_signal == 4 || android_signal == 5 || android_signal == 7 ||
          android_signal == 8 || android_signal == 11) {
        void* fault_address = host_info->si_addr;
        memcpy(android_info + 16, &fault_address, sizeof(fault_address));
      } else {
        const int sender_pid = host_info->si_pid;
        const unsigned sender_uid = host_info->si_uid;
        memcpy(android_info + 16, &sender_pid, sizeof(sender_pid));
        memcpy(android_info + 20, &sender_uid, sizeof(sender_uid));
      }
    }
    if (host_context != NULL) {
      const ucontext_t* context = (const ucontext_t*)host_context;
      android_context.link = NULL;
      android_context.stack.pointer = context->uc_stack.ss_sp;
      android_context.stack.flags = context->uc_stack.ss_flags;
      android_context.stack.size = context->uc_stack.ss_size;
      android_context.signal_mask = MaskFromHost(&context->uc_sigmask);
#if defined(__aarch64__)
      if (context->uc_mcontext != NULL) {
        const _STRUCT_ARM_THREAD_STATE64* state = &context->uc_mcontext->__ss;
        for (size_t index = 0; index < 29; ++index)
          android_context.machine.registers[index] = state->__x[index];
        android_context.machine.registers[29] = state->__fp;
        android_context.machine.registers[30] = state->__lr;
        android_context.machine.stack_pointer = state->__sp;
        android_context.machine.program_counter = state->__pc;
        android_context.machine.processor_state = state->__cpsr;
        if (host_info != NULL)
          android_context.machine.fault_address =
              (uint64_t)(uintptr_t)host_info->si_addr;
      }
#endif
    }
    ((void (*)(int, void*, void*))address)(android_signal, android_info,
                                           &android_context);
  } else {
    ((void (*)(int))address)(android_signal);
  }
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
  AndroidSigaction action = {.flags = 0x10000000,
                             .handler = handler,
                             .mask = 0,
                             .restorer = NULL};
  AndroidSigaction old_action;
  if (darwin_art_bionic_sigaction(signal_number, &action, &old_action) != 0)
    return SIG_ERR;
  return old_action.handler;
}

unsigned darwin_art_bionic_getuid(void) { return (unsigned)getuid(); }
unsigned darwin_art_bionic_getgid(void) { return (unsigned)getgid(); }
unsigned darwin_art_bionic_getegid(void) { return (unsigned)getegid(); }
int darwin_art_bionic_setuid(unsigned uid) { return setuid((uid_t)uid); }
int darwin_art_bionic_gethostname(char* name, size_t length) {
  return gethostname(name, length);
}

typedef struct AndroidPasswd {
  char* name;
  char* password;
  uint32_t uid;
  uint32_t gid;
  char* gecos;
  char* directory;
  char* shell;
} AndroidPasswd;

typedef struct AndroidGroup {
  char* name;
  char* password;
  uint32_t gid;
  char** members;
} AndroidGroup;

void* darwin_art_bionic_getpwuid(unsigned uid) {
  static char name[] = "u0_a0";
  static char empty[] = "";
  static char directory[] = "/data";
  static char shell[] = "/system/bin/sh";
  static _Thread_local AndroidPasswd value;
  value.name = name;
  value.password = empty;
  value.gecos = empty;
  value.directory = directory;
  value.shell = shell;
  value.uid = uid;
  value.gid = uid;
  return &value;
}

void* darwin_art_bionic_getgrgid(unsigned gid) {
  static char name[] = "u0_a0";
  static char empty[] = "";
  static char* members[] = {NULL};
  static _Thread_local AndroidGroup value;
  value.name = name;
  value.password = empty;
  value.members = members;
  value.gid = gid;
  return &value;
}

typedef struct AndroidRlimit {
  uint64_t current;
  uint64_t maximum;
} AndroidRlimit;

static int CopyToGuest(void* destination, const void* source, size_t size) {
  if (destination == NULL || source == NULL || size == 0) return -1;
  const uintptr_t address = (uintptr_t)destination;
  if (address > UINTPTR_MAX - size) return -1;
  return mach_vm_write(mach_task_self(), (mach_vm_address_t)address,
                       (vm_offset_t)source, (mach_msg_type_number_t)size) ==
                 KERN_SUCCESS
             ? 0
             : -1;
}

static int CopyFromGuest(void* destination, const void* source, size_t size) {
  if (destination == NULL || source == NULL || size == 0) return -1;
  const uintptr_t address = (uintptr_t)source;
  if (address > UINTPTR_MAX - size) return -1;
  mach_vm_size_t copied = 0;
  return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)address,
                                (mach_vm_size_t)size,
                                (mach_vm_address_t)destination, &copied) ==
                     KERN_SUCCESS &&
                 copied == size
             ? 0
             : -1;
}

int darwin_art_bionic_getrlimit(int resource, AndroidRlimit* limit) {
  if (limit == NULL || resource < 0 || resource > 15) {
    darwin_art_bionic_errno_store(22);
    return -1;
  }
  AndroidRlimit value = {UINT64_MAX, UINT64_MAX};
  if (resource == 3) value.current = UINT64_C(8) * 1024 * 1024;
  if (resource == 7) value.current = value.maximum = 4096;
  if (CopyToGuest(limit, &value, sizeof(value)) != 0) {
    darwin_art_bionic_errno_store(14);
    return -1;
  }
  return 0;
}

int darwin_art_bionic_setrlimit(int resource, const AndroidRlimit* limit) {
  AndroidRlimit value;
  if (limit == NULL || CopyFromGuest(&value, limit, sizeof(value)) != 0) {
    darwin_art_bionic_errno_store(14);
    return -1;
  }
  if (resource < 0 || resource > 15 || value.current > value.maximum) {
    darwin_art_bionic_errno_store(22);
    return -1;
  }
  return 0;
}

int darwin_art_bionic_getopt_long(void) { return -1; }

char* darwin_art_bionic_strsignal(int signal_number) {
  static char unknown[] = "Unknown signal";
  static char abort_name[] = "Aborted";
  static char segmentation[] = "Segmentation fault";
  static char termination[] = "Terminated";
  if (signal_number == 6) return abort_name;
  if (signal_number == 11) return segmentation;
  if (signal_number == 15) return termination;
  return unknown;
}

typedef struct AndroidUtsname {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
} AndroidUtsname;

typedef struct AndroidSysinfo {
  int64_t uptime;
  uint64_t loads[3];
  uint64_t totalram;
  uint64_t freeram;
  uint64_t sharedram;
  uint64_t bufferram;
  uint64_t totalswap;
  uint64_t freeswap;
  uint16_t procs;
  uint16_t pad;
  uint64_t totalhigh;
  uint64_t freehigh;
  uint32_t mem_unit;
} AndroidSysinfo;

_Static_assert(sizeof(AndroidSysinfo) == 112,
               "Android arm64 struct sysinfo size drift");

int darwin_art_bionic_sysinfo(AndroidSysinfo* information) {
  if (information == NULL) {
    darwin_art_bionic_errno_store(14);
    return -1;
  }
  memset(information, 0, sizeof(*information));
  information->uptime = 1;
  information->totalram = UINT64_C(8) * 1024 * 1024 * 1024;
  information->freeram = UINT64_C(4) * 1024 * 1024 * 1024;
  information->procs = 1;
  information->mem_unit = 1;
  return 0;
}

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

static int darwin_art_bionic_fork(void) {
  const pid_t result = fork();
  if (result == 0) {
    // Darwin ART is a multithreaded detached runtime, not a zygote. Continuing
    // guest ART after Darwin fork would inherit locks whose owning threads no
    // longer exist. Android native clients use this path for exec-only helper
    // launchers (not app-process creation, which is handled by the Service
    // bridge), so terminate the intermediate child until exec is brokered.
    _exit(127);
  }
  if (result < 0) {
    // The portable failures are numerically identical on Darwin and Bionic.
    // Normalize Darwin's EAGAIN value explicitly.
    darwin_art_bionic_errno_store(errno == EAGAIN ? 11 : errno);
  }
  return (int)result;
}

static int darwin_art_bionic_waitpid(int process, int* status, int options) {
  const pid_t result = waitpid((pid_t)process, status, options);
  if (result < 0) {
    // waitpid's portable errors have the same values on Darwin and Bionic.
    darwin_art_bionic_errno_store(errno);
  }
  return (int)result;
}

int darwin_art_bionic_inotify_init(void) {
  return darwin_art_bionic_process_unsupported();
}

int darwin_art_bionic_inotify_add_watch(int descriptor, const char* path,
                                        uint32_t mask) {
  (void)descriptor;
  (void)path;
  (void)mask;
  return darwin_art_bionic_process_unsupported();
}

int darwin_art_bionic_inotify_rm_watch(int descriptor, int watch) {
  (void)descriptor;
  (void)watch;
  return darwin_art_bionic_process_unsupported();
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

int darwin_art_bionic_sched_getscheduler(int pid) {
  // Guest tids are scoped to the Android process broker, not Darwin pids.
  // Until realtime policy is modeled, every guest thread uses SCHED_OTHER.
  if (pid < 0) {
    darwin_art_bionic_errno_store(22);
    return -1;
  }
  return 0;
}

int darwin_art_bionic_sched_getaffinity(int pid, size_t capacity, void* mask) {
  if (pid < 0 || mask == NULL || capacity == 0) {
    darwin_art_bionic_errno_store(22);
    return -1;
  }
  volatile unsigned char* bytes = (volatile unsigned char*)mask;
  for (size_t index = 0; index < capacity; ++index) bytes[index] = 0;
  const size_t virtual_cpu_count = 8;
  for (size_t cpu = 0; cpu < virtual_cpu_count && cpu < capacity * 8; ++cpu)
    bytes[cpu / 8] |= (unsigned char)(1u << (cpu % 8));
  return 0;
}

int darwin_art_bionic_sched_setaffinity(int pid, size_t capacity,
                                        const void* mask) {
  if (pid < 0 || mask == NULL || capacity == 0) {
    darwin_art_bionic_errno_store(22);
    return -1;
  }
  return 0;
}

int darwin_art_bionic___sched_cpucount(size_t capacity, const void* mask) {
  if (mask == NULL) return 0;
  const volatile unsigned char* bytes = (const volatile unsigned char*)mask;
  int count = 0;
  for (size_t index = 0; index < capacity; ++index)
    count += __builtin_popcount((unsigned)bytes[index]);
  return count;
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
int darwin_art_bionic_unsetenv(const char* name) { return unsetenv(name); }

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
  uintptr_t previous_guest_handler = atomic_load_explicit(
      &gAndroidSignalHandlers[host_signal], memory_order_acquire);
  const int previous_guest_flags = atomic_load_explicit(
      &gAndroidSignalFlags[host_signal], memory_order_relaxed);
  const uint64_t previous_guest_mask = atomic_load_explicit(
      &gAndroidSignalMasks[host_signal], memory_order_relaxed);
  const uintptr_t previous_guest_restorer = atomic_load_explicit(
      &gAndroidSignalRestorers[host_signal], memory_order_relaxed);
  DarwinArtSigchainOwnsSignal sigchain_owns_signal =
      (DarwinArtSigchainOwnsSignal)dlsym(RTLD_DEFAULT,
                                         "darwin_art_sigchain_owns_signal");
  DarwinArtEnsureFrontOfChain ensure_front_of_chain =
      (DarwinArtEnsureFrontOfChain)dlsym(RTLD_DEFAULT,
                                         "EnsureFrontOfChain");
  const int sigchain_owned = sigchain_owns_signal != NULL &&
                             sigchain_owns_signal(host_signal) != 0;
  if (action != NULL) {
    memset(&host_action, 0, sizeof(host_action));
    if (action->handler == SIG_DFL || action->handler == SIG_IGN) {
      host_action.sa_handler = action->handler;
    } else {
      host_action.sa_sigaction = DarwinArtAndroidSignalTrampoline;
    }
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
  if (action != NULL) {
    atomic_store_explicit(&gAndroidSignalHandlers[host_signal],
                          (uintptr_t)action->handler, memory_order_release);
    atomic_store_explicit(&gAndroidSignalFlags[host_signal], action->flags,
                          memory_order_relaxed);
    atomic_store_explicit(&gAndroidSignalMasks[host_signal], action->mask,
                          memory_order_relaxed);
    atomic_store_explicit(&gAndroidSignalRestorers[host_signal],
                          (uintptr_t)action->restorer, memory_order_relaxed);
    if (sigchain_owned && ensure_front_of_chain != NULL)
      ensure_front_of_chain(host_signal);
  }
  if (old_action != NULL) {
    memset(old_action, 0, sizeof(*old_action));
    if (sigchain_owned ||
        host_old.sa_sigaction == DarwinArtAndroidSignalTrampoline) {
      old_action->handler = previous_guest_handler == 0
                                ? SIG_DFL
                                : (void (*)(int))previous_guest_handler;
      old_action->flags = previous_guest_flags;
      old_action->mask = previous_guest_mask;
      old_action->restorer = (void (*)(void))previous_guest_restorer;
    } else {
      old_action->handler = host_old.sa_handler;
      old_action->mask = MaskFromHost(&host_old.sa_mask);
      if ((host_old.sa_flags & SA_ONSTACK) != 0)
        old_action->flags |= 0x08000000;
      if ((host_old.sa_flags & SA_RESTART) != 0)
        old_action->flags |= 0x10000000;
      if ((host_old.sa_flags & SA_NODEFER) != 0)
        old_action->flags |= 0x40000000;
      if ((host_old.sa_flags & SA_RESETHAND) != 0)
        old_action->flags |= (int)UINT32_C(0x80000000);
      if ((host_old.sa_flags & SA_SIGINFO) != 0) old_action->flags |= 4;
    }
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
    {"__sched_cpucount",
     (DarwinArtBionicProcessFunction)darwin_art_bionic___sched_cpucount},
    {"__system_property_find",
     (DarwinArtBionicProcessFunction)darwin_art_bionic___system_property_find},
    {"__system_property_get",
     (DarwinArtBionicProcessFunction)darwin_art_bionic___system_property_get},
    {"__system_property_read_callback",
     (DarwinArtBionicProcessFunction)darwin_art_bionic___system_property_read_callback},
    {"_exit", (DarwinArtBionicProcessFunction)darwin_art_bionic__exit},
    {"_longjmp", (DarwinArtBionicProcessFunction)darwin_art_bionic__longjmp},
    {"_setjmp", (DarwinArtBionicProcessFunction)darwin_art_bionic__setjmp},
    {"android_get_device_api_level",
     (DarwinArtBionicProcessFunction)darwin_art_bionic_android_get_device_api_level},
    {"arc4random", (DarwinArtBionicProcessFunction)darwin_art_bionic_arc4random},
    {"arc4random_buf", (DarwinArtBionicProcessFunction)darwin_art_bionic_arc4random_buf},
    {"daemon", (DarwinArtBionicProcessFunction)darwin_art_bionic_daemon},
    {"erand48", (DarwinArtBionicProcessFunction)darwin_art_bionic_erand48},
    {"execlp", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
    {"execv", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
    {"execve", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
    {"execvp", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
    {"exit", (DarwinArtBionicProcessFunction)darwin_art_bionic_exit},
    {"fork", (DarwinArtBionicProcessFunction)darwin_art_bionic_fork},
    {"getauxval", (DarwinArtBionicProcessFunction)darwin_art_bionic_getauxval},
    {"getegid", (DarwinArtBionicProcessFunction)darwin_art_bionic_getegid},
    {"getenv", (DarwinArtBionicProcessFunction)darwin_art_bionic_getenv},
    {"geteuid", (DarwinArtBionicProcessFunction)darwin_art_bionic_geteuid},
    {"getgid", (DarwinArtBionicProcessFunction)darwin_art_bionic_getgid},
    {"getgrgid", (DarwinArtBionicProcessFunction)darwin_art_bionic_getgrgid},
    {"gethostname", (DarwinArtBionicProcessFunction)darwin_art_bionic_gethostname},
    {"getopt_long", (DarwinArtBionicProcessFunction)darwin_art_bionic_getopt_long},
    {"getpagesize", (DarwinArtBionicProcessFunction)darwin_art_bionic_getpagesize},
    {"getpid", (DarwinArtBionicProcessFunction)darwin_art_bionic_getpid},
    {"getppid", (DarwinArtBionicProcessFunction)darwin_art_bionic_getppid},
    {"getpriority", (DarwinArtBionicProcessFunction)darwin_art_bionic_getpriority},
    {"getprogname", (DarwinArtBionicProcessFunction)darwin_art_bionic_getprogname},
    {"getpwuid", (DarwinArtBionicProcessFunction)darwin_art_bionic_getpwuid},
    {"getpwuid_r", (DarwinArtBionicProcessFunction)darwin_art_bionic_getpwuid_r_unsupported},
    {"getrandom", (DarwinArtBionicProcessFunction)darwin_art_bionic_getrandom},
    {"getrlimit", (DarwinArtBionicProcessFunction)darwin_art_bionic_getrlimit},
    {"getrusage", (DarwinArtBionicProcessFunction)darwin_art_bionic_getrusage},
    {"getservbyport", (DarwinArtBionicProcessFunction)darwin_art_bionic_getservbyport_unsupported},
    {"getuid", (DarwinArtBionicProcessFunction)darwin_art_bionic_getuid},
    {"inotify_add_watch", (DarwinArtBionicProcessFunction)darwin_art_bionic_inotify_add_watch},
    {"inotify_init", (DarwinArtBionicProcessFunction)darwin_art_bionic_inotify_init},
    {"inotify_rm_watch", (DarwinArtBionicProcessFunction)darwin_art_bionic_inotify_rm_watch},
    {"jrand48", (DarwinArtBionicProcessFunction)darwin_art_bionic_jrand48},
    {"kill", (DarwinArtBionicProcessFunction)darwin_art_bionic_kill},
    {"longjmp", (DarwinArtBionicProcessFunction)darwin_art_bionic_longjmp},
    {"nice", (DarwinArtBionicProcessFunction)darwin_art_bionic_nice},
    {"nrand48", (DarwinArtBionicProcessFunction)darwin_art_bionic_nrand48},
    {"posix_spawn", (DarwinArtBionicProcessFunction)darwin_art_bionic_posix_spawn},
    {"posix_spawnp", (DarwinArtBionicProcessFunction)darwin_art_bionic_posix_spawn},
    {"prctl", (DarwinArtBionicProcessFunction)darwin_art_bionic_prctl},
    {"process_vm_readv", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
    {"ptrace", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
    {"raise", (DarwinArtBionicProcessFunction)darwin_art_bionic_raise},
    {"rand", (DarwinArtBionicProcessFunction)darwin_art_bionic_rand},
    {"rand_r", (DarwinArtBionicProcessFunction)darwin_art_bionic_rand_r},
    {"random", (DarwinArtBionicProcessFunction)darwin_art_bionic_random},
    {"sched_get_priority_max", (DarwinArtBionicProcessFunction)darwin_art_bionic_sched_get_priority_max},
    {"sched_get_priority_min", (DarwinArtBionicProcessFunction)darwin_art_bionic_sched_get_priority_min},
    {"sched_getaffinity", (DarwinArtBionicProcessFunction)darwin_art_bionic_sched_getaffinity},
    {"sched_getparam", (DarwinArtBionicProcessFunction)darwin_art_bionic_sched_getparam},
    {"sched_getscheduler", (DarwinArtBionicProcessFunction)darwin_art_bionic_sched_getscheduler},
    {"sched_setaffinity", (DarwinArtBionicProcessFunction)darwin_art_bionic_sched_setaffinity},
    {"sched_setscheduler", (DarwinArtBionicProcessFunction)darwin_art_bionic_sched_setscheduler},
    {"sched_yield", (DarwinArtBionicProcessFunction)darwin_art_bionic_sched_yield},
    {"setenv", (DarwinArtBionicProcessFunction)darwin_art_bionic_setenv},
    {"setjmp", (DarwinArtBionicProcessFunction)darwin_art_bionic_setjmp},
    {"setpriority", (DarwinArtBionicProcessFunction)darwin_art_bionic_setpriority},
    {"setrlimit", (DarwinArtBionicProcessFunction)darwin_art_bionic_setrlimit},
    {"setsid", (DarwinArtBionicProcessFunction)darwin_art_bionic_setsid},
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
    {"strsignal", (DarwinArtBionicProcessFunction)darwin_art_bionic_strsignal},
    {"sysinfo", (DarwinArtBionicProcessFunction)darwin_art_bionic_sysinfo},
    {"system", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
    {"uname", (DarwinArtBionicProcessFunction)darwin_art_bionic_uname},
    {"unsetenv", (DarwinArtBionicProcessFunction)darwin_art_bionic_unsetenv},
    {"vfork", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
    {"vmsplice", (DarwinArtBionicProcessFunction)darwin_art_bionic_process_unsupported},
    {"waitpid", (DarwinArtBionicProcessFunction)darwin_art_bionic_waitpid},
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

extern char** darwin_art_bionic_environ;
char* darwin_art_bionic_optarg;
int darwin_art_bionic_optind = 1;

uintptr_t darwin_art_bionic_process_state_data_resolve(const char* name) {
  if (name != NULL && strcmp(name, "environ") == 0) {
    return (uintptr_t)&darwin_art_bionic_environ;
  }
  if (name != NULL && strcmp(name, "optarg") == 0) {
    return (uintptr_t)&darwin_art_bionic_optarg;
  }
  if (name != NULL && strcmp(name, "optind") == 0) {
    return (uintptr_t)&darwin_art_bionic_optind;
  }
  return 0;
}
