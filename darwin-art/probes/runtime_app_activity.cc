#include "runtime_app_activity.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <string>
#include <sstream>
#include <unistd.h>

#include "mirror/throwable.h"
#include "darwin_binder_wire.h"
#include "runtime_process_state.h"
#include "thread-current-inl.h"

namespace darwin_art_app_activity {
namespace {

bool DecodeHex(const std::string& encoded, std::string* decoded) {
  if (decoded == nullptr || encoded.size() % 2 != 0) return false;
  decoded->clear();
  decoded->reserve(encoded.size() / 2);
  auto nibble = [](char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  };
  for (size_t index = 0; index < encoded.size(); index += 2) {
    const int high = nibble(encoded[index]);
    const int low = nibble(encoded[index + 1]);
    if (high < 0 || low < 0) return false;
    decoded->push_back(static_cast<char>((high << 4) | low));
  }
  return true;
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> result;
  size_t begin = 0;
  while (begin <= value.size()) {
    const size_t end = value.find(delimiter, begin);
    result.push_back(value.substr(begin, end == std::string::npos
                                           ? std::string::npos
                                           : end - begin));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return result;
}

jobject AllocateWithoutConstructor(JNIEnv* env, jclass type) {
  if (env == nullptr || type == nullptr) return nullptr;
  jclass unsafe_class = env->FindClass("sun/misc/Unsafe");
  if (unsafe_class == nullptr) {
    env->ExceptionClear();
    unsafe_class = env->FindClass("jdk/internal/misc/Unsafe");
  }
  if (unsafe_class == nullptr) {
    env->ExceptionClear();
    return nullptr;
  }
  jfieldID singleton = env->GetStaticFieldID(unsafe_class, "theUnsafe",
                                             "Lsun/misc/Unsafe;");
  if (singleton == nullptr) {
    env->ExceptionClear();
    singleton = env->GetStaticFieldID(unsafe_class, "theUnsafe",
                                      "Ljdk/internal/misc/Unsafe;");
  }
  jobject unsafe = singleton == nullptr
                       ? nullptr
                       : env->GetStaticObjectField(unsafe_class, singleton);
  jmethodID allocate = unsafe == nullptr
                           ? nullptr
                           : env->GetMethodID(unsafe_class, "allocateInstance",
                                              "(Ljava/lang/Class;)Ljava/lang/Object;");
  jobject result = (unsafe != nullptr && allocate != nullptr)
                       ? env->CallObjectMethod(unsafe, allocate, type)
                       : nullptr;
  env->ExceptionClear();
  env->DeleteLocalRef(unsafe);
  env->DeleteLocalRef(unsafe_class);
  return result;
}

bool InstallDeclaredContentProviders(JNIEnv* env, jobject context,
                                     jobject application_info,
                                     jobject app_loader) {
  const char* encoded = std::getenv("DARWIN_ART_APK_APP_PROVIDERS");
  if (env == nullptr || context == nullptr || app_loader == nullptr ||
      encoded == nullptr || encoded[0] == '\0' ||
      std::strcmp(encoded, "none") == 0) {
    return true;
  }
  jclass loader_class = env->GetObjectClass(app_loader);
  jmethodID load_class = loader_class == nullptr
                             ? nullptr
                             : env->GetMethodID(
                                   loader_class, "loadClass",
                                   "(Ljava/lang/String;)Ljava/lang/Class;");
  jclass provider_info_class = env->FindClass("android/content/pm/ProviderInfo");
  jmethodID provider_info_constructor =
      provider_info_class == nullptr
          ? nullptr
          : env->GetMethodID(provider_info_class, "<init>", "()V");
  jclass bundle_class = env->FindClass("android/os/Bundle");
  jmethodID bundle_constructor = bundle_class == nullptr
                                     ? nullptr
                                     : env->GetMethodID(bundle_class, "<init>", "()V");
  jmethodID put_string = bundle_class == nullptr
                             ? nullptr
                             : env->GetMethodID(bundle_class, "putString",
                                                "(Ljava/lang/String;Ljava/lang/String;)V");
  jmethodID put_int = bundle_class == nullptr
                          ? nullptr
                          : env->GetMethodID(bundle_class, "putInt",
                                             "(Ljava/lang/String;I)V");
  jmethodID put_boolean = bundle_class == nullptr
                              ? nullptr
                              : env->GetMethodID(bundle_class, "putBoolean",
                                                 "(Ljava/lang/String;Z)V");
  jfieldID info_name = provider_info_class == nullptr
                           ? nullptr
                           : env->GetFieldID(provider_info_class, "name",
                                             "Ljava/lang/String;");
  jfieldID info_authority = provider_info_class == nullptr
                                ? nullptr
                                : env->GetFieldID(provider_info_class, "authority",
                                                  "Ljava/lang/String;");
  jfieldID info_init_order = provider_info_class == nullptr
                                 ? nullptr
                                 : env->GetFieldID(provider_info_class, "initOrder", "I");
  jfieldID info_application = provider_info_class == nullptr
                                  ? nullptr
                                  : env->GetFieldID(
                                        provider_info_class, "applicationInfo",
                                        "Landroid/content/pm/ApplicationInfo;");
  jfieldID info_metadata = provider_info_class == nullptr
                               ? nullptr
                               : env->GetFieldID(provider_info_class, "metaData",
                                                 "Landroid/os/Bundle;");
  if (load_class == nullptr || provider_info_constructor == nullptr ||
      bundle_constructor == nullptr || info_name == nullptr ||
      info_authority == nullptr || info_init_order == nullptr ||
      info_application == nullptr || info_metadata == nullptr ||
      env->ExceptionCheck()) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    return false;
  }
  for (const std::string& item : Split(encoded, ';')) {
    if (item.empty()) continue;
    const size_t first = item.find('>');
    const size_t second = first == std::string::npos
                              ? std::string::npos
                              : item.find('>', first + 1);
    const size_t third = second == std::string::npos
                             ? std::string::npos
                             : item.find('>', second + 1);
    if (first == std::string::npos || second == std::string::npos ||
        third == std::string::npos) {
      continue;
    }
    std::string provider_name;
    std::string authority;
    if (!DecodeHex(item.substr(0, first), &provider_name) ||
        !DecodeHex(item.substr(first + 1, second - first - 1), &authority)) {
      continue;
    }
    // FirebaseInitProvider is intentionally owned by the APK Application in
    // this detached process.  Calling it here as well starts a second
    // Firebase singleton before ActivityThread has published the application
    // context and causes Firebase's fatal-exit path.  Other manifest
    // providers (notably AndroidX Startup/WorkManager) still follow the
    // Android attachInfo -> onCreate ordering below.
    if (provider_name ==
        "com.google.firebase.provider.FirebaseInitProvider") {
      continue;
    }
    const unsigned long init_order = std::strtoul(
        item.substr(second + 1, third - second - 1).c_str(), nullptr, 16);
    jstring class_name = env->NewStringUTF(provider_name.c_str());
    jclass provider_class = reinterpret_cast<jclass>(
        env->CallObjectMethod(app_loader, load_class, class_name));
    env->DeleteLocalRef(class_name);
    if (provider_class == nullptr || env->ExceptionCheck()) {
      if (env->ExceptionCheck()) env->ExceptionClear();
      env->DeleteLocalRef(provider_class);
      continue;  // Optional Play Services providers may be absent.
    }
    jmethodID provider_constructor =
        env->GetMethodID(provider_class, "<init>", "()V");
    jmethodID attach_info = env->GetMethodID(
        provider_class, "attachInfo",
        "(Landroid/content/Context;Landroid/content/pm/ProviderInfo;)V");
    jmethodID on_create = env->GetMethodID(provider_class, "onCreate", "()Z");
    jobject provider = provider_constructor == nullptr
                           ? nullptr
                           : env->NewObject(provider_class, provider_constructor);
    // ProviderInfo is an SDK-stub class in the compact framework image and
    // its public constructor throws RuntimeException("Stub!"). Android's
    // system_server allocates it as a parcelable data holder, so mirror that
    // behavior with Unsafe when the constructor is a stub.
    jobject info = env->NewObject(provider_info_class, provider_info_constructor);
    if (info == nullptr && env->ExceptionCheck()) {
      env->ExceptionClear();
      info = AllocateWithoutConstructor(env, provider_info_class);
    }
    jobject metadata = env->NewObject(bundle_class, bundle_constructor);
    jstring name_value = env->NewStringUTF(provider_name.c_str());
    jstring authority_value = env->NewStringUTF(authority.c_str());
    if (provider != nullptr && info != nullptr && metadata != nullptr &&
        name_value != nullptr && authority_value != nullptr &&
        !env->ExceptionCheck()) {
      env->SetObjectField(info, info_name, name_value);
      env->SetObjectField(info, info_authority, authority_value);
      env->SetIntField(info, info_init_order, static_cast<jint>(init_order));
      if (application_info != nullptr) {
        env->SetObjectField(info, info_application, application_info);
      }
      for (const std::string& metadata_item :
           Split(item.substr(third + 1), ',')) {
        const size_t colon = metadata_item.find(':');
        const size_t value_colon = colon == std::string::npos
                                       ? std::string::npos
                                       : metadata_item.find(':', colon + 1);
        if (colon == std::string::npos || value_colon == std::string::npos) continue;
        std::string metadata_name;
        if (!DecodeHex(metadata_item.substr(0, colon), &metadata_name)) continue;
        jstring key = env->NewStringUTF(metadata_name.c_str());
        const char kind = metadata_item[colon + 1];
        const std::string value = metadata_item.substr(value_colon + 1);
        if (kind == 's' && put_string != nullptr) {
          std::string string_value;
          if (DecodeHex(value, &string_value)) {
            jstring text = env->NewStringUTF(string_value.c_str());
            env->CallVoidMethod(metadata, put_string, key, text);
            env->DeleteLocalRef(text);
          }
        } else if ((kind == 'i' || kind == 'r') && put_int != nullptr) {
          const jint integer = static_cast<jint>(std::strtoul(value.c_str(), nullptr, 16));
          env->CallVoidMethod(metadata, put_int, key, integer);
        } else if (kind == 'b' && put_boolean != nullptr) {
          env->CallVoidMethod(metadata, put_boolean, key, value == "1");
        }
        env->DeleteLocalRef(key);
      }
      env->SetObjectField(info, info_metadata, metadata);
      if (attach_info != nullptr) {
        env->CallVoidMethod(provider, attach_info, context, info);
      }
      if (on_create != nullptr && !env->ExceptionCheck()) {
        env->CallBooleanMethod(provider, on_create);
      }
    }
    env->DeleteLocalRef(authority_value);
    env->DeleteLocalRef(name_value);
    env->DeleteLocalRef(metadata);
    env->DeleteLocalRef(info);
    env->DeleteLocalRef(provider);
    env->DeleteLocalRef(provider_class);
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
  }
  env->DeleteLocalRef(bundle_class);
  env->DeleteLocalRef(provider_info_class);
  env->DeleteLocalRef(loader_class);
  return !env->ExceptionCheck();
}

std::string JavaString(JNIEnv* env, jstring value) {
  if (value == nullptr) return {};
  const char* utf = env->GetStringUTFChars(value, nullptr);
  if (utf == nullptr) return {};
  std::string result(utf);
  env->ReleaseStringUTFChars(value, utf);
  return result;
}

jintArray SpawnService(JNIEnv* env, jclass, jstring component,
                       jstring instance_name, jstring process_name,
                       jboolean isolated, jobject intent) {
  const bool debug_timing =
      std::getenv("DARWIN_ART_DEBUG_SLOW_FRAME") != nullptr;
  const auto started = std::chrono::steady_clock::now();
  const auto log_stage = [&](const char* stage) {
    if (!debug_timing) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    std::cerr << "DARWIN_ART service-spawn stage=" << stage
              << " elapsed_us=" << elapsed << "\n";
  };
  const std::string component_utf = JavaString(env, component);
  const std::string instance_utf = JavaString(env, instance_name);
  const std::string process_utf = JavaString(env, process_name);
  if (component_utf.empty() || process_utf.empty() || env->ExceptionCheck()) {
    return nullptr;
  }
  int32_t host_pid = -1;
  int32_t control_fd = -1;
  const int32_t spawn_status = darwin_art_process::spawn_service_process(
      component_utf.c_str(), instance_utf.c_str(), process_utf.c_str(),
      isolated == JNI_TRUE, &host_pid, &control_fd);
  log_stage("spawn");
  if (spawn_status != 0) {
    return nullptr;
  }
  const bool intent_sent =
      intent != nullptr && darwin_art::SendServiceBindIntent(env, control_fd, intent);
  log_stage("bind-intent");
  const bool dispatcher_started =
      intent_sent && darwin_art::StartRemoteBinderDispatcher(env, control_fd);
  log_stage("dispatcher");
  if (!dispatcher_started) {
    close(control_fd);
    darwin_art_process::release_service_process(host_pid);
    return nullptr;
  }
  jint values[2] = {host_pid, control_fd};
  jintArray result = env->NewIntArray(2);
  if (result == nullptr || env->ExceptionCheck()) {
    close(control_fd);
    darwin_art_process::release_service_process(host_pid);
    return nullptr;
  }
  env->SetIntArrayRegion(result, 0, 2, values);
  return env->ExceptionCheck() ? nullptr : result;
}

jint ReleaseRemoteService(JNIEnv* env, jclass, jint host_pid, jint control_fd) {
  darwin_art::CloseRemoteBinderChannel(env, control_fd);
  const int close_status = control_fd < 0 ? -1 : close(control_fd);
  const int32_t release_status =
      darwin_art_process::release_service_process(host_pid);
  return close_status == 0 && release_status == 0 ? 0 : -1;
}

jboolean RemoteTransact(JNIEnv* env, jclass, jint control_fd, jint target_id,
                        jint code, jobject data, jobject reply, jint flags) {
  return darwin_art::TransactRemoteBinder(env, control_fd, target_id, code,
                                         data, reply, flags);
}

jstring ResolveInstalledPackage(JNIEnv* env, jclass, jstring package_name) {
  const std::string package = JavaString(env, package_name);
  const char* socket_path = std::getenv("DARWIN_ART_SYSTEM_SERVER_SOCKET");
  const std::string record = darwin_art::QuerySystemPackageRecord(
      env, socket_path, package.c_str());
  return record.empty() ? nullptr : env->NewStringUTF(record.c_str());
}

bool InstallPackageManagerNatives(JNIEnv* env, jobject package_manager) {
  jclass package_manager_class = env->GetObjectClass(package_manager);
  JNINativeMethod methods[] = {
      {const_cast<char*>("nativeResolveInstalledPackage"),
       const_cast<char*>("(Ljava/lang/String;)Ljava/lang/String;"),
       reinterpret_cast<void*>(&ResolveInstalledPackage)},
  };
  const bool installed = package_manager_class != nullptr &&
                         env->RegisterNatives(package_manager_class, methods, 1) == JNI_OK;
  const char* verify_package = std::getenv("DARWIN_ART_VERIFY_SYSTEM_PACKAGE");
  if (installed && verify_package != nullptr && *verify_package != '\0') {
    jmethodID get_package_info = env->GetMethodID(
        package_manager_class, "getPackageInfo",
        "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
    jstring requested = env->NewStringUTF(verify_package);
    jobject info = get_package_info == nullptr
                       ? nullptr
                       : env->CallObjectMethod(package_manager, get_package_info,
                                               requested, 0);
    env->DeleteLocalRef(requested);
    if (info == nullptr || env->ExceptionCheck()) {
      if (env->ExceptionCheck()) env->ExceptionClear();
      env->DeleteLocalRef(info);
      env->DeleteLocalRef(package_manager_class);
      return false;
    }
    std::cerr << "ART system_server-lite: PackageManager resolved "
              << verify_package << " over Binder\n";
    env->DeleteLocalRef(info);
  }
  env->DeleteLocalRef(package_manager_class);
  return installed;
}

bool InstallHostServiceNatives(JNIEnv* env, jclass probe_context_class) {
  JNINativeMethod methods[] = {
      {const_cast<char*>("nativeSpawnService"),
       const_cast<char*>(
           "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
           "ZLandroid/content/Intent;)[I"),
       reinterpret_cast<void*>(&SpawnService)},
      {const_cast<char*>("nativeReleaseRemoteService"),
       const_cast<char*>("(II)I"),
       reinterpret_cast<void*>(&ReleaseRemoteService)},
      {const_cast<char*>("nativeRemoteTransact"),
       const_cast<char*>(
           "(IIILandroid/os/Parcel;Landroid/os/Parcel;I)Z"),
       reinterpret_cast<void*>(&RemoteTransact)},
  };
  return env->RegisterNatives(probe_context_class, methods,
                              static_cast<jint>(std::size(methods))) == JNI_OK;
}

bool EnsureActivityThreadConfigurationController(JNIEnv* env,
                                                 jobject activity_thread) {
  if (env == nullptr || activity_thread == nullptr) return false;
  jclass activity_thread_class = env->FindClass("android/app/ActivityThread");
  jclass controller_class =
      env->FindClass("android/app/ConfigurationController");
  jfieldID controller_field =
      activity_thread_class == nullptr
          ? nullptr
          : env->GetFieldID(activity_thread_class, "mConfigurationController",
                            "Landroid/app/ConfigurationController;");
  if (controller_class == nullptr || controller_field == nullptr ||
      env->ExceptionCheck()) {
    return false;
  }
  jobject controller =
      env->GetObjectField(activity_thread, controller_field);
  if (controller != nullptr) return !env->ExceptionCheck();
  jmethodID constructor = env->GetMethodID(
      controller_class, "<init>", "(Landroid/app/ActivityThreadInternal;)V");
  controller = constructor == nullptr
                   ? nullptr
                   : env->NewObject(controller_class, constructor,
                                    activity_thread);
  if (controller == nullptr || env->ExceptionCheck()) return false;
  env->SetObjectField(activity_thread, controller_field, controller);
  return !env->ExceptionCheck();
}

}  // namespace

int prepare(JNIEnv* env, art::Thread* self, jobject* activity_instance_out,
            jclass probe_activity_class, jclass probe_context_class,
            const darwin_art_app_resources::Bundle* resources,
            jobject package_manager, bool run_apk_app,
            bool use_framework_resources, const char* apk_app_package,
            const char* apk_app_activity, bool application_only, Bundle* out) {
  if (env == nullptr || self == nullptr || activity_instance_out == nullptr ||
      probe_activity_class == nullptr || probe_context_class == nullptr ||
      resources == nullptr || package_manager == nullptr || out == nullptr) {
    return 27;
  }
  jobject activity_instance = *activity_instance_out;
  if (!run_apk_app && activity_instance == nullptr) {
    return 27;
  }
  if (!InstallHostServiceNatives(env, probe_context_class) ||
      !InstallPackageManagerNatives(env, package_manager) ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android process: host service JNI setup failed\n";
    return 27;
  }
  *out = {};
  const char* configured_theme =
      run_apk_app ? std::getenv("DARWIN_ART_APK_APP_THEME") : nullptr;
  const jint app_theme = configured_theme == nullptr
                             ? 0
                             : static_cast<jint>(
                                   std::strtoul(configured_theme, nullptr, 0));
  const char* configured_screen_orientation =
      run_apk_app ? std::getenv("DARWIN_ART_APK_APP_SCREEN_ORIENTATION") : nullptr;
  const jint app_screen_orientation =
      configured_screen_orientation == nullptr
          ? -1
          : static_cast<jint>(std::strtol(configured_screen_orientation, nullptr, 10));
  const char* configured_target_sdk =
      run_apk_app ? std::getenv("DARWIN_ART_APK_APP_TARGET_SDK") : nullptr;
  const jint app_target_sdk =
      configured_target_sdk == nullptr
          ? 36
          : static_cast<jint>(std::strtoul(configured_target_sdk, nullptr, 10));
  const char* configured_version_code =
      run_apk_app ? std::getenv("DARWIN_ART_APK_APP_VERSION_CODE") : nullptr;
  const jint app_version_code =
      configured_version_code == nullptr
          ? 0
          : static_cast<jint>(
                std::strtoul(configured_version_code, nullptr, 10));
  const char* configured_version_name =
      run_apk_app ? std::getenv("DARWIN_ART_APK_APP_VERSION_NAME") : nullptr;
  std::string app_label = run_apk_app &&
                                  std::getenv("DARWIN_ART_APK_APP_LABEL") != nullptr
                              ? std::getenv("DARWIN_ART_APK_APP_LABEL")
                              : "Darwin ART APK";
  const char* configured_app_label_res =
      run_apk_app ? std::getenv("DARWIN_ART_APK_APP_LABEL_RES") : nullptr;
  const jint app_label_res =
      configured_app_label_res == nullptr
          ? 0
          : static_cast<jint>(std::strtoul(configured_app_label_res, nullptr, 0));
  const char* configured_activity_label =
      run_apk_app ? std::getenv("DARWIN_ART_APK_ACTIVITY_LABEL") : nullptr;
  std::string activity_label = configured_activity_label == nullptr
                                   ? std::string()
                                   : configured_activity_label;
  const char* configured_activity_label_res =
      run_apk_app ? std::getenv("DARWIN_ART_APK_ACTIVITY_LABEL_RES") : nullptr;
  const jint activity_label_res =
      configured_activity_label_res == nullptr
          ? 0
          : static_cast<jint>(std::strtoul(configured_activity_label_res,
                                           nullptr, 0));
  if (run_apk_app) {
    jclass vm_runtime_class = env->FindClass("dalvik/system/VMRuntime");
    jmethodID get_vm_runtime =
        vm_runtime_class == nullptr
            ? nullptr
            : env->GetStaticMethodID(vm_runtime_class, "getRuntime",
                                     "()Ldalvik/system/VMRuntime;");
    jobject vm_runtime =
        get_vm_runtime == nullptr
            ? nullptr
            : env->CallStaticObjectMethod(vm_runtime_class, get_vm_runtime);
    jmethodID set_vm_target_sdk =
        vm_runtime_class == nullptr
            ? nullptr
            : env->GetMethodID(vm_runtime_class, "setTargetSdkVersion", "(I)V");
    if (vm_runtime == nullptr || set_vm_target_sdk == nullptr ||
        env->ExceptionCheck()) {
      std::cerr << "ART Android window: VM target SDK setup failed\n";
      return 27;
    }
    env->CallVoidMethod(vm_runtime, set_vm_target_sdk, app_target_sdk);
    env->DeleteLocalRef(vm_runtime);
    env->DeleteLocalRef(vm_runtime_class);

  }
  if (app_theme != 0 && resources->activity_info != nullptr) {
    jfieldID activity_theme = env->GetFieldID(
        resources->activity_info_class, "theme", "I");
    if (activity_theme != nullptr) {
      env->SetIntField(resources->activity_info, activity_theme, app_theme);
    }
  }
  if (run_apk_app && configured_screen_orientation != nullptr &&
      resources->activity_info != nullptr) {
    jfieldID activity_orientation = env->GetFieldID(
        resources->activity_info_class, "screenOrientation", "I");
    if (activity_orientation != nullptr && !env->ExceptionCheck()) {
      env->SetIntField(resources->activity_info, activity_orientation,
                       app_screen_orientation);
    }
  }
  if (run_apk_app && resources->activity_info != nullptr) {
    jfieldID activity_label_resource = env->GetFieldID(
        resources->activity_info_class, "labelRes", "I");
    jfieldID non_localized_label = env->GetFieldID(
        resources->activity_info_class, "nonLocalizedLabel",
        "Ljava/lang/CharSequence;");
    if (activity_label_res != 0 && activity_label_resource != nullptr &&
        !env->ExceptionCheck()) {
      // Keep resource-backed labels as a resource reference. Storing the
      // package fallback in nonLocalizedLabel would mask localization in
      // ActivityInfo.loadLabel().
      env->SetIntField(resources->activity_info, activity_label_resource,
                       activity_label_res);
      if (non_localized_label != nullptr && !env->ExceptionCheck()) {
        env->SetObjectField(resources->activity_info, non_localized_label,
                            nullptr);
      }
    }
    if (activity_label_res == 0 && !activity_label.empty()) {
      if (activity_label_resource != nullptr && !env->ExceptionCheck()) {
        env->SetIntField(resources->activity_info, activity_label_resource, 0);
      }
      jclass char_sequence_class = env->FindClass("java/lang/String");
      jstring literal = char_sequence_class == nullptr
                            ? nullptr
                            : env->NewStringUTF(activity_label.c_str());
      if (non_localized_label != nullptr && literal != nullptr &&
          !env->ExceptionCheck()) {
        env->SetObjectField(resources->activity_info, non_localized_label,
                            literal);
      }
      if (literal != nullptr) env->DeleteLocalRef(literal);
      if (char_sequence_class != nullptr)
        env->DeleteLocalRef(char_sequence_class);
    }
  }
  out->activity_class = env->GetSuperclass(probe_activity_class);
  jclass package_manager_class = env->GetObjectClass(package_manager);
  jmethodID configure_package_manager =
      package_manager_class == nullptr
          ? nullptr
          : env->GetMethodID(
                package_manager_class, "configure",
                "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                "Ljava/lang/String;"
                "Ljava/lang/String;"
                "Landroid/content/pm/ActivityInfo;I"
                "Landroid/content/res/Resources;ILjava/lang/String;)V");
  jstring configured_package = env->NewStringUTF(
      run_apk_app ? apk_app_package : "dev.darwinart.probe");
  jstring configured_activity = env->NewStringUTF(
      run_apk_app ? apk_app_activity : "dev.darwinart.probe.ProbeActivity");
  const char* configured_activities =
      run_apk_app ? std::getenv("DARWIN_ART_APK_APP_ACTIVITIES") : nullptr;
  jstring configured_activity_names = env->NewStringUTF(
      configured_activities == nullptr
          ? "dev.darwinart.probe.ProbeActivity=0x0"
          : configured_activities);
  const char* configured_aliases =
      run_apk_app ? std::getenv("DARWIN_ART_APK_APP_ACTIVITY_ALIASES") : nullptr;
  jstring configured_activity_aliases = env->NewStringUTF(
      configured_aliases == nullptr ? "none" : configured_aliases);
  const char* configured_services =
      run_apk_app ? std::getenv("DARWIN_ART_APK_APP_SERVICES") : "none";
  jstring configured_service_names = env->NewStringUTF(
      configured_services == nullptr ? "none" : configured_services);
  jstring configured_version = env->NewStringUTF(
      configured_version_name == nullptr ? "0" : configured_version_name);
  if (configure_package_manager != nullptr && configured_package != nullptr &&
      configured_activity != nullptr && configured_service_names != nullptr &&
      configured_activity_names != nullptr &&
      configured_activity_aliases != nullptr &&
      configured_version != nullptr &&
      resources->activity_info != nullptr) {
    env->CallVoidMethod(package_manager, configure_package_manager,
                        configured_package, configured_activity,
                        configured_activity_names,
                        configured_activity_aliases,
                        configured_service_names,
                        resources->activity_info, app_target_sdk,
                        resources->probe_resources, app_version_code,
                        configured_version);
  }
  env->DeleteLocalRef(configured_version);
  env->DeleteLocalRef(configured_service_names);
  env->DeleteLocalRef(configured_activity_aliases);
  env->DeleteLocalRef(configured_activity_names);
  env->DeleteLocalRef(configured_activity);
  env->DeleteLocalRef(configured_package);
  env->DeleteLocalRef(package_manager_class);
  if (configure_package_manager == nullptr || configured_version == nullptr ||
      env->ExceptionCheck()) {
    std::cerr << "ART Android window: package metadata setup failed\n";
    return 27;
  }
  jclass intent_class = env->FindClass("android/content/Intent");
  jclass component_name_class =
      env->FindClass("android/content/ComponentName");
  jclass configuration_class =
      env->FindClass("android/content/res/Configuration");
  jmethodID configuration_constructor =
      configuration_class == nullptr
          ? nullptr
          : env->GetMethodID(configuration_class, "<init>",
                             "(Landroid/content/res/Configuration;)V");
  jclass resources_class = env->FindClass("android/content/res/Resources");
  jmethodID get_configuration =
      resources_class == nullptr
          ? nullptr
          : env->GetMethodID(resources_class, "getConfiguration",
                             "()Landroid/content/res/Configuration;");
  jobject resource_configuration =
      get_configuration == nullptr || resources->probe_resources == nullptr
          ? nullptr
          : env->CallObjectMethod(resources->probe_resources,
                                  get_configuration);
  jmethodID probe_context_constructor = env->GetMethodID(
      probe_context_class, "<init>",
      "(Landroid/content/res/Resources;"
      "Landroid/content/pm/PackageManager;Ljava/lang/String;)V");
  if (!run_apk_app) {
    probe_context_constructor = env->GetMethodID(
        probe_context_class, "<init>",
        "(Landroid/content/res/Resources;"
        "Landroid/content/pm/PackageManager;)V");
  }
  jstring context_package = env->NewStringUTF(
      run_apk_app ? apk_app_package : "dev.darwinart.probe");
  out->probe_context =
      probe_context_constructor == nullptr || resources->probe_resources == nullptr
          ? nullptr
          : (run_apk_app
                 ? env->NewObject(probe_context_class, probe_context_constructor,
                                  resources->probe_resources, package_manager,
                                  context_package)
                 : env->NewObject(probe_context_class, probe_context_constructor,
                                  resources->probe_resources, package_manager));
  env->DeleteLocalRef(context_package);
  if (out->probe_context == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android window: ProbeContext construction failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 27;
  }
  jmethodID set_target_sdk = env->GetMethodID(
      probe_context_class, "setTargetSdkVersion", "(I)V");
  if (set_target_sdk == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android window: target SDK setup failed\n";
    return 27;
  }
  env->CallVoidMethod(out->probe_context, set_target_sdk, app_target_sdk);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android window: target SDK setup threw\n";
    return 27;
  }
  jmethodID configure_compatibility = env->GetStaticMethodID(
      probe_context_class, "configureCompatibility", "(I)V");
  if (configure_compatibility == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android window: compatibility setup failed\n";
    return 27;
  }
  env->CallStaticVoidMethod(probe_context_class, configure_compatibility,
                            app_target_sdk);
  if (env->ExceptionCheck()) {
    std::cerr << "ART Android window: compatibility setup threw\n";
    return 27;
  }

