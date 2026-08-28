#include "runtime_shutdown_probe.h"

#include <iostream>

#include "darwin_art/darwin_art.h"
#include "darwin_art_bionic_dns.h"
#include "darwin_art_bionic_socket_broker.h"
#include "darwin_android_jni_trampoline.h"
#include "darwin_framework_natives.h"
#include "darwin_icu_natives.h"
#include "darwin_libcore_natives.h"
#include "darwin_provider_owners.h"
#include "runtime_frame_probe.h"
#include "runtime_graphics_probe.h"
#include "runtime_process_state.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "thread-current-inl.h"

extern "C" int darwin_art_elf_jni_fixture_registration_status();
extern "C" int darwin_art_elf_jni_fixture_lifecycle_status();
extern "C" int darwin_art_elf_jni_fixture_namespace_lifecycle_status();

namespace darwin_art_process {

namespace {

bool ShutdownAndroidAsyncTaskExecutor(JNIEnv* env) {
  jclass async_task_class = env->FindClass("android/os/AsyncTask");
  if (async_task_class == nullptr) {
    env->ExceptionClear();
    return true;
  }
  jfieldID executor_field = env->GetStaticFieldID(
      async_task_class, "THREAD_POOL_EXECUTOR",
      "Ljava/util/concurrent/Executor;");
  if (executor_field == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(async_task_class);
    return true;
  }
  jobject executor =
      env->GetStaticObjectField(async_task_class, executor_field);
  env->DeleteLocalRef(async_task_class);
  if (executor == nullptr) {
    return !env->ExceptionCheck();
  }

  jclass executor_service_class =
      env->FindClass("java/util/concurrent/ExecutorService");
  if (executor_service_class == nullptr ||
      !env->IsInstanceOf(executor, executor_service_class)) {
    env->ExceptionClear();
    env->DeleteLocalRef(executor);
    if (executor_service_class != nullptr) {
      env->DeleteLocalRef(executor_service_class);
    }
    return true;
  }
  jmethodID shutdown_now = env->GetMethodID(
      executor_service_class, "shutdownNow", "()Ljava/util/List;");
  if (shutdown_now == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(executor_service_class);
    env->DeleteLocalRef(executor);
    return false;
  }
  jobject abandoned_tasks = env->CallObjectMethod(executor, shutdown_now);
  const bool succeeded = !env->ExceptionCheck();
  if (!succeeded) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  if (abandoned_tasks != nullptr) {
    env->DeleteLocalRef(abandoned_tasks);
  }
  env->DeleteLocalRef(executor_service_class);
  env->DeleteLocalRef(executor);
  return succeeded;
}

bool StopAndroidApplicationThreads(JNIEnv* env) {
  jclass thread_class = env->FindClass("java/lang/Thread");
  jclass handler_thread_class = env->FindClass("android/os/HandlerThread");
  if (thread_class == nullptr || handler_thread_class == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(thread_class);
    env->DeleteLocalRef(handler_thread_class);
    return false;
  }
  jmethodID current_thread =
      env->GetStaticMethodID(thread_class, "currentThread", "()Ljava/lang/Thread;");
  jmethodID all_stacks = env->GetStaticMethodID(
      thread_class, "getAllStackTraces", "()Ljava/util/Map;");
  jmethodID is_daemon = env->GetMethodID(thread_class, "isDaemon", "()Z");
  jmethodID get_name =
      env->GetMethodID(thread_class, "getName", "()Ljava/lang/String;");
  jmethodID interrupt = env->GetMethodID(thread_class, "interrupt", "()V");
  jfieldID thread_target =
      env->GetFieldID(thread_class, "target", "Ljava/lang/Runnable;");
  jmethodID quit_safely =
      env->GetMethodID(handler_thread_class, "quitSafely", "()Z");
  jclass thread_pool_class =
      env->FindClass("java/util/concurrent/ThreadPoolExecutor");
  jmethodID shutdown_pool =
      thread_pool_class == nullptr
          ? nullptr
          : env->GetMethodID(thread_pool_class, "shutdownNow", "()Ljava/util/List;");
  if (current_thread == nullptr || all_stacks == nullptr ||
      is_daemon == nullptr || get_name == nullptr || interrupt == nullptr ||
      thread_target == nullptr ||
      quit_safely == nullptr || thread_pool_class == nullptr ||
      shutdown_pool == nullptr) {
    env->ExceptionClear();
    env->DeleteLocalRef(thread_class);
    env->DeleteLocalRef(handler_thread_class);
    env->DeleteLocalRef(thread_pool_class);
    return false;
  }
  jobject current = env->CallStaticObjectMethod(thread_class, current_thread);
  jobject stacks = env->CallStaticObjectMethod(thread_class, all_stacks);
  jclass map_class = env->FindClass("java/util/Map");
  jmethodID key_set = map_class == nullptr
                          ? nullptr
                          : env->GetMethodID(map_class, "keySet", "()Ljava/util/Set;");
  jobject threads = key_set == nullptr ? nullptr
                                       : env->CallObjectMethod(stacks, key_set);
  jclass collection_class = env->FindClass("java/util/Collection");
  jmethodID to_array =
      collection_class == nullptr
          ? nullptr
          : env->GetMethodID(collection_class, "toArray", "()[Ljava/lang/Object;");
  jobjectArray array = to_array == nullptr
                           ? nullptr
                           : static_cast<jobjectArray>(
                                 env->CallObjectMethod(threads, to_array));
  bool succeeded = array != nullptr && !env->ExceptionCheck();
  if (succeeded) {
    const jsize count = env->GetArrayLength(array);
    for (jsize index = 0; index < count; ++index) {
      jobject thread = env->GetObjectArrayElement(array, index);
      if (thread == nullptr || env->IsSameObject(thread, current) ||
          env->CallBooleanMethod(thread, is_daemon) == JNI_TRUE) {
        env->DeleteLocalRef(thread);
        continue;
      }
      if (env->IsInstanceOf(thread, handler_thread_class)) {
        env->CallBooleanMethod(thread, quit_safely);
      }
      jobject target = env->GetObjectField(thread, thread_target);
      jstring name =
          static_cast<jstring>(env->CallObjectMethod(thread, get_name));
      const char* native_name =
          name == nullptr ? nullptr : env->GetStringUTFChars(name, nullptr);
      jclass concrete_thread_class = env->GetObjectClass(thread);
      jclass class_class_for_thread = env->FindClass("java/lang/Class");
      jmethodID class_name_for_thread =
          class_class_for_thread == nullptr
              ? nullptr
              : env->GetMethodID(class_class_for_thread, "getName",
                                 "()Ljava/lang/String;");
      jstring concrete_name =
          class_name_for_thread == nullptr
              ? nullptr
              : static_cast<jstring>(env->CallObjectMethod(
                    concrete_thread_class, class_name_for_thread));
      const char* native_concrete_name =
          concrete_name == nullptr
              ? nullptr
              : env->GetStringUTFChars(concrete_name, nullptr);
      std::cerr << "ART Darwin shutdown: stopping non-daemon thread="
                << (native_name == nullptr ? "<unnamed>" : native_name)
                << " class="
                << (native_concrete_name == nullptr ? "<unknown>"
                                                    : native_concrete_name)
                << " target=" << (target == nullptr ? 0 : 1) << "\n";
      if (native_name != nullptr) env->ReleaseStringUTFChars(name, native_name);
      if (native_concrete_name != nullptr) {
        env->ReleaseStringUTFChars(concrete_name, native_concrete_name);
      }
      env->DeleteLocalRef(name);
      env->DeleteLocalRef(concrete_name);
      env->DeleteLocalRef(class_class_for_thread);
      jmethodID shutdown_method =
          env->GetMethodID(concrete_thread_class, "shutdown", "()V");
      if (shutdown_method == nullptr) {
        env->ExceptionClear();
      } else {
        env->CallVoidMethod(thread, shutdown_method);
      }
      if (target != nullptr) {
        jclass target_class = env->GetObjectClass(target);
        jclass class_class = env->FindClass("java/lang/Class");
        jmethodID class_name =
            class_class == nullptr
                ? nullptr
                : env->GetMethodID(class_class, "getName", "()Ljava/lang/String;");
        jstring target_name =
            class_name == nullptr
                ? nullptr
                : static_cast<jstring>(
                      env->CallObjectMethod(target_class, class_name));
        const char* native_target_name =
            target_name == nullptr
                ? nullptr
                : env->GetStringUTFChars(target_name, nullptr);
        std::cerr << "ART Darwin shutdown: thread target class="
                  << (native_target_name == nullptr ? "<unknown>"
                                                    : native_target_name)
                  << "\n";
        if (native_target_name != nullptr) {
          env->ReleaseStringUTFChars(target_name, native_target_name);
        }
        env->DeleteLocalRef(target_name);
        env->DeleteLocalRef(class_class);
        jfieldID owner = env->GetFieldID(
            target_class, "this$0", "Ljava/util/concurrent/ThreadPoolExecutor;");
        if (owner == nullptr) {
          env->ExceptionClear();
        } else {
          jobject pool = env->GetObjectField(target, owner);
          if (pool != nullptr && env->IsInstanceOf(pool, thread_pool_class)) {
            jobject abandoned = env->CallObjectMethod(pool, shutdown_pool);
            env->DeleteLocalRef(abandoned);
          }
          env->DeleteLocalRef(pool);
        }
        env->DeleteLocalRef(target_class);
      }
      env->DeleteLocalRef(target);
      env->DeleteLocalRef(concrete_thread_class);
      env->CallVoidMethod(thread, interrupt);
      if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        succeeded = false;
      }
      env->DeleteLocalRef(thread);
    }
  } else {
    env->ExceptionClear();
  }
  env->DeleteLocalRef(array);
  env->DeleteLocalRef(collection_class);
  env->DeleteLocalRef(threads);
  env->DeleteLocalRef(map_class);
  env->DeleteLocalRef(stacks);
  env->DeleteLocalRef(current);
  env->DeleteLocalRef(handler_thread_class);
  env->DeleteLocalRef(thread_pool_class);
  env->DeleteLocalRef(thread_class);
  return succeeded;
}

}  // namespace

