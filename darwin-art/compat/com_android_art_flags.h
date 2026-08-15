#pragma once

// Minimal aconfig surface for the interpreter-only Darwin bootstrap. AOSP's
// generated header contains many release flags; this first native runtime gate
// disables optional always-on profiling.
namespace com::android::art::flags {

inline bool always_enable_profile_code() {
  return false;
}

}  // namespace com::android::art::flags
