#ifndef DARWIN_ART_COMPAT_HWUI_GPU_MODE_H_
#define DARWIN_ART_COMPAT_HWUI_GPU_MODE_H_

namespace darwin_art {

// GPU mode is the Darwin production path.  A CPU build is available only for
// source/registrar diagnostics; an application launcher must not silently
// downgrade when Metal is unavailable.
inline bool hwui_gpu_enabled() {
#if defined(DARWIN_ART_HWUI_GPU)
    // Production graphics is intentionally GPU-only.  CPU rendering is a
    // separate registrar/layout diagnostic build and is never selected by an
    // application environment variable.
    return true;
#else
    return false;
#endif
}

}  // namespace darwin_art

#endif  // DARWIN_ART_COMPAT_HWUI_GPU_MODE_H_
