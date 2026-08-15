#include <jni.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

extern int DarwinArtFixtureChildValue(void);
extern void darwin_art_fixture_record_lifecycle(int phase);
extern int* __errno(void);
extern int close(int fd);
extern int ioctl(int fd, int request, ...);
extern int open(const char* path, int flags, ...);
extern intptr_t read(int fd, void* buffer, size_t count);
extern int sscanf(const char* input, const char* format, ...);
extern size_t strlen(const char* string);
extern int vsscanf(const char* input, const char* format, va_list arguments);
extern int swprintf(uint32_t* output, size_t capacity,
                    const uint32_t* format, ...);
typedef struct {
  int32_t tm_sec;
  int32_t tm_min;
  int32_t tm_hour;
  int32_t tm_mday;
  int32_t tm_mon;
  int32_t tm_year;
  int32_t tm_wday;
  int32_t tm_yday;
  int32_t tm_isdst;
  int64_t tm_gmtoff;
  const char* tm_zone;
} FixtureTm;
extern size_t strftime_l(char* output, size_t capacity, const char* format,
                         const FixtureTm* value, void* locale);
extern int __cxa_atexit(void (*function)(void*), void* argument, void* dso);

__attribute__((visibility("hidden"))) void* __dso_handle = &__dso_handle;

static int g_root_initialized;
static int g_bionic_provider_initialized;
static int g_cxa_registered;
static int g_filesystem_initialized;
static int g_scanf_initialized;
static int g_swprintf_initialized;
static int g_ioctl_initialized;
static int g_strftime_initialized;

typedef struct {
  uint64_t words[2];
} FixtureBinary128;
_Static_assert(sizeof(FixtureBinary128) == 16, "binary128 storage required");

__attribute__((noinline)) static int FixtureVsscanf(const char* input,
                                                    const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  int result = vsscanf(input, format, arguments);
  va_end(arguments);
  return result;
}

static void RootCxaFinalize(void* argument) {
  if (argument == &g_cxa_registered) {
    darwin_art_fixture_record_lifecycle(4);
  }
}

