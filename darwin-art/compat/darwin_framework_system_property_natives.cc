#include "darwin_framework_natives.h"

#include <charconv>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace {

std::mutex g_system_properties_mutex;
std::unordered_map<std::string, std::string> g_system_properties{
    // Keep Unity's native device query in agreement with the bounded procfs
    // capability snapshot.  Android exposes this as a system property on
    // devices where the CPU topology service is available.
    {"device.cpu.count", "8"},
    {"device.cpu.frequency_mhz", "2400"},
    {"device.cpu.model", "Darwin ARM64"},
    {"ro.product.cpu.abilist", "arm64-v8a"},
    {"ro.product.cpu.abilist64", "arm64-v8a"},
    {"ro.product.cpu.abilist32", ""},
    {"ro.build.version.sdk", "36"},
    {"ro.build.version.sdk_full", "36.0"},
    {"ro.build.version.release", "16"},
    {"ro.build.version.release_or_codename", "16"},
    {"ro.build.version.codename", "REL"},
    {"ro.build.version.all_codenames", "REL"},
    {"ro.build.version.known_codenames", "REL"},
};
// Android's native property area returns process-lifetime-stable prop_info
// pointers from __system_property_find().  The Java Handle API relies on the
// same stability: PropertyInvalidatedCache stores the opaque value and reuses
// it after the property's value changes.  Keep numeric tokens instead of C++
// container addresses so a forged/reflected Handle cannot become an arbitrary
// pointer dereference and unordered_map rehashing cannot invalidate a token.
jlong g_next_system_property_handle = 1;
std::unordered_map<std::string, jlong> g_system_property_handles_by_name;
std::unordered_map<jlong, std::string> g_system_property_names_by_handle;

std::optional<std::string> JavaString(JNIEnv* env, jstring value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  const char* utf = env->GetStringUTFChars(value, nullptr);
  if (utf == nullptr) {
    return std::nullopt;
  }
  std::string result(utf);
  env->ReleaseStringUTFChars(value, utf);
  return result;
}

std::optional<std::string> GetSystemProperty(JNIEnv* env, jstring key) {
  const std::optional<std::string> name = JavaString(env, key);
  if (!name.has_value()) {
    return std::nullopt;
  }
  std::lock_guard lock(g_system_properties_mutex);
  const auto found = g_system_properties.find(*name);
  return found == g_system_properties.end()
             ? std::nullopt
             : std::optional<std::string>(found->second);
}

std::optional<std::string> GetSystemPropertyByHandle(jlong handle) {
  if (handle == 0) {
    return std::nullopt;
  }
  std::lock_guard lock(g_system_properties_mutex);
  const auto named = g_system_property_names_by_handle.find(handle);
  if (named == g_system_property_names_by_handle.end()) {
    return std::nullopt;
  }
  const auto found = g_system_properties.find(named->second);
  return found == g_system_properties.end()
             ? std::nullopt
             : std::optional<std::string>(found->second);
}

jlong FindSystemPropertyHandle(const std::string& name) {
  std::lock_guard lock(g_system_properties_mutex);
  if (!g_system_properties.contains(name)) {
    return 0;
  }
  const auto existing = g_system_property_handles_by_name.find(name);
  if (existing != g_system_property_handles_by_name.end()) {
    return existing->second;
  }
  const jlong handle = g_next_system_property_handle++;
  g_system_property_handles_by_name.emplace(name, handle);
  g_system_property_names_by_handle.emplace(handle, name);
  return handle;
}

void SetSystemPropertyValue(const std::string& name, const std::string& value) {
  std::lock_guard lock(g_system_properties_mutex);
  g_system_properties[name] = value;
}

jstring SystemPropertiesGet(JNIEnv* env, jclass, jstring key,
                            jstring default_value) {
  const std::optional<std::string> value = GetSystemProperty(env, key);
  return value.has_value() ? env->NewStringUTF(value->c_str()) : default_value;
}

template <typename Integer>
Integer ParseSystemPropertyIntegerValue(const std::optional<std::string>& value,
                                        Integer default_value) {
  if (!value.has_value()) {
    return default_value;
  }
  Integer parsed{};
  const auto result =
      std::from_chars(value->data(), value->data() + value->size(), parsed);
  return result.ec == std::errc{} && result.ptr == value->data() + value->size()
             ? parsed
             : default_value;
}

template <typename Integer>
Integer ParseSystemPropertyInteger(JNIEnv* env, jstring key,
                                   Integer default_value) {
  return ParseSystemPropertyIntegerValue(GetSystemProperty(env, key),
                                         default_value);
}

jboolean ParseSystemPropertyBooleanValue(
    const std::optional<std::string>& value, jboolean default_value) {
  if (!value.has_value()) {
    return default_value;
  }
  if (*value == "1" || *value == "y" || *value == "yes" || *value == "on" ||
      *value == "true") {
    return JNI_TRUE;
  }
  if (*value == "0" || *value == "n" || *value == "no" || *value == "off" ||
      *value == "false") {
    return JNI_FALSE;
  }
  return default_value;
}

