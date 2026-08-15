#include "darwin_libcore_natives.h"

#include <bit>
#include <cstdlib>
#include <cstdint>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <pwd.h>
#include <string>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>

namespace {

jint FloatToRawIntBits(JNIEnv*, jclass, jfloat value) {
  return std::bit_cast<std::int32_t>(value);
}

jfloat IntBitsToFloat(JNIEnv*, jclass, jint bits) {
  return std::bit_cast<float>(static_cast<std::int32_t>(bits));
}

jlong DoubleToRawLongBits(JNIEnv*, jclass, jdouble value) {
  return std::bit_cast<std::int64_t>(value);
}

jdouble LongBitsToDouble(JNIEnv*, jclass, jlong bits) {
  return std::bit_cast<double>(static_cast<std::int64_t>(bits));
}

void OsConstantsInitConstants(JNIEnv* env, jclass klass) {
  jfieldID processors = env->GetStaticFieldID(klass, "_SC_NPROCESSORS_CONF", "I");
  if (processors != nullptr) {
    env->SetStaticIntField(klass, processors, _SC_NPROCESSORS_CONF);
  }
}

jlong LinuxSysconf(JNIEnv*, jobject, jint name) {
  return static_cast<jlong>(sysconf(name));
}

jstring LinuxGetenv(JNIEnv* env, jobject, jstring name) {
  if (name == nullptr) {
    return nullptr;
  }
  const char* utf_name = env->GetStringUTFChars(name, nullptr);
  if (utf_name == nullptr) {
    return nullptr;
  }
  const char* value = std::getenv(utf_name);
  env->ReleaseStringUTFChars(name, utf_name);
  return value == nullptr ? nullptr : env->NewStringUTF(value);
}

jint LinuxNativeGettid() {
  std::uint64_t tid = 0;
  return pthread_threadid_np(nullptr, &tid) == 0 ? static_cast<jint>(tid) : -1;
}

jint LinuxNativeGetuid() {
  return static_cast<jint>(getuid());
}

jobject LinuxGetpwuid(JNIEnv* env, jobject, jint uid) {
  long configured_size = sysconf(_SC_GETPW_R_SIZE_MAX);
  std::size_t buffer_size = configured_size > 0 ? static_cast<std::size_t>(configured_size) : 16384;
  std::vector<char> buffer(buffer_size);
  passwd entry{};
  passwd* result = nullptr;
  if (getpwuid_r(static_cast<uid_t>(uid), &entry, buffer.data(), buffer.size(), &result) != 0 ||
      result == nullptr) {
    return nullptr;
  }

  jclass struct_passwd = env->FindClass("android/system/StructPasswd");
  if (struct_passwd == nullptr) {
    return nullptr;
  }
  jmethodID constructor = env->GetMethodID(
      struct_passwd,
      "<init>",
      "(Ljava/lang/String;IILjava/lang/String;Ljava/lang/String;)V");
  if (constructor == nullptr) {
    env->DeleteLocalRef(struct_passwd);
    return nullptr;
  }
  jstring name = env->NewStringUTF(entry.pw_name == nullptr ? "" : entry.pw_name);
  jstring directory = env->NewStringUTF(entry.pw_dir == nullptr ? "" : entry.pw_dir);
  jstring shell = env->NewStringUTF(entry.pw_shell == nullptr ? "" : entry.pw_shell);
  jobject value = env->NewObject(struct_passwd,
                                 constructor,
                                 name,
                                 static_cast<jint>(entry.pw_uid),
                                 static_cast<jint>(entry.pw_gid),
                                 directory,
                                 shell);
  env->DeleteLocalRef(name);
  env->DeleteLocalRef(directory);
  env->DeleteLocalRef(shell);
  env->DeleteLocalRef(struct_passwd);
  return value;
}

jobject LinuxUname(JNIEnv* env, jobject) {
  utsname host{};
  if (uname(&host) != 0) {
    return nullptr;
  }
  jclass struct_utsname = env->FindClass("android/system/StructUtsname");
  if (struct_utsname == nullptr) {
    return nullptr;
  }
  jmethodID constructor = env->GetMethodID(
      struct_utsname,
      "<init>",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
  if (constructor == nullptr) {
    env->DeleteLocalRef(struct_utsname);
    return nullptr;
  }
  // Preserve Android-visible kernel identity while retaining useful host build
  // details in the fields that do not affect ABI selection.
  jstring sysname = env->NewStringUTF("Linux");
  jstring nodename = env->NewStringUTF(host.nodename);
  jstring release = env->NewStringUTF(host.release);
  jstring version = env->NewStringUTF(host.version);
  jstring machine = env->NewStringUTF("aarch64");
  jobject value =
      env->NewObject(struct_utsname, constructor, sysname, nodename, release, version, machine);
  env->DeleteLocalRef(sysname);
  env->DeleteLocalRef(nodename);
  env->DeleteLocalRef(release);
  env->DeleteLocalRef(version);
  env->DeleteLocalRef(machine);
  env->DeleteLocalRef(struct_utsname);
  return value;
}

jstring IcuGetIcuVersion(JNIEnv* env, jclass) {
  return env->NewStringUTF("76.1");
}

jstring IcuGetUnicodeVersion(JNIEnv* env, jclass) {
  return env->NewStringUTF("16.0");
}

jstring IcuGetCldrVersion(JNIEnv* env, jclass) {
  return env->NewStringUTF("46");
}

jobjectArray SystemSpecialProperties(JNIEnv* env, jclass) {
  jclass string_class = env->FindClass("java/lang/String");
  if (string_class == nullptr) {
    return nullptr;
  }
  jobjectArray properties = env->NewObjectArray(4, string_class, nullptr);
  env->DeleteLocalRef(string_class);
  if (properties == nullptr) {
    return nullptr;
  }

  char current_directory[PATH_MAX];
  const char* directory = getcwd(current_directory, sizeof(current_directory));
  const char* library_path = std::getenv("DYLD_LIBRARY_PATH");
  const std::string values[] = {
      std::string("user.dir=") + (directory == nullptr ? "/" : directory),
      std::string("android.zlib.version=") + ZLIB_VERSION,
      "android.openssl.version=Darwin Security.framework",
      std::string("java.library.path=") + (library_path == nullptr ? "" : library_path),
  };
  for (jsize index = 0; index < 4; ++index) {
    jstring value = env->NewStringUTF(values[index].c_str());
    if (value == nullptr) {
      return nullptr;
    }
    env->SetObjectArrayElement(properties, index, value);
    env->DeleteLocalRef(value);
    if (env->ExceptionCheck()) {
      return nullptr;
    }
  }
  return properties;
}

jboolean FileDescriptorGetAppend(jint descriptor) {
  const int flags = fcntl(descriptor, F_GETFL);
  return flags >= 0 && (flags & O_APPEND) != 0 ? JNI_TRUE : JNI_FALSE;
}

jboolean FileDescriptorIsSocket(jint descriptor) {
  int socket_type = 0;
  socklen_t length = sizeof(socket_type);
  return getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &socket_type, &length) == 0 ? JNI_TRUE
                                                                               : JNI_FALSE;
}

bool Register(JNIEnv* env,
              const char* class_name,
              const JNINativeMethod* methods,
              jint method_count) {
  jclass klass = env->FindClass(class_name);
  if (klass == nullptr) {
    return false;
  }
  const bool registered = env->RegisterNatives(klass, methods, method_count) == JNI_OK;
  env->DeleteLocalRef(klass);
  return registered;
}

}  // namespace

