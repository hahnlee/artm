#include "darwin_libcore_natives.h"
#include "darwin_android_time.h"
#include "darwin_framework_natives.h"
#include "darwin_libcore_filesystem_bridge.h"

extern "C" int darwin_art_bionic_fs_stat_core(
    const char*, DarwinArtAndroidStat*);
extern "C" int darwin_art_bionic_fs_mkdir_core(const char*, uint32_t);
extern "C" int darwin_art_bionic_fs_chmod_core(const char*, uint32_t);
extern "C" int32_t darwin_art_bionic_errno_load(void);
extern "C" int darwin_art_bionic_open(const char*, int, uint32_t);
extern "C" int darwin_art_bionic_socket_broker_dup(int);
extern "C" int darwin_art_bionic_socket_broker_fcntl(int, int, intptr_t);
extern "C" int darwin_art_bionic_close(int);
extern "C" intptr_t darwin_art_bionic_read(int, void*, size_t);
extern "C" intptr_t darwin_art_bionic_write(int, const void*, size_t);
extern "C" intptr_t darwin_art_bionic_pread(int, void*, size_t, int64_t);
extern "C" intptr_t darwin_art_bionic_pwrite(int, const void*, size_t, int64_t);
extern "C" int darwin_art_bionic_fstat(int, DarwinArtAndroidStat*);
extern "C" int darwin_art_bionic_stat(const char*, DarwinArtAndroidStat*);
extern "C" int64_t darwin_art_bionic_lseek(int, int64_t, int);
extern "C" int darwin_art_bionic_access(const char*, int);
extern "C" int darwin_art_bionic_remove(const char*);
extern "C" int darwin_art_bionic_rename(const char*, const char*);
extern "C" int darwin_art_bionic_fs_adopt_host_fd_core(int);
extern "C" intptr_t darwin_art_bionic_sendfile(int, int, int64_t*, size_t);

#if defined(DARWIN_ART_FULL_LIBCORE_LINUX)
#include "darwin_os_constants.h"
#include "libcore_darwin_linux.h"

void register_libcore_io_AsynchronousCloseMonitor(JNIEnv* env);
extern "C" void register_java_io_UnixFileSystem(JNIEnv* env);
extern "C" void register_java_io_FileDescriptor(JNIEnv* env);
extern "C" void register_java_io_FileInputStream(JNIEnv* env);
extern "C" void register_java_lang_System(JNIEnv* env);
extern "C" void register_java_lang_Runtime(JNIEnv* env);
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

bool Register(JNIEnv* env, const char* class_name,
              const JNINativeMethod* methods, jint method_count);

// java.util.zip.Inflater is part of the Android boot class path and is used
// while framework and APK resources are read.  Keep the Java-facing ABI
// identical to AOSP, but own the z_stream in this provider rather than
// forwarding a guest pointer to the host.  The packed result format is the
// Android contract: input-consumed in bits 0..30, output-produced in bits
// 31..61, finished in bit 62, and needs-dictionary in bit 63.
struct DarwinInflaterState {
  z_stream stream{};
};

jfieldID g_file_key_device = nullptr;
jfieldID g_file_key_inode = nullptr;

void FileKeyInitIds(JNIEnv* env, jclass file_key_class) {
  g_file_key_device = env->GetFieldID(file_key_class, "st_dev", "J");
  g_file_key_inode = env->GetFieldID(file_key_class, "st_ino", "J");
}

void FileKeyInit(JNIEnv* env, jobject file_key, jobject file_descriptor) {
  if (file_key == nullptr || file_descriptor == nullptr) return;
  jclass descriptor_class = env->GetObjectClass(file_descriptor);
  jfieldID descriptor_field = descriptor_class == nullptr
                                  ? nullptr
                                  : env->GetFieldID(descriptor_class,
                                                    "descriptor", "I");
  const jint descriptor = descriptor_field == nullptr
                              ? -1
                              : env->GetIntField(file_descriptor,
                                                 descriptor_field);
  if (descriptor_class != nullptr) env->DeleteLocalRef(descriptor_class);
  struct stat status {};
  if (descriptor < 0 || fstat(descriptor, &status) != 0) {
    jclass exception = env->FindClass("java/io/IOException");
    if (exception != nullptr) {
      env->ThrowNew(exception, "fstat failed while constructing FileKey");
      env->DeleteLocalRef(exception);
    }
    return;
  }
  if (g_file_key_device == nullptr || g_file_key_inode == nullptr) {
    jclass file_key_class = env->GetObjectClass(file_key);
    if (file_key_class != nullptr) {
      FileKeyInitIds(env, file_key_class);
      env->DeleteLocalRef(file_key_class);
    }
  }
  if (g_file_key_device != nullptr && g_file_key_inode != nullptr &&
      !env->ExceptionCheck()) {
    env->SetLongField(file_key, g_file_key_device,
                      static_cast<jlong>(status.st_dev));
    env->SetLongField(file_key, g_file_key_inode,
                      static_cast<jlong>(status.st_ino));
  }
}

