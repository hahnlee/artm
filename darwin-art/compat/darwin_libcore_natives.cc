#include "darwin_libcore_natives.h"
#include "darwin_framework_natives.h"

#if defined(DARWIN_ART_FULL_LIBCORE_LINUX)
#include "darwin_os_constants.h"
#include "libcore_darwin_linux.h"

void register_libcore_io_AsynchronousCloseMonitor(JNIEnv* env);
extern "C" void register_java_io_UnixFileSystem(JNIEnv* env);
extern "C" void register_java_io_FileDescriptor(JNIEnv* env);
extern "C" void register_java_io_FileInputStream(JNIEnv* env);
extern "C" void register_java_lang_System(JNIEnv* env);
extern "C" void register_java_sun_nio_fs_UnixNativeDispatcher(JNIEnv* env);
extern "C" void register_sun_nio_ch_IOUtil(JNIEnv* env);
extern "C" void register_sun_nio_ch_FileChannelImpl(JNIEnv* env);
extern "C" void register_sun_nio_ch_FileDispatcherImpl(JNIEnv* env);
extern "C" void register_sun_nio_ch_NativeThread(JNIEnv* env);
extern "C" int darwin_art_restore_sun_nio_ch_NativeThread_signal();
void register_libcore_io_Memory(JNIEnv* env);

class DarwinArtLibcoreJniConstants {
 public:
  static void Initialize(JNIEnv* env);
};
#endif

#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <zlib.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace {

#if !defined(DARWIN_ART_FULL_LIBCORE_LINUX)
void UnixFileSystemInitIds(JNIEnv*, jclass) {}

jint UnixFileSystemGetBooleanAttributes(JNIEnv* env, jobject, jstring path) {
  if (path == nullptr) {
    return 0;
  }
  const char* utf_path = env->GetStringUTFChars(path, nullptr);
  if (utf_path == nullptr) {
    return 0;
  }
  struct stat status {};
  const int result = stat(utf_path, &status);
  const char* basename = std::strrchr(utf_path, '/');
  basename = basename == nullptr ? utf_path : basename + 1;
  constexpr jint kExists = 0x01;
  constexpr jint kRegular = 0x02;
  constexpr jint kDirectory = 0x04;
  constexpr jint kHidden = 0x08;
  jint attributes = 0;
  if (result == 0) {
    attributes |= kExists;
    attributes |= S_ISREG(status.st_mode) ? kRegular : 0;
    attributes |= S_ISDIR(status.st_mode) ? kDirectory : 0;
    attributes |= basename[0] == '.' && basename[1] != '\0' ? kHidden : 0;
  }
  env->ReleaseStringUTFChars(path, utf_path);
  return attributes;
}
#endif

#if !defined(DARWIN_ART_FULL_LIBCORE_LINUX)
void OsConstantsInitConstants(JNIEnv* env, jclass klass) {
  jfieldID processors =
      env->GetStaticFieldID(klass, "_SC_NPROCESSORS_CONF", "I");
  if (processors != nullptr) {
    env->SetStaticIntField(klass, processors, _SC_NPROCESSORS_CONF);
  }
}
#endif

jlong LinuxSysconf(JNIEnv*, jobject, jint name) {
  return static_cast<jlong>(sysconf(name));
}

