#ifndef DARWIN_ANDROID_JNI_TRAMPOLINE_H_
#define DARWIN_ANDROID_JNI_TRAMPOLINE_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace darwin_art::android_jni {

struct TrampolineSet;

struct TrampolineRequest {
  void* android_target;
  const char* shorty;
  uint32_t entry_mask;
};

// Creates regular-JNI Darwin-entry-to-Android thunks. A shorty consists of a
// return type followed by argument types and supports only scalar/reference
// JNI types Z/B/C/S/I/J/F/D/L/V (V is return-only). GP and FP argument banks
// are planned independently. Darwin's naturally packed stack tail is copied
// into Android AAPCS64's eight-byte argument slots. Aggregates, HFA, varargs,
// and CriticalNative are outside this API and therefore fail closed.
// The returned owner must outlive every call; the caller must establish
// external invocation quiescence before destroying it.
TrampolineSet* CreateRegularTrampolines(void* proxy_jni_env,
                                        const TrampolineRequest* requests,
                                        size_t request_count,
                                        std::string* error);

void DestroyRegularTrampolines(TrampolineSet* trampolines);

size_t TrampolineCount(const TrampolineSet* trampolines);
void* TrampolineEntry(const TrampolineSet* trampolines, size_t index);
uint64_t TrampolineGeneration(const TrampolineSet* trampolines);
size_t TrampolineLiveCount();

// Returns the caller-supplied stable identity of a published callable entry,
// never of a literal or an arbitrary address in the RX allocation.
uint32_t TrampolineEntryMask(const void* pointer);
bool IsTrampolineEntry(const void* pointer);

}  // namespace darwin_art::android_jni

#endif  // DARWIN_ANDROID_JNI_TRAMPOLINE_H_
