#ifndef DARWIN_ART_COMPAT_HWUI_GPU_MODE_H_
#define DARWIN_ART_COMPAT_HWUI_GPU_MODE_H_

#include <cstdlib>
#include <cstring>

namespace darwin_art {

// GPU mode is intentionally opt-in.  The compile-time define keeps the
// upstream host build unchanged, while the environment switch makes the
// mode selectable by the Darwin launcher without pretending that Android
// system properties exist on macOS.
inline bool hwui_gpu_enabled() {
#if defined(DARWIN_ART_HWUI_GPU)
    const char* value = std::getenv("DARWIN_ART_HWUI_GPU");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0 &&
            std::strcmp(value, "false") != 0 && std::strcmp(value, "off") != 0;
#else
    return false;
#endif
}

}  // namespace darwin_art

#endif  // DARWIN_ART_COMPAT_HWUI_GPU_MODE_H_
