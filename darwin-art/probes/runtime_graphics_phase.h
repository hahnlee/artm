#pragma once

#include <jni.h>

namespace darwin_art_graphics_phase {

// Completes the Android-owned content presentation step after Activity setup.
// The heavy RenderNode/Metal implementation remains in runtime_graphics_probe;
// this phase only owns validation and the short-lived JNI orchestration.
int present_and_retain(JNIEnv* env, jobject decor_view,
                       jclass content_root_class, jobject content_root,
                       jclass probe_view_class, jobject probe_view,
                       bool run_apk_app, bool expect_apk_widgets,
                       bool retain_interactive, jint width, jint height);

}  // namespace darwin_art_graphics_phase
