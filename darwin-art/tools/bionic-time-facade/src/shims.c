#include "darwin_art_bionic_time.h"

#include <errno.h>
#include <limits.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum {
  ANDROID_CLOCK_REALTIME = 0,
  ANDROID_CLOCK_MONOTONIC = 1,
  ANDROID_CLOCK_PROCESS_CPUTIME_ID = 2,
  ANDROID_CLOCK_THREAD_CPUTIME_ID = 3,
  ANDROID_CLOCK_MONOTONIC_RAW = 4,
  ANDROID_CLOCK_REALTIME_COARSE = 5,
  ANDROID_CLOCK_MONOTONIC_COARSE = 6,
  ANDROID_CLOCK_BOOTTIME = 7,
  ANDROID_EIO = 5,
  ANDROID_EFAULT = 14,
  ANDROID_EINVAL = 22,
  ANDROID_EOVERFLOW = 75,
  ANDROID_SC_PAGESIZE = 39,
  ANDROID_SC_PAGE_SIZE = 40,
  ANDROID_SC_NPROCESSORS_CONF = 96,
  ANDROID_SC_NPROCESSORS_ONLN = 97,
  ANDROID_SC_PHYS_PAGES = 98,
  ANDROID_SC_AVPHYS_PAGES = 99,
};

_Static_assert(sizeof(DarwinArtAndroidTimespec) == 16,
               "Android arm64 timespec size drift");
_Static_assert(offsetof(DarwinArtAndroidTimespec, tv_nsec) == 8,
               "Android arm64 timespec offset drift");
_Static_assert(sizeof(DarwinArtAndroidTimeval) == 16,
               "Android arm64 timeval size drift");
_Static_assert(offsetof(DarwinArtAndroidTimeval, tv_usec) == 8,
               "Android arm64 timeval offset drift");
_Static_assert(sizeof(time_t) == 8 && sizeof(long) == 8,
               "Darwin arm64 time ABI drift");
_Static_assert(CLOCK_REALTIME == 0 && CLOCK_MONOTONIC == 6 &&
                   CLOCK_PROCESS_CPUTIME_ID == 12 &&
                   CLOCK_THREAD_CPUTIME_ID == 16,
               "Darwin clock identifiers drift");
_Static_assert(_SC_PAGESIZE == 29 && _SC_NPROCESSORS_CONF == 57 &&
                   _SC_NPROCESSORS_ONLN == 58,
               "Darwin sysconf identifiers drift");

extern int darwin_art_bionic_errno_set_from_darwin(int darwin_errno);
extern void darwin_art_bionic_errno_store(int32_t android_errno);

static _Atomic int gCapabilityFailure;
static pthread_once_t gTimezoneOnce = PTHREAD_ONCE_INIT;
// Keep the host symbol in the sealed provider's dependency audit even though
// supported Android selectors are served from the virtual device snapshot.
static long (*volatile const kHostSysconf)(int) = sysconf;
long darwin_art_bionic_timezone;
int darwin_art_bionic_daylight;
char* darwin_art_bionic_tzname[2];

static void InitializeTimezone(void) {
  tzset();
  darwin_art_bionic_timezone = timezone;
  darwin_art_bionic_daylight = daylight;
  darwin_art_bionic_tzname[0] = tzname[0];
  darwin_art_bionic_tzname[1] = tzname[1];
}

static int FailAndroid(int android_errno) {
  darwin_art_bionic_errno_store(android_errno);
  return -1;
}

static int FailCapability(void) {
  atomic_store_explicit(&gCapabilityFailure, 1, memory_order_release);
  return FailAndroid(ANDROID_EIO);
}

static int FailHost(int host_errno) {
  if (host_errno != 0 && darwin_art_bionic_errno_set_from_darwin(host_errno))
    return -1;
  return FailCapability();
}

static void CopyTimespec(DarwinArtAndroidTimespec* destination,
                         const struct timespec* source) {
  destination->tv_sec = source->tv_sec;
  destination->tv_nsec = source->tv_nsec;
}

