#include <cstring>

#include "darwin_jni_shorty.h"
#include "darwin_runtime_adapters_internal.h"

namespace android {

void* ProxyCurrentEnv(void*) { return CurrentArtEnv(); }

void* ProxyFindClass(void* context, const char* name) {
  auto* library = static_cast<ElfLibrary*>(context);
  JNIEnv* art_env = CurrentArtEnv();
  if (library == nullptr || art_env == nullptr || name == nullptr) {
    return nullptr;
  }
  void* clazz = art_env->FindClass(name);
  if (library->fixture_graph && clazz != nullptr &&
      std::strcmp(name, "darwin/art/nativefixture/NativeFixture") == 0) {
    g_elf_fixture_status.fetch_or(kElfFoundFixtureClass, std::memory_order_relaxed);
  }
  return clazz;
}

}  // namespace android
