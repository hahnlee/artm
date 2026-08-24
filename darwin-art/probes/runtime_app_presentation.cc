#include "runtime_app_presentation.h"

#include <iostream>
#include <cstdlib>

#include "runtime_graphics_phase.h"
#include "runtime_graphics_gpu.h"
#include "runtime_graphics_state.h"
#include "runtime_jni_scope.h"
#include "runtime_app_resources.h"
#include "runtime_app_activity.h"
#include "mirror/throwable.h"
#include "thread-current-inl.h"

namespace darwin_art_presentation {

namespace {

bool attach_android_window(JNIEnv* env, jobject activity, jobject window,
                           jobject decor_view, jobject window_attributes,
                           darwin_art_graphics::GraphicsState* graphics_state) {
  if (env == nullptr || activity == nullptr || window == nullptr ||
      decor_view == nullptr || window_attributes == nullptr ||
      graphics_state == nullptr) {
    return false;
  }
  jclass activity_class = env->GetObjectClass(activity);
  jmethodID get_display =
      activity_class == nullptr
          ? nullptr
          : env->GetMethodID(activity_class, "getDisplay",
                             "()Landroid/view/Display;");
  jobject display =
      get_display == nullptr ? nullptr : env->CallObjectMethod(activity, get_display);
  jclass params_class =
      env->FindClass("android/view/WindowManager$LayoutParams");
  jfieldID flags_field = params_class == nullptr
                             ? nullptr
                             : env->GetFieldID(params_class, "flags", "I");
  if (flags_field != nullptr && !env->ExceptionCheck()) {
    // The Darwin compatibility runtime owns the Metal RenderNode target.  A
    // framework ThreadedRenderer would try to allocate a second Android
    // SurfaceControl, so keep ViewRoot's traversal/input lifecycle while the
    // existing HWUI owner performs the sole GPU submission.
    constexpr jint kFlagHardwareAccelerated = 0x01000000;
    const jint flags = env->GetIntField(window_attributes, flags_field);
    env->SetIntField(window_attributes, flags_field,
                     flags & ~kFlagHardwareAccelerated);
  }
  jclass global_class = env->FindClass("android/view/WindowManagerGlobal");
  jmethodID get_instance =
      global_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(global_class, "getInstance",
                                   "()Landroid/view/WindowManagerGlobal;");
  jobject global =
      get_instance == nullptr
          ? nullptr
          : env->CallStaticObjectMethod(global_class, get_instance);
  jmethodID add_view =
      global_class == nullptr
          ? nullptr
          : env->GetMethodID(
                global_class, "addView",
                "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;"
                "Landroid/view/Display;Landroid/view/Window;I)V");
  if (display == nullptr || global == nullptr || add_view == nullptr ||
      env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(global);
    env->DeleteLocalRef(global_class);
    env->DeleteLocalRef(params_class);
    env->DeleteLocalRef(display);
    env->DeleteLocalRef(activity_class);
    return false;
  }
  env->CallVoidMethod(global, add_view, decor_view, window_attributes, display,
                      window, static_cast<jint>(0));
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android window: WindowManagerGlobal.addView failed\n";
    env->ExceptionDescribe();
    env->ExceptionClear();
    env->DeleteLocalRef(global);
    env->DeleteLocalRef(global_class);
    env->DeleteLocalRef(params_class);
    env->DeleteLocalRef(display);
    env->DeleteLocalRef(activity_class);
    return false;
  }

  jfieldID views_field =
      env->GetFieldID(global_class, "mViews", "Ljava/util/ArrayList;");
  jfieldID roots_field =
      env->GetFieldID(global_class, "mRoots", "Ljava/util/ArrayList;");
  jobject views = views_field == nullptr ? nullptr
                                         : env->GetObjectField(global, views_field);
  jobject roots = roots_field == nullptr ? nullptr
                                         : env->GetObjectField(global, roots_field);
  jclass list_class = env->FindClass("java/util/ArrayList");
  jmethodID size = list_class == nullptr
                       ? nullptr
                       : env->GetMethodID(list_class, "size", "()I");
  jmethodID get = list_class == nullptr
                      ? nullptr
                      : env->GetMethodID(list_class, "get",
                                         "(I)Ljava/lang/Object;");
  jobject view_root = nullptr;
  if (views != nullptr && roots != nullptr && size != nullptr && get != nullptr &&
      !env->ExceptionCheck()) {
    const jint count = env->CallIntMethod(views, size);
    for (jint index = count - 1; index >= 0 && !env->ExceptionCheck(); --index) {
      jobject candidate = env->CallObjectMethod(views, get, index);
      const bool matches = candidate != nullptr &&
                           env->IsSameObject(candidate, decor_view) == JNI_TRUE;
      env->DeleteLocalRef(candidate);
      if (matches) {
        view_root = env->CallObjectMethod(roots, get, index);
        break;
      }
    }
  }
  const bool retained =
      view_root != nullptr && !env->ExceptionCheck() &&
      darwin_art_graphics::retain_interactive_view_root(
          graphics_state, env, view_root);
  env->DeleteLocalRef(view_root);
  env->DeleteLocalRef(list_class);
  env->DeleteLocalRef(roots);
  env->DeleteLocalRef(views);
  env->DeleteLocalRef(global);
  env->DeleteLocalRef(global_class);
  env->DeleteLocalRef(params_class);
  env->DeleteLocalRef(display);
  env->DeleteLocalRef(activity_class);
  return retained && !env->ExceptionCheck();
}

}  // namespace