jobject LinuxStat(JNIEnv* env, jobject, jstring path) {
  if (path == nullptr) {
    return nullptr;
  }
  const char* utf_path = env->GetStringUTFChars(path, nullptr);
  if (utf_path == nullptr) {
    return nullptr;
  }
  struct stat status {};
  const int result = stat(utf_path, &status);
  env->ReleaseStringUTFChars(path, utf_path);
  if (result != 0) {
    jclass exception_class = env->FindClass("android/system/ErrnoException");
    jmethodID constructor =
        exception_class == nullptr
            ? nullptr
            : env->GetMethodID(exception_class, "<init>",
                               "(Ljava/lang/String;I)V");
    jstring function_name = env->NewStringUTF("stat");
    jobject exception =
        constructor == nullptr
            ? nullptr
            : env->NewObject(exception_class, constructor, function_name, errno);
    if (exception != nullptr) {
      env->Throw(static_cast<jthrowable>(exception));
    }
    env->DeleteLocalRef(exception);
    env->DeleteLocalRef(function_name);
    env->DeleteLocalRef(exception_class);
    return nullptr;
  }

  jclass stat_class = env->FindClass("android/system/StructStat");
  jmethodID constructor =
      stat_class == nullptr
          ? nullptr
          : env->GetMethodID(stat_class, "<init>", "(JJIJIIJJJJJJJ)V");
  jobject value =
      constructor == nullptr
          ? nullptr
          : env->NewObject(
                stat_class, constructor, static_cast<jlong>(status.st_dev),
                static_cast<jlong>(status.st_ino),
                static_cast<jint>(status.st_mode),
                static_cast<jlong>(status.st_nlink),
                static_cast<jint>(status.st_uid),
                static_cast<jint>(status.st_gid),
                static_cast<jlong>(status.st_rdev),
                static_cast<jlong>(status.st_size),
                static_cast<jlong>(status.st_atimespec.tv_sec),
                static_cast<jlong>(status.st_mtimespec.tv_sec),
                static_cast<jlong>(status.st_ctimespec.tv_sec),
                static_cast<jlong>(status.st_blksize),
                static_cast<jlong>(status.st_blocks));
  env->DeleteLocalRef(stat_class);
  return value;
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

#if !defined(DARWIN_ART_FULL_LIBCORE_LINUX)
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
#endif

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
  // NativeAllocationRegistry is part of libcore's boot class path rather than
  // framework.jar. Register its free-function ABI from the libcore owner so
  // Cleaner can release RippleShader/VectorDrawable allocations.
  if (!darwin_art::RegisterFrameworkSupportNatives(env)) {
    return false;
  }
#if !defined(DARWIN_ART_FULL_LIBCORE_LINUX)
  JNINativeMethod unix_file_system_methods[] = {
      {const_cast<char*>("initIDs"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&UnixFileSystemInitIds)},
      {const_cast<char*>("getBooleanAttributes0"),
       const_cast<char*>("(Ljava/lang/String;)I"),
       reinterpret_cast<void*>(&UnixFileSystemGetBooleanAttributes)},
  };
  if (!Register(env, "java/io/UnixFileSystem", unix_file_system_methods,
                static_cast<jint>(std::size(unix_file_system_methods)))) {
    return false;
  }
#else
  register_java_io_UnixFileSystem(env);
  if (env->ExceptionCheck()) {
    return false;
  }
#endif

#if !defined(DARWIN_ART_FULL_LIBCORE_LINUX)
  JNINativeMethod os_constants_methods[] = {
      {const_cast<char*>("initConstants"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&OsConstantsInitConstants)},
  };
#endif
#if !defined(DARWIN_ART_FULL_LIBCORE_LINUX)
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
      {const_cast<char*>("stat"),
       const_cast<char*>("(Ljava/lang/String;)Landroid/system/StructStat;"),
       reinterpret_cast<void*>(&LinuxStat)},
      {const_cast<char*>("uname"),
       const_cast<char*>("()Landroid/system/StructUtsname;"),
       reinterpret_cast<void*>(&LinuxUname)},
      {const_cast<char*>("writeBytes"),
       const_cast<char*>("(Ljava/io/FileDescriptor;Ljava/lang/Object;II)I"),
       reinterpret_cast<void*>(&LinuxWriteBytes)},
  };
#endif
#if !defined(DARWIN_ART_FULL_LIBCORE_LINUX)
  JNINativeMethod system_methods[] = {
      {const_cast<char*>("currentTimeMillis"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SystemCurrentTimeMillis)},
      {const_cast<char*>("nanoTime"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&SystemNanoTime)},
      {const_cast<char*>("specialProperties"),
       const_cast<char*>("()[Ljava/lang/String;"),
       reinterpret_cast<void*>(&SystemSpecialProperties)},
  };
#endif
#if !defined(DARWIN_ART_FULL_LIBCORE_LINUX)
  JNINativeMethod file_descriptor_methods[] = {
      {const_cast<char*>("getAppend"), const_cast<char*>("(I)Z"),
       reinterpret_cast<void*>(&FileDescriptorGetAppend)},
      {const_cast<char*>("isSocket"), const_cast<char*>("(I)Z"),
       reinterpret_cast<void*>(&FileDescriptorIsSocket)},
  };
