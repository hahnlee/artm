#pragma once

#include <jni.h>

#include <string>

namespace darwin_art_elf_probe {

// Exercises the Android NativeLoader/NativeBridge path without making the
// process orchestration TU own ELF graph policy.
bool run_android_elf_self_test(JNIEnv* env, JavaVM* vm, jobject class_loader,
                               const char* path, std::string* error);

}  // namespace darwin_art_elf_probe
