#pragma once

#include <jni.h>

#include "runtime_graphics_state.h"

namespace art {
class Thread;
}

namespace darwin_art_presentation {

// Builds the detached Activity/PhoneWindow/DecorView hierarchy and presents its
// Android-owned frame. All references created by this operation are released
// before returning; the caller retains only the app bootstrap classes it owns.
int run(JNIEnv* env, art::Thread* self, jobject activity_instance,
         jclass probe_activity_class, jclass probe_context_class,
         jclass probe_resources_class, jclass probe_view_class,
         jclass probe_canvas_class, jclass content_root_class,
         jobject package_manager, bool run_apk_app,
         bool use_framework_resources, bool expect_apk_widgets,
         bool run_framework_button, jint window_scale,
         const char* framework_res_apk, const char* apk_app_package,
         const char* apk_app_activity,
         darwin_art_graphics::GraphicsState* graphics_state);

}  // namespace darwin_art_presentation