jint SystemPropertiesGetInt(JNIEnv* env, jclass, jstring key,
                            jint default_value) {
  return ParseSystemPropertyInteger(env, key, default_value);
}

jlong SystemPropertiesGetLong(JNIEnv* env, jclass, jstring key,
                              jlong default_value) {
  return ParseSystemPropertyInteger(env, key, default_value);
}

jboolean SystemPropertiesGetBoolean(JNIEnv* env, jclass, jstring key,
                                    jboolean default_value) {
  return ParseSystemPropertyBooleanValue(GetSystemProperty(env, key),
                                         default_value);
}

jlong SystemPropertiesFind(JNIEnv* env, jclass, jstring key) {
  const std::optional<std::string> name = JavaString(env, key);
  if (!name.has_value()) {
    return 0;
  }
  return FindSystemPropertyHandle(*name);
}

jstring SystemPropertiesGetByHandle(JNIEnv* env, jclass, jlong handle) {
  const std::optional<std::string> value = GetSystemPropertyByHandle(handle);
  return env->NewStringUTF(value.has_value() ? value->c_str() : "");
}

jint SystemPropertiesGetIntByHandle(JNIEnv*, jclass, jlong handle,
                                    jint default_value) {
  return ParseSystemPropertyIntegerValue(GetSystemPropertyByHandle(handle),
                                         default_value);
}

jlong SystemPropertiesGetLongByHandle(JNIEnv*, jclass, jlong handle,
                                      jlong default_value) {
  return ParseSystemPropertyIntegerValue(GetSystemPropertyByHandle(handle),
                                         default_value);
}

jboolean SystemPropertiesGetBooleanByHandle(JNIEnv*, jclass, jlong handle,
                                            jboolean default_value) {
  return ParseSystemPropertyBooleanValue(GetSystemPropertyByHandle(handle),
                                         default_value);
}

void SystemPropertiesSet(JNIEnv* env, jclass, jstring key, jstring value) {
  const std::optional<std::string> name = JavaString(env, key);
  const std::optional<std::string> text = JavaString(env, value);
  if (!name.has_value() || !text.has_value()) {
    return;
  }
  SetSystemPropertyValue(*name, *text);
}

void SystemPropertiesNoOp(JNIEnv*, jclass) {}

bool Register(JNIEnv* env, const char* class_name, JNINativeMethod* methods,
              jint method_count) {
  jclass klass = env->FindClass(class_name);
  if (klass == nullptr) {
    return false;
  }
  const bool registered =
      env->RegisterNatives(klass, methods, method_count) == JNI_OK;
  env->DeleteLocalRef(klass);
  return registered;
}

}  // namespace

namespace darwin_art {

bool RegisterFrameworkSystemPropertyNatives(JNIEnv* env) {
  JNINativeMethod methods[] = {
      {const_cast<char*>("native_get"),
       const_cast<char*>(
           "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
       reinterpret_cast<void*>(&SystemPropertiesGet)},
      {const_cast<char*>("native_get_int"),
       const_cast<char*>("(Ljava/lang/String;I)I"),
       reinterpret_cast<void*>(&SystemPropertiesGetInt)},
      {const_cast<char*>("native_get_long"),
       const_cast<char*>("(Ljava/lang/String;J)J"),
       reinterpret_cast<void*>(&SystemPropertiesGetLong)},
      {const_cast<char*>("native_get_boolean"),
       const_cast<char*>("(Ljava/lang/String;Z)Z"),
       reinterpret_cast<void*>(&SystemPropertiesGetBoolean)},
      {const_cast<char*>("native_find"),
       const_cast<char*>("(Ljava/lang/String;)J"),
       reinterpret_cast<void*>(&SystemPropertiesFind)},
      {const_cast<char*>("native_get"),
       const_cast<char*>("(J)Ljava/lang/String;"),
       reinterpret_cast<void*>(&SystemPropertiesGetByHandle)},
      {const_cast<char*>("native_get_int"), const_cast<char*>("(JI)I"),
       reinterpret_cast<void*>(&SystemPropertiesGetIntByHandle)},
      {const_cast<char*>("native_get_long"), const_cast<char*>("(JJ)J"),
       reinterpret_cast<void*>(&SystemPropertiesGetLongByHandle)},
      {const_cast<char*>("native_get_boolean"), const_cast<char*>("(JZ)Z"),
       reinterpret_cast<void*>(&SystemPropertiesGetBooleanByHandle)},
      {const_cast<char*>("native_set"),
       const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)V"),
       reinterpret_cast<void*>(&SystemPropertiesSet)},
      {const_cast<char*>("native_add_change_callback"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&SystemPropertiesNoOp)},
      {const_cast<char*>("native_report_sysprop_change"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&SystemPropertiesNoOp)},
  };
  return Register(env, "android/os/SystemProperties", methods,
                  static_cast<jint>(std::size(methods)));
}

}  // namespace darwin_art