__attribute__((constructor)) static void RootInitialize(void) {
  static const char provider_probe[] = "bionic";
  uint8_t random_bytes[16];
  int* bionic_errno = __errno();
  int random_fd = open("/dev/random", 0);
  int32_t entropy_bits = 0;
  int ioctl_result =
      random_fd < 0 ? -1 : ioctl(random_fd, (int)0x80045200U, &entropy_bits);
  intptr_t random_read =
      random_fd < 0 ? -1 : read(random_fd, random_bytes, sizeof(random_bytes));
  int random_close = random_fd < 0 ? -1 : close(random_fd);
  g_filesystem_initialized =
      random_read == (intptr_t)sizeof(random_bytes) && random_close == 0;
  g_ioctl_initialized = ioctl_result == 0 && entropy_bits == 32;
  FixtureBinary128 binary128 = {0};
  int values[8] = {0};
  const char* binary128_format = "%Lf";
  int scalar_count = sscanf("1.5", binary128_format, &binary128);
  int list_count = FixtureVsscanf(
      "1 2 3 4 5 6 7 8", "%d %d %d %d %d %d %d %d", 1.0, &values[0],
      2.0, &values[1], 3.0, &values[2], 4.0, &values[3], 5.0, &values[4],
      6.0, &values[5], 7.0, &values[6], 8.0, &values[7]);
  g_scanf_initialized =
      scalar_count == 1 && binary128.words[0] == 0 &&
      binary128.words[1] == UINT64_C(0x3fff800000000000) && list_count == 8;
  for (int index = 0; index < 8; ++index) {
    g_scanf_initialized &= values[index] == index + 1;
  }
  uint32_t wide_output[16] = {0};
  static const uint32_t wide_format[] = {'%', 'f', 0};
  static const uint32_t wide_expected[] = {
      '1', '.', '2', '5', '0', '0', '0', '0', 0};
  int wide_length = swprintf(wide_output, 16, wide_format, 1.25);
  g_swprintf_initialized = wide_length == 8;
  for (size_t index = 0; index < sizeof(wide_expected) / sizeof(*wide_expected);
       ++index) {
    g_swprintf_initialized &= wide_output[index] == wide_expected[index];
  }
  FixtureTm broken_down = {0};
  broken_down.tm_sec = 5;
  broken_down.tm_min = 4;
  broken_down.tm_hour = 3;
  broken_down.tm_mday = 2;
  broken_down.tm_mon = 0;
  broken_down.tm_year = 124;
  broken_down.tm_wday = 2;
  broken_down.tm_yday = 1;
  broken_down.tm_isdst = 0;
  char time_output[64] = {0};
  static const char time_expected[] = "2024-01-02 03:04:05 +0000 UTC";
  size_t time_length = strftime_l(time_output, sizeof(time_output),
                                  "%Y-%m-%d %H:%M:%S %z %Z",
                                  &broken_down, (void*)(uintptr_t)0x5a5a);
  g_strftime_initialized =
      time_length == sizeof(time_expected) - 1 &&
      strlen(time_output) == sizeof(time_expected) - 1;
  for (size_t index = 0; index < sizeof(time_expected); ++index) {
    g_strftime_initialized &= time_output[index] == time_expected[index];
  }
  if (DarwinArtFixtureChildValue() == 20 && bionic_errno != NULL &&
      strlen(provider_probe) == sizeof(provider_probe) - 1 &&
      g_filesystem_initialized == 1 && g_scanf_initialized == 1 &&
      g_swprintf_initialized == 1 && g_ioctl_initialized == 1 &&
      g_strftime_initialized == 1) {
    *bionic_errno = 4242;
    g_bionic_provider_initialized = *__errno() == 4242;
    g_root_initialized = 1;
    g_cxa_registered =
        __cxa_atexit(&RootCxaFinalize, &g_cxa_registered, __dso_handle) == 0;
    darwin_art_fixture_record_lifecycle(2);
  }
}

__attribute__((destructor)) static void RootFinalize(void) {
  darwin_art_fixture_record_lifecycle(5);
}

static jlong NativeAdd(JNIEnv* env, jclass fixture_class, jint left,
                       jlong middle, jint right) {
  (void)env;
  (void)fixture_class;
  return DarwinArtFixtureChildValue() == 20
             ? (jlong)left + middle + (jlong)right
             : -1;
}

static uint64_t Mix(uint64_t digest, uint64_t value) {
  return (digest ^ value) * UINT64_C(1099511628211);
}

static jlong NativeSpill(JNIEnv* env, jclass fixture_class, jboolean z, jbyte b,
                         jchar c, jshort s, jint i, jlong j, jobject reference,
                         jfloat f0, jdouble d0, jfloat f1, jdouble d1, jfloat f2,
                         jdouble d2, jfloat f3, jdouble d3, jfloat f4,
                         jfloat f5, jdouble d4) {
  (void)env;
  (void)fixture_class;
  union {
    jfloat value;
    uint32_t bits;
  } floats[] = {{f0}, {f1}, {f2}, {f3}, {f4}, {f5}};
  union {
    jdouble value;
    uint64_t bits;
  } doubles[] = {{d0}, {d1}, {d2}, {d3}, {d4}};

  uint64_t digest = UINT64_C(1469598103934665603);
  digest = Mix(digest, z);
  digest = Mix(digest, (uint8_t)b);
  digest = Mix(digest, c);
  digest = Mix(digest, (uint16_t)s);
  digest = Mix(digest, (uint32_t)i);
  digest = Mix(digest, (uint64_t)j);
  digest = Mix(digest, reference != NULL);
  for (unsigned index = 0; index < 5; ++index) {
    digest = Mix(digest, floats[index].bits);
    digest = Mix(digest, doubles[index].bits);
  }
  digest = Mix(digest, floats[5].bits);
  return (jlong)digest;
}

