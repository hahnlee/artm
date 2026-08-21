#include "runtime_app_presentation.h"

#include <iostream>

#include "runtime_graphics_phase.h"
#include "runtime_jni_scope.h"
#include "runtime_app_resources.h"
#include "runtime_app_activity.h"
#include "mirror/throwable.h"
#include "thread-current-inl.h"

namespace darwin_art_presentation {

int run(JNIEnv* env, art::Thread* self, jobject activity_instance,
         jclass probe_activity_class, jclass probe_context_class,
         jclass probe_resources_class, jclass probe_view_class,
         jclass probe_canvas_class, jclass content_root_class,
         jobject package_manager, bool run_apk_app,
         bool use_framework_resources, bool expect_apk_widgets,
         bool run_framework_button, jint window_scale,
         const char* framework_res_apk, const char* apk_app_package,
         const char* apk_app_activity,
         darwin_art_graphics::GraphicsState* graphics_state) {
  // Every construction step below has multiple fail-fast returns.  Keep all
  // JNI locals created by the detached Activity/Window transaction in one
  // frame so an exception or an incomplete framework resource cannot leak a
  // local reference into the ART owner thread.
  darwin_art_jni_scope::ScopedLocalFrame local_frame(env);
  if (!local_frame.valid()) {
    std::cerr << "ART Android window: JNI local frame allocation failed\n";
    return 26;
  }
  constexpr jint kApkFrameWidth = 360;
  constexpr jint kApkFrameHeight = 640;
  darwin_art_app_resources::Bundle resources;
  if (darwin_art_app_resources::prepare(
          env, probe_resources_class, use_framework_resources, window_scale,
          framework_res_apk, &resources) != 0) {
    std::cerr << "ART Android resources: bootstrap construction failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 27;
  }
  darwin_art_app_activity::Bundle activity;
  if (darwin_art_app_activity::prepare(
          env, self, activity_instance, probe_activity_class,
          probe_context_class, &resources, package_manager, run_apk_app,
          use_framework_resources, apk_app_package, apk_app_activity,
          &activity) != 0) {
    return 27;
  }
  jclass window_class = activity.window_class;
  jclass phone_window_class = activity.phone_window_class;
  jobject window = activity.window;
  jobject probe_theme = activity.probe_theme;
  jobject probe_resources = resources.probe_resources;

  // A detached hierarchy should observe accessibility as disabled until the
  // Darwin service bridge exists. Seed the framework singleton without
  // invoking its Binder-backed constructor.
  jclass accessibility_class =
      env->FindClass("android/view/accessibility/AccessibilityManager");
  jobject accessibility =
      accessibility_class == nullptr ? nullptr : env->AllocObject(accessibility_class);
  jclass object_class = env->FindClass("java/lang/Object");
  jmethodID object_constructor =
      object_class == nullptr
          ? nullptr
          : env->GetMethodID(object_class, "<init>", "()V");
  jobject accessibility_lock =
      object_constructor == nullptr
          ? nullptr
          : env->NewObject(object_class, object_constructor);
  jfieldID accessibility_lock_field =
      accessibility_class == nullptr
          ? nullptr
          : env->GetFieldID(accessibility_class, "mLock", "Ljava/lang/Object;");
  jfieldID accessibility_instance =
      accessibility_class == nullptr
          ? nullptr
          : env->GetStaticFieldID(
                accessibility_class, "sInstance",
                "Landroid/view/accessibility/AccessibilityManager;");
  if (accessibility == nullptr || accessibility_lock == nullptr ||
      accessibility_lock_field == nullptr || accessibility_instance == nullptr ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android window: accessibility stub setup failed\n";
    return 31;
  }
  env->SetObjectField(accessibility, accessibility_lock_field,
                      accessibility_lock);
  env->SetStaticObjectField(accessibility_class, accessibility_instance,
                            accessibility);

  jmethodID get_window_attributes =
      window_class == nullptr
          ? nullptr
          : env->GetMethodID(window_class, "getAttributes",
                             "()Landroid/view/WindowManager$LayoutParams;");
  jobject window_attributes =
      get_window_attributes == nullptr
          ? nullptr
          : env->CallObjectMethod(window, get_window_attributes);
  jclass decor_view_class =
      env->FindClass("com/android/internal/policy/DecorView");
  jmethodID decor_view_constructor =
      decor_view_class == nullptr
          ? nullptr
          : env->GetMethodID(
                decor_view_class, "<init>",
                "(Landroid/content/Context;I"
                "Lcom/android/internal/policy/PhoneWindow;"
                "Landroid/view/WindowManager$LayoutParams;)V");
  jobject decor_view =
      decor_view_constructor == nullptr || window_attributes == nullptr
          ? nullptr
          : env->NewObject(decor_view_class, decor_view_constructor,
                           activity_instance, static_cast<jint>(-1), window,
                           window_attributes);
  // PhoneWindow.installDecor() normally resolves windowBackground from the
  // active Theme and installs it on DecorView. The standalone launcher builds
  // the same objects directly, so preserve that framework-owned resource path
  // explicitly instead of substituting a host color.
  jobject window_background = nullptr;
  if (use_framework_resources && decor_view != nullptr) {
    jclass typed_value_class = env->FindClass("android/util/TypedValue");
    jmethodID typed_value_constructor =
        typed_value_class == nullptr
            ? nullptr
            : env->GetMethodID(typed_value_class, "<init>", "()V");
    jobject typed_value =
        typed_value_constructor == nullptr
            ? nullptr
            : env->NewObject(typed_value_class, typed_value_constructor);
    jclass theme_class = env->GetObjectClass(probe_theme);
    jmethodID resolve_attribute =
        theme_class == nullptr
            ? nullptr
            : env->GetMethodID(theme_class, "resolveAttribute",
                               "(ILandroid/util/TypedValue;Z)Z");
    jclass framework_attr_class = env->FindClass("android/R$attr");
    jfieldID window_background_attr =
        framework_attr_class == nullptr
            ? nullptr
            : env->GetStaticFieldID(framework_attr_class, "windowBackground",
                                    "I");
    jfieldID typed_value_resource_id =
        typed_value_class == nullptr
            ? nullptr
            : env->GetFieldID(typed_value_class, "resourceId", "I");
    jmethodID get_drawable =
        probe_resources_class == nullptr
            ? nullptr
            : env->GetMethodID(
                  probe_resources_class, "getDrawable",
                  "(ILandroid/content/res/Resources$Theme;)"
                  "Landroid/graphics/drawable/Drawable;");
    if (typed_value != nullptr && resolve_attribute != nullptr &&
        window_background_attr != nullptr &&
        typed_value_resource_id != nullptr && get_drawable != nullptr) {
      const jint attr = env->GetStaticIntField(framework_attr_class,
                                               window_background_attr);
      const jboolean resolved = env->CallBooleanMethod(
          probe_theme, resolve_attribute, attr, typed_value, JNI_TRUE);
      const jint resource_id =
          resolved == JNI_TRUE && !env->ExceptionCheck()
              ? env->GetIntField(typed_value, typed_value_resource_id)
              : 0;
      if (resource_id != 0) {
        window_background = env->CallObjectMethod(
            probe_resources, get_drawable, resource_id, probe_theme);
      }
    }
    env->DeleteLocalRef(framework_attr_class);
    env->DeleteLocalRef(theme_class);
    env->DeleteLocalRef(typed_value);
    env->DeleteLocalRef(typed_value_class);
  }
  jmethodID content_root_constructor =
      content_root_class == nullptr
          ? nullptr
          : env->GetMethodID(content_root_class, "<init>",
                             "(Landroid/content/Context;)V");
  jobject content_root =
      content_root_constructor == nullptr
          ? nullptr
          : env->NewObject(content_root_class, content_root_constructor,
                           activity_instance);
  jmethodID add_view =
      decor_view_class == nullptr
          ? nullptr
          : env->GetMethodID(decor_view_class, "addView",
                             "(Landroid/view/View;)V");
  jfieldID phone_decor =
      phone_window_class == nullptr
          ? nullptr
          : env->GetFieldID(phone_window_class, "mDecor",
                            "Lcom/android/internal/policy/DecorView;");
  jfieldID phone_content_parent =
      phone_window_class == nullptr
          ? nullptr
          : env->GetFieldID(phone_window_class, "mContentParent",
                            "Landroid/view/ViewGroup;");
  if (decor_view == nullptr ||
      (use_framework_resources && window_background == nullptr) ||
      content_root == nullptr || add_view == nullptr ||
      phone_decor == nullptr || phone_content_parent == nullptr ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android window: DecorView setup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 31;
  }
  env->CallVoidMethod(decor_view, add_view, content_root);
  env->SetObjectField(window, phone_decor, decor_view);
  env->SetObjectField(window, phone_content_parent, content_root);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android window: DecorView attachment threw\n"
              << self->GetException()->Dump() << "\n";
    return 31;
  }

