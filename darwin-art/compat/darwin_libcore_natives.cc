#include "darwin_libcore_natives.h"

#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unicode/uchar.h>
#include <unicode/ulocdata.h>
#include <unicode/uversion.h>
#include <unistd.h>
#include <zlib.h>

#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

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
  jfieldID processors =
      env->GetStaticFieldID(klass, "_SC_NPROCESSORS_CONF", "I");
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

jint LinuxNativeGetuid() { return static_cast<jint>(getuid()); }

jobject LinuxGetpwuid(JNIEnv* env, jobject, jint uid) {
  long configured_size = sysconf(_SC_GETPW_R_SIZE_MAX);
  std::size_t buffer_size =
      configured_size > 0 ? static_cast<std::size_t>(configured_size) : 16384;
  std::vector<char> buffer(buffer_size);
  passwd entry{};
  passwd* result = nullptr;
  if (getpwuid_r(static_cast<uid_t>(uid), &entry, buffer.data(), buffer.size(),
                 &result) != 0 ||
      result == nullptr) {
    return nullptr;
  }

  jclass struct_passwd = env->FindClass("android/system/StructPasswd");
  if (struct_passwd == nullptr) {
    return nullptr;
  }
  jmethodID constructor = env->GetMethodID(
      struct_passwd, "<init>",
      "(Ljava/lang/String;IILjava/lang/String;Ljava/lang/String;)V");
  if (constructor == nullptr) {
    env->DeleteLocalRef(struct_passwd);
    return nullptr;
  }
  jstring name =
      env->NewStringUTF(entry.pw_name == nullptr ? "" : entry.pw_name);
  jstring directory =
      env->NewStringUTF(entry.pw_dir == nullptr ? "" : entry.pw_dir);
  jstring shell =
      env->NewStringUTF(entry.pw_shell == nullptr ? "" : entry.pw_shell);
  jobject value = env->NewObject(
      struct_passwd, constructor, name, static_cast<jint>(entry.pw_uid),
      static_cast<jint>(entry.pw_gid), directory, shell);
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
  jmethodID constructor =
      env->GetMethodID(struct_utsname, "<init>",
                       "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/"
                       "String;Ljava/lang/String;Ljava/lang/String;)V");
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
  jobject value = env->NewObject(struct_utsname, constructor, sysname, nodename,
                                 release, version, machine);
  env->DeleteLocalRef(sysname);
  env->DeleteLocalRef(nodename);
  env->DeleteLocalRef(release);
  env->DeleteLocalRef(version);
  env->DeleteLocalRef(machine);
  env->DeleteLocalRef(struct_utsname);
  return value;
}

jint LinuxWriteBytes(JNIEnv* env, jobject, jobject file_descriptor,
                     jobject buffer, jint offset, jint byte_count) {
  if (file_descriptor == nullptr || buffer == nullptr || offset < 0 ||
      byte_count < 0) {
    return -1;
  }
  jclass descriptor_class = env->GetObjectClass(file_descriptor);
  jfieldID descriptor_field =
      env->GetFieldID(descriptor_class, "descriptor", "I");
  const jint descriptor =
      descriptor_field == nullptr
          ? -1
          : env->GetIntField(file_descriptor, descriptor_field);
  env->DeleteLocalRef(descriptor_class);
  if (descriptor < 0) {
    return -1;
  }

  const void* bytes = nullptr;
  jbyteArray byte_array = nullptr;
  jbyte* array_elements = nullptr;
  jclass byte_array_class = env->FindClass("[B");
  if (byte_array_class != nullptr &&
      env->IsInstanceOf(buffer, byte_array_class)) {
    byte_array = reinterpret_cast<jbyteArray>(buffer);
    const jsize length = env->GetArrayLength(byte_array);
    if (offset > length || byte_count > length - offset) {
      env->DeleteLocalRef(byte_array_class);
      return -1;
    }
    array_elements = env->GetByteArrayElements(byte_array, nullptr);
    bytes = array_elements == nullptr ? nullptr : array_elements + offset;
  } else {
    void* direct = env->GetDirectBufferAddress(buffer);
    const jlong capacity = env->GetDirectBufferCapacity(buffer);
    if (direct != nullptr && offset <= capacity &&
        byte_count <= capacity - offset) {
      bytes = static_cast<const std::byte*>(direct) + offset;
    }
  }
  env->DeleteLocalRef(byte_array_class);
  if (bytes == nullptr) {
    return -1;
  }

  ssize_t written;
  do {
    written = write(descriptor, bytes, static_cast<std::size_t>(byte_count));
  } while (written < 0 && errno == EINTR);
  if (array_elements != nullptr) {
    env->ReleaseByteArrayElements(byte_array, array_elements, JNI_ABORT);
  }
  return written < 0 ? -1 : static_cast<jint>(written);
}

jstring IcuVersionString(JNIEnv* env, const UVersionInfo version) {
  char text[U_MAX_VERSION_STRING_LENGTH];
  u_versionToString(version, text);
  return env->NewStringUTF(text);
}

