#include "darwin_art_liblog_provider.h"

#include <android/log.h>

#include <cstring>

namespace {

struct Entry {
  const char* name;
  uintptr_t address;
};

#define LIBLOG_ENTRY(symbol) {#symbol, reinterpret_cast<uintptr_t>(&symbol)}

// Exact sorted dynsym surface of the NDK r28c API 35 liblog.so stub.
const Entry kEntries[] = {
    LIBLOG_ENTRY(__android_log_assert),
    LIBLOG_ENTRY(__android_log_buf_print),
    LIBLOG_ENTRY(__android_log_buf_write),
    LIBLOG_ENTRY(__android_log_call_aborter),
    LIBLOG_ENTRY(__android_log_default_aborter),
    LIBLOG_ENTRY(__android_log_get_minimum_priority),
    LIBLOG_ENTRY(__android_log_is_loggable),
    LIBLOG_ENTRY(__android_log_is_loggable_len),
    LIBLOG_ENTRY(__android_log_logd_logger),
    LIBLOG_ENTRY(__android_log_print),
    LIBLOG_ENTRY(__android_log_set_aborter),
    LIBLOG_ENTRY(__android_log_set_default_tag),
    LIBLOG_ENTRY(__android_log_set_logger),
    LIBLOG_ENTRY(__android_log_set_minimum_priority),
    LIBLOG_ENTRY(__android_log_stderr_logger),
    LIBLOG_ENTRY(__android_log_vprint),
    LIBLOG_ENTRY(__android_log_write),
    LIBLOG_ENTRY(__android_log_write_log_message),
};

static_assert(sizeof(kEntries) / sizeof(kEntries[0]) == DARWIN_ART_LIBLOG_PROVIDER_COUNT);

}  // namespace

extern "C" size_t darwin_art_liblog_provider_count() {
  return sizeof(kEntries) / sizeof(kEntries[0]);
}

extern "C" const char* darwin_art_liblog_provider_name(uint32_t ordinal) {
  return ordinal < darwin_art_liblog_provider_count() ? kEntries[ordinal].name : nullptr;
}

extern "C" uintptr_t darwin_art_liblog_provider_address(uint32_t ordinal) {
  return ordinal < darwin_art_liblog_provider_count() ? kEntries[ordinal].address : 0;
}

extern "C" uintptr_t darwin_art_liblog_provider_resolve(const char* symbol,
                                                          const char* version) {
  if (symbol == nullptr || (version != nullptr && version[0] != '\0')) return 0;
  for (const Entry& entry : kEntries) {
    if (std::strcmp(entry.name, symbol) == 0) return entry.address;
  }
  return 0;
}