  jmethodID probe_on_create =
      run_apk_app
          ? env->GetMethodID(probe_activity_class, "onCreate",
                             "(Landroid/os/Bundle;)V")
          : env->GetMethodID(probe_activity_class, "probeOnCreate", "()I");
  jint lifecycle_result = -1;
  if (probe_on_create != nullptr) {
    if (run_apk_app) {
      env->CallVoidMethod(activity_instance, probe_on_create, nullptr);
      lifecycle_result = env->ExceptionCheck() ? -1 : 43;
    } else {
      lifecycle_result =
          env->CallIntMethod(activity_instance, probe_on_create);
    }
  }
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android lifecycle: Activity.onCreate() threw\n"
              << self->GetException()->Dump() << "\n";
    return 28;
  }
  // PhoneWindow applies the resolved theme background while installing its
  // decor.  The standalone launcher supplies the decor before Activity's
  // setContentView(), so finish the same Android-owned operation after the
  // activity has installed its content.  Going through PhoneWindow keeps the
  // Drawable callback/window-background state in the framework path.
  if (use_framework_resources && window_background != nullptr) {
    jmethodID set_window_background =
        phone_window_class == nullptr
            ? nullptr
            : env->GetMethodID(
                  phone_window_class, "setBackgroundDrawable",
                  "(Landroid/graphics/drawable/Drawable;)V");
    if (set_window_background == nullptr || env->ExceptionCheck()) {
      std::cerr << "ART Android window: PhoneWindow background setup failed\n";
      env->ExceptionClear();
      return 31;
    }
    env->CallVoidMethod(window, set_window_background, window_background);
    if (env->ExceptionCheck()) {
      std::cerr << "ART Android window: PhoneWindow background setup threw\n"
                << self->GetException()->Dump() << "\n";
      return 31;
    }
  }
  // We just installed this exact DecorView in PhoneWindow.mDecor above.  The
  // detached probe Window has no ViewRoot to lazily materialize a decor, so
  // relying on PhoneWindow.getDecorView() here can legitimately return null.
  // Keep the authoritative local object instead.
  jmethodID get_child_at =
      content_root_class == nullptr
          ? nullptr
          : env->GetMethodID(content_root_class, "getChildAt",
                             "(I)Landroid/view/View;");
  jobject probe_view =
      get_child_at == nullptr
          ? nullptr
          : env->CallObjectMethod(content_root, get_child_at,
                                  static_cast<jint>(0));
  if (graphics_state != nullptr &&
      darwin_art_graphics_phase::present_and_retain(
          graphics_state, env, decor_view, content_root_class, content_root,
          probe_view_class, probe_view, run_apk_app, expect_apk_widgets,
          run_apk_app || run_framework_button, kApkFrameWidth * window_scale,
          kApkFrameHeight * window_scale) != 0) {
    return 33;
  }
  darwin_art_app_activity::release(env, &activity);
  darwin_art_app_resources::release(env, &resources);
  env->DeleteLocalRef(window_background);
  env->DeleteLocalRef(content_root);
  env->DeleteLocalRef(decor_view);
  env->DeleteLocalRef(decor_view_class);
  env->DeleteLocalRef(window_attributes);
  env->DeleteLocalRef(accessibility_lock);
  env->DeleteLocalRef(object_class);
  env->DeleteLocalRef(accessibility);
  env->DeleteLocalRef(accessibility_class);
  env->DeleteLocalRef(probe_view);
  env->DeleteLocalRef(probe_resources_class);
  env->DeleteLocalRef(probe_view_class);
  env->DeleteLocalRef(probe_canvas_class);
  env->DeleteLocalRef(content_root_class);
  env->DeleteLocalRef(probe_context_class);
  env->DeleteLocalRef(activity_instance);
  if (lifecycle_result != 43) {
    std::cerr << "ART Android lifecycle: expected 43, got " << lifecycle_result
              << "\n";
    return 29;
  }

  return 0;
}

}  // namespace darwin_art_presentation
