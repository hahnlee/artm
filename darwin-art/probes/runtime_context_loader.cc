#include <iostream>

#include <jni.h>

extern "C" int darwin_art_install_context_loader(JNIEnv* env,
                                                    jobject app_loader) {
  if (env == nullptr || app_loader == nullptr) return 4;
  jclass thread_class = env->FindClass("java/lang/Thread");
  jmethodID current_thread =
      thread_class == nullptr
          ? nullptr
          : env->GetStaticMethodID(thread_class, "currentThread",
                                   "()Ljava/lang/Thread;");
  jmethodID set_context_loader =
      thread_class == nullptr
          ? nullptr
          : env->GetMethodID(thread_class, "setContextClassLoader",
                             "(Ljava/lang/ClassLoader;)V");
  jobject managed_thread =
      current_thread == nullptr
          ? nullptr
          : env->CallStaticObjectMethod(thread_class, current_thread);
  if (managed_thread == nullptr || set_context_loader == nullptr ||
      env->ExceptionCheck()) {
    std::cerr << "ART Darwin DEX: context ClassLoader setup failed\n";
    if (managed_thread != nullptr) env->DeleteLocalRef(managed_thread);
    if (thread_class != nullptr) env->DeleteLocalRef(thread_class);
    env->ExceptionClear();
    return 4;
  }
  env->CallVoidMethod(managed_thread, set_context_loader, app_loader);
  env->DeleteLocalRef(managed_thread);
  env->DeleteLocalRef(thread_class);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Darwin DEX: context ClassLoader install failed\n";
    env->ExceptionClear();
    return 4;
  }
  return 0;
}