namespace darwin_art {

bool RegisterLibcoreNatives(JNIEnv* env) {
  JNINativeMethod float_methods[] = {
      {const_cast<char*>("floatToRawIntBits"),
       const_cast<char*>("(F)I"),
       reinterpret_cast<void*>(&FloatToRawIntBits)},
      {const_cast<char*>("intBitsToFloat"),
       const_cast<char*>("(I)F"),
       reinterpret_cast<void*>(&IntBitsToFloat)},
  };
  JNINativeMethod double_methods[] = {
      {const_cast<char*>("doubleToRawLongBits"),
       const_cast<char*>("(D)J"),
       reinterpret_cast<void*>(&DoubleToRawLongBits)},
      {const_cast<char*>("longBitsToDouble"),
       const_cast<char*>("(J)D"),
       reinterpret_cast<void*>(&LongBitsToDouble)},
  };
  JNINativeMethod os_constants_methods[] = {
      {const_cast<char*>("initConstants"),
       const_cast<char*>("()V"),
       reinterpret_cast<void*>(&OsConstantsInitConstants)},
  };
  JNINativeMethod linux_methods[] = {
      {const_cast<char*>("getenv"),
       const_cast<char*>("(Ljava/lang/String;)Ljava/lang/String;"),
       reinterpret_cast<void*>(&LinuxGetenv)},
      {const_cast<char*>("getpwuid"),
       const_cast<char*>("(I)Landroid/system/StructPasswd;"),
       reinterpret_cast<void*>(&LinuxGetpwuid)},
      {const_cast<char*>("nativeGettid"),
       const_cast<char*>("()I"),
       reinterpret_cast<void*>(&LinuxNativeGettid)},
      {const_cast<char*>("nativeGetuid"),
       const_cast<char*>("()I"),
       reinterpret_cast<void*>(&LinuxNativeGetuid)},
      {const_cast<char*>("sysconf"),
       const_cast<char*>("(I)J"),
       reinterpret_cast<void*>(&LinuxSysconf)},
      {const_cast<char*>("uname"),
       const_cast<char*>("()Landroid/system/StructUtsname;"),
       reinterpret_cast<void*>(&LinuxUname)},
  };
  JNINativeMethod icu_methods[] = {
      {const_cast<char*>("getIcuVersion"),
       const_cast<char*>("()Ljava/lang/String;"),
       reinterpret_cast<void*>(&IcuGetIcuVersion)},
      {const_cast<char*>("getUnicodeVersion"),
       const_cast<char*>("()Ljava/lang/String;"),
       reinterpret_cast<void*>(&IcuGetUnicodeVersion)},
      {const_cast<char*>("getCldrVersion"),
       const_cast<char*>("()Ljava/lang/String;"),
       reinterpret_cast<void*>(&IcuGetCldrVersion)},
  };
  JNINativeMethod system_methods[] = {
      {const_cast<char*>("specialProperties"),
       const_cast<char*>("()[Ljava/lang/String;"),
       reinterpret_cast<void*>(&SystemSpecialProperties)},
  };
  JNINativeMethod file_descriptor_methods[] = {
      {const_cast<char*>("getAppend"),
       const_cast<char*>("(I)Z"),
       reinterpret_cast<void*>(&FileDescriptorGetAppend)},
      {const_cast<char*>("isSocket"),
       const_cast<char*>("(I)Z"),
       reinterpret_cast<void*>(&FileDescriptorIsSocket)},
  };
  return Register(env, "java/lang/Float", float_methods, 2) &&
         Register(env, "java/lang/Double", double_methods, 2) &&
         Register(env, "android/system/OsConstants", os_constants_methods, 1) &&
         Register(env, "libcore/io/Linux", linux_methods, 6) &&
         Register(env, "libcore/icu/ICU", icu_methods, 3) &&
         Register(env, "java/lang/System", system_methods, 1) &&
         Register(env, "java/io/FileDescriptor", file_descriptor_methods, 2);
}

}  // namespace darwin_art