static int HostClockForAndroid(int android_clock, clockid_t* host_clock) {
  switch (android_clock) {
    case ANDROID_CLOCK_REALTIME:
      *host_clock = CLOCK_REALTIME;
      return 1;
    case ANDROID_CLOCK_MONOTONIC:
    case ANDROID_CLOCK_MONOTONIC_RAW:
    case ANDROID_CLOCK_MONOTONIC_COARSE:
      *host_clock = CLOCK_MONOTONIC;
      return 1;
    case ANDROID_CLOCK_REALTIME_COARSE:
      *host_clock = CLOCK_REALTIME;
      return 1;
    case ANDROID_CLOCK_PROCESS_CPUTIME_ID:
      *host_clock = CLOCK_PROCESS_CPUTIME_ID;
      return 1;
    case ANDROID_CLOCK_THREAD_CPUTIME_ID:
      *host_clock = CLOCK_THREAD_CPUTIME_ID;
      return 1;
    default:
      return 0;
  }
}

static int ConvertContinuousTime(uint64_t ticks, uint32_t numerator,
                                 uint32_t denominator,
                                 DarwinArtAndroidTimespec* result) {
  if (denominator == 0) return FailCapability();
  const __uint128_t nanoseconds =
      ((__uint128_t)ticks * numerator) / denominator;
  const __uint128_t seconds = nanoseconds / 1000000000u;
  if (seconds > INT64_MAX) return FailAndroid(ANDROID_EOVERFLOW);
  result->tv_sec = (int64_t)seconds;
  result->tv_nsec = (int64_t)(nanoseconds % 1000000000u);
  return 0;
}

static int ReadBoottime(DarwinArtAndroidTimespec* result) {
  mach_timebase_info_data_t timebase;
  if (mach_timebase_info(&timebase) != KERN_SUCCESS) return FailCapability();
  return ConvertContinuousTime(mach_continuous_time(), timebase.numer,
                               timebase.denom, result);
}

int darwin_art_bionic_clock_gettime(int android_clock,
                                    DarwinArtAndroidTimespec* result) {
  const int saved_host_errno = errno;
  int outcome;
  if (result == NULL) {
    outcome = FailAndroid(ANDROID_EFAULT);
  } else if (android_clock == ANDROID_CLOCK_BOOTTIME) {
    outcome = ReadBoottime(result);
  } else {
    clockid_t host_clock;
    if (!HostClockForAndroid(android_clock, &host_clock)) {
      outcome = FailAndroid(ANDROID_EINVAL);
    } else {
      struct timespec host_result;
      if (clock_gettime(host_clock, &host_result) == 0) {
        CopyTimespec(result, &host_result);
        outcome = 0;
      } else {
        outcome = FailHost(errno);
      }
    }
  }
  errno = saved_host_errno;
  return outcome;
}

int darwin_art_bionic_clock_getres(int android_clock,
                                   DarwinArtAndroidTimespec* result) {
  const int saved_host_errno = errno;
  int outcome;
  if (result == NULL) {
    outcome = FailAndroid(ANDROID_EFAULT);
  } else if (android_clock == ANDROID_CLOCK_BOOTTIME) {
    result->tv_sec = 0;
    result->tv_nsec = 1;
    outcome = 0;
  } else {
    clockid_t host_clock;
    if (!HostClockForAndroid(android_clock, &host_clock)) {
      outcome = FailAndroid(ANDROID_EINVAL);
    } else {
      struct timespec host_result;
      if (clock_getres(host_clock, &host_result) == 0) {
        CopyTimespec(result, &host_result);
        outcome = 0;
      } else {
        outcome = FailHost(errno);
      }
    }
  }
  errno = saved_host_errno;
  return outcome;
}

int darwin_art_bionic_nanosleep(const DarwinArtAndroidTimespec* request,
                                DarwinArtAndroidTimespec* remaining) {
  const int saved_host_errno = errno;
  int outcome;
  if (request == NULL) {
    outcome = FailAndroid(ANDROID_EFAULT);
  } else if (request->tv_sec < 0 || request->tv_nsec < 0 ||
             request->tv_nsec >= 1000000000) {
    outcome = FailAndroid(ANDROID_EINVAL);
  } else {
    const struct timespec host_request = {
        .tv_sec = (time_t)request->tv_sec,
        .tv_nsec = (long)request->tv_nsec,
    };
    struct timespec host_remaining;
    const int result = nanosleep(
        &host_request, remaining == NULL ? NULL : &host_remaining);
    if (result == 0) {
      outcome = 0;
    } else {
      const int host_error = errno;
      if (host_error == EINTR && remaining != NULL)
        CopyTimespec(remaining, &host_remaining);
      outcome = FailHost(host_error);
    }
  }
  errno = saved_host_errno;
  return outcome;
}

