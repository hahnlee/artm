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