jlong InflaterInit(JNIEnv*, jclass, jboolean nowrap) {
  auto* state = new DarwinInflaterState();
  const int window_bits = nowrap == JNI_TRUE ? -MAX_WBITS : MAX_WBITS;
  if (inflateInit2(&state->stream, window_bits) != Z_OK) {
    delete state;
    return 0;
  }
  return reinterpret_cast<jlong>(state);
}

void InflaterSetDictionary(JNIEnv* env, jclass, jlong address,
                           jbyteArray dictionary, jint offset, jint length) {
  auto* state = reinterpret_cast<DarwinInflaterState*>(address);
  if (state == nullptr || dictionary == nullptr || offset < 0 || length < 0 ||
      offset > env->GetArrayLength(dictionary) ||
      length > env->GetArrayLength(dictionary) - offset) {
    return;
  }
  std::vector<jbyte> bytes(static_cast<std::size_t>(length));
  env->GetByteArrayRegion(dictionary, offset, length, bytes.data());
  if (!env->ExceptionCheck()) {
    inflateSetDictionary(&state->stream,
                         reinterpret_cast<const Bytef*>(bytes.data()),
                         static_cast<uInt>(bytes.size()));
  }
}

jlong InflaterBytesBytes(JNIEnv* env, jobject, jlong address,
                          jbyteArray input, jint input_offset,
                          jint input_length, jbyteArray output,
                          jint output_offset, jint output_length) {
  auto* state = reinterpret_cast<DarwinInflaterState*>(address);
  if (state == nullptr || input == nullptr || output == nullptr ||
      input_offset < 0 || input_length < 0 || output_offset < 0 ||
      output_length < 0 ||
      input_offset > env->GetArrayLength(input) ||
      input_length > env->GetArrayLength(input) - input_offset ||
      output_offset > env->GetArrayLength(output) ||
      output_length > env->GetArrayLength(output) - output_offset) {
    return 0;
  }
  std::vector<jbyte> input_bytes(static_cast<std::size_t>(input_length));
  env->GetByteArrayRegion(input, input_offset, input_length,
                          input_bytes.data());
  if (env->ExceptionCheck()) {
    return 0;
  }
  std::vector<jbyte> output_bytes(static_cast<std::size_t>(output_length));
  state->stream.next_in = reinterpret_cast<Bytef*>(input_bytes.data());
  state->stream.avail_in = static_cast<uInt>(input_bytes.size());
  state->stream.next_out = reinterpret_cast<Bytef*>(output_bytes.data());
  state->stream.avail_out = static_cast<uInt>(output_bytes.size());
  const int result = inflate(&state->stream, Z_NO_FLUSH);
  const jint consumed = input_length - static_cast<jint>(state->stream.avail_in);
  const jint produced = output_length - static_cast<jint>(state->stream.avail_out);
  if (produced > 0) {
    env->SetByteArrayRegion(output, output_offset, produced,
                            output_bytes.data());
  }
  if (result == Z_NEED_DICT) {
    return (static_cast<jlong>(consumed) & 0x7fffffffLL) |
           ((static_cast<jlong>(produced) & 0x7fffffffLL) << 31) |
           (1LL << 63);
  }
  if (result == Z_DATA_ERROR || result == Z_STREAM_ERROR) {
    jclass exception = env->FindClass("java/util/zip/DataFormatException");
    if (exception != nullptr) {
      env->ThrowNew(exception, "invalid compressed data");
      env->DeleteLocalRef(exception);
    }
    return 0;
  }
  const jlong finished = result == Z_STREAM_END ? (1LL << 62) : 0;
  return (static_cast<jlong>(consumed) & 0x7fffffffLL) |
         ((static_cast<jlong>(produced) & 0x7fffffffLL) << 31) | finished;
}

jint InflaterGetAdler(JNIEnv*, jclass, jlong address) {
  const auto* state = reinterpret_cast<const DarwinInflaterState*>(address);
  return state == nullptr ? 0 : static_cast<jint>(state->stream.adler);
}

void InflaterReset(JNIEnv*, jclass, jlong address) {
  auto* state = reinterpret_cast<DarwinInflaterState*>(address);
  if (state != nullptr) {
    inflateReset(&state->stream);
  }
}

void InflaterEnd(JNIEnv*, jclass, jlong address) {
  auto* state = reinterpret_cast<DarwinInflaterState*>(address);
  if (state != nullptr) {
    inflateEnd(&state->stream);
    delete state;
  }
}