int run(JNIEnv* env, art::Thread* self, jobject activity_instance,
         jclass probe_activity_class, jclass probe_context_class,
         jclass probe_resources_class, jclass probe_view_class,
         jclass probe_canvas_class, jclass content_root_class,
         jobject package_manager, bool run_apk_app,
         bool use_framework_resources, bool expect_apk_widgets,
         bool apk_native_loaded, bool run_framework_button, jint window_scale,
         const char* framework_res_apk, const char* apk_app_package,
         const char* apk_app_activity, const char* app_apk_path,
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
          framework_res_apk, app_apk_path, &resources) != 0) {
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
  if (run_apk_app) {
    // ContextImpl normally carries the application's complete PathClassLoader.
    // Install that exact loader after Activity.attach() and before
    // PhoneWindow/LayoutInflater touches APK XML custom views.
    jclass activity_class = env->GetObjectClass(activity_instance);
    jmethodID get_base_context =
        activity_class == nullptr
            ? nullptr
            : env->GetMethodID(activity_class, "getBaseContext",
                               "()Landroid/content/Context;");
    jobject base_context =
        get_base_context == nullptr
            ? nullptr
            : env->CallObjectMethod(activity_instance, get_base_context);
    jclass base_context_class =
        base_context == nullptr ? nullptr : env->GetObjectClass(base_context);
    jmethodID get_class_loader =
        base_context_class == nullptr
            ? nullptr
            : env->GetMethodID(base_context_class, "getClassLoader",
                               "()Ljava/lang/ClassLoader;");
    jobject app_loader =
        get_class_loader == nullptr
            ? nullptr
            : env->CallObjectMethod(base_context, get_class_loader);
    jmethodID install_loader =
        env->GetStaticMethodID(probe_context_class,
                               "installApplicationClassLoader",
                               "(Ljava/lang/ClassLoader;)V");
    if (install_loader == nullptr || app_loader == nullptr ||
        env->ExceptionCheck()) {
      std::cerr << "ART Android APK: application ClassLoader bootstrap failed"
                << " activity=" << (activity_class != nullptr)
                << " base=" << (base_context != nullptr)
                << " base_class=" << (base_context_class != nullptr)
                << " get_loader=" << (get_class_loader != nullptr)
                << " app_loader=" << (app_loader != nullptr)
                << " install=" << (install_loader != nullptr) << "\n";
      if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
      }
      return 27;
    }
    env->CallStaticVoidMethod(probe_context_class, install_loader, app_loader);
    env->DeleteLocalRef(app_loader);
    env->DeleteLocalRef(base_context_class);
    env->DeleteLocalRef(base_context);
    env->DeleteLocalRef(activity_class);
    if (env->ExceptionCheck()) {
      std::cerr << "ART Android APK: application ClassLoader install failed\n";
      env->ExceptionDescribe();
      env->ExceptionClear();
      return 27;
    }
  }
  jclass window_class = activity.window_class;
  jclass phone_window_class = activity.phone_window_class;
  jobject window = activity.window;
  jobject probe_theme = activity.probe_theme;
  jobject probe_resources = resources.probe_resources;

  // Construct the real framework singleton against the process-local
  // accessibility Binder. Popup ViewRoots register listeners in its internal
  // maps, so an AllocObject shell is not a valid ContextImpl substitute.
  jclass accessibility_class =
      env->FindClass("android/view/accessibility/AccessibilityManager");
  jmethodID get_accessibility_instance =
      accessibility_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(
                accessibility_class, "getInstance",
                "(Landroid/content/Context;)"
                "Landroid/view/accessibility/AccessibilityManager;");
  jobject accessibility =
      get_accessibility_instance == nullptr
          ? nullptr
          : env->CallStaticObjectMethod(accessibility_class,
                                        get_accessibility_instance,
                                        activity.probe_context);
  if (accessibility == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android window: accessibility stub setup failed\n";
    return 31;
  }

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
  jobject decor_view = nullptr;
  jobject content_root = nullptr;
  if (run_apk_app) {
    // Let PhoneWindow install the framework decor selected by the APK theme.
    // This is what requests FEATURE_ACTION_BAR and creates DecorContentParent;
    // constructing DecorView directly leaves Activity.getActionBar() null.
    jmethodID get_decor_view =
        window_class == nullptr
            ? nullptr
            : env->GetMethodID(window_class, "getDecorView",
                               "()Landroid/view/View;");
    jmethodID find_view_by_id =
        window_class == nullptr
            ? nullptr
            : env->GetMethodID(window_class, "findViewById",
                               "(I)Landroid/view/View;");
    jclass framework_id_class = env->FindClass("android/R$id");
    jfieldID content_id =
        framework_id_class == nullptr
            ? nullptr
            : env->GetStaticFieldID(framework_id_class, "content", "I");
    if (get_decor_view != nullptr && find_view_by_id != nullptr &&
        content_id != nullptr && !env->ExceptionCheck()) {
      decor_view = env->CallObjectMethod(window, get_decor_view);
      if (!env->ExceptionCheck()) {
        content_root = env->CallObjectMethod(
            window, find_view_by_id,
            env->GetStaticIntField(framework_id_class, content_id));
      }
    }
    env->DeleteLocalRef(framework_id_class);
  } else {
    decor_view =
        decor_view_constructor == nullptr || window_attributes == nullptr
            ? nullptr
            : env->NewObject(decor_view_class, decor_view_constructor,
                             activity_instance, static_cast<jint>(-1), window,
                             window_attributes);
  }
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
  if (!run_apk_app) {
    content_root =
        content_root_constructor == nullptr
            ? nullptr
            : env->NewObject(content_root_class, content_root_constructor,
                             activity_instance);
  }
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
  if (!run_apk_app) {
    env->CallVoidMethod(decor_view, add_view, content_root);
    env->SetObjectField(window, phone_decor, decor_view);
    env->SetObjectField(window, phone_content_parent, content_root);
  }
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android window: DecorView attachment threw\n"
              << self->GetException()->Dump() << "\n";
    return 31;
  }

  jmethodID probe_on_create =
      run_apk_app
          ? env->GetMethodID(probe_activity_class, "performCreate",
                             "(Landroid/os/Bundle;)V")
          : env->GetMethodID(probe_activity_class, "probeOnCreate", "()I");
  jmethodID perform_start =
      run_apk_app
          ? env->GetMethodID(probe_activity_class, "performStart",
                             "(Ljava/lang/String;)V")
          : nullptr;
  jmethodID perform_resume =
      run_apk_app
          ? env->GetMethodID(probe_activity_class, "performResume",
                             "(ZLjava/lang/String;)V")
          : nullptr;
  jint lifecycle_result = -1;
  if (probe_on_create != nullptr) {
    if (run_apk_app) {
      // A device initializes Typeface's system font map during framework
      // startup.  The detached host has no Zygote/SystemServer phase, so run
      // the same pinned AOSP font bootstrap from the support DEX before an
      // arbitrary APK constructs TextView/Button objects.  The APK itself is
      // untouched; this is a runtime-owned framework initialization seam.
      // The support DEX is registered in the same PathClassLoader as the APK,
      // not in the boot class loader.  FindClass from this native frame would
      // therefore miss it for arbitrary APKs; resolve it through the actual
      // Activity class loader instead.
      jclass class_class = env->FindClass("java/lang/Class");
      jmethodID get_class_loader =
          class_class == nullptr
              ? nullptr
              : env->GetMethodID(
                    class_class, "getClassLoader",
                    "()Ljava/lang/ClassLoader;");
      jobject app_loader =
          get_class_loader == nullptr
              ? nullptr
              : env->CallObjectMethod(
                    reinterpret_cast<jobject>(probe_activity_class),
                    get_class_loader);
      jclass loader_class =
          app_loader == nullptr ? nullptr : env->GetObjectClass(app_loader);
      jmethodID load_class =
          loader_class == nullptr
              ? nullptr
              : env->GetMethodID(
                    loader_class, "loadClass",
                    "(Ljava/lang/String;)Ljava/lang/Class;");
      jstring bootstrap_name =
          env->NewStringUTF("dev.darwinart.probe.FontBootstrap");
      jobject bootstrap_class_object =
          load_class == nullptr || bootstrap_name == nullptr
              ? nullptr
              : env->CallObjectMethod(app_loader, load_class, bootstrap_name);
      jclass font_bootstrap =
          reinterpret_cast<jclass>(bootstrap_class_object);
      jmethodID install_fonts =
          font_bootstrap == nullptr
              ? nullptr
              : env->GetStaticMethodID(font_bootstrap, "install", "()V");
      if (install_fonts != nullptr) {
        env->CallStaticVoidMethod(font_bootstrap, install_fonts);
      }
      env->DeleteLocalRef(bootstrap_name);
      env->DeleteLocalRef(loader_class);
      env->DeleteLocalRef(app_loader);
      env->DeleteLocalRef(class_class);
      env->DeleteLocalRef(font_bootstrap);
      if (env->ExceptionCheck()) {
        std::cerr << "ART Android framework: system font bootstrap failed\n"
                  << self->GetException()->Dump() << "\n";
        return 28;
      }
    }
    if (run_apk_app) {
      env->CallVoidMethod(activity_instance, probe_on_create, nullptr);
      jstring lifecycle_reason = env->NewStringUTF("darwin-art launch");
      if (!env->ExceptionCheck() && perform_start != nullptr) {
        env->CallVoidMethod(activity_instance, perform_start,
                            lifecycle_reason);
      }
      if (!env->ExceptionCheck() && perform_resume != nullptr) {
        env->CallVoidMethod(activity_instance, perform_resume, JNI_FALSE,
                            lifecycle_reason);
      }
      env->DeleteLocalRef(lifecycle_reason);
      lifecycle_result = env->ExceptionCheck() ? -1 : 43;
    } else {
      lifecycle_result =
          env->CallIntMethod(activity_instance, probe_on_create);
    }
  }
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    std::cerr << "ART Android lifecycle: Activity.onCreate() threw\n"
              << self->GetException()->Dump() << "\n";
    return 28;
  }
  const bool expect_apk_native_answer =
      std::getenv("DARWIN_ART_APK_EXPECT_NATIVE_ANSWER") != nullptr &&
      std::strcmp(std::getenv("DARWIN_ART_APK_EXPECT_NATIVE_ANSWER"), "1") == 0;
  if (run_apk_app && apk_native_loaded && expect_apk_native_answer) {
    jmethodID native_answer = env->GetStaticMethodID(
        probe_activity_class, "nativeAnswer", "()I");
    const jint value = native_answer == nullptr
                           ? -1
                           : env->CallStaticIntMethod(probe_activity_class,
                                                       native_answer);
    if (native_answer == nullptr || env->ExceptionCheck() || value != 42) {
      std::cerr << "ART Android APK JNI: nativeAnswer() expected 42, got "
                << value << "\n";
      if (self->IsExceptionPending()) {
        std::cerr << self->GetException()->Dump() << "\n";
      }
      return 46;
    }
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
  if (run_apk_app &&
      !attach_android_window(env, activity_instance, window, decor_view,
                             window_attributes, graphics_state)) {
    std::cerr << "ART Android window: real ViewRoot attachment failed\n";
    return 31;
  }
  // A real ViewRootImpl performs the final DecorView layout before input
  // dispatch.  The detached launcher has no ViewRoot, so mirror that one
  // ownership step explicitly; without it DecorView remains 0x0 even though
  // its content display list has already been recorded, making all hit tests
  // fail closed.
  jmethodID layout = decor_view_class == nullptr
                         ? nullptr
                         : env->GetMethodID(decor_view_class, "layout",
                                            "(IIII)V");
  if (layout == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android input: DecorView layout lookup failed\n";
    env->ExceptionClear();
    return 31;
  }
  env->CallVoidMethod(decor_view, layout, 0, 0,
                      kApkFrameWidth * window_scale,
                      kApkFrameHeight * window_scale);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android input: DecorView layout failed\n"
              << self->GetException()->Dump() << "\n";
    return 31;
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
      !darwin_art_graphics::retain_hardware_context(graphics_state, env,
                                                    activity_instance)) {
    std::cerr << "ART Android graphics: activity context retention failed\n";
    return 33;
  }
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
  env->DeleteLocalRef(accessibility);
  env->DeleteLocalRef(accessibility_class);
  env->DeleteLocalRef(probe_view);
  if (lifecycle_result != 43) {
    std::cerr << "ART Android lifecycle: expected 43, got " << lifecycle_result
              << "\n";
    return 29;
  }

  return 0;
}

}  // namespace darwin_art_presentation
