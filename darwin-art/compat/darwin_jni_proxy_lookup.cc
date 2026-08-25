#include <cstring>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "darwin_jni_shorty.h"
#include "darwin_runtime_adapters_internal.h"

namespace android {

namespace {

struct AndroidArm64VaList {
  uint8_t* stack;
  uint8_t* gr_top;
  uint8_t* vr_top;
  int32_t gr_offs;
  int32_t vr_offs;
};

uintptr_t AlignUp(uintptr_t value, uintptr_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

uint64_t ReadGuestGeneral(AndroidArm64VaList* args) {
  const uint8_t* source = nullptr;
  if (args->gr_offs < 0) {
    source = args->gr_top + args->gr_offs;
    args->gr_offs += 8;
  } else {
    args->stack = reinterpret_cast<uint8_t*>(
        AlignUp(reinterpret_cast<uintptr_t>(args->stack), 8));
    source = args->stack;
    args->stack += 8;
  }
  uint64_t value = 0;
  std::memcpy(&value, source, sizeof(value));
  return value;
}

double ReadGuestFloating(AndroidArm64VaList* args) {
  const uint8_t* source = nullptr;
  if (args->vr_offs < 0) {
    source = args->vr_top + args->vr_offs;
    args->vr_offs += 16;
  } else {
    args->stack = reinterpret_cast<uint8_t*>(
        AlignUp(reinterpret_cast<uintptr_t>(args->stack), 8));
    source = args->stack;
    args->stack += 8;
  }
  double value = 0;
  std::memcpy(&value, source, sizeof(value));
  return value;
}

bool DecodeGuestArguments(const std::string& descriptor, void* raw_args,
                          std::vector<jvalue>* output) {
  if (raw_args == nullptr || output == nullptr || descriptor.empty() ||
      descriptor.front() != '(') {
    return false;
  }
  AndroidArm64VaList args{};
  std::memcpy(&args, raw_args, sizeof(args));
  for (size_t index = 1; index < descriptor.size() && descriptor[index] != ')';) {
    char kind = descriptor[index++];
    if (kind == '[') {
      while (index < descriptor.size() && descriptor[index] == '[') ++index;
      if (index >= descriptor.size()) return false;
      kind = descriptor[index++];
      if (kind == 'L') {
        const size_t end = descriptor.find(';', index);
        if (end == std::string::npos) return false;
        index = end + 1;
      }
      kind = 'L';
    } else if (kind == 'L') {
      const size_t end = descriptor.find(';', index);
      if (end == std::string::npos) return false;
      index = end + 1;
    }

    jvalue value{};
    switch (kind) {
      case 'L':
        value.l = reinterpret_cast<jobject>(ReadGuestGeneral(&args));
        break;
      case 'Z':
        value.z = static_cast<jboolean>(ReadGuestGeneral(&args));
        break;
      case 'B':
        value.b = static_cast<jbyte>(ReadGuestGeneral(&args));
        break;
      case 'C':
        value.c = static_cast<jchar>(ReadGuestGeneral(&args));
        break;
      case 'S':
        value.s = static_cast<jshort>(ReadGuestGeneral(&args));
        break;
      case 'I':
        value.i = static_cast<jint>(ReadGuestGeneral(&args));
        break;
      case 'J':
        value.j = static_cast<jlong>(ReadGuestGeneral(&args));
        break;
      case 'F':
        value.f = static_cast<jfloat>(ReadGuestFloating(&args));
        break;
      case 'D':
        value.d = static_cast<jdouble>(ReadGuestFloating(&args));
        break;
      default:
        return false;
    }
    output->push_back(value);
  }
  return descriptor.find(')') != std::string::npos;
}

bool LookupArguments(ElfLibrary* library, void* method, void* raw_args,
                     std::vector<jvalue>* output) {
  if (library == nullptr || method == nullptr) return false;
  std::string descriptor;
  {
    std::lock_guard<std::mutex> lock(library->method_descriptor_mutex);
    const auto found = library->method_descriptors.find(method);
    if (found == library->method_descriptors.end()) return false;
    descriptor = found->second;
  }
  return DecodeGuestArguments(descriptor, raw_args, output);
}

}  // namespace

void* ProxyCurrentEnv(void*) { return CurrentArtEnv(); }

int32_t ProxyAttachCurrentThread(void* context, void* arguments,
                                 int32_t as_daemon) {
  auto* library = static_cast<ElfLibrary*>(context);
  if (library == nullptr || library->art_vm == nullptr) return JNI_ERR;
  JNIEnv* env = nullptr;
  return as_daemon != 0
             ? library->art_vm->AttachCurrentThreadAsDaemon(&env, arguments)
             : library->art_vm->AttachCurrentThread(&env, arguments);
}

int32_t ProxyDetachCurrentThread(void* context) {
  auto* library = static_cast<ElfLibrary*>(context);
  return library == nullptr || library->art_vm == nullptr
             ? JNI_ERR
             : library->art_vm->DetachCurrentThread();
}

void* ProxyFindClass(void* context, const char* name) {
  auto* library = static_cast<ElfLibrary*>(context);
  JNIEnv* art_env = CurrentArtEnv();
  if (library == nullptr || art_env == nullptr || name == nullptr) {
    return nullptr;
  }
  void* clazz = nullptr;
  if (library->app_loader != nullptr) {
    std::cerr << "DARWIN JNI ProxyFindClass app-loader name=" << name << "\n";
    jclass loader_class = art_env->FindClass("java/lang/ClassLoader");
    jmethodID load_class = loader_class == nullptr
                                ? nullptr
                                : art_env->GetMethodID(
                                      loader_class, "loadClass",
                                      "(Ljava/lang/String;)Ljava/lang/Class;");
    std::string binary(name);
    for (char& ch : binary) {
      if (ch == '/') ch = '.';
    }
    jstring binary_name = art_env->NewStringUTF(binary.c_str());
    clazz = load_class == nullptr || binary_name == nullptr
                ? nullptr
                : art_env->CallObjectMethod(
                      static_cast<jobject>(library->app_loader), load_class,
                      binary_name);
    if (art_env->ExceptionCheck()) art_env->ExceptionClear();
    std::cerr << "DARWIN JNI ProxyFindClass result=" << clazz << "\n";
    if (binary_name != nullptr) art_env->DeleteLocalRef(binary_name);
    if (loader_class != nullptr) art_env->DeleteLocalRef(loader_class);
  } else {
    std::cerr << "DARWIN JNI ProxyFindClass boot name=" << name << "\n";
    clazz = art_env->FindClass(name);
  }
  if (library->fixture_graph && clazz != nullptr &&
      std::strcmp(name, "darwin/art/nativefixture/NativeFixture") == 0) {
    g_elf_fixture_status.fetch_or(kElfFoundFixtureClass, std::memory_order_relaxed);
  }
  return clazz;
}

void* ProxyGetMethodId(void* context, void* clazz, const char* name,
                       const char* signature, int32_t is_static) {
  auto* library = static_cast<ElfLibrary*>(context);
  JNIEnv* art_env = CurrentArtEnv();
  if (library == nullptr || art_env == nullptr || clazz == nullptr ||
      name == nullptr || signature == nullptr) {
    return nullptr;
  }
  jmethodID method = is_static != 0
                         ? art_env->GetStaticMethodID(static_cast<jclass>(clazz),
                                                      name, signature)
                         : art_env->GetMethodID(static_cast<jclass>(clazz), name,
                                                signature);
  if (method != nullptr) {
    std::lock_guard<std::mutex> lock(library->method_descriptor_mutex);
    library->method_descriptors[method] = signature;
  }
  return method;
}

uint64_t ProxyCallMethodV(void* context, void* object, void* method,
                          void* android_va_list, int32_t return_shorty,
                          int32_t is_static) {
  auto* library = static_cast<ElfLibrary*>(context);
  JNIEnv* art_env = CurrentArtEnv();
  std::vector<jvalue> arguments;
  if (art_env == nullptr || object == nullptr ||
      !LookupArguments(library, method, android_va_list, &arguments)) {
    return 0;
  }
  const jvalue* values = arguments.empty() ? nullptr : arguments.data();
  const jobject receiver = static_cast<jobject>(object);
  const jclass clazz = static_cast<jclass>(object);
  const jmethodID id = static_cast<jmethodID>(method);
  if (is_static == 2) {
    return reinterpret_cast<uint64_t>(art_env->NewObjectA(clazz, id, values));
  }
  if (is_static != 0) {
    switch (return_shorty) {
      case 'L':
        return reinterpret_cast<uint64_t>(
            art_env->CallStaticObjectMethodA(clazz, id, values));
      case 'Z': return art_env->CallStaticBooleanMethodA(clazz, id, values);
      case 'B':
        return static_cast<uint64_t>(
            art_env->CallStaticByteMethodA(clazz, id, values));
      case 'C': return art_env->CallStaticCharMethodA(clazz, id, values);
      case 'S':
        return static_cast<uint64_t>(
            art_env->CallStaticShortMethodA(clazz, id, values));
      case 'I':
        return static_cast<uint64_t>(
            art_env->CallStaticIntMethodA(clazz, id, values));
      case 'J':
        return static_cast<uint64_t>(
            art_env->CallStaticLongMethodA(clazz, id, values));
      case 'F': {
        const jfloat value = art_env->CallStaticFloatMethodA(clazz, id, values);
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
      }
      case 'D': {
        const jdouble value = art_env->CallStaticDoubleMethodA(clazz, id, values);
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
      }
      case 'V':
        art_env->CallStaticVoidMethodA(clazz, id, values);
        return 0;
      default:
        return 0;
    }
  }
  switch (return_shorty) {
    case 'L':
      return reinterpret_cast<uint64_t>(art_env->CallObjectMethodA(receiver, id, values));
    case 'Z': return art_env->CallBooleanMethodA(receiver, id, values);
    case 'B': return static_cast<uint64_t>(art_env->CallByteMethodA(receiver, id, values));
    case 'C': return art_env->CallCharMethodA(receiver, id, values);
    case 'S': return static_cast<uint64_t>(art_env->CallShortMethodA(receiver, id, values));
    case 'I':
      return static_cast<uint64_t>(art_env->CallIntMethodA(receiver, id, values));
    case 'J': return static_cast<uint64_t>(art_env->CallLongMethodA(receiver, id, values));
    case 'F': {
      const jfloat value = art_env->CallFloatMethodA(receiver, id, values);
      uint32_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      return bits;
    }
    case 'D': {
      const jdouble value = art_env->CallDoubleMethodA(receiver, id, values);
      uint64_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      return bits;
    }
    case 'V':
      art_env->CallVoidMethodA(receiver, id, values);
      return 0;
    default:
      return 0;
  }
}

}  // namespace android