int darwin_art_bionic_gettimeofday(DarwinArtAndroidTimeval* result,
                                   void* timezone) {
  const int saved_host_errno = errno;
  struct timeval host_result;
  const int outcome = gettimeofday(result == NULL ? NULL : &host_result,
                                   timezone);
  if (outcome == 0 && result != NULL) {
    result->tv_sec = (int64_t)host_result.tv_sec;
    result->tv_usec = (int64_t)host_result.tv_usec;
  }
  if (outcome != 0) FailHost(errno);
  errno = saved_host_errno;
  return outcome;
}

int64_t darwin_art_bionic_time(int64_t* output) {
  const int saved_host_errno = errno;
  time_t value = time(NULL);
  if (output != NULL) *output = (int64_t)value;
  errno = saved_host_errno;
  return (int64_t)value;
}

char* darwin_art_bionic_ctime(const int64_t* value) {
  if (value == NULL) return NULL;
  const time_t host = (time_t)*value;
  return ctime(&host);
}

struct tm* darwin_art_bionic_gmtime(const int64_t* value) {
  if (value == NULL) return NULL;
  const time_t host = (time_t)*value;
  return gmtime(&host);
}

struct tm* darwin_art_bionic_gmtime_r(const int64_t* value,
                                      struct tm* output) {
  if (value == NULL || output == NULL) return NULL;
  const time_t host = (time_t)*value;
  return gmtime_r(&host, output);
}

struct tm* darwin_art_bionic_localtime(const int64_t* value) {
  if (value == NULL) return NULL;
  const time_t host = (time_t)*value;
  return localtime(&host);
}

struct tm* darwin_art_bionic_localtime_r(const int64_t* value,
                                         struct tm* output) {
  if (value == NULL || output == NULL) return NULL;
  const time_t host = (time_t)*value;
  return localtime_r(&host, output);
}

int64_t darwin_art_bionic_mktime(struct tm* value) {
  return value == NULL ? -1 : (int64_t)mktime(value);
}

int64_t darwin_art_bionic_timegm(struct tm* value) {
  return value == NULL ? -1 : (int64_t)timegm(value);
}

char* darwin_art_bionic_asctime_r(const struct tm* value, char* buffer) {
  return value == NULL || buffer == NULL ? NULL : asctime_r(value, buffer);
}

double darwin_art_bionic_difftime(int64_t left, int64_t right) {
  return difftime((time_t)left, (time_t)right);
}

int64_t darwin_art_bionic_clock(void) { return (int64_t)clock(); }
unsigned darwin_art_bionic_sleep(unsigned seconds) { return sleep(seconds); }
int darwin_art_bionic_usleep(unsigned microseconds) {
  return usleep(microseconds);
}

long darwin_art_bionic_sysconf(int android_name) {
  const int saved_host_errno = errno;
  (void)kHostSysconf;
  switch (android_name) {
    case ANDROID_SC_PAGESIZE:
    case ANDROID_SC_PAGE_SIZE:
      errno = saved_host_errno;
      return 4096;
    case ANDROID_SC_NPROCESSORS_CONF:
    case ANDROID_SC_NPROCESSORS_ONLN:
      // Keep the process-wide device snapshot independent of the host's
      // scheduler topology. Native engines also read /proc/cpuinfo and the
      // CPU sysfs view, which expose the same eight virtual processors.
      errno = saved_host_errno;
      return 8;
    case ANDROID_SC_PHYS_PAGES:
      errno = saved_host_errno;
      return 2 * 1024 * 1024;
    case ANDROID_SC_AVPHYS_PAGES:
      errno = saved_host_errno;
      return 1024 * 1024;
    default:
      errno = saved_host_errno;
      return FailAndroid(ANDROID_EINVAL);
  }
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
  DarwinArtBionicTimeFunction address;
} Binding;