int32_t run_shutdown(const ShutdownState& state) {
  darwin_art_process::ShutdownSnapshot shutdown{};
  switch (darwin_art_process::begin_shutdown(&shutdown)) {
    case darwin_art_process::ShutdownBeginResult::kAlreadyComplete:
      return DARWIN_ART_STATUS_SHUTDOWN_ALREADY_COMPLETED;
    case darwin_art_process::ShutdownBeginResult::kFailed:
      return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
    case darwin_art_process::ShutdownBeginResult::kNotReady:
      return DARWIN_ART_STATUS_SHUTDOWN_NOT_READY;
    case darwin_art_process::ShutdownBeginResult::kWrongThread:
      return DARWIN_ART_STATUS_SHUTDOWN_WRONG_THREAD;
    case darwin_art_process::ShutdownBeginResult::kReady:
      break;
  }
  JavaVM* java_vm = shutdown.java_vm;
  art::Thread* art_thread = shutdown.art_thread;
  const bool resource_runtime_installed = shutdown.resource_runtime_installed;

  CHECK(java_vm != nullptr);
  if (art_thread != nullptr) {
    CHECK_EQ(art_thread->GetState(), art::ThreadState::kNative);
    {
      art::ScopedObjectAccess soa(art_thread);
      if (art_thread->IsExceptionPending()) {
        std::cerr << "ART Darwin shutdown: clearing pending exception: "
                  << art_thread->GetException()->Dump() << "\n";
        art_thread->ClearException();
      }
      // Android's process lifetime normally ends without DestroyJavaVM, so
      // framework-owned executors are not automatically stopped.  Darwin ART
      // does destroy the VM, and ART correctly waits for non-daemon Java
      // threads.  Quiesce AsyncTask's shared pool before releasing framework
      // state so a loader worker cannot keep shutdown blocked indefinitely.
      if (!ShutdownAndroidAsyncTaskExecutor(art_thread->GetJniEnv())) {
        std::cerr << "ART Darwin shutdown: AsyncTask executor shutdown failed\n";
        darwin_art_process::mark_shutdown_failed();
        return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
      }
      darwin_art_graphics::shutdown(shutdown.graphics_state,
                                    art_thread->GetJniEnv());
      if (!StopAndroidApplicationThreads(art_thread->GetJniEnv())) {
        std::cerr << "ART Darwin shutdown: application thread stop failed\n";
        darwin_art_process::mark_shutdown_failed();
        return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
      }
      if (art_thread->IsExceptionPending()) {
        std::cerr << "ART Darwin shutdown: global reference cleanup threw: "
                  << art_thread->GetException()->Dump() << "\n";
        art_thread->ClearException();
      }
      // GraphicsState has released every JNI/HWUI reference above.  Finalize
      // the opaque session while ART is still attached; Rust may retain the
      // owner until after DestroyJavaVM, but its eventual Drop is now a
      // memory-only erase and cannot re-enter ART.
      if (shutdown.graphics_state != nullptr) {
        const int32_t graphics_finalize_status =
            darwin_art_graphics::finalize_bound_session(
                shutdown.graphics_state);
        if (graphics_finalize_status != 0) {
          std::cerr << "ART Darwin shutdown: graphics session finalization failed status="
                    << graphics_finalize_status << "\n";
          darwin_art_process::mark_shutdown_failed();
          return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
        }
      }
      if (!darwin_art::ShutdownLibcoreNatives()) {
        std::cerr << "ART Darwin shutdown: libcore host state restore failed\n";
        darwin_art_process::mark_shutdown_failed();
        return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
      }
      if (resource_runtime_installed &&
          !darwin_art::ShutdownFrameworkResourceRuntime(
              art_thread->GetJniEnv())) {
        std::cerr << "ART Darwin shutdown: AndroidRuntime ownership uninstall failed\n";
        darwin_art_process::mark_shutdown_failed();
        return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
      }
    }
    CHECK_EQ(art_thread->GetState(), art::ThreadState::kNative);
  }

  // DexFile owners remain live until DestroyJavaVM has finished tearing down
  // ClassLinker and Heap.
  if (java_vm->DestroyJavaVM() != JNI_OK) {
    darwin_art_process::mark_shutdown_failed();
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
  }
  darwin_art_frame_probe::reset();
  if (darwin_art::android_jni::TrampolineLiveCount() != 0) {
    std::cerr << "ART Darwin shutdown: ELF JNI trampolines remain live\n";
    darwin_art_process::mark_shutdown_failed();
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
  }
  if (state.network_elf_loaded &&
      (darwin_art_bionic_socket_broker_is_active() != 0 ||
       darwin_art_bionic_socket_broker_live_objects() != 0 ||
       darwin_art_bionic_dns_live_results_for_test() != 0 ||
       darwin_art_bionic_dns_retired_results_for_test() != 0)) {
    std::cerr << "ART Darwin shutdown: network owner did not quiesce\n";
    darwin_art_process::mark_shutdown_failed();
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
  }
  if (state.apk_elf_loaded) {
    std::cout << "ART Android APK ELF: apk-sha256=" << state.apk_sha256
              << " root-sha256=" << state.apk_root_sha256
              << " graph=root+child+grandchild load=JavaVMExt+NativeBridge "
                 "unload=shutdown-trampolines-zero\n"
              << std::flush;
  }
  if (state.direct_apk_loaded) {
    std::cout << "ART Android direct APK ELF: source=readonly-fd-slices "
                 "copy=0 extract=0 alignment=16384 graph=root+child+grandchild "
                 "load=JavaVMExt+NativeBridge JNI_OnLoad=0x00010006 "
                 "unload=shutdown-trampolines-zero authority=isolated-process\n"
              << std::flush;
  }
  if (darwin_art_elf_jni_fixture_registration_status() != 0 &&
      darwin_art_elf_jni_fixture_lifecycle_status() != 1234567) {
    std::cerr << "ART Darwin shutdown: ELF JNI graph finalizer order failed, status="
              << darwin_art_elf_jni_fixture_lifecycle_status() << "\n";
    darwin_art_process::mark_shutdown_failed();
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
  }
  if (darwin_art_elf_jni_fixture_registration_status() != 0 &&
      darwin_art_elf_jni_fixture_namespace_lifecycle_status() != 5) {
    std::cerr << "ART Darwin shutdown: Bionic namespace teardown order failed, status="
              << darwin_art_elf_jni_fixture_namespace_lifecycle_status() << "\n";
    darwin_art_process::mark_shutdown_failed();
    return DARWIN_ART_STATUS_SHUTDOWN_FAILED;
  }

  darwin_art::ShutdownIcuCharsetNatives();
  darwin_art::ShutdownFrameworkGraphicsRuntime();
  if (state.provider_hooks_installed) {
    darwin_art::providers::darwin_art_provider_clear_hooks();
  }
  darwin_art_process::clear_app_dex_files();
  darwin_art_process::mark_shutdown_complete();
  return 0;
}

}  // namespace darwin_art_process

extern "C" DARWIN_ART_EXPORT int32_t darwin_art_shutdown_process() {
  const auto acceptance = darwin_art_process::acceptance_snapshot();
  darwin_art_process::ShutdownState state;
  state.network_elf_loaded = acceptance.network_elf_loaded;
  state.apk_elf_loaded = acceptance.apk_elf_loaded;
  state.direct_apk_loaded = acceptance.direct_apk_loaded;
  state.provider_hooks_installed = acceptance.provider_hooks_installed;
  state.apk_sha256 = acceptance.apk_sha256;
  state.apk_root_sha256 = acceptance.apk_root_sha256;
  const int32_t status = darwin_art_process::run_shutdown(state);
  if (status == 0) {
    darwin_art_process::clear_provider_hooks_state();
  }
  return status;
}
