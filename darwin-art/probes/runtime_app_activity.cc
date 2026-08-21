#include "runtime_app_activity.h"

#include <iostream>

#include "mirror/throwable.h"
#include "thread-current-inl.h"

namespace darwin_art_app_activity {

int prepare(JNIEnv* env, art::Thread* self, jobject activity_instance,
            jclass probe_activity_class, jclass probe_context_class,
            const darwin_art_app_resources::Bundle* resources,
            jobject package_manager, bool run_apk_app,
            bool use_framework_resources, const char* apk_app_package,
            const char* apk_app_activity, Bundle* out) {
  if (env == nullptr || self == nullptr || activity_instance == nullptr ||
      probe_activity_class == nullptr || probe_context_class == nullptr ||
      resources == nullptr || package_manager == nullptr || out == nullptr) {
    return 27;
  }
  *out = {};
  out->activity_class = env->GetSuperclass(probe_activity_class);
  jclass intent_class = env->FindClass("android/content/Intent");
  jclass component_name_class =
      env->FindClass("android/content/ComponentName");
  jclass configuration_class =
      env->FindClass("android/content/res/Configuration");
  jmethodID configuration_constructor =
      configuration_class == nullptr
          ? nullptr
          : env->GetMethodID(configuration_class, "<init>", "()V");
  jmethodID probe_context_constructor = env->GetMethodID(
      probe_context_class, "<init>",
      "(Landroid/content/res/Resources;"
      "Landroid/content/pm/PackageManager;)V");
  out->probe_context =
      probe_context_constructor == nullptr || resources->probe_resources == nullptr
          ? nullptr
          : env->NewObject(probe_context_class, probe_context_constructor,
                           resources->probe_resources, package_manager);
  if (out->probe_context == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android window: ProbeContext construction failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 27;
  }

  jmethodID intent_constructor =
      intent_class == nullptr
          ? nullptr
          : env->GetMethodID(intent_class, "<init>", "()V");
  jmethodID component_name_constructor =
      component_name_class == nullptr
          ? nullptr
          : env->GetMethodID(component_name_class, "<init>",
                             "(Ljava/lang/String;Ljava/lang/String;)V");
  jmethodID set_component =
      intent_class == nullptr
          ? nullptr
          : env->GetMethodID(intent_class, "setComponent",
                             "(Landroid/content/ComponentName;)"
                             "Landroid/content/Intent;");
  jstring package_name = env->NewStringUTF(
      run_apk_app ? apk_app_package : "dev.darwinart.probe");
  jstring class_name = env->NewStringUTF(
      run_apk_app ? apk_app_activity : "dev.darwinart.probe.ProbeActivity");
  jstring title =
      env->NewStringUTF(run_apk_app ? "Darwin ART APK" : "Darwin ART Probe");
  jobject component_name =
      component_name_constructor == nullptr
          ? nullptr
          : env->NewObject(component_name_class, component_name_constructor,
                           package_name, class_name);
  jobject intent = intent_constructor == nullptr
                       ? nullptr
                       : env->NewObject(intent_class, intent_constructor);
  jobject configuration =
      configuration_constructor == nullptr
          ? nullptr
          : env->NewObject(configuration_class, configuration_constructor);
  if (intent != nullptr && set_component != nullptr && component_name != nullptr) {
    jobject configured_intent =
        env->CallObjectMethod(intent, set_component, component_name);
    env->DeleteLocalRef(configured_intent);
  }
  static constexpr const char* kActivityAttachSignature =
      "(Landroid/content/Context;Landroid/app/ActivityThread;"
      "Landroid/app/Instrumentation;Landroid/os/IBinder;I"
      "Landroid/app/Application;Landroid/content/Intent;"
      "Landroid/content/pm/ActivityInfo;Ljava/lang/CharSequence;"
      "Landroid/app/Activity;Ljava/lang/String;"
      "Landroid/app/Activity$NonConfigurationInstances;"
      "Landroid/content/res/Configuration;Ljava/lang/String;"
      "Lcom/android/internal/app/IVoiceInteractor;Landroid/view/Window;"
      "Landroid/view/ViewRootImpl$ActivityConfigCallback;"
      "Landroid/os/IBinder;Landroid/os/IBinder;)V";
  jmethodID attach_activity =
      out->activity_class == nullptr
          ? nullptr
          : env->GetMethodID(out->activity_class, "attach",
                             kActivityAttachSignature);
  if (resources->activity_info == nullptr || resources->application == nullptr ||
      intent == nullptr || configuration == nullptr ||
      attach_activity == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android window: Activity.attach() setup failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 27;
  }
  out->context_theme_wrapper_class =
      env->FindClass("android/view/ContextThemeWrapper");
  jmethodID attach_base_context =
      out->context_theme_wrapper_class == nullptr
          ? nullptr
          : env->GetMethodID(out->context_theme_wrapper_class,
                             "attachBaseContext",
                             "(Landroid/content/Context;)V");
  if (!run_apk_app && attach_base_context != nullptr) {
    env->CallNonvirtualVoidMethod(activity_instance,
                                  out->context_theme_wrapper_class,
                                  attach_base_context, out->probe_context);
  }
  if ((!run_apk_app && attach_base_context == nullptr) ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android window: base Context preparation failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 30;
  }
  env->CallNonvirtualVoidMethod(
      activity_instance, out->activity_class, attach_activity,
      out->probe_context, nullptr, nullptr, nullptr, static_cast<jint>(1),
      resources->application, intent, resources->activity_info, title, nullptr,
      nullptr, nullptr, configuration, nullptr, nullptr, nullptr, nullptr,
      nullptr, nullptr);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android window: Activity.attach() threw\n"
              << self->GetException()->Dump() << "\n";
    return 30;
  }
  jmethodID get_window = env->GetMethodID(
      out->activity_class, "getWindow", "()Landroid/view/Window;");
  out->window = get_window == nullptr
                    ? nullptr
                    : env->CallObjectMethod(activity_instance, get_window);
  out->window_class = env->FindClass("android/view/Window");
  out->phone_window_class =
      env->FindClass("com/android/internal/policy/PhoneWindow");
  if (out->window == nullptr || out->phone_window_class == nullptr ||
      !env->IsInstanceOf(out->window, out->phone_window_class) ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android window: PhoneWindow attachment failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 31;
  }
  jmethodID get_probe_theme = env->GetMethodID(
      probe_context_class, "getTheme",
      "()Landroid/content/res/Resources$Theme;");
  out->probe_theme = get_probe_theme == nullptr
                         ? nullptr
                         : env->CallObjectMethod(out->probe_context,
                                                 get_probe_theme);
  if (use_framework_resources && out->probe_theme != nullptr) {
    jclass theme_class = env->GetObjectClass(out->probe_theme);
    jclass framework_style_class = env->FindClass("android/R$style");
    jfieldID framework_light_no_action_bar =
        framework_style_class == nullptr
            ? nullptr
            : env->GetStaticFieldID(framework_style_class,
                                    "Theme_Material_Light_NoActionBar", "I");
    jmethodID apply_style =
        theme_class == nullptr
            ? nullptr
            : env->GetMethodID(theme_class, "applyStyle", "(IZ)V");
    if (framework_light_no_action_bar != nullptr && apply_style != nullptr) {
      const jint style = env->GetStaticIntField(
          framework_style_class, framework_light_no_action_bar);
      env->CallVoidMethod(out->probe_theme, apply_style, style, JNI_TRUE);
    }
    env->DeleteLocalRef(framework_style_class);
    env->DeleteLocalRef(theme_class);
  }
  jmethodID set_activity_theme =
      env->GetMethodID(out->context_theme_wrapper_class, "setTheme",
                       "(Landroid/content/res/Resources$Theme;)V");
  if (out->probe_theme == nullptr || set_activity_theme == nullptr ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android window: Activity theme setup failed\n";
    return 31;
  }
  env->CallVoidMethod(activity_instance, set_activity_theme, out->probe_theme);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android window: Activity.setTheme() threw\n"
              << self->GetException()->Dump() << "\n";
    return 31;
  }
  env->DeleteLocalRef(configuration);
  env->DeleteLocalRef(component_name);
  env->DeleteLocalRef(intent);
  env->DeleteLocalRef(title);
  env->DeleteLocalRef(class_name);
  env->DeleteLocalRef(package_name);
  env->DeleteLocalRef(configuration_class);
  env->DeleteLocalRef(component_name_class);
  env->DeleteLocalRef(intent_class);
  return 0;
}

void release(JNIEnv* env, Bundle* bundle) {
  if (env == nullptr || bundle == nullptr) {
    return;
  }
  env->DeleteLocalRef(bundle->probe_theme);
  env->DeleteLocalRef(bundle->window);
  env->DeleteLocalRef(bundle->phone_window_class);
  env->DeleteLocalRef(bundle->window_class);
  env->DeleteLocalRef(bundle->context_theme_wrapper_class);
  env->DeleteLocalRef(bundle->activity_class);
  env->DeleteLocalRef(bundle->probe_context);
  *bundle = {};
}

}  // namespace darwin_art_app_activity