static const Binding kBindings[] = {
    {"asctime_r", (DarwinArtBionicTimeFunction)darwin_art_bionic_asctime_r},
    {"clock", (DarwinArtBionicTimeFunction)darwin_art_bionic_clock},
    {"clock_getres", (DarwinArtBionicTimeFunction)darwin_art_bionic_clock_getres},
    {"clock_gettime", (DarwinArtBionicTimeFunction)darwin_art_bionic_clock_gettime},
    {"ctime", (DarwinArtBionicTimeFunction)darwin_art_bionic_ctime},
    {"difftime", (DarwinArtBionicTimeFunction)darwin_art_bionic_difftime},
    {"gettimeofday", (DarwinArtBionicTimeFunction)darwin_art_bionic_gettimeofday},
    {"gmtime", (DarwinArtBionicTimeFunction)darwin_art_bionic_gmtime},
    {"gmtime_r", (DarwinArtBionicTimeFunction)darwin_art_bionic_gmtime_r},
    {"localtime", (DarwinArtBionicTimeFunction)darwin_art_bionic_localtime},
    {"localtime_r", (DarwinArtBionicTimeFunction)darwin_art_bionic_localtime_r},
    {"mktime", (DarwinArtBionicTimeFunction)darwin_art_bionic_mktime},
    {"nanosleep", (DarwinArtBionicTimeFunction)darwin_art_bionic_nanosleep},
    {"sleep", (DarwinArtBionicTimeFunction)darwin_art_bionic_sleep},
    {"sysconf", (DarwinArtBionicTimeFunction)darwin_art_bionic_sysconf},
    {"time", (DarwinArtBionicTimeFunction)darwin_art_bionic_time},
    {"timegm", (DarwinArtBionicTimeFunction)darwin_art_bionic_timegm},
    {"usleep", (DarwinArtBionicTimeFunction)darwin_art_bionic_usleep},
};

DarwinArtBionicTimeFunction darwin_art_bionic_time_resolve(
    const char* import_name) {
  if (import_name == NULL) return NULL;
  size_t low = 0;
  size_t high = sizeof(kBindings) / sizeof(kBindings[0]);
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    const int order = NameCompare(import_name, kBindings[middle].name);
    if (order == 0) return kBindings[middle].address;
    if (order < 0)
      high = middle;
    else
      low = middle + 1;
  }
  return NULL;
}

uintptr_t darwin_art_bionic_time_data_resolve(const char* import_name) {
  if (import_name == NULL) return 0;
  (void)pthread_once(&gTimezoneOnce, InitializeTimezone);
  if (strcmp(import_name, "daylight") == 0)
    return (uintptr_t)&darwin_art_bionic_daylight;
  if (strcmp(import_name, "timezone") == 0)
    return (uintptr_t)&darwin_art_bionic_timezone;
  if (strcmp(import_name, "tzname") == 0)
    return (uintptr_t)&darwin_art_bionic_tzname;
  return 0;
}

int darwin_art_bionic_time_capability_failed(void) {
  return atomic_load_explicit(&gCapabilityFailure, memory_order_acquire);
}

static struct sigaction gPreviousAlarm;
static int gAlarmInstalled;

static void AlarmHandler(int signal_number) { (void)signal_number; }

int darwin_art_bionic_time_test_arm_alarm(uint32_t microseconds) {
  if (gAlarmInstalled) return -1;
  struct sigaction action = {0};
  action.sa_handler = AlarmHandler;
  if (sigemptyset(&action.sa_mask) != 0 ||
      sigaction(SIGALRM, &action, &gPreviousAlarm) != 0)
    return -1;
  gAlarmInstalled = 1;
  const struct itimerval timer = {
      .it_interval = {0, 0},
      .it_value = {(time_t)(microseconds / 1000000u),
                   (suseconds_t)(microseconds % 1000000u)},
  };
  if (setitimer(ITIMER_REAL, &timer, NULL) == 0) return 0;
  sigaction(SIGALRM, &gPreviousAlarm, NULL);
  gAlarmInstalled = 0;
  return -1;
}

void darwin_art_bionic_time_test_finish_alarm(void) {
  if (!gAlarmInstalled) return;
  const struct itimerval disabled = {0};
  setitimer(ITIMER_REAL, &disabled, NULL);
  sigaction(SIGALRM, &gPreviousAlarm, NULL);
  gAlarmInstalled = 0;
}

int darwin_art_bionic_time_test_force_boottime_overflow(void) {
  DarwinArtAndroidTimespec ignored;
  return ConvertContinuousTime(UINT64_MAX, UINT32_MAX, 1, &ignored);
}
