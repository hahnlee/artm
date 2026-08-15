#ifndef DARWIN_ANDROID_JNI_TRAMPOLINE_H_
#define DARWIN_ANDROID_JNI_TRAMPOLINE_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace darwin_art::android_jni {

struct FixtureTrampolineSet;

// Creates exactly the two reviewed fixture thunks and publishes their RX range
// to NativeBridge pointer classification. The Android targets and proxy JNIEnv
// must remain alive until DestroyFixtureTrampolines after external quiescence.
FixtureTrampolineSet* CreateFixtureTrampolines(void* proxy_jni_env,
                                               void* native_add,
                                               void* native_spill,
                                               std::string* error);

void DestroyFixtureTrampolines(FixtureTrampolineSet* trampolines);

void* FixtureNativeAddEntry(const FixtureTrampolineSet* trampolines);
void* FixtureNativeSpillEntry(const FixtureTrampolineSet* trampolines);
uint64_t FixtureTrampolineGeneration(const FixtureTrampolineSet* trampolines);
size_t FixtureTrampolineLiveCount();

constexpr uint32_t kFixtureNativeAddEntryMask = 1u << 0;
constexpr uint32_t kFixtureNativeSpillEntryMask = 1u << 1;

// Returns a stable one-bit identity for either callable entry. Repeated
// classification of one entry cannot be mistaken for observing both thunks.
uint32_t FixtureTrampolineEntryMask(const void* pointer);

// Returns true only for a published callable entry, not for literals or an
// arbitrary address elsewhere in the executable allocation.
bool IsFixtureTrampolineEntry(const void* pointer);

}  // namespace darwin_art::android_jni

#endif  // DARWIN_ANDROID_JNI_TRAMPOLINE_H_
