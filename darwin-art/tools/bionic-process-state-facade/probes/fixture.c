#include <asm/hwcap.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <sys/system_properties.h>

static const char* gEnvironment;
static const unsigned char* gRandom;

static int Equal(const char* left, const char* right) {
  while (*left == *right && *left != '\0') {
    ++left;
    ++right;
  }
  return *left == *right;
}

__attribute__((visibility("default"))) int bionic_process_fixture_basic(void) {
  gEnvironment = getenv("ANDROID_ROOT");
  if (gEnvironment == NULL || !Equal(gEnvironment, "/system") ||
      getenv("ANDROID_ROOT") != gEnvironment) return 1;
  errno = 777;
  if (getenv("HOME") != NULL || getenv("HOST_SECRET") != NULL || errno != 777)
    return 2;

  char value[PROP_VALUE_MAX];
  int length = __system_property_get("ro.build.version.sdk", value);
  if (length != 2 || !Equal(value, "36")) return 3;
  length = __system_property_get("test.boundary", value);
  if (length != PROP_VALUE_MAX - 1 || value[0] != 'X' ||
      value[PROP_VALUE_MAX - 2] != 'X' || value[PROP_VALUE_MAX - 1] != '\0')
    return 4;
  value[0] = 'Q';
  errno = 778;
  if (__system_property_get("missing.property", value) != 0 || value[0] != '\0' ||
      errno != 778) return 5;

  if (getauxval(AT_PAGESZ) != 16384 ||
      getauxval(AT_HWCAP) != (HWCAP_FP | HWCAP_ASIMD)) return 6;
  errno = 779;
  if (getauxval(AT_HWCAP2) != 0 || getauxval(AT_SECURE) != 0 || errno != 779)
    return 7;
  gRandom = (const unsigned char*)getauxval(AT_RANDOM);
  if (gRandom == NULL || gRandom[0] != 0x10 || gRandom[15] != 0x0f ||
      (const unsigned char*)getauxval(AT_RANDOM) != gRandom) return 8;
  errno = 0;
  if (getauxval(0x7fffffffUL) != 0 || errno != ENOENT) return 9;
  return 42;
}

__attribute__((visibility("default"))) int
bionic_process_fixture_concurrent(void) {
  char value[PROP_VALUE_MAX];
  if (getenv("ANDROID_ROOT") != gEnvironment ||
      __system_property_get("ro.product.cpu.abi", value) != 9 ||
      !Equal(value, "arm64-v8a") || getauxval(AT_RANDOM) != (unsigned long)gRandom ||
      getauxval(AT_PAGESZ) != 16384) return 10;
  return 42;
}

__attribute__((visibility("default"))) int
bionic_process_fixture_verify_pointers(void) {
  if (!Equal(gEnvironment, "/system") || gRandom[0] != 0x10 ||
      gRandom[15] != 0x0f) return 11;
  return 42;
}

__attribute__((visibility("default"))) int
bionic_process_fixture_after_teardown(void) {
  char value[PROP_VALUE_MAX] = {'Q'};
  errno = 0;
  if (getenv("ANDROID_ROOT") != NULL || errno != EIO) return 12;
  errno = 0;
  if (__system_property_get("ro.build.version.sdk", value) != 0 ||
      value[0] != '\0' || errno != EIO) return 13;
  errno = 0;
  if (getauxval(AT_PAGESZ) != 0 || errno != EIO) return 14;
  return 42;
}
