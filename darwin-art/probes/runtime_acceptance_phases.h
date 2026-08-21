#pragma once

#include <jni.h>

extern "C" int darwin_art_network_load_fixture(JNIEnv* env,
                                                  const char* fixture_path,
                                                  jobject app_loader,
                                                  jclass fixture_class);

namespace darwin_art_network_phase {

// Run the network fixture through the same JavaVMExt/JNI path as the main
// process, while keeping the loopback server and network acceptance policy in
// their own translation unit.
int run(JNIEnv* env, const char* fixture_path, jobject app_loader,
        jclass fixture_class);

}  // namespace darwin_art_network_phase
