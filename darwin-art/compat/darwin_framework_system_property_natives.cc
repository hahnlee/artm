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

jstring SystemPropertiesGet(JNIEnv* env, jclass, jstring key,
                            jstring default_value) {
  const std::optional<std::string> value = GetSystemProperty(env, key);
  return value.has_value() ? env->NewStringUTF(value->c_str()) : default_value;
}

template <typename Integer>
Integer ParseSystemPropertyInteger(JNIEnv* env, jstring key,
                                   Integer default_value) {
  const std::optional<std::string> value = GetSystemProperty(env, key);
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
  const std::optional<std::string> value = GetSystemProperty(env, key);
  if (!value.has_value()) {
    return default_value;
  }
  if (*value == "1" || *value == "true" || *value == "on" || *value == "yes") {
    return JNI_TRUE;
  }
  if (*value == "0" || *value == "false" || *value == "off" || *value == "no") {
    return JNI_FALSE;
  }
  return default_value;
}

jlong SystemPropertiesFind(JNIEnv*, jclass, jstring) { return 0; }

jstring SystemPropertiesGetByHandle(JNIEnv* env, jclass, jlong) {
  return env->NewStringUTF("");
}

jint SystemPropertiesGetIntByHandle(jlong, jint default_value) {
  return default_value;
}

jlong SystemPropertiesGetLongByHandle(jlong, jlong default_value) {
  return default_value;
}

jboolean SystemPropertiesGetBooleanByHandle(jlong, jboolean default_value) {
  return default_value;
}

void SystemPropertiesSet(JNIEnv* env, jclass, jstring key, jstring value) {
  const std::optional<std::string> name = JavaString(env, key);
  const std::optional<std::string> text = JavaString(env, value);
  if (!name.has_value() || !text.has_value()) {
    return;
  }
  std::lock_guard lock(g_system_properties_mutex);
  g_system_properties[*name] = *text;
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