bool RegisterInflaterNatives(JNIEnv* env) {
  JNINativeMethod methods[] = {
      {const_cast<char*>("init"), const_cast<char*>("(Z)J"),
       reinterpret_cast<void*>(&InflaterInit)},
      {const_cast<char*>("setDictionary"), const_cast<char*>("(J[BII)V"),
       reinterpret_cast<void*>(&InflaterSetDictionary)},
      {const_cast<char*>("inflateBytesBytes"),
       const_cast<char*>("(J[BII[BII)J"),
       reinterpret_cast<void*>(&InflaterBytesBytes)},
      {const_cast<char*>("getAdler"), const_cast<char*>("(J)I"),
       reinterpret_cast<void*>(&InflaterGetAdler)},
      {const_cast<char*>("reset"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&InflaterReset)},
      {const_cast<char*>("end"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(&InflaterEnd)},
  };
  return Register(env, "java/util/zip/Inflater", methods,
                  static_cast<jint>(std::size(methods)));
}

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
  return darwin_art::AndroidUptimeNanos();
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
  // The standalone managed smoke intentionally uses host filesystem paths.
  // A real Android process installs its capability-scoped guest provider
  // before java.io.UnixFileSystem is registered, making silent host fallback
  // impossible for /data and /system paths.
  darwin_art_libcore_install_filesystem_provider(
      &darwin_art_bionic_fs_stat_core, &darwin_art_bionic_fs_mkdir_core,
      &darwin_art_bionic_fs_chmod_core, &darwin_art_bionic_errno_load);
#if defined(DARWIN_ART_FULL_LIBCORE_LINUX)
  using namespace darwin_art::libcore_darwin;
  InstallLinuxSyscallProviders(
      kLinuxSyscallProviderAbiVersion, sizeof(LinuxSyscallProviders),
      {
          .open = &darwin_art_bionic_open,
          .dup = &darwin_art_bionic_socket_broker_dup,
          .fcntl = &darwin_art_bionic_socket_broker_fcntl,
          .close = &darwin_art_bionic_close,
          .read = &darwin_art_bionic_read,
          .write = &darwin_art_bionic_write,
          .pread = &darwin_art_bionic_pread,
          .pwrite = &darwin_art_bionic_pwrite,
          .fstat = &darwin_art_bionic_fstat,
          .stat = &darwin_art_bionic_stat,
          .lseek = &darwin_art_bionic_lseek,
          .sendfile = &darwin_art_bionic_sendfile,
          .access = &darwin_art_bionic_access,
          .remove = &darwin_art_bionic_remove,
          .rename = &darwin_art_bionic_rename,
          .adopt_host_fd = &darwin_art_bionic_fs_adopt_host_fd_core,
          .load_errno = &darwin_art_bionic_errno_load,
      });
#endif
  // NativeAllocationRegistry is part of libcore's boot class path rather than
  // framework.jar. Register its free-function ABI from the libcore owner so
  // Cleaner can release RippleShader/VectorDrawable allocations.
  if (!darwin_art::RegisterFrameworkSupportNatives(env)) {
    return false;
  }
  if (!RegisterInflaterNatives(env)) {
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
    if (env->ExceptionCheck()) {
      return false;
    }
    const JNINativeMethod file_key_methods[] = {
        {const_cast<char*>("init"),
         const_cast<char*>("(Ljava/io/FileDescriptor;)V"),
         reinterpret_cast<void*>(&FileKeyInit)},
    };
    return Register(env, "sun/nio/ch/FileKey", file_key_methods,
                    static_cast<jint>(std::size(file_key_methods)));
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
         register_openjdk_file_mapping() &&
         register_libcore_memory() && RegisterLibcoreIcuNatives(env);
}

bool RegisterManagedLoadNatives(JNIEnv* env) {
#if defined(DARWIN_ART_FULL_LIBCORE_LINUX)
  if (env == nullptr) {
    return false;
  }
  // Match Android 16 OnLoad.cpp: Runtime follows Math, then the complete
  // UnixNativeDispatcher table. Each registrar is one atomic JNI table.
  register_java_lang_Runtime(env);
  if (env->ExceptionCheck()) {
    return false;
  }
  register_java_sun_nio_fs_UnixNativeDispatcher(env);
  return !env->ExceptionCheck();
#else
  (void)env;
  return true;
#endif
}

bool ShutdownLibcoreNatives() {
#if defined(DARWIN_ART_FULL_LIBCORE_LINUX)
  return darwin_art_restore_sun_nio_ch_NativeThread_signal() == 0;
#else
  return true;
#endif
}

}  // namespace darwin_art