#endif
  const auto register_linux = [&]() {
#if defined(DARWIN_ART_FULL_LIBCORE_LINUX)
    // Android's Register.cpp initializes the asynchronous-close signal owner
    // immediately before libcore.io.Linux. Keep that order so every supported
    // blocking Darwin syscall can install a live monitor before Java closes it.
    register_libcore_io_AsynchronousCloseMonitor(env);
    if (env->ExceptionCheck()) {
      return false;
    }
    return libcore_darwin::RegisterLinuxNatives(env);
#else
    return Register(env, "libcore/io/Linux", linux_methods,
                    static_cast<jint>(std::size(linux_methods)));
#endif
  };
  const auto register_system = [&]() {
#if defined(DARWIN_ART_FULL_LIBCORE_LINUX)
    register_java_lang_System(env);
    return !env->ExceptionCheck();
#else
    return Register(env, "java/lang/System", system_methods, 3);
#endif
  };
  const auto register_os_constants = [&]() {
#if defined(DARWIN_ART_FULL_LIBCORE_LINUX)
    register_android_system_OsConstants(env);
    return !env->ExceptionCheck();
#else
    return Register(env, "android/system/OsConstants", os_constants_methods, 1);
#endif
  };
  const auto register_openjdk_file_mapping = [&]() {
#if defined(DARWIN_ART_FULL_LIBCORE_LINUX)
    // Preserve the relative order from Android 16 libopenjdk's OnLoad.cpp.
    // Each call owns the complete upstream method table for its Java class;
    // partial or repeated RegisterNatives owners are forbidden.
    register_sun_nio_ch_IOUtil(env);
    if (env->ExceptionCheck()) {
      return false;
    }
    register_sun_nio_ch_FileChannelImpl(env);
    if (env->ExceptionCheck()) {
      return false;
    }
    register_sun_nio_ch_FileDispatcherImpl(env);
    if (env->ExceptionCheck()) {
      return false;
    }
    register_java_io_FileInputStream(env);
    if (env->ExceptionCheck()) {
      return false;
    }
    register_sun_nio_ch_NativeThread(env);
    return !env->ExceptionCheck();
#else
    return true;
#endif
  };
  const auto register_file_descriptor = [&]() {
#if defined(DARWIN_ART_FULL_LIBCORE_LINUX)
    register_java_io_FileDescriptor(env);
    return !env->ExceptionCheck();
#else
    return Register(env, "java/io/FileDescriptor", file_descriptor_methods, 2);
#endif
  };
  const auto register_unix_native_dispatcher = [&]() {
#if defined(DARWIN_ART_FULL_LIBCORE_LINUX)
    register_java_sun_nio_fs_UnixNativeDispatcher(env);
    return !env->ExceptionCheck();
#else
    return true;
#endif
  };
  const auto register_libcore_memory = [&]() {
#if defined(DARWIN_ART_FULL_LIBCORE_LINUX)
    // StartMinimal has already registered ART's complementary seven array
    // methods. Android libjavacore owns the disjoint scalar/bulk eighteen.
    register_libcore_io_Memory(env);
    if (env->ExceptionCheck()) {
      return false;
    }
    DarwinArtLibcoreJniConstants::Initialize(env);
    return !env->ExceptionCheck();
#else
    return true;
#endif
  };
  return RegisterLibcoreCharacterNatives(env) &&
         // Android's OpenJDK OnLoad registers System before owners whose
         // FindClass/GetFieldID paths may initialize java.io or NIO classes.
         register_system() &&
         register_os_constants() &&
         register_linux() &&
         register_file_descriptor() &&
         register_unix_native_dispatcher() &&
         register_openjdk_file_mapping() &&
         register_libcore_memory() && RegisterLibcoreIcuNatives(env);
}

bool ShutdownLibcoreNatives() {
#if defined(DARWIN_ART_FULL_LIBCORE_LINUX)
  return darwin_art_restore_sun_nio_ch_NativeThread_signal() == 0;
#else
  return true;
#endif
}

}  // namespace darwin_art