jstring IcuGetIcuVersion(JNIEnv* env, jclass) {
  UVersionInfo version;
  u_getVersion(version);
  return IcuVersionString(env, version);
}

jstring IcuGetUnicodeVersion(JNIEnv* env, jclass) {
  UVersionInfo version;
  u_getUnicodeVersion(version);
  return IcuVersionString(env, version);
}

jstring IcuGetCldrVersion(JNIEnv* env, jclass) {
  UVersionInfo version;
  UErrorCode status = U_ZERO_ERROR;
  ulocdata_getCLDRVersion(version, &status);
  return U_SUCCESS(status) ? IcuVersionString(env, version) : nullptr;
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
      std::string("java.library.path=") +
          (library_path == nullptr ? "" : library_path),
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

jlong SystemCurrentTimeMillis(JNIEnv*, jclass) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

jlong SystemNanoTime(JNIEnv*, jclass) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

jboolean FileDescriptorGetAppend(jint descriptor) {
  const int flags = fcntl(descriptor, F_GETFL);
  return flags >= 0 && (flags & O_APPEND) != 0 ? JNI_TRUE : JNI_FALSE;
}

jboolean FileDescriptorIsSocket(jint descriptor) {
  int socket_type = 0;
  socklen_t length = sizeof(socket_type);
  return getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &socket_type, &length) == 0
             ? JNI_TRUE
             : JNI_FALSE;
}

bool Register(JNIEnv* env, const char* class_name,
              const JNINativeMethod* methods, jint method_count) {
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

bool RegisterLibcoreNatives(JNIEnv* env) {
  JNINativeMethod float_methods[] = {
      {const_cast<char*>("floatToRawIntBits"), const_cast<char*>("(F)I"),
       reinterpret_cast<void*>(&FloatToRawIntBits)},
      {const_cast<char*>("intBitsToFloat"), const_cast<char*>("(I)F"),
       reinterpret_cast<void*>(&IntBitsToFloat)},
  };
  JNINativeMethod double_methods[] = {
      {const_cast<char*>("doubleToRawLongBits"), const_cast<char*>("(D)J"),
       reinterpret_cast<void*>(&DoubleToRawLongBits)},
      {const_cast<char*>("longBitsToDouble"), const_cast<char*>("(J)D"),
       reinterpret_cast<void*>(&LongBitsToDouble)},
  };
  JNINativeMethod os_constants_methods[] = {
      {const_cast<char*>("initConstants"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&OsConstantsInitConstants)},
  };
  JNINativeMethod linux_methods[] = {
      {const_cast<char*>("getenv"),
       const_cast<char*>("(Ljava/lang/String;)Ljava/lang/String;"),
       reinterpret_cast<void*>(&LinuxGetenv)},
      {const_cast<char*>("getpwuid"),
       const_cast<char*>("(I)Landroid/system/StructPasswd;"),
       reinterpret_cast<void*>(&LinuxGetpwuid)},
      {const_cast<char*>("nativeGettid"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&LinuxNativeGettid)},
      {const_cast<char*>("nativeGetuid"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&LinuxNativeGetuid)},
      {const_cast<char*>("sysconf"), const_cast<char*>("(I)J"),
       reinterpret_cast<void*>(&LinuxSysconf)},
      {const_cast<char*>("uname"),
       const_cast<char*>("()Landroid/system/StructUtsname;"),
       reinterpret_cast<void*>(&LinuxUname)},
      {const_cast<char*>("writeBytes"),
       const_cast<char*>("(Ljava/io/FileDescriptor;Ljava/lang/Object;II)I"),
       reinterpret_cast<void*>(&LinuxWriteBytes)},
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
      {const_cast<char*>("currentTimeMillis"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SystemCurrentTimeMillis)},
      {const_cast<char*>("nanoTime"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SystemNanoTime)},
      {const_cast<char*>("specialProperties"),
       const_cast<char*>("()[Ljava/lang/String;"),
       reinterpret_cast<void*>(&SystemSpecialProperties)},
  };
  JNINativeMethod file_descriptor_methods[] = {
      {const_cast<char*>("getAppend"), const_cast<char*>("(I)Z"),
       reinterpret_cast<void*>(&FileDescriptorGetAppend)},
      {const_cast<char*>("isSocket"), const_cast<char*>("(I)Z"),
       reinterpret_cast<void*>(&FileDescriptorIsSocket)},
  };
  return Register(env, "java/lang/Float", float_methods, 2) &&
         Register(env, "java/lang/Double", double_methods, 2) &&
         Register(env, "android/system/OsConstants", os_constants_methods, 1) &&
         Register(env, "libcore/io/Linux", linux_methods, 7) &&
         Register(env, "libcore/icu/ICU", icu_methods, 3) &&
         Register(env, "java/lang/System", system_methods, 3) &&
         Register(env, "java/io/FileDescriptor", file_descriptor_methods, 2);
}

}  // namespace darwin_art