static jint NativeUsesEnv(JNIEnv* env, jclass fixture_class) {
  (void)fixture_class;
  if ((*env)->GetVersion(env) != JNI_VERSION_1_6) {
    return -1;
  }
  // This is deliberately invoked after JNI_OnLoad returns. The proxy backend
  // must obtain the current invocation thread's ART JNIEnv, not retain the
  // synchronous load thread's JNIEnv.
  jclass string_class = (*env)->FindClass(env, "java/lang/String");
  return string_class == NULL ? -2 : 42;
}

static jint NativeNarrowStack(JNIEnv* env, jclass fixture_class, jint a0,
                              jint a1, jint a2, jint a3, jint a4, jint a5,
                              jboolean z, jbyte b, jchar c, jshort s, jint i,
                              jlong j, jobject reference) {
  (void)env;
  (void)fixture_class;
  return a0 == 10 && a1 == 11 && a2 == 12 && a3 == 13 && a4 == 14 &&
                 a5 == 15 && z == JNI_TRUE && b == (jbyte)0x81 &&
                 c == (jchar)0xabcd && s == (jshort)0x8765 &&
                 i == (jint)0x45678923 &&
                 j == (jlong)INT64_C(0x2233445566778899) && reference != NULL
             ? 42
             : -1;
}

static jobject NativeEcho(JNIEnv* env, jclass fixture_class, jobject value) {
  (void)env;
  (void)fixture_class;
  return value;
}

static jfloat NativeFloat(JNIEnv* env, jclass fixture_class, jfloat value) {
  (void)env;
  (void)fixture_class;
  return value + 0.5f;
}

static jdouble NativeDouble(JNIEnv* env, jclass fixture_class, jdouble value) {
  (void)env;
  (void)fixture_class;
  return value + 0.25;
}

static void NativeVoid(JNIEnv* env, jclass fixture_class) {
  (void)env;
  (void)fixture_class;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)reserved;
  if (g_root_initialized != 1 || g_bionic_provider_initialized != 1 ||
      g_cxa_registered != 1 || g_filesystem_initialized != 1 ||
      g_scanf_initialized != 1 || g_swprintf_initialized != 1 ||
      g_ioctl_initialized != 1 || g_strftime_initialized != 1 ||
      DarwinArtFixtureChildValue() != 20) {
    return JNI_ERR;
  }
  JNIEnv* env = NULL;
  if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK || env == NULL) {
    return JNI_ERR;
  }

  jclass fixture_class =
      (*env)->FindClass(env, "darwin/art/nativefixture/NativeFixture");
  if (fixture_class == NULL) {
    return JNI_ERR;
  }

  const JNINativeMethod methods[] = {
      {"nativeAdd", "(IJI)J", (void*)&NativeAdd},
      {"nativeSpill",
       "(ZBCSIJLjava/lang/Object;FDFDFDFDFFD)J",
       (void*)&NativeSpill},
      {"nativeUsesEnv", "()I", (void*)&NativeUsesEnv},
      {"nativeNarrowStack", "(IIIIIIZBCSIJLjava/lang/Object;)I",
       (void*)&NativeNarrowStack},
      {"nativeEcho", "(Ljava/lang/Object;)Ljava/lang/Object;",
       (void*)&NativeEcho},
      {"nativeFloat", "(F)F", (void*)&NativeFloat},
      {"nativeDouble", "(D)D", (void*)&NativeDouble},
      {"nativeVoid", "()V", (void*)&NativeVoid},
  };
  if ((*env)->RegisterNatives(env, fixture_class, methods, 8) != JNI_OK) {
    return JNI_ERR;
  }
  darwin_art_fixture_record_lifecycle(3);
  return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
  (void)vm;
  (void)reserved;
}