  jobject application = resources->application;
  jobject apk_application = nullptr;
  const char* application_name =
      run_apk_app ? std::getenv("DARWIN_ART_APK_APP_APPLICATION") : nullptr;
  // ActivityThread attaches every Application, including the base
  // android.app.Application class, to the process Context before any Activity
  // lifecycle callback. Do not special-case the framework class: apps that do
  // not declare a subclass still rely on its database/files/services context.
  if (application_name != nullptr) {
    jclass class_class = env->FindClass("java/lang/Class");
    jmethodID get_class_loader =
        class_class == nullptr
            ? nullptr
            : env->GetMethodID(class_class, "getClassLoader",
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
            : env->GetMethodID(loader_class, "loadClass",
                               "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring application_class_name = env->NewStringUTF(application_name);
    jclass application_class =
        load_class == nullptr || application_class_name == nullptr
            ? nullptr
            : reinterpret_cast<jclass>(env->CallObjectMethod(
                  app_loader, load_class, application_class_name));
    jmethodID application_constructor =
        application_class == nullptr
            ? nullptr
            : env->GetMethodID(application_class, "<init>", "()V");
    apk_application =
        application_constructor == nullptr
            ? nullptr
            : env->NewObject(application_class, application_constructor);
    jclass context_wrapper_class =
        env->FindClass("android/content/ContextWrapper");
    jfieldID application_base =
        context_wrapper_class == nullptr
            ? nullptr
            : env->GetFieldID(context_wrapper_class, "mBase",
                              "Landroid/content/Context;");
    jmethodID application_attach_base =
        context_wrapper_class == nullptr
            ? nullptr
            : env->GetMethodID(context_wrapper_class, "attachBaseContext",
                               "(Landroid/content/Context;)V");
    jmethodID application_on_create =
        resources->application_class == nullptr
            ? nullptr
            : env->GetMethodID(resources->application_class, "onCreate", "()V");
    jclass activity_thread_class = env->FindClass("android/app/ActivityThread");
    jfieldID current_activity_thread =
        activity_thread_class == nullptr
            ? nullptr
            : env->GetStaticFieldID(activity_thread_class,
                                    "sCurrentActivityThread",
                                    "Landroid/app/ActivityThread;");
    jfieldID initial_application =
        activity_thread_class == nullptr
            ? nullptr
            : env->GetFieldID(activity_thread_class, "mInitialApplication",
                              "Landroid/app/Application;");
    jfieldID bound_application =
        activity_thread_class == nullptr
            ? nullptr
            : env->GetFieldID(activity_thread_class, "mBoundApplication",
                              "Landroid/app/ActivityThread$AppBindData;");
    jobject activity_thread =
        current_activity_thread == nullptr
            ? nullptr
            : env->GetStaticObjectField(activity_thread_class,
                                        current_activity_thread);
    if (activity_thread == nullptr && activity_thread_class != nullptr &&
        !env->ExceptionCheck()) {
      jmethodID activity_thread_constructor =
          env->GetMethodID(activity_thread_class, "<init>", "()V");
      if (activity_thread_constructor != nullptr && !env->ExceptionCheck()) {
        activity_thread =
            env->NewObject(activity_thread_class, activity_thread_constructor);
        if (activity_thread != nullptr && !env->ExceptionCheck()) {
          env->SetStaticObjectField(activity_thread_class,
                                    current_activity_thread, activity_thread);
        }
      }
    }
    if (!EnsureActivityThreadConfigurationController(env, activity_thread)) {
      std::cerr << "ART Android process: ActivityThread configuration setup "
                   "failed\n";
      if (env->ExceptionCheck()) env->ExceptionDescribe();
      return 27;
    }
    // bindApplication() publishes this record before Application.attach().
    // Application.getProcessName/currentPackageName intentionally read it.
    jclass bind_data_class =
        env->FindClass("android/app/ActivityThread$AppBindData");
    jmethodID bind_data_constructor =
        bind_data_class == nullptr
            ? nullptr
            : env->GetMethodID(bind_data_class, "<init>", "()V");
    jobject bind_data =
        bind_data_constructor == nullptr
            ? nullptr
            : env->NewObject(bind_data_class, bind_data_constructor);
    jfieldID bind_process_name =
        bind_data_class == nullptr
            ? nullptr
            : env->GetFieldID(bind_data_class, "processName",
                              "Ljava/lang/String;");
    jclass application_info_class =
        env->FindClass("android/content/pm/ApplicationInfo");
    jmethodID application_info_constructor =
        application_info_class == nullptr
            ? nullptr
            : env->GetMethodID(application_info_class, "<init>", "()V");
    jobject application_info =
        application_info_constructor == nullptr
            ? nullptr
            : env->NewObject(application_info_class,
                             application_info_constructor);
    jfieldID bind_app_info =
        bind_data_class == nullptr
            ? nullptr
            : env->GetFieldID(bind_data_class, "appInfo",
                              "Landroid/content/pm/ApplicationInfo;");
    jfieldID app_info_package =
        application_info_class == nullptr
            ? nullptr
            : env->GetFieldID(application_info_class, "packageName",
                              "Ljava/lang/String;");
    jfieldID app_info_process =
        application_info_class == nullptr
            ? nullptr
            : env->GetFieldID(application_info_class, "processName",
                              "Ljava/lang/String;");
    const char* configured_process_name =
        std::getenv("DARWIN_ART_APK_PROCESS_NAME");
    jstring process_name = env->NewStringUTF(
        configured_process_name == nullptr ? apk_app_package
                                           : configured_process_name);
    jstring application_package = env->NewStringUTF(apk_app_package);
    // Zygote sets Process.sArgV0 before ActivityThread.bindApplication().
    // Process.myProcessName() is a Java accessor for that field (not a native
    // method), and Android SDKs require its @NonNull contract during eager
    // application initialization.
    jclass process_class = env->FindClass("android/os/Process");
    jfieldID process_arg_v0 =
        process_class == nullptr
            ? nullptr
            : env->GetStaticFieldID(process_class, "sArgV0",
                                    "Ljava/lang/String;");
    if (process_arg_v0 != nullptr && process_name != nullptr &&
        !env->ExceptionCheck()) {
      env->SetStaticObjectField(process_class, process_arg_v0, process_name);
    }
    if (activity_thread != nullptr && bind_data != nullptr &&
        application_info != nullptr && bound_application != nullptr &&
        bind_process_name != nullptr && bind_app_info != nullptr &&
        app_info_package != nullptr && app_info_process != nullptr &&
        process_name != nullptr && application_package != nullptr &&
        !env->ExceptionCheck()) {
      env->SetObjectField(bind_data, bind_process_name, process_name);
      env->SetObjectField(application_info, app_info_package,
                          application_package);
      env->SetObjectField(application_info, app_info_process, process_name);
      env->SetObjectField(bind_data, bind_app_info, application_info);
      env->SetObjectField(activity_thread, bound_application, bind_data);
    }
    if (apk_application != nullptr && application_base != nullptr &&
        application_attach_base != nullptr && application_on_create != nullptr &&
        activity_thread != nullptr && bind_data != nullptr &&
        !env->ExceptionCheck()) {
      // ActivityThread's Application.attach() invokes the application's
      // virtual attachBaseContext() before onCreate(). Calling the virtual hook
      // here preserves application bootstrap logic (Chromium publishes its
      // process Context from this callback) while avoiding Application.attach's
      // ContextImpl-only LoadedApk bookkeeping in the detached host.
      env->CallVoidMethod(apk_application, application_attach_base,
                          out->probe_context);
      if (env->ExceptionCheck()) {
        std::cerr << "ART Android window: application attachBaseContext failed\n";
        if (self->IsExceptionPending()) {
          std::cerr << self->GetException()->Dump() << "\n";
        }
        return 27;
      }
      // Application.attach() guarantees a usable ContextWrapper base before
      // returning to ActivityThread. Some split-aware Applications publish
      // themselves from an override before eventually delegating to the
      // framework implementation; preserve the callback, then establish the
      // framework invariant if that delegation did not attach the base.
      jobject application_context_base =
          env->GetObjectField(apk_application, application_base);
      if (application_context_base == nullptr && !env->ExceptionCheck()) {
        env->CallNonvirtualVoidMethod(apk_application, context_wrapper_class,
                                      application_attach_base,
                                      out->probe_context);
      }
      env->DeleteLocalRef(application_context_base);
      if (env->ExceptionCheck()) {
        std::cerr << "ART Android window: Application base Context setup failed\n";
        return 27;
      }
      jmethodID set_application_context = env->GetMethodID(
          probe_context_class, "setApplicationContext",
          "(Landroid/content/Context;)V");
      if (set_application_context == nullptr || env->ExceptionCheck()) {
        std::cerr << "ART Android window: application context identity setup failed\n";
        return 27;
      }
      env->CallVoidMethod(out->probe_context, set_application_context,
                          apk_application);
      if (activity_thread == nullptr || initial_application == nullptr ||
          env->ExceptionCheck()) {
        std::cerr << "ART Android window: ActivityThread application setup failed\n";
        return 27;
      }
      env->SetObjectField(activity_thread, initial_application,
                          apk_application);
      env->DeleteLocalRef(application_package);
      env->DeleteLocalRef(process_name);
      env->DeleteLocalRef(application_info);
      env->DeleteLocalRef(application_info_class);
      env->DeleteLocalRef(bind_data);
      env->DeleteLocalRef(bind_data_class);
      env->DeleteLocalRef(activity_thread);
      env->DeleteLocalRef(activity_thread_class);
      jobject installed_base =
          env->GetObjectField(apk_application, application_base);
      if (installed_base == nullptr && !env->ExceptionCheck()) {
        std::cerr << "ART Android window: application base context was not installed\n";
      }
      env->DeleteLocalRef(installed_base);
      jclass context_class = env->FindClass("android/content/Context");
      jmethodID get_application_context =
          context_class == nullptr
              ? nullptr
              : env->GetMethodID(context_class, "getApplicationContext",
                                 "()Landroid/content/Context;");
      jobject installed_application_context =
          get_application_context == nullptr || env->ExceptionCheck()
              ? nullptr
              : env->CallObjectMethod(apk_application,
                                      get_application_context);
      if (installed_application_context == nullptr && !env->ExceptionCheck()) {
        std::cerr << "ART Android window: application context is null after install\n";
      }
      env->DeleteLocalRef(installed_application_context);
      env->DeleteLocalRef(context_class);
      if (!env->ExceptionCheck()) {
        jfieldID activity_application_info =
            resources->activity_info_class == nullptr
                ? nullptr
                : env->GetFieldID(resources->activity_info_class,
                                  "applicationInfo",
                                  "Landroid/content/pm/ApplicationInfo;");
        jobject provider_application_info =
            activity_application_info == nullptr ||
                    resources->activity_info == nullptr
                ? nullptr
                : env->GetObjectField(resources->activity_info,
                                      activity_application_info);
        if (!InstallDeclaredContentProviders(
                env, out->probe_context, provider_application_info, app_loader)) {
          std::cerr << "ART Android process: ContentProvider bootstrap failed\n";
          env->DeleteLocalRef(provider_application_info);
          return 27;
        }
        env->DeleteLocalRef(provider_application_info);
        // Zygote initializes Typeface before Application.onCreate(). Arbitrary
        // APKs, including Chromium, can resolve fonts from their Application
        // callback, so the detached process must establish the same invariant
        // before entering app code rather than waiting for Activity creation.
        jstring font_bootstrap_name =
            env->NewStringUTF("dev.darwinart.probe.FontBootstrap");
        jclass font_bootstrap =
            load_class == nullptr || font_bootstrap_name == nullptr
                ? nullptr
                : reinterpret_cast<jclass>(env->CallObjectMethod(
                      app_loader, load_class, font_bootstrap_name));
        jmethodID install_fonts =
            font_bootstrap == nullptr
                ? nullptr
                : env->GetStaticMethodID(font_bootstrap, "install", "()V");
        if (install_fonts != nullptr && !env->ExceptionCheck()) {
          env->CallStaticVoidMethod(font_bootstrap, install_fonts);
          if (env->ExceptionCheck()) {
            // A minimal Darwin image may not expose the platform Minikin
            // native Typeface factory even though the APK's Java framework
            // classes are present. Keep Application.onCreate reachable while
            // preserving the pending exception as a diagnostic; text paths
            // can then use the Skia-backed host fallback instead of aborting
            // the entire Android process during zygote-equivalent bootstrap.
            std::cerr << "ART Android framework: system font bootstrap unavailable;"
                      << " continuing with host font fallback\n";
            env->ExceptionDescribe();
            env->ExceptionClear();
          }
        }
        env->DeleteLocalRef(font_bootstrap);
        env->DeleteLocalRef(font_bootstrap_name);
      }
      if (!env->ExceptionCheck()) {
        env->CallVoidMethod(apk_application, application_on_create);
      }
    }
    env->DeleteLocalRef(application_class);
    env->DeleteLocalRef(context_wrapper_class);
    env->DeleteLocalRef(application_class_name);
    env->DeleteLocalRef(loader_class);
    env->DeleteLocalRef(app_loader);
    env->DeleteLocalRef(class_class);
    if (apk_application == nullptr || env->ExceptionCheck()) {
      std::cerr << "ART Android window: application bootstrap failed\n";
      if (self->IsExceptionPending()) {
        std::cerr << self->GetException()->Dump() << "\n";
      }
      return 27;
    }
    application = apk_application;
  }

  // ActivityThread creates and attaches the Application before invoking the
  // Activity constructor. Chromium consults process-wide application state
  // directly from ChromeTabbedActivity's constructor, so constructing it in
  // the caller would invert a platform lifecycle guarantee.
  if (run_apk_app && activity_instance == nullptr) {
    out->application = application;
    if (application_only) return 0;
    jmethodID activity_constructor =
        env->GetMethodID(probe_activity_class, "<init>", "()V");
    activity_instance = activity_constructor == nullptr
                            ? nullptr
                            : env->NewObject(probe_activity_class,
                                             activity_constructor);
    if (activity_instance == nullptr || env->ExceptionCheck()) {
      std::cerr << "ART Android owner: Activity construction failed\n";
      if (self->IsExceptionPending()) {
        std::cerr << self->GetException()->Dump() << "\n";
      }
      return 23;
    }
    *activity_instance_out = activity_instance;
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
  jmethodID set_action =
      intent_class == nullptr
          ? nullptr
          : env->GetMethodID(intent_class, "setAction",
                             "(Ljava/lang/String;)Landroid/content/Intent;");
  jmethodID add_category =
      intent_class == nullptr
          ? nullptr
          : env->GetMethodID(intent_class, "addCategory",
                             "(Ljava/lang/String;)Landroid/content/Intent;");
  jmethodID add_flags =
      intent_class == nullptr
          ? nullptr
          : env->GetMethodID(intent_class, "addFlags",
                             "(I)Landroid/content/Intent;");
  jmethodID set_data_and_type =
      intent_class == nullptr
          ? nullptr
          : env->GetMethodID(intent_class, "setDataAndType",
                             "(Landroid/net/Uri;Ljava/lang/String;)"
                             "Landroid/content/Intent;");
  jmethodID set_data =
      intent_class == nullptr
          ? nullptr
          : env->GetMethodID(intent_class, "setData",
                             "(Landroid/net/Uri;)Landroid/content/Intent;");
  jstring package_name = env->NewStringUTF(
      run_apk_app ? apk_app_package : "dev.darwinart.probe");
  const char* configured_launch_component =
      run_apk_app ? std::getenv("DARWIN_ART_APK_APP_LAUNCH_COMPONENT") : nullptr;
  jstring class_name = env->NewStringUTF(
      run_apk_app && configured_launch_component != nullptr
          ? configured_launch_component
          : (run_apk_app ? apk_app_activity
                         : "dev.darwinart.probe.ProbeActivity"));
  std::string window_title = run_apk_app ? app_label : "Darwin ART Probe";
  if (run_apk_app && app_label_res != 0 && resources->probe_resources != nullptr) {
    jclass resources_class = env->FindClass("android/content/res/Resources");
    jmethodID get_string =
        resources_class == nullptr
            ? nullptr
            : env->GetMethodID(resources_class, "getString",
                               "(I)Ljava/lang/String;");
    jstring resolved =
        get_string == nullptr || env->ExceptionCheck()
            ? nullptr
            : static_cast<jstring>(env->CallObjectMethod(
                  resources->probe_resources, get_string, app_label_res));
    if (resolved != nullptr && !env->ExceptionCheck()) {
      const char* utf = env->GetStringUTFChars(resolved, nullptr);
      if (utf != nullptr) {
        window_title = utf;
        env->ReleaseStringUTFChars(resolved, utf);
      }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (resolved != nullptr) env->DeleteLocalRef(resolved);
    if (resources_class != nullptr) env->DeleteLocalRef(resources_class);
  }
  if (run_apk_app) {
    // Host presentation consumes a resolved title, while package metadata
    // keeps the original resource reference and literal label separate.
    setenv("DARWIN_ART_APK_WINDOW_TITLE", window_title.c_str(), 1);
  }
  jstring title = env->NewStringUTF(window_title.c_str());
  jobject component_name =
      component_name_constructor == nullptr
          ? nullptr
          : env->NewObject(component_name_class, component_name_constructor,
                           package_name, class_name);
  jobject intent = intent_constructor == nullptr
                       ? nullptr
                       : env->NewObject(intent_class, intent_constructor);
  jobject configuration =
      configuration_constructor == nullptr || resource_configuration == nullptr
          ? nullptr
          : env->NewObject(configuration_class, configuration_constructor,
                           resource_configuration);
  jclass activity_thread_class = env->FindClass("android/app/ActivityThread");
  jfieldID current_activity_thread =
      activity_thread_class == nullptr
          ? nullptr
          : env->GetStaticFieldID(activity_thread_class,
                                  "sCurrentActivityThread",
                                  "Landroid/app/ActivityThread;");
  jobject activity_thread =
      current_activity_thread == nullptr
          ? nullptr
          : env->GetStaticObjectField(activity_thread_class,
                                      current_activity_thread);
  if (activity_thread == nullptr && activity_thread_class != nullptr &&
      !env->ExceptionCheck()) {
    jmethodID activity_thread_constructor =
        env->GetMethodID(activity_thread_class, "<init>", "()V");
    if (activity_thread_constructor != nullptr && !env->ExceptionCheck()) {
      activity_thread = env->NewObject(activity_thread_class,
                                       activity_thread_constructor);
      if (activity_thread != nullptr && !env->ExceptionCheck()) {
        env->SetStaticObjectField(activity_thread_class,
                                  current_activity_thread,
                                  activity_thread);
      }
    }
  }
  if (!EnsureActivityThreadConfigurationController(env, activity_thread)) {
    std::cerr << "ART Android process: ActivityThread configuration setup "
                 "failed\n";
    if (env->ExceptionCheck()) env->ExceptionDescribe();
    return 27;
  }
  jclass instrumentation_class = env->FindClass("android/app/Instrumentation");
  jmethodID instrumentation_constructor =
      instrumentation_class == nullptr
          ? nullptr
          : env->GetMethodID(instrumentation_class, "<init>", "()V");
  jobject instrumentation =
      instrumentation_constructor == nullptr
          ? nullptr
          : env->NewObject(instrumentation_class,
                           instrumentation_constructor);
  jclass binder_class = env->FindClass("android/os/Binder");
  jmethodID binder_constructor =
      binder_class == nullptr
          ? nullptr
          : env->GetMethodID(binder_class, "<init>", "()V");
  jobject activity_token =
      binder_constructor == nullptr
          ? nullptr
          : env->NewObject(binder_class, binder_constructor);
  if (intent != nullptr && set_component != nullptr && component_name != nullptr) {
    jobject configured_intent =
        env->CallObjectMethod(intent, set_component, component_name);
    env->DeleteLocalRef(configured_intent);
  }
  const char* launch_action =
      run_apk_app ? std::getenv("DARWIN_ART_APK_APP_INTENT_ACTION") : nullptr;
  const bool default_launcher_intent =
      run_apk_app && (launch_action == nullptr || *launch_action == '\0');
  if (intent != nullptr && set_action != nullptr && run_apk_app) {
    jstring action = env->NewStringUTF(
        default_launcher_intent ? "android.intent.action.MAIN" : launch_action);
    jobject configured_intent = env->CallObjectMethod(intent, set_action, action);
    env->DeleteLocalRef(configured_intent);
    env->DeleteLocalRef(action);
  }
  if (intent != nullptr && add_category != nullptr && default_launcher_intent &&
      !env->ExceptionCheck()) {
    jstring category =
        env->NewStringUTF("android.intent.category.LAUNCHER");
    jobject configured_intent =
        env->CallObjectMethod(intent, add_category, category);
    env->DeleteLocalRef(configured_intent);
    env->DeleteLocalRef(category);
  }
  if (intent != nullptr && add_flags != nullptr && default_launcher_intent &&
      !env->ExceptionCheck()) {
    constexpr jint kFlagActivityNewTask = 0x10000000;
    jobject configured_intent =
        env->CallObjectMethod(intent, add_flags, kFlagActivityNewTask);
    env->DeleteLocalRef(configured_intent);
  }
  const char* launch_uri =
      run_apk_app ? std::getenv("DARWIN_ART_APK_APP_INTENT_URI") : nullptr;
  const char* launch_type =
      run_apk_app ? std::getenv("DARWIN_ART_APK_APP_INTENT_TYPE") : nullptr;
  if (intent != nullptr && launch_uri != nullptr && *launch_uri != '\0' &&
      (set_data != nullptr || set_data_and_type != nullptr)) {
    jclass uri_class = env->FindClass("android/net/Uri");
    jmethodID parse_uri =
        uri_class == nullptr
            ? nullptr
            : env->GetStaticMethodID(uri_class, "parse",
                                     "(Ljava/lang/String;)Landroid/net/Uri;");
    jstring uri_text = env->NewStringUTF(launch_uri);
    jobject uri = parse_uri == nullptr
                      ? nullptr
                      : env->CallStaticObjectMethod(uri_class, parse_uri, uri_text);
    if (uri != nullptr && !env->ExceptionCheck()) {
      const bool has_explicit_type =
          launch_type != nullptr && *launch_type != '\0';
      jstring mime_type = has_explicit_type ? env->NewStringUTF(launch_type)
                                            : nullptr;
      jobject configured_intent = has_explicit_type && set_data_and_type != nullptr
                                      ? env->CallObjectMethod(
                                            intent, set_data_and_type, uri,
                                            mime_type)
                                      : env->CallObjectMethod(intent, set_data,
                                                              uri);
      env->DeleteLocalRef(configured_intent);
      env->DeleteLocalRef(mime_type);
    }
    env->DeleteLocalRef(uri);
    env->DeleteLocalRef(uri_text);
    env->DeleteLocalRef(uri_class);
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
      activity_thread == nullptr || instrumentation == nullptr ||
      activity_token == nullptr || attach_activity == nullptr ||
      env->ExceptionCheck()) {
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
  // Activity.attach() invokes the virtual attachBaseContext hook itself.  A
  // pre-call here leaves ContextWrapper.mBase initialized twice and AOSP
  // correctly throws "Base context already set" on the second call.
  if (attach_base_context == nullptr || env->ExceptionCheck()) {
    std::cerr << "ART Android window: base Context preparation failed\n";
    if (self->IsExceptionPending()) {
      std::cerr << self->GetException()->Dump() << "\n";
    }
    return 30;
  }
  env->CallNonvirtualVoidMethod(
      activity_instance, out->activity_class, attach_activity,
      out->probe_context, activity_thread, instrumentation, activity_token,
      static_cast<jint>(1),
      application, intent, resources->activity_info, title, nullptr,
      nullptr, nullptr, configuration, nullptr, nullptr, nullptr, nullptr,
      activity_token, activity_token);
  env->DeleteLocalRef(activity_token);
  env->DeleteLocalRef(binder_class);
  env->DeleteLocalRef(instrumentation);
  env->DeleteLocalRef(instrumentation_class);
  env->DeleteLocalRef(activity_thread);
  env->DeleteLocalRef(activity_thread_class);
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
    if (run_apk_app && app_theme != 0 && apply_style != nullptr &&
        !env->ExceptionCheck()) {
      env->CallVoidMethod(out->probe_theme, apply_style, app_theme, JNI_TRUE);
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
  env->DeleteLocalRef(resource_configuration);
  env->DeleteLocalRef(resources_class);
  env->DeleteLocalRef(component_name_class);
  env->DeleteLocalRef(intent_class);
  env->DeleteLocalRef(apk_application);
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
