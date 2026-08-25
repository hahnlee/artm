#include "darwin_angle_egl.h"

#include "darwin_surface_bridge.h"

#include <android/bitmap.h>
#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using EGLBoolean = std::uint32_t;
using EGLint = std::int32_t;
using EGLenum = std::uint32_t;
using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLSurface = void*;

constexpr EGLint kEglNone = 0x3038;
constexpr EGLint kEglWidth = 0x3057;
constexpr EGLint kEglHeight = 0x3056;
constexpr EGLint kEglNotInitialized = 0x3001;
constexpr EGLenum kEglIosurfaceAngle = 0x3454;
constexpr EGLint kEglIosurfacePlaneAngle = 0x345A;
constexpr EGLint kEglTextureTypeAngle = 0x345C;
constexpr EGLint kEglTextureInternalFormatAngle = 0x345D;
constexpr EGLint kEglBindToTextureTargetAngle = 0x348D;
constexpr EGLint kEglTextureFormat = 0x3080;
constexpr EGLint kEglTextureTarget = 0x3081;
constexpr EGLint kEglTextureRgba = 0x305E;
constexpr EGLint kEglBackBuffer = 0x3084;
constexpr EGLint kEglTexture2d = 0x305F;
constexpr EGLint kEglTextureRectangleAngle = 0x345B;
constexpr EGLint kGlBgraExt = 0x80E1;
constexpr EGLint kGlUnsignedByte = 0x1401;
constexpr std::uint32_t kGlTexture2d = 0x0DE1;
constexpr std::uint32_t kGlTextureRectangleAngle = 0x84F5;
constexpr std::uint32_t kGlFramebuffer = 0x8D40;
constexpr std::uint32_t kGlDrawFramebuffer = 0x8CA9;
constexpr std::uint32_t kGlColorAttachment0 = 0x8CE0;
constexpr std::uint32_t kGlFramebufferComplete = 0x8CD5;

struct AngleApi {
  void* egl_library = nullptr;
  void* gles_library = nullptr;
  EGLDisplay (*get_display)(void*) = nullptr;
  EGLBoolean (*initialize)(EGLDisplay, EGLint*, EGLint*) = nullptr;
  EGLBoolean (*choose_config)(EGLDisplay, const EGLint*, EGLConfig*, EGLint,
                              EGLint*) = nullptr;
  EGLBoolean (*get_configs)(EGLDisplay, EGLConfig*, EGLint, EGLint*) = nullptr;
  EGLBoolean (*get_config_attrib)(EGLDisplay, EGLConfig, EGLint, EGLint*) =
      nullptr;
  EGLContext (*create_context)(EGLDisplay, EGLConfig, EGLContext,
                               const EGLint*) = nullptr;
  EGLSurface (*create_pbuffer_surface)(EGLDisplay, EGLConfig,
                                       const EGLint*) = nullptr;
  EGLSurface (*create_pbuffer_from_client_buffer)(
      EGLDisplay, EGLenum, void*, EGLConfig, const EGLint*) = nullptr;
  EGLBoolean (*make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) =
      nullptr;
  EGLBoolean (*destroy_context)(EGLDisplay, EGLContext) = nullptr;
  EGLBoolean (*destroy_surface)(EGLDisplay, EGLSurface) = nullptr;
  EGLBoolean (*terminate)(EGLDisplay) = nullptr;
  EGLBoolean (*swap_buffers)(EGLDisplay, EGLSurface) = nullptr;
  EGLBoolean (*bind_tex_image)(EGLDisplay, EGLSurface, EGLint) = nullptr;
  EGLBoolean (*release_tex_image)(EGLDisplay, EGLSurface, EGLint) = nullptr;
  EGLBoolean (*query_context)(EGLDisplay, EGLContext, EGLint, EGLint*) =
      nullptr;
  EGLBoolean (*query_surface)(EGLDisplay, EGLSurface, EGLint, EGLint*) =
      nullptr;
  const char* (*query_string)(EGLDisplay, EGLint) = nullptr;
  EGLDisplay (*get_current_display)() = nullptr;
  EGLContext (*get_current_context)() = nullptr;
  EGLSurface (*get_current_surface)(EGLint) = nullptr;
  EGLint (*get_error)() = nullptr;
  EGLBoolean (*release_thread)() = nullptr;
  EGLBoolean (*wait_gl)() = nullptr;
  EGLBoolean (*wait_native)(EGLint) = nullptr;

  void (*gl_pixel_store_i)(std::uint32_t, std::int32_t) = nullptr;
  void (*gl_disable)(std::uint32_t) = nullptr;
  void (*gl_enable)(std::uint32_t) = nullptr;
  std::uint8_t (*gl_is_enabled)(std::uint32_t) = nullptr;
  void (*gl_scissor)(std::int32_t, std::int32_t, std::int32_t,
                     std::int32_t) = nullptr;
  void (*gl_depth_mask)(std::uint8_t) = nullptr;
  void (*gl_clear_color)(float, float, float, float) = nullptr;
  void (*gl_clear)(std::uint32_t) = nullptr;
  void (*gl_viewport)(std::int32_t, std::int32_t, std::int32_t,
                      std::int32_t) = nullptr;
  std::uint8_t (*gl_is_texture)(std::uint32_t) = nullptr;
  void (*gl_gen_textures)(std::int32_t, std::uint32_t*) = nullptr;
  void (*gl_active_texture)(std::uint32_t) = nullptr;
  void (*gl_bind_texture)(std::uint32_t, std::uint32_t) = nullptr;
  void (*gl_tex_parameter_i)(std::uint32_t, std::uint32_t, std::int32_t) =
      nullptr;
  std::uint32_t (*gl_get_error)() = nullptr;
  void (*gl_delete_textures)(std::int32_t, const std::uint32_t*) = nullptr;
  void (*gl_tex_image_2d)(std::uint32_t, std::int32_t, std::int32_t,
                          std::int32_t, std::int32_t, std::int32_t,
                          std::uint32_t, std::uint32_t, const void*) = nullptr;
  void (*gl_tex_sub_image_2d)(std::uint32_t, std::int32_t, std::int32_t,
                              std::int32_t, std::int32_t, std::int32_t,
                              std::uint32_t, std::uint32_t, const void*) =
      nullptr;
  void (*gl_get_integer_v)(std::uint32_t, std::int32_t*) = nullptr;
  void (*gl_read_pixels)(std::int32_t, std::int32_t, std::int32_t,
                         std::int32_t, std::uint32_t, std::uint32_t,
                         void*) = nullptr;
  void (*gl_gen_framebuffers)(std::int32_t, std::uint32_t*) = nullptr;
  void (*gl_bind_framebuffer)(std::uint32_t, std::uint32_t) = nullptr;
  void (*gl_framebuffer_texture_2d)(std::uint32_t, std::uint32_t,
                                    std::uint32_t, std::uint32_t,
                                    std::int32_t) = nullptr;
  std::uint32_t (*gl_check_framebuffer_status)(std::uint32_t) = nullptr;
  void (*gl_delete_framebuffers)(std::int32_t, const std::uint32_t*) = nullptr;
  void (*gl_blit_framebuffer_angle)(std::int32_t, std::int32_t, std::int32_t,
                                    std::int32_t, std::int32_t, std::int32_t,
                                    std::int32_t, std::int32_t, std::uint32_t,
                                    std::uint32_t) = nullptr;

  bool ready = false;
};

template <typename T>
T LoadSymbol(void* library, const char* name) {
  return reinterpret_cast<T>(dlsym(library, name));
}

AngleApi& GetAngleApi() {
  static AngleApi api;
  static std::once_flag once;
  std::call_once(once, [] {
    const char* directory = std::getenv("DARWIN_ART_ANGLE_DIRECTORY");
    if (directory == nullptr || directory[0] != '/') {
      std::cerr << "ART Android EGL: DARWIN_ART_ANGLE_DIRECTORY must be an "
                   "absolute path\n";
      return;
    }
    const std::string gles_path =
        std::string(directory) + "/libGLESv2.dylib";
    const std::string egl_path = std::string(directory) + "/libEGL.dylib";
    api.gles_library = dlopen(gles_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    api.egl_library = dlopen(egl_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (api.gles_library == nullptr || api.egl_library == nullptr) {
      const char* error = dlerror();
      std::cerr << "ART Android EGL: ANGLE load failed: "
                << (error == nullptr ? "unknown error" : error) << "\n";
      return;
    }
#define LOAD_EGL(field, symbol)                                               \
  api.field = LoadSymbol<decltype(api.field)>(api.egl_library, symbol)
    LOAD_EGL(get_display, "eglGetDisplay");
    LOAD_EGL(initialize, "eglInitialize");
    LOAD_EGL(choose_config, "eglChooseConfig");
    LOAD_EGL(get_configs, "eglGetConfigs");
    LOAD_EGL(get_config_attrib, "eglGetConfigAttrib");
    LOAD_EGL(create_context, "eglCreateContext");
    LOAD_EGL(create_pbuffer_surface, "eglCreatePbufferSurface");
    LOAD_EGL(create_pbuffer_from_client_buffer,
             "eglCreatePbufferFromClientBuffer");
    LOAD_EGL(make_current, "eglMakeCurrent");
    LOAD_EGL(destroy_context, "eglDestroyContext");
    LOAD_EGL(destroy_surface, "eglDestroySurface");
    LOAD_EGL(terminate, "eglTerminate");
    LOAD_EGL(swap_buffers, "eglSwapBuffers");
    LOAD_EGL(bind_tex_image, "eglBindTexImage");
    LOAD_EGL(release_tex_image, "eglReleaseTexImage");
    LOAD_EGL(query_context, "eglQueryContext");
    LOAD_EGL(query_surface, "eglQuerySurface");
    LOAD_EGL(query_string, "eglQueryString");
    LOAD_EGL(get_current_display, "eglGetCurrentDisplay");
    LOAD_EGL(get_current_context, "eglGetCurrentContext");
    LOAD_EGL(get_current_surface, "eglGetCurrentSurface");
    LOAD_EGL(get_error, "eglGetError");
    LOAD_EGL(release_thread, "eglReleaseThread");
    LOAD_EGL(wait_gl, "eglWaitGL");
    LOAD_EGL(wait_native, "eglWaitNative");
#undef LOAD_EGL
#define LOAD_GL(field, symbol)                                                \
  api.field = LoadSymbol<decltype(api.field)>(api.gles_library, symbol)
    LOAD_GL(gl_pixel_store_i, "glPixelStorei");
    LOAD_GL(gl_disable, "glDisable");
    LOAD_GL(gl_enable, "glEnable");
    LOAD_GL(gl_is_enabled, "glIsEnabled");
    LOAD_GL(gl_scissor, "glScissor");
    LOAD_GL(gl_depth_mask, "glDepthMask");
    LOAD_GL(gl_clear_color, "glClearColor");
    LOAD_GL(gl_clear, "glClear");
    LOAD_GL(gl_viewport, "glViewport");
    LOAD_GL(gl_is_texture, "glIsTexture");
    LOAD_GL(gl_gen_textures, "glGenTextures");
    LOAD_GL(gl_active_texture, "glActiveTexture");
    LOAD_GL(gl_bind_texture, "glBindTexture");
    LOAD_GL(gl_tex_parameter_i, "glTexParameteri");
    LOAD_GL(gl_get_error, "glGetError");
    LOAD_GL(gl_delete_textures, "glDeleteTextures");
    LOAD_GL(gl_tex_image_2d, "glTexImage2D");
    LOAD_GL(gl_tex_sub_image_2d, "glTexSubImage2D");
    LOAD_GL(gl_get_integer_v, "glGetIntegerv");
    LOAD_GL(gl_read_pixels, "glReadPixels");
    LOAD_GL(gl_gen_framebuffers, "glGenFramebuffers");
    LOAD_GL(gl_bind_framebuffer, "glBindFramebuffer");
    LOAD_GL(gl_framebuffer_texture_2d, "glFramebufferTexture2D");
    LOAD_GL(gl_check_framebuffer_status, "glCheckFramebufferStatus");
    LOAD_GL(gl_delete_framebuffers, "glDeleteFramebuffers");
    LOAD_GL(gl_blit_framebuffer_angle, "glBlitFramebufferANGLE");
#undef LOAD_GL
    api.ready = api.get_display != nullptr && api.initialize != nullptr &&
                api.choose_config != nullptr &&
                api.get_configs != nullptr &&
                api.get_config_attrib != nullptr &&
                api.create_context != nullptr &&
                api.create_pbuffer_surface != nullptr &&
                api.create_pbuffer_from_client_buffer != nullptr &&
                api.make_current != nullptr && api.destroy_context != nullptr &&
                api.destroy_surface != nullptr && api.terminate != nullptr &&
                api.swap_buffers != nullptr && api.bind_tex_image != nullptr &&
                api.release_tex_image != nullptr &&
                api.query_context != nullptr &&
                api.query_surface != nullptr && api.query_string != nullptr &&
                api.get_current_display != nullptr &&
                api.get_current_context != nullptr &&
                api.get_current_surface != nullptr && api.get_error != nullptr &&
                api.release_thread != nullptr && api.wait_gl != nullptr &&
                api.wait_native != nullptr && api.gl_pixel_store_i != nullptr &&
                api.gl_disable != nullptr && api.gl_depth_mask != nullptr &&
                api.gl_enable != nullptr && api.gl_is_enabled != nullptr &&
                api.gl_scissor != nullptr &&
                api.gl_clear_color != nullptr && api.gl_clear != nullptr &&
                api.gl_viewport != nullptr && api.gl_is_texture != nullptr &&
                api.gl_gen_textures != nullptr &&
                api.gl_active_texture != nullptr &&
                api.gl_bind_texture != nullptr &&
                api.gl_tex_parameter_i != nullptr &&
                api.gl_get_error != nullptr &&
                api.gl_delete_textures != nullptr &&
                api.gl_tex_image_2d != nullptr &&
                api.gl_tex_sub_image_2d != nullptr &&
                api.gl_get_integer_v != nullptr &&
                api.gl_read_pixels != nullptr &&
                api.gl_gen_framebuffers != nullptr &&
                api.gl_bind_framebuffer != nullptr &&
                api.gl_framebuffer_texture_2d != nullptr &&
                api.gl_check_framebuffer_status != nullptr &&
                api.gl_delete_framebuffers != nullptr &&
                api.gl_blit_framebuffer_angle != nullptr;
    if (!api.ready) {
      std::cerr << "ART Android EGL: ANGLE export set is incomplete\n";
    }
  });
  return api;
}

void EglNativeClassInit(JNIEnv*, jclass) { (void)GetAngleApi(); }
void GlNativeClassInit(JNIEnv*, jclass) { (void)GetAngleApi(); }

jlong GetHandle(JNIEnv* env, jobject object, const char* field_name) {
  if (object == nullptr) return 0;
  jclass klass = env->GetObjectClass(object);
  jfieldID field =
      klass == nullptr ? nullptr : env->GetFieldID(klass, field_name, "J");
  const jlong result =
      field == nullptr || env->ExceptionCheck()
          ? 0
          : env->GetLongField(object, field);
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (klass != nullptr) env->DeleteLocalRef(klass);
  return result;
}

template <typename T>
T HandleAs(JNIEnv* env, jobject object, const char* field_name) {
  return reinterpret_cast<T>(static_cast<std::uintptr_t>(
      GetHandle(env, object, field_name)));
}

std::vector<EGLint> CopyAttributes(JNIEnv* env, jintArray attributes) {
  if (attributes == nullptr) return {};
  const jsize count = env->GetArrayLength(attributes);
  std::vector<EGLint> result(static_cast<std::size_t>(std::max(0, count)));
  if (count > 0) env->GetIntArrayRegion(attributes, 0, count, result.data());
  return result;
}

jobject NewEglConfig(JNIEnv* env, EGLConfig config) {
  jclass klass = env->FindClass("com/google/android/gles_jni/EGLConfigImpl");
  jmethodID constructor =
      klass == nullptr ? nullptr : env->GetMethodID(klass, "<init>", "(J)V");
  jobject result =
      constructor == nullptr
          ? nullptr
          : env->NewObject(klass, constructor,
                           static_cast<jlong>(reinterpret_cast<std::uintptr_t>(
                               config)));
  if (klass != nullptr) env->DeleteLocalRef(klass);
  return result;
}

jlong EglCreateContext(JNIEnv* env, jobject, jobject display, jobject config,
                       jobject share, jintArray attributes) {
  auto& api = GetAngleApi();
  if (!api.ready) return 0;
  const auto values = CopyAttributes(env, attributes);
  EGLContext context = api.create_context(
      HandleAs<EGLDisplay>(env, display, "mEGLDisplay"),
      HandleAs<EGLConfig>(env, config, "mEGLConfig"),
      HandleAs<EGLContext>(env, share, "mEGLContext"),
      values.empty() ? nullptr : values.data());
  return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(context));
}

jlong EglCreatePbufferSurface(JNIEnv* env, jobject, jobject display,
                              jobject config, jintArray attributes) {
  auto& api = GetAngleApi();
  if (!api.ready) return 0;
  const auto values = CopyAttributes(env, attributes);
  EGLSurface surface = api.create_pbuffer_surface(
      HandleAs<EGLDisplay>(env, display, "mEGLDisplay"),
      HandleAs<EGLConfig>(env, config, "mEGLConfig"),
      values.empty() ? nullptr : values.data());
  return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(surface));
}

std::array<EGLint, 5> HostPbufferAttributes() {
  const bool retina =
      std::getenv("DARWIN_ART_WINDOW_SCALE") != nullptr &&
      std::string(std::getenv("DARWIN_ART_WINDOW_SCALE")) == "2";
  return {kEglWidth, retina ? 720 : 360, kEglHeight,
          retina ? 1280 : 640, kEglNone};
}

struct HostWindowSurface {
  DarwinArtSurface* host = nullptr;
  EGLConfig config = nullptr;
  EGLint bind_target = 0;
  void* iosurface = nullptr;
  EGLSurface iosurface_target = nullptr;
  std::uint32_t texture_target = 0;
  std::uint32_t texture = 0;
  std::uint32_t framebuffer = 0;
  std::uint32_t render_width = 0;
  std::uint32_t render_height = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool target_bound = false;
};

std::mutex& HostWindowSurfaceMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<EGLSurface, HostWindowSurface>& HostWindowSurfaces() {
  static std::unordered_map<EGLSurface, HostWindowSurface> surfaces;
  return surfaces;
}

EGLSurface CreateHostWindowSurface(EGLDisplay native_display,
                                   EGLConfig native_config) {
  auto& api = GetAngleApi();
  if (!api.ready) return nullptr;
  DarwinArtSurface* host = darwin_art_surface_active_gpu();
  void* iosurface = nullptr;
  uint32_t backing_width = 0;
  uint32_t backing_height = 0;
  EGLSurface surface = nullptr;
  if (host != nullptr && darwin_art_surface_gpu_acquire_iosurface(
                             host, &iosurface, &backing_width,
                             &backing_height)) {
    EGLint bind_target = 0;
    int32_t embedded_x = 0;
    int32_t embedded_y = 0;
    uint32_t embedded_width = backing_width;
    uint32_t embedded_height = backing_height;
    darwin_art_surface_gpu_get_embedded_geometry(
        host, &embedded_x, &embedded_y, &embedded_width, &embedded_height);
    (void)embedded_x;
    (void)embedded_y;
    const uint32_t requested_width = std::min(
        backing_width, std::max<uint32_t>(1, embedded_width));
    const uint32_t requested_height = std::min(
        backing_height, std::max<uint32_t>(1, embedded_height));
    if (api.get_config_attrib(native_display, native_config,
                              kEglBindToTextureTargetAngle, &bind_target)) {
      const EGLint attributes[] = {
          kEglWidth,
          static_cast<EGLint>(requested_width),
          kEglHeight,
          static_cast<EGLint>(requested_height),
          kEglIosurfacePlaneAngle,
          0,
          kEglTextureTarget,
          bind_target,
          kEglTextureInternalFormatAngle,
          kGlBgraExt,
          kEglTextureFormat,
          kEglTextureRgba,
          kEglTextureTypeAngle,
          kGlUnsignedByte,
          kEglNone,
      };
      EGLSurface iosurface_target = api.create_pbuffer_from_client_buffer(
          native_display, kEglIosurfaceAngle, iosurface, native_config,
          attributes);
      const EGLint render_attributes[] = {
          kEglWidth, static_cast<EGLint>(requested_width), kEglHeight,
          static_cast<EGLint>(requested_height), kEglNone};
      surface = iosurface_target == nullptr
                    ? nullptr
                    : api.create_pbuffer_surface(native_display, native_config,
                                                 render_attributes);
      if (surface != nullptr) {
        const std::uint32_t gl_texture_target =
            bind_target == kEglTextureRectangleAngle
                ? kGlTextureRectangleAngle
                : (bind_target == kEglTexture2d ? kGlTexture2d : 0);
        if (gl_texture_target != 0) {
          std::lock_guard<std::mutex> lock(HostWindowSurfaceMutex());
          HostWindowSurfaces().emplace(
              surface,
              HostWindowSurface{.host = host,
                                .config = native_config,
                                .bind_target = bind_target,
                                .iosurface = iosurface,
                                .iosurface_target = iosurface_target,
                                .texture_target = gl_texture_target,
                                .render_width = requested_width,
                                .render_height = requested_height,
                                .width = requested_width,
                                .height = requested_height});
          std::cerr << "ART Android EGL: SurfaceView uses GPU blit into shared "
                       "IOSurface "
                    << requested_width << "x" << requested_height << "\n";
          return surface;
        }
        api.destroy_surface(native_display, surface);
        surface = nullptr;
      }
      if (iosurface_target != nullptr) {
        api.destroy_surface(native_display, iosurface_target);
      }
    }
    if (surface == nullptr) {
      darwin_art_surface_gpu_release_iosurface(iosurface);
      iosurface = nullptr;
      std::cerr << "ART Android EGL: IOSurface EGL target unavailable error=0x"
                << std::hex << api.get_error() << std::dec
                << "; retaining GPU pbuffer fallback\n";
    }
  }
  if (surface == nullptr) {
    const auto attributes = HostPbufferAttributes();
    surface = api.create_pbuffer_surface(native_display, native_config,
                                         attributes.data());
  }
  return surface;
}

jlong EglCreateWindowSurface(JNIEnv* env, jobject, jobject display,
                             jobject config, jobject, jintArray) {
  return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(
      CreateHostWindowSurface(
          HandleAs<EGLDisplay>(env, display, "mEGLDisplay"),
          HandleAs<EGLConfig>(env, config, "mEGLConfig"))));
}

void EglCreatePixmapSurface(JNIEnv* env, jobject self, jobject result,
                            jobject display, jobject config, jobject native,
                            jintArray attributes) {
  const jlong surface = EglCreateWindowSurface(
      env, self, display, config, native, attributes);
  jclass klass = result == nullptr ? nullptr : env->GetObjectClass(result);
  jfieldID field =
      klass == nullptr ? nullptr : env->GetFieldID(klass, "mEGLSurface", "J");
  if (field != nullptr && !env->ExceptionCheck()) {
    env->SetLongField(result, field, surface);
  }
  if (klass != nullptr) env->DeleteLocalRef(klass);
}

jlong EglGetCurrentContext(JNIEnv*, jobject) {
  auto& api = GetAngleApi();
  return !api.ready
             ? 0
             : static_cast<jlong>(reinterpret_cast<std::uintptr_t>(
                   api.get_current_context()));
}
jlong EglGetCurrentDisplay(JNIEnv*, jobject) {
  auto& api = GetAngleApi();
  return !api.ready
             ? 0
             : static_cast<jlong>(reinterpret_cast<std::uintptr_t>(
                   api.get_current_display()));
}
jlong EglGetCurrentSurface(JNIEnv*, jobject, jint selector) {
  auto& api = GetAngleApi();
  return !api.ready
             ? 0
             : static_cast<jlong>(reinterpret_cast<std::uintptr_t>(
                   api.get_current_surface(selector)));
}
jlong EglGetDisplay(JNIEnv*, jobject, jobject) {
  auto& api = GetAngleApi();
  return !api.ready
             ? 0
             : static_cast<jlong>(reinterpret_cast<std::uintptr_t>(
                   api.get_display(nullptr)));
}
jint EglGetInitCount(JNIEnv*, jclass, jobject) {
  return GetAngleApi().ready ? 1 : 0;
}

jboolean EglChooseConfig(JNIEnv* env, jobject, jobject display,
                         jintArray attributes, jobjectArray configs,
                         jint config_size, jintArray count_out) {
  auto& api = GetAngleApi();
  if (!api.ready) return JNI_FALSE;
  const auto values = CopyAttributes(env, attributes);
  const EGLint capacity = std::max<jint>(0, config_size);
  std::vector<EGLConfig> native_configs(static_cast<std::size_t>(capacity));
  EGLint count = 0;
  const EGLBoolean ok = api.choose_config(
      HandleAs<EGLDisplay>(env, display, "mEGLDisplay"),
      values.empty() ? nullptr : values.data(),
      native_configs.empty() ? nullptr : native_configs.data(), capacity,
      &count);
  if (count_out != nullptr && env->GetArrayLength(count_out) > 0) {
    env->SetIntArrayRegion(count_out, 0, 1, &count);
  }
  const jsize output_capacity =
      configs == nullptr ? 0 : env->GetArrayLength(configs);
  const jint output_count =
      std::min<jint>({count, capacity, output_capacity});
  for (jint index = 0; index < output_count && !env->ExceptionCheck(); ++index) {
    jobject config = NewEglConfig(env, native_configs[index]);
    if (config == nullptr) return JNI_FALSE;
    env->SetObjectArrayElement(configs, index, config);
    env->DeleteLocalRef(config);
  }
  return ok ? JNI_TRUE : JNI_FALSE;
}

jboolean EglGetConfigs(JNIEnv* env, jobject, jobject display,
                       jobjectArray configs, jint config_size,
                       jintArray count_out) {
  auto& api = GetAngleApi();
  if (!api.ready) return JNI_FALSE;
  const EGLint capacity = std::max<jint>(0, config_size);
  std::vector<EGLConfig> native_configs(static_cast<std::size_t>(capacity));
  EGLint count = 0;
  const EGLBoolean ok = api.get_configs(
      HandleAs<EGLDisplay>(env, display, "mEGLDisplay"),
      native_configs.empty() ? nullptr : native_configs.data(), capacity,
      &count);
  if (count_out != nullptr && env->GetArrayLength(count_out) > 0) {
    env->SetIntArrayRegion(count_out, 0, 1, &count);
  }
  const jsize output_capacity =
      configs == nullptr ? 0 : env->GetArrayLength(configs);
  const jint output_count =
      std::min<jint>({count, capacity, output_capacity});
  for (jint index = 0; index < output_count && !env->ExceptionCheck(); ++index) {
    jobject config = NewEglConfig(env, native_configs[index]);
    if (config == nullptr) return JNI_FALSE;
    env->SetObjectArrayElement(configs, index, config);
    env->DeleteLocalRef(config);
  }
  return ok ? JNI_TRUE : JNI_FALSE;
}

jboolean EglInitialize(JNIEnv* env, jobject, jobject display,
                       jintArray version) {
  auto& api = GetAngleApi();
  if (!api.ready) return JNI_FALSE;
  EGLint major = 0;
  EGLint minor = 0;
  const EGLBoolean ok = api.initialize(
      HandleAs<EGLDisplay>(env, display, "mEGLDisplay"), &major, &minor);
  if (version != nullptr && env->GetArrayLength(version) >= 2) {
    const jint values[] = {major, minor};
    env->SetIntArrayRegion(version, 0, 2, values);
  }
  return ok ? JNI_TRUE : JNI_FALSE;
}

jboolean EglGetConfigAttrib(JNIEnv* env, jobject, jobject display,
                            jobject config, jint attribute,
                            jintArray value_out) {
  auto& api = GetAngleApi();
  if (!api.ready || value_out == nullptr ||
      env->GetArrayLength(value_out) == 0) {
    return JNI_FALSE;
  }
  EGLint value = 0;
  const EGLBoolean ok = api.get_config_attrib(
      HandleAs<EGLDisplay>(env, display, "mEGLDisplay"),
      HandleAs<EGLConfig>(env, config, "mEGLConfig"), attribute, &value);
  if (ok) env->SetIntArrayRegion(value_out, 0, 1, &value);
  return ok ? JNI_TRUE : JNI_FALSE;
}

jboolean EglMakeCurrent(JNIEnv* env, jobject, jobject display, jobject draw,
                        jobject read, jobject context) {
  auto& api = GetAngleApi();
  return api.ready &&
                 api.make_current(
                     HandleAs<EGLDisplay>(env, display, "mEGLDisplay"),
                     HandleAs<EGLSurface>(env, draw, "mEGLSurface"),
                     HandleAs<EGLSurface>(env, read, "mEGLSurface"),
                     HandleAs<EGLContext>(env, context, "mEGLContext"))
             ? JNI_TRUE
             : JNI_FALSE;
}

jboolean EglDestroyContext(JNIEnv* env, jobject, jobject display,
                           jobject context) {
  auto& api = GetAngleApi();
  return api.ready && api.destroy_context(
                          HandleAs<EGLDisplay>(env, display, "mEGLDisplay"),
                          HandleAs<EGLContext>(env, context, "mEGLContext"))
             ? JNI_TRUE
             : JNI_FALSE;
}
bool DestroyHostWindowSurface(EGLDisplay native_display,
                              EGLSurface native_surface) {
  auto& api = GetAngleApi();
  HostWindowSurface window;
  {
    std::lock_guard<std::mutex> lock(HostWindowSurfaceMutex());
    auto found = HostWindowSurfaces().find(native_surface);
    if (found != HostWindowSurfaces().end()) {
      window = found->second;
      HostWindowSurfaces().erase(found);
    }
  }
  if (window.target_bound) {
    api.release_tex_image(native_display, window.iosurface_target,
                          kEglBackBuffer);
    api.gl_delete_framebuffers(1, &window.framebuffer);
    api.gl_delete_textures(1, &window.texture);
  }
  if (window.iosurface_target != nullptr) {
    api.destroy_surface(native_display, window.iosurface_target);
  }
  darwin_art_surface_gpu_release_iosurface(window.iosurface);
  return api.ready && api.destroy_surface(native_display, native_surface);
}
jboolean EglDestroySurface(JNIEnv* env, jobject, jobject display,
                           jobject surface) {
  return DestroyHostWindowSurface(
             HandleAs<EGLDisplay>(env, display, "mEGLDisplay"),
             HandleAs<EGLSurface>(env, surface, "mEGLSurface"))
             ? JNI_TRUE
             : JNI_FALSE;
}
jboolean EglTerminate(JNIEnv* env, jobject, jobject display) {
  auto& api = GetAngleApi();
  return api.ready && api.terminate(
                          HandleAs<EGLDisplay>(env, display, "mEGLDisplay"))
             ? JNI_TRUE
             : JNI_FALSE;
}
bool SwapHostWindowSurface(EGLDisplay native_display,
                           EGLSurface native_surface) {
  auto& api = GetAngleApi();
  if (!api.ready) return false;
  DarwinArtSurface* host = nullptr;
  bool transferred = false;
  uint32_t transferred_width = 0;
  uint32_t transferred_height = 0;
  {
    std::lock_guard<std::mutex> lock(HostWindowSurfaceMutex());
    auto found = HostWindowSurfaces().find(native_surface);
    if (found != HostWindowSurfaces().end()) {
      HostWindowSurface& window = found->second;
      host = window.host;
      std::int32_t previous_active_texture = 0;
      std::int32_t previous_texture = 0;
      std::int32_t previous_framebuffer = 0;
      std::int32_t previous_scissor[4] = {};
      const bool scissor_enabled = api.gl_is_enabled(0x0C11) != 0;
      api.gl_get_integer_v(0x84E0, &previous_active_texture);  // GL_ACTIVE_TEXTURE
      api.gl_get_integer_v(window.texture_target == kGlTexture2d ? 0x8069
                                                                 : 0x84F6,
                           &previous_texture);
      api.gl_get_integer_v(0x8CA6,
                           &previous_framebuffer);  // GL_FRAMEBUFFER_BINDING
      api.gl_get_integer_v(0x0C10, previous_scissor);  // GL_SCISSOR_BOX
      // AppKit resizing replaces the host IOSurface atomically. Android keeps
      // the same Surface/ANativeWindow identity across surfaceChanged(), so
      // refresh only the EGL client-buffer target while preserving the app's
      // current context and render pbuffer. The subsequent framebuffer blit
      // is still GPU-only and scales the old swapchain extent into the new
      // Android backing extent.
      void* current_iosurface = nullptr;
      uint32_t current_width = 0;
      uint32_t current_height = 0;
      if (darwin_art_surface_gpu_acquire_iosurface(
              host, &current_iosurface, &current_width, &current_height)) {
        // A BufferQueue producer keeps its negotiated buffer dimensions when
        // only the consumer's window grows; SurfaceFlinger scales that buffer
        // into the new layer bounds. Keep the existing EGL producer extent
        // for the same Android Surface identity and let the Metal compositor
        // perform the equivalent scaling. Forcing the client-buffer target to
        // the AppKit window extent makes ANGLE reject the scaled framebuffer
        // blit and freezes the last pre-resize picture.
        const uint32_t requested_width =
            std::min(current_width, std::max<uint32_t>(1, window.render_width));
        const uint32_t requested_height = std::min(
            current_height, std::max<uint32_t>(1, window.render_height));
        if (current_iosurface != window.iosurface ||
            requested_width != window.width ||
            requested_height != window.height) {
          const EGLint attributes[] = {
              kEglWidth,
              static_cast<EGLint>(requested_width),
              kEglHeight,
              static_cast<EGLint>(requested_height),
              kEglIosurfacePlaneAngle,
              0,
              kEglTextureTarget,
              window.bind_target,
              kEglTextureInternalFormatAngle,
              kGlBgraExt,
              kEglTextureFormat,
              kEglTextureRgba,
              kEglTextureTypeAngle,
              kGlUnsignedByte,
              kEglNone,
          };
          EGLSurface replacement = api.create_pbuffer_from_client_buffer(
              native_display, kEglIosurfaceAngle, current_iosurface,
              window.config, attributes);
          if (replacement != nullptr) {
            if (window.target_bound) {
              api.gl_bind_texture(window.texture_target, window.texture);
              api.release_tex_image(native_display, window.iosurface_target,
                                    kEglBackBuffer);
              api.gl_delete_framebuffers(1, &window.framebuffer);
              api.gl_delete_textures(1, &window.texture);
            }
            api.destroy_surface(native_display, window.iosurface_target);
            darwin_art_surface_gpu_release_iosurface(window.iosurface);
            window.iosurface = current_iosurface;
            window.iosurface_target = replacement;
            window.texture = 0;
            window.framebuffer = 0;
            window.width = requested_width;
            window.height = requested_height;
            window.target_bound = false;
            current_iosurface = nullptr;
            std::cerr << "ART Android EGL: refreshed resized IOSurface target "
                      << requested_width << "x" << requested_height << "\n";
          }
        }
        if (current_iosurface != nullptr) {
          darwin_art_surface_gpu_release_iosurface(current_iosurface);
        }
      }
      if (!window.target_bound) {
        api.gl_gen_textures(1, &window.texture);
        api.gl_bind_texture(window.texture_target, window.texture);
        api.gl_tex_parameter_i(window.texture_target, 0x2801,
                               0x2601);  // MIN_FILTER / LINEAR
        api.gl_tex_parameter_i(window.texture_target, 0x2800,
                               0x2601);  // MAG_FILTER / LINEAR
        if (api.bind_tex_image(native_display, window.iosurface_target,
                               kEglBackBuffer)) {
          api.gl_gen_framebuffers(1, &window.framebuffer);
          api.gl_bind_framebuffer(kGlFramebuffer, window.framebuffer);
          api.gl_framebuffer_texture_2d(
              kGlFramebuffer, kGlColorAttachment0, window.texture_target,
              window.texture, 0);
          window.target_bound =
              api.gl_check_framebuffer_status(kGlFramebuffer) ==
              kGlFramebufferComplete;
        }
        if (!window.target_bound) {
          std::cerr << "ART Android EGL: IOSurface transfer framebuffer "
                       "incomplete error=0x"
                    << std::hex << api.gl_get_error() << std::dec << "\n";
        }
      }
      if (window.target_bound) {
        api.gl_bind_framebuffer(kGlFramebuffer, 0);
        api.gl_bind_framebuffer(kGlDrawFramebuffer, window.framebuffer);
        // glBlitFramebuffer honors GL_SCISSOR_TEST. Native renderers commonly
        // leave a swapchain-sized scissor enabled; after growth that would
        // update only the old extent and leave a black strip in the resized
        // IOSurface. Preserve the app's state around our transfer.
        api.gl_disable(0x0C11);  // GL_SCISSOR_TEST
        api.gl_blit_framebuffer_angle(
            0, 0, static_cast<int32_t>(window.render_width),
            static_cast<int32_t>(window.render_height), 0, 0,
            static_cast<int32_t>(window.width),
            static_cast<int32_t>(window.height), 0x00004000,
            0x2600);  // GL_COLOR_BUFFER_BIT / GL_NEAREST
        // GL error state belongs to the guest context. Reading it here both
        // steals an app-visible error and can misattribute an earlier guest
        // error to this transfer, which used to suppress every publication
        // after a resize. A complete destination FBO plus the queued blit is
        // the host-side success contract; wait_gl() below establishes GPU
        // completion before the IOSurface generation is published.
        transferred = true;
        if (transferred) {
          transferred_width = std::min(window.render_width, window.width);
          transferred_height = std::min(window.render_height, window.height);
        }
      }
      api.gl_bind_framebuffer(kGlFramebuffer, previous_framebuffer);
      api.gl_scissor(previous_scissor[0], previous_scissor[1],
                     previous_scissor[2], previous_scissor[3]);
      if (scissor_enabled) api.gl_enable(0x0C11);  // GL_SCISSOR_TEST
      api.gl_bind_texture(window.texture_target,
                          static_cast<uint32_t>(previous_texture));
      api.gl_active_texture(static_cast<uint32_t>(previous_active_texture));
    }
  }
  static std::atomic<uint32_t> debug_clear_frames{0};
  if (std::getenv("DARWIN_ART_DEBUG_ANGLE_CLEAR") != nullptr &&
      debug_clear_frames.fetch_add(1, std::memory_order_relaxed) < 120) {
    using ColorMaskFunction = void (*)(uint8_t, uint8_t, uint8_t, uint8_t);
    static ColorMaskFunction color_mask = LoadSymbol<ColorMaskFunction>(
        api.gles_library, "glColorMask");
    if (color_mask != nullptr) color_mask(1, 1, 1, 1);
    api.gl_disable(0x0C11);  // GL_SCISSOR_TEST
    api.gl_clear_color(1.0f, 0.0f, 0.0f, 1.0f);
    api.gl_clear(0x00004000);  // GL_COLOR_BUFFER_BIT
  }
  if (std::getenv("DARWIN_ART_DEBUG_ANGLE") != nullptr) {
    static std::atomic<uint32_t> sampled_frames{0};
    const uint32_t sample =
        sampled_frames.fetch_add(1, std::memory_order_relaxed);
    if (sample < 4 || sample % 60 == 59) {
      std::int32_t viewport[4] = {};
      std::int32_t framebuffer = 0;
      std::int32_t color_bits[4] = {};
      std::int32_t scissor[4] = {};
      std::int32_t surface_width = 0;
      std::int32_t surface_height = 0;
      std::uint8_t pixel[4] = {};
      api.gl_get_integer_v(0x0BA2, viewport);  // GL_VIEWPORT
      api.gl_get_integer_v(0x8CA6, &framebuffer);  // GL_FRAMEBUFFER_BINDING
      api.gl_get_integer_v(0x0D52, &color_bits[0]);  // GL_RED_BITS
      api.gl_get_integer_v(0x0D53, &color_bits[1]);  // GL_GREEN_BITS
      api.gl_get_integer_v(0x0D54, &color_bits[2]);  // GL_BLUE_BITS
      api.gl_get_integer_v(0x0D55, &color_bits[3]);  // GL_ALPHA_BITS
      api.gl_get_integer_v(0x0C10, scissor);  // GL_SCISSOR_BOX
      api.query_surface(native_display, native_surface, kEglWidth,
                        &surface_width);
      api.query_surface(native_display, native_surface, kEglHeight,
                        &surface_height);
      const EGLSurface current_draw = api.get_current_surface(0x3059);
      api.gl_read_pixels(viewport[0] + viewport[2] / 2,
                         viewport[1] + viewport[3] / 2, 1, 1, 0x1908,
                         0x1401, pixel);  // GL_RGBA / GL_UNSIGNED_BYTE
      std::cerr << "ART Android EGL: pre-swap frame=" << sample + 1
                << " viewport=" << viewport[0] << "," << viewport[1] << ","
                << viewport[2] << "x" << viewport[3] << " center_rgba="
                << static_cast<int>(pixel[0]) << ","
                << static_cast<int>(pixel[1]) << ","
                << static_cast<int>(pixel[2]) << ","
                << static_cast<int>(pixel[3]) << " fbo=" << framebuffer
                << " surface=" << surface_width << "x" << surface_height
                << " bits=" << color_bits[0] << "," << color_bits[1] << ","
                << color_bits[2] << "," << color_bits[3] << " scissor="
                << scissor[0] << "," << scissor[1] << "," << scissor[2]
                << "x" << scissor[3]
                << " current_match=" << (current_draw == native_surface)
                << " error=0x" << std::hex
                << api.gl_get_error() << std::dec << "\n";
    }
  }
  if (!api.swap_buffers(native_display, native_surface)) return false;
  // Establish completion and visibility between ANGLE's Metal command queue
  // and the independent Skia/Metal queue before publishing this texture.
  if (host != nullptr && transferred && api.wait_gl()) {
    darwin_art_surface_gpu_set_embedded_buffer_extent(
        host, transferred_width, transferred_height);
    darwin_art_surface_gpu_publish_embedded(host);
    if (std::getenv("DARWIN_ART_DEBUG_ANGLE") != nullptr) {
      static std::atomic<uint32_t> reported_frames{0};
      const uint32_t frame =
          reported_frames.fetch_add(1, std::memory_order_relaxed) + 1;
      if (frame <= 4 || frame % 60 == 0) {
        std::cerr << "ART Android EGL: published IOSurface frame=" << frame
                  << " gl_error=0x" << std::hex << api.gl_get_error()
                  << std::dec << "\n";
      }
    }
  }
  return true;
}
jboolean EglSwapBuffers(JNIEnv* env, jobject, jobject display,
                        jobject surface) {
  return SwapHostWindowSurface(
             HandleAs<EGLDisplay>(env, display, "mEGLDisplay"),
             HandleAs<EGLSurface>(env, surface, "mEGLSurface"))
             ? JNI_TRUE
             : JNI_FALSE;
}
jboolean EglQueryContext(JNIEnv* env, jobject, jobject display, jobject context,
                         jint attribute, jintArray value_out) {
  auto& api = GetAngleApi();
  if (!api.ready || value_out == nullptr ||
      env->GetArrayLength(value_out) == 0) {
    return JNI_FALSE;
  }
  EGLint value = 0;
  const EGLBoolean ok = api.query_context(
      HandleAs<EGLDisplay>(env, display, "mEGLDisplay"),
      HandleAs<EGLContext>(env, context, "mEGLContext"), attribute, &value);
  if (ok) env->SetIntArrayRegion(value_out, 0, 1, &value);
  return ok ? JNI_TRUE : JNI_FALSE;
}
jboolean EglQuerySurface(JNIEnv* env, jobject, jobject display, jobject surface,
                         jint attribute, jintArray value_out) {
  auto& api = GetAngleApi();
  if (!api.ready || value_out == nullptr ||
      env->GetArrayLength(value_out) == 0) {
    return JNI_FALSE;
  }
  EGLint value = 0;
  const EGLBoolean ok = api.query_surface(
      HandleAs<EGLDisplay>(env, display, "mEGLDisplay"),
      HandleAs<EGLSurface>(env, surface, "mEGLSurface"), attribute, &value);
  if (ok) env->SetIntArrayRegion(value_out, 0, 1, &value);
  return ok ? JNI_TRUE : JNI_FALSE;
}
jstring EglQueryString(JNIEnv* env, jobject, jobject display, jint name) {
  auto& api = GetAngleApi();
  const char* value =
      !api.ready
          ? nullptr
          : api.query_string(
                HandleAs<EGLDisplay>(env, display, "mEGLDisplay"), name);
  return value == nullptr ? nullptr : env->NewStringUTF(value);
}
jint EglGetError(JNIEnv*, jobject) {
  auto& api = GetAngleApi();
  return api.ready ? api.get_error() : kEglNotInitialized;
}
jboolean EglReleaseThread(JNIEnv*, jobject) {
  auto& api = GetAngleApi();
  return api.ready && api.release_thread() ? JNI_TRUE : JNI_FALSE;
}
jboolean EglWaitGl(JNIEnv*, jobject) {
  auto& api = GetAngleApi();
  return api.ready && api.wait_gl() ? JNI_TRUE : JNI_FALSE;
}
jboolean EglWaitNative(JNIEnv*, jobject, jint engine, jobject) {
  auto& api = GetAngleApi();
  return api.ready && api.wait_native(engine) ? JNI_TRUE : JNI_FALSE;
}
jboolean EglCopyBuffers(JNIEnv*, jobject, jobject, jobject, jobject) {
  return JNI_FALSE;
}

void GlesPixelStoreI(JNIEnv*, jclass, jint name, jint value) {
  auto& api = GetAngleApi();
  if (api.ready) api.gl_pixel_store_i(name, value);
}
void GlesDisable(JNIEnv*, jclass, jint cap) {
  auto& api = GetAngleApi();
  if (api.ready) api.gl_disable(cap);
}
void GlesDepthMask(JNIEnv*, jclass, jboolean enabled) {
  auto& api = GetAngleApi();
  if (api.ready) api.gl_depth_mask(enabled == JNI_TRUE ? 1 : 0);
}
void GlesClearColor(JNIEnv*, jclass, jfloat red, jfloat green, jfloat blue,
                    jfloat alpha) {
  auto& api = GetAngleApi();
  if (api.ready) api.gl_clear_color(red, green, blue, alpha);
}
void GlesClear(JNIEnv*, jclass, jint mask) {
  auto& api = GetAngleApi();
  if (api.ready) api.gl_clear(mask);
}
void GlesViewport(JNIEnv*, jclass, jint x, jint y, jint width, jint height) {
  auto& api = GetAngleApi();
  if (api.ready) api.gl_viewport(x, y, width, height);
}
jboolean GlesIsTexture(JNIEnv*, jclass, jint texture) {
  auto& api = GetAngleApi();
  return api.ready && api.gl_is_texture(static_cast<std::uint32_t>(texture))
             ? JNI_TRUE
             : JNI_FALSE;
}
void GlesGenTextures(JNIEnv* env, jclass, jint count, jintArray textures,
                     jint offset) {
  auto& api = GetAngleApi();
  if (!api.ready || textures == nullptr || count < 0 || offset < 0 ||
      offset > env->GetArrayLength(textures) - count) {
    return;
  }
  std::vector<std::uint32_t> values(static_cast<std::size_t>(count));
  api.gl_gen_textures(count, values.data());
  env->SetIntArrayRegion(textures, offset, count,
                         reinterpret_cast<const jint*>(values.data()));
}
void GlesActiveTexture(JNIEnv*, jclass, jint texture) {
  auto& api = GetAngleApi();
  if (api.ready) api.gl_active_texture(static_cast<std::uint32_t>(texture));
}
void GlesBindTexture(JNIEnv*, jclass, jint target, jint texture) {
  auto& api = GetAngleApi();
  if (api.ready) {
    api.gl_bind_texture(static_cast<std::uint32_t>(target),
                        static_cast<std::uint32_t>(texture));
  }
}
void GlesTexParameterI(JNIEnv*, jclass, jint target, jint name, jint value) {
  auto& api = GetAngleApi();
  if (api.ready) {
    api.gl_tex_parameter_i(static_cast<std::uint32_t>(target),
                           static_cast<std::uint32_t>(name), value);
  }
}
jint GlesGetError(JNIEnv*, jclass) {
  auto& api = GetAngleApi();
  return api.ready ? static_cast<jint>(api.gl_get_error()) : kEglNotInitialized;
}
void GlesDeleteTextures(JNIEnv* env, jclass, jint count, jintArray textures,
                        jint offset) {
  auto& api = GetAngleApi();
  if (!api.ready || textures == nullptr || count < 0 || offset < 0 ||
      offset > env->GetArrayLength(textures) - count) {
    return;
  }
  std::vector<jint> values(static_cast<std::size_t>(count));
  env->GetIntArrayRegion(textures, offset, count, values.data());
  api.gl_delete_textures(
      count, reinterpret_cast<const std::uint32_t*>(values.data()));
}

struct BitmapGlFormat {
  jint format;
  jint type;
  std::size_t bytes_per_pixel;
};

bool GetBitmapGlFormat(const AndroidBitmapInfo& info, BitmapGlFormat* out) {
  if (out == nullptr) return false;
  switch (info.format) {
    case ANDROID_BITMAP_FORMAT_RGBA_8888:
      *out = {0x1908, 0x1401, 4};  // GL_RGBA, GL_UNSIGNED_BYTE
      return true;
    case ANDROID_BITMAP_FORMAT_RGB_565:
      *out = {0x1907, 0x8363, 2};  // GL_RGB, GL_UNSIGNED_SHORT_5_6_5
      return true;
    case ANDROID_BITMAP_FORMAT_RGBA_4444:
      *out = {0x1908, 0x8033, 2};  // GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4
      return true;
    case ANDROID_BITMAP_FORMAT_A_8:
      *out = {0x1906, 0x1401, 1};  // GL_ALPHA, GL_UNSIGNED_BYTE
      return true;
    default:
      return false;
  }
}

const void* CompactBitmapPixels(const AndroidBitmapInfo& info,
                                const BitmapGlFormat& format,
                                const void* pixels,
                                std::vector<std::uint8_t>* compact) {
  const std::size_t row_bytes =
      static_cast<std::size_t>(info.width) * format.bytes_per_pixel;
  if (info.stride == row_bytes) return pixels;
  compact->resize(row_bytes * static_cast<std::size_t>(info.height));
  const auto* source = static_cast<const std::uint8_t*>(pixels);
  for (std::uint32_t row = 0; row < info.height; ++row) {
    std::copy_n(source + static_cast<std::size_t>(row) * info.stride, row_bytes,
                compact->data() + static_cast<std::size_t>(row) * row_bytes);
  }
  return compact->data();
}

jint GlUtilsGetInternalFormat(JNIEnv* env, jclass, jobject bitmap) {
  AndroidBitmapInfo info = {};
  BitmapGlFormat format = {};
  return AndroidBitmap_getInfo(env, bitmap, &info) ==
                 ANDROID_BITMAP_RESULT_SUCCESS &&
             GetBitmapGlFormat(info, &format)
         ? format.format
         : -1;
}

jint GlUtilsGetType(JNIEnv* env, jclass, jobject bitmap) {
  AndroidBitmapInfo info = {};
  BitmapGlFormat format = {};
  return AndroidBitmap_getInfo(env, bitmap, &info) ==
                 ANDROID_BITMAP_RESULT_SUCCESS &&
             GetBitmapGlFormat(info, &format)
         ? format.type
         : -1;
}

jint GlUtilsTexImage2D(JNIEnv* env, jclass, jint target, jint level,
                       jint internal_format, jobject bitmap, jint type,
                       jint border) {
  auto& api = GetAngleApi();
  AndroidBitmapInfo info = {};
  BitmapGlFormat format = {};
  void* pixels = nullptr;
  if (!api.ready ||
      AndroidBitmap_getInfo(env, bitmap, &info) !=
          ANDROID_BITMAP_RESULT_SUCCESS ||
      !GetBitmapGlFormat(info, &format) ||
      AndroidBitmap_lockPixels(env, bitmap, &pixels) !=
          ANDROID_BITMAP_RESULT_SUCCESS ||
      pixels == nullptr) {
    return -1;
  }
  std::vector<std::uint8_t> compact;
  const void* upload = CompactBitmapPixels(info, format, pixels, &compact);
  api.gl_tex_image_2d(
      static_cast<std::uint32_t>(target), level,
      internal_format < 0 ? format.format : internal_format, info.width,
      info.height, border, static_cast<std::uint32_t>(format.format),
      static_cast<std::uint32_t>(type < 0 ? format.type : type), upload);
  AndroidBitmap_unlockPixels(env, bitmap);
  return 0;
}

jint GlUtilsTexSubImage2D(JNIEnv* env, jclass, jint target, jint level,
                          jint x_offset, jint y_offset, jobject bitmap,
                          jint requested_format, jint type) {
  auto& api = GetAngleApi();
  AndroidBitmapInfo info = {};
  BitmapGlFormat format = {};
  void* pixels = nullptr;
  if (!api.ready ||
      AndroidBitmap_getInfo(env, bitmap, &info) !=
          ANDROID_BITMAP_RESULT_SUCCESS ||
      !GetBitmapGlFormat(info, &format) ||
      AndroidBitmap_lockPixels(env, bitmap, &pixels) !=
          ANDROID_BITMAP_RESULT_SUCCESS ||
      pixels == nullptr) {
    return -1;
  }
  std::vector<std::uint8_t> compact;
  const void* upload = CompactBitmapPixels(info, format, pixels, &compact);
  api.gl_tex_sub_image_2d(
      static_cast<std::uint32_t>(target), level, x_offset, y_offset, info.width,
      info.height,
      static_cast<std::uint32_t>(requested_format < 0 ? format.format
                                                       : requested_format),
      static_cast<std::uint32_t>(type < 0 ? format.type : type), upload);
  AndroidBitmap_unlockPixels(env, bitmap);
  return 0;
}

bool Register(JNIEnv* env, const char* class_name, JNINativeMethod* methods,
              jint count) {
  jclass klass = env->FindClass(class_name);
  if (klass == nullptr) return false;
  const bool ok = env->RegisterNatives(klass, methods, count) == JNI_OK;
  env->DeleteLocalRef(klass);
  return ok;
}

}  // namespace

namespace {
struct DarwinAndroidNativeWindowBuffer {
  std::vector<uint8_t> pixels;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_pixels = 0;
  int32_t format = 1;
  uint64_t generation = 0;
};

struct DarwinAndroidNativeWindow {
  std::atomic<uint32_t> references{1};
  std::atomic<int32_t> width{0};
  std::atomic<int32_t> height{0};
  std::atomic<int32_t> format{1};
  std::mutex mutex;
  std::shared_ptr<DarwinAndroidNativeWindowBuffer> locked;
  std::shared_ptr<DarwinAndroidNativeWindowBuffer> published;
};

struct AndroidNativeWindowBufferAbi {
  int32_t width;
  int32_t height;
  int32_t stride;
  int32_t format;
  void* bits;
  uint32_t reserved[6];
};

std::mutex g_android_native_window_mutex;
DarwinAndroidNativeWindow* g_android_native_window_published = nullptr;
std::atomic<uint64_t> g_android_native_window_generation{0};

bool DebugAndroidNativeWindow() {
  const char* value = std::getenv("DARWIN_ART_DEBUG_ANATIVEWINDOW");
  return value != nullptr && std::strcmp(value, "0") != 0;
}

uint32_t AndroidNativeWindowBytesPerPixel(int32_t format) {
  // android/native_window.h: RGBA_8888=1, RGBX_8888=2, RGB_565=4.
  return format == 4 ? 2u : 4u;
}
}  // namespace

extern "C" void* darwin_art_android_ANativeWindow_fromSurface(void*, void*) {
  auto* window = new DarwinAndroidNativeWindow();
  window->width.store(darwin_art::DarwinAngleHostSurfaceWidth(),
                      std::memory_order_relaxed);
  window->height.store(darwin_art::DarwinAngleHostSurfaceHeight(),
                       std::memory_order_relaxed);
  return window;
}

extern "C" void darwin_art_android_ANativeWindow_release(void* opaque) {
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  if (window != nullptr &&
      window->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    {
      std::lock_guard<std::mutex> global_lock(g_android_native_window_mutex);
      if (g_android_native_window_published == window) {
        g_android_native_window_published = nullptr;
      }
    }
    delete window;
  }
}

extern "C" int32_t darwin_art_android_ANativeWindow_lock(
    void* opaque, void* buffer, void*) {
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  auto* native_buffer = static_cast<AndroidNativeWindowBufferAbi*>(buffer);
  if (window == nullptr || native_buffer == nullptr) return -22;
  const int32_t width = window->width.load(std::memory_order_relaxed);
  const int32_t height = window->height.load(std::memory_order_relaxed);
  const int32_t format = window->format.load(std::memory_order_relaxed);
  if (width <= 0 || height <= 0 ||
      (format != 1 && format != 2 && format != 4)) {
    return -22;
  }
  const uint32_t stride =
      (static_cast<uint32_t>(width) + 15u) & ~uint32_t{15};
  const size_t row_bytes =
      static_cast<size_t>(stride) * AndroidNativeWindowBytesPerPixel(format);
  if (row_bytes > SIZE_MAX / static_cast<size_t>(height)) return -12;
  auto storage = std::make_shared<DarwinAndroidNativeWindowBuffer>();
  storage->width = static_cast<uint32_t>(width);
  storage->height = static_cast<uint32_t>(height);
  storage->stride_pixels = stride;
  storage->format = format;
  try {
    storage->pixels.resize(row_bytes * static_cast<size_t>(height));
  } catch (const std::bad_alloc&) {
    return -12;
  }
  {
    std::lock_guard<std::mutex> lock(window->mutex);
    if (window->locked != nullptr) return -16;
    window->locked = storage;
  }
  *native_buffer = AndroidNativeWindowBufferAbi{
      .width = width,
      .height = height,
      .stride = static_cast<int32_t>(stride),
      .format = format,
      .bits = storage->pixels.data(),
      .reserved = {},
  };
  if (DebugAndroidNativeWindow()) {
    std::cerr << "ART Android ANativeWindow: lock " << width << "x" << height
              << " stride=" << stride << " format=" << format << "\n";
  }
  return 0;
}

extern "C" int32_t darwin_art_android_ANativeWindow_unlockAndPost(
    void* opaque) {
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  if (window == nullptr) return -22;
  {
    std::lock_guard<std::mutex> lock(window->mutex);
    if (window->locked == nullptr) return -22;
    window->locked->generation =
        g_android_native_window_generation.fetch_add(
            1, std::memory_order_acq_rel) +
        1;
    window->published = std::move(window->locked);
  }
  {
    std::lock_guard<std::mutex> global_lock(g_android_native_window_mutex);
    g_android_native_window_published = window;
  }
  darwin_art_surface_gpu_publish_embedded(darwin_art_surface_active_gpu());
  if (DebugAndroidNativeWindow()) {
    std::cerr << "ART Android ANativeWindow: post generation="
              << g_android_native_window_generation.load(
                     std::memory_order_relaxed)
              << "\n";
  }
  return 0;
}

extern "C" int32_t darwin_art_android_ANativeWindow_setBuffersGeometry(
    void* opaque, int32_t width, int32_t height, int32_t format) {
  auto* window = static_cast<DarwinAndroidNativeWindow*>(opaque);
  if (window == nullptr) return -22;
  if (width > 0) window->width.store(width, std::memory_order_relaxed);
  if (height > 0) window->height.store(height, std::memory_order_relaxed);
  if (format != 0) window->format.store(format, std::memory_order_relaxed);
  if (DebugAndroidNativeWindow()) {
    std::cerr << "ART Android ANativeWindow: geometry " << width << "x"
              << height << " format=" << format << "\n";
  }
  return 0;
}

extern "C" bool darwin_art_android_ANativeWindow_acquire_frame(
    DarwinArtAndroidNativeWindowFrame* frame) {
  if (frame == nullptr) return false;
  *frame = DarwinArtAndroidNativeWindowFrame{};
  std::shared_ptr<DarwinAndroidNativeWindowBuffer> storage;
  {
    std::lock_guard<std::mutex> global_lock(g_android_native_window_mutex);
    DarwinAndroidNativeWindow* window = g_android_native_window_published;
    if (window == nullptr) return false;
    std::lock_guard<std::mutex> lock(window->mutex);
    storage = window->published;
  }
  if (storage == nullptr || storage->pixels.empty()) return false;
  auto* owner = new (std::nothrow)
      std::shared_ptr<DarwinAndroidNativeWindowBuffer>(std::move(storage));
  if (owner == nullptr) return false;
  const auto& held = **owner;
  *frame = DarwinArtAndroidNativeWindowFrame{
      .pixels = held.pixels.data(),
      .size = held.pixels.size(),
      .width = held.width,
      .height = held.height,
      .stride_pixels = held.stride_pixels,
      .format = held.format,
      .generation = held.generation,
      .owner = owner,
  };
  return true;
}

extern "C" void darwin_art_android_ANativeWindow_release_frame(
    DarwinArtAndroidNativeWindowFrame* frame) {
  if (frame == nullptr) return;
  delete static_cast<
      std::shared_ptr<DarwinAndroidNativeWindowBuffer>*>(frame->owner);
  *frame = DarwinArtAndroidNativeWindowFrame{};
}

extern "C" void* darwin_art_android_eglCreateWindowSurface(
    void* display, void* config, void*, const int32_t*) {
  return CreateHostWindowSurface(display, config);
}

extern "C" uint32_t darwin_art_android_eglSwapBuffers(void* display,
                                                        void* surface) {
  return SwapHostWindowSurface(display, surface) ? 1u : 0u;
}

extern "C" uint32_t darwin_art_android_eglDestroySurface(void* display,
                                                           void* surface) {
  return DestroyHostWindowSurface(display, surface) ? 1u : 0u;
}

extern "C" void darwin_art_android_glTexImage2D(
    uint32_t target, int32_t level, int32_t internal_format, int32_t width,
    int32_t height, int32_t border, uint32_t format, uint32_t type,
    const void* pixels) {
  static std::atomic<uint32_t> calls{0};
  const uint32_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (call <= 12 || call % 120 == 0) {
    std::cerr << "ART Android GLES: glTexImage2D call=" << call << " size="
              << width << "x" << height << " internal=0x" << std::hex
              << internal_format << " format=0x" << format << " type=0x"
              << type << std::dec << " pixels=" << (pixels != nullptr)
              << "\n";
  }
  GetAngleApi().gl_tex_image_2d(target, level, internal_format, width, height,
                                border, format, type, pixels);
}

extern "C" void darwin_art_android_glTexSubImage2D(
    uint32_t target, int32_t level, int32_t x, int32_t y, int32_t width,
    int32_t height, uint32_t format, uint32_t type, const void* pixels) {
  static std::atomic<uint32_t> calls{0};
  const uint32_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (call <= 12 || call % 120 == 0) {
    uint8_t minimum = 255;
    uint8_t maximum = 0;
    uint32_t checksum = 2166136261u;
    const size_t sample_size =
        pixels == nullptr
            ? 0
            : std::min<size_t>(static_cast<size_t>(std::max(0, width)) *
                                   static_cast<size_t>(std::max(0, height)),
                               4096);
    const auto* sample = static_cast<const uint8_t*>(pixels);
    for (size_t index = 0; index < sample_size; ++index) {
      minimum = std::min(minimum, sample[index]);
      maximum = std::max(maximum, sample[index]);
      checksum = (checksum ^ sample[index]) * 16777619u;
    }
    std::cerr << "ART Android GLES: glTexSubImage2D call=" << call << " size="
              << width << "x" << height << " offset=" << x << "," << y
              << " format=0x" << std::hex << format << " type=0x" << type
              << " sample_crc=0x" << checksum << std::dec
              << " sample_minmax=" << static_cast<int>(minimum) << ","
              << static_cast<int>(maximum)
              << " pixels=" << (pixels != nullptr) << "\n";
  }
  GetAngleApi().gl_tex_sub_image_2d(target, level, x, y, width, height, format,
                                    type, pixels);
}

extern "C" void darwin_art_android_glDrawArrays(uint32_t mode, int32_t first,
                                                  int32_t count) {
  static std::atomic<uint32_t> calls{0};
  const uint32_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (call <= 12 || call % 120 == 0) {
    std::cerr << "ART Android GLES: glDrawArrays call=" << call << " mode=0x"
              << std::hex << mode << std::dec << " first=" << first
              << " count=" << count << "\n";
  }
  using Function = void (*)(uint32_t, int32_t, int32_t);
  static Function function =
      LoadSymbol<Function>(GetAngleApi().gles_library, "glDrawArrays");
  function(mode, first, count);
}

extern "C" void darwin_art_android_glDrawElements(
    uint32_t mode, int32_t count, uint32_t type, const void* indices) {
  static std::atomic<uint32_t> calls{0};
  const uint32_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool sample = call <= 12 || call % 120 == 0;
  if (sample) {
    int32_t program = 0;
    int32_t array_buffer = 0;
    int32_t element_buffer = 0;
    int32_t viewport[4] = {};
    auto& api = GetAngleApi();
    api.gl_get_integer_v(0x8B8D, &program);  // GL_CURRENT_PROGRAM
    api.gl_get_integer_v(0x8894, &array_buffer);  // GL_ARRAY_BUFFER_BINDING
    api.gl_get_integer_v(0x8895, &element_buffer);  // GL_ELEMENT_ARRAY_BUFFER_BINDING
    api.gl_get_integer_v(0x0BA2, viewport);  // GL_VIEWPORT
    std::cerr << "ART Android GLES: glDrawElements call=" << call << " mode=0x"
              << std::hex << mode << " type=0x" << type << std::dec
              << " count=" << count << " indices=" << indices
              << " program=" << program << " buffers=" << array_buffer << ","
              << element_buffer << " viewport=" << viewport[0] << ","
              << viewport[1] << "," << viewport[2] << "x" << viewport[3]
              << "\n";
  }
  using Function = void (*)(uint32_t, int32_t, uint32_t, const void*);
  static Function function =
      LoadSymbol<Function>(GetAngleApi().gles_library, "glDrawElements");
  function(mode, count, type, indices);
  if (sample) {
    auto& api = GetAngleApi();
    int32_t viewport[4] = {};
    uint8_t pixel[4] = {};
    api.gl_get_integer_v(0x0BA2, viewport);
    api.gl_read_pixels(viewport[0] + viewport[2] / 2,
                       viewport[1] + viewport[3] / 2, 1, 1, 0x1908, 0x1401,
                       pixel);
    std::cerr << "ART Android GLES: draw result rgba="
              << static_cast<int>(pixel[0]) << ","
              << static_cast<int>(pixel[1]) << ","
              << static_cast<int>(pixel[2]) << ","
              << static_cast<int>(pixel[3]) << " error=0x" << std::hex
              << api.gl_get_error() << std::dec << "\n";
  }
}

extern "C" void darwin_art_android_glUseProgram(uint32_t program) {
  static std::atomic<uint32_t> calls{0};
  const uint32_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (call <= 12 || call % 120 == 0) {
    std::cerr << "ART Android GLES: glUseProgram call=" << call
              << " program=" << program << "\n";
  }
  using Function = void (*)(uint32_t);
  static Function function =
      LoadSymbol<Function>(GetAngleApi().gles_library, "glUseProgram");
  function(program);
}

namespace darwin_art {

namespace {
std::atomic<jint> g_host_surface_width{0};
std::atomic<jint> g_host_surface_height{0};
}  // namespace

void ConfigureDarwinAngleHostSurface(jint x, jint y, jint width, jint height) {
  DarwinArtSurface* surface = darwin_art_surface_active_gpu();
  if (surface == nullptr || width <= 0 || height <= 0) return;
  g_host_surface_width.store(width, std::memory_order_release);
  g_host_surface_height.store(height, std::memory_order_release);
  darwin_art_surface_gpu_configure_embedded(
      surface, x, y, static_cast<uint32_t>(width),
      static_cast<uint32_t>(height));
  if (std::getenv("DARWIN_ART_DEBUG_RESIZE") != nullptr) {
    std::cerr << "ART Android EGL: embedded geometry=" << x << "," << y
              << " " << width << "x" << height << "\n";
  }
}

jint DarwinAngleHostSurfaceWidth() {
  return g_host_surface_width.load(std::memory_order_acquire);
}

jint DarwinAngleHostSurfaceHeight() {
  return g_host_surface_height.load(std::memory_order_acquire);
}

bool RegisterDarwinAngleEglNatives(JNIEnv* env) {
  JNINativeMethod egl_methods[] = {
      {const_cast<char*>("_nativeClassInit"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&EglNativeClassInit)},
      {const_cast<char*>("_eglCreateContext"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLConfig;Ljavax/microedition/khronos/egl/EGLContext;[I)J"),
       reinterpret_cast<void*>(&EglCreateContext)},
      {const_cast<char*>("_eglCreatePbufferSurface"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLConfig;[I)J"),
       reinterpret_cast<void*>(&EglCreatePbufferSurface)},
      {const_cast<char*>("_eglCreatePixmapSurface"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLSurface;Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLConfig;Ljava/lang/Object;[I)V"),
       reinterpret_cast<void*>(&EglCreatePixmapSurface)},
      {const_cast<char*>("_eglCreateWindowSurface"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLConfig;Ljava/lang/Object;[I)J"),
       reinterpret_cast<void*>(&EglCreateWindowSurface)},
      {const_cast<char*>("_eglCreateWindowSurfaceTexture"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLConfig;Ljava/lang/Object;[I)J"),
       reinterpret_cast<void*>(&EglCreateWindowSurface)},
      {const_cast<char*>("_eglGetCurrentContext"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&EglGetCurrentContext)},
      {const_cast<char*>("_eglGetCurrentDisplay"), const_cast<char*>("()J"),
       reinterpret_cast<void*>(&EglGetCurrentDisplay)},
      {const_cast<char*>("_eglGetCurrentSurface"), const_cast<char*>("(I)J"),
       reinterpret_cast<void*>(&EglGetCurrentSurface)},
      {const_cast<char*>("_eglGetDisplay"),
       const_cast<char*>("(Ljava/lang/Object;)J"),
       reinterpret_cast<void*>(&EglGetDisplay)},
      {const_cast<char*>("getInitCount"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;)I"),
       reinterpret_cast<void*>(&EglGetInitCount)},
      {const_cast<char*>("eglChooseConfig"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;[I[Ljavax/microedition/khronos/egl/EGLConfig;I[I)Z"),
       reinterpret_cast<void*>(&EglChooseConfig)},
      {const_cast<char*>("eglCopyBuffers"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLSurface;Ljava/lang/Object;)Z"),
       reinterpret_cast<void*>(&EglCopyBuffers)},
      {const_cast<char*>("eglDestroyContext"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLContext;)Z"),
       reinterpret_cast<void*>(&EglDestroyContext)},
      {const_cast<char*>("eglDestroySurface"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLSurface;)Z"),
       reinterpret_cast<void*>(&EglDestroySurface)},
      {const_cast<char*>("eglGetConfigAttrib"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLConfig;I[I)Z"),
       reinterpret_cast<void*>(&EglGetConfigAttrib)},
      {const_cast<char*>("eglGetConfigs"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;[Ljavax/microedition/khronos/egl/EGLConfig;I[I)Z"),
       reinterpret_cast<void*>(&EglGetConfigs)},
      {const_cast<char*>("eglGetError"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&EglGetError)},
      {const_cast<char*>("eglInitialize"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;[I)Z"),
       reinterpret_cast<void*>(&EglInitialize)},
      {const_cast<char*>("eglMakeCurrent"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLSurface;Ljavax/microedition/khronos/egl/EGLSurface;Ljavax/microedition/khronos/egl/EGLContext;)Z"),
       reinterpret_cast<void*>(&EglMakeCurrent)},
      {const_cast<char*>("eglQueryContext"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLContext;I[I)Z"),
       reinterpret_cast<void*>(&EglQueryContext)},
      {const_cast<char*>("eglQueryString"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;I)Ljava/lang/String;"),
       reinterpret_cast<void*>(&EglQueryString)},
      {const_cast<char*>("eglQuerySurface"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLSurface;I[I)Z"),
       reinterpret_cast<void*>(&EglQuerySurface)},
      {const_cast<char*>("eglReleaseThread"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(&EglReleaseThread)},
      {const_cast<char*>("eglSwapBuffers"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLSurface;)Z"),
       reinterpret_cast<void*>(&EglSwapBuffers)},
      {const_cast<char*>("eglTerminate"),
       const_cast<char*>("(Ljavax/microedition/khronos/egl/EGLDisplay;)Z"),
       reinterpret_cast<void*>(&EglTerminate)},
      {const_cast<char*>("eglWaitGL"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(&EglWaitGl)},
      {const_cast<char*>("eglWaitNative"),
       const_cast<char*>("(ILjava/lang/Object;)Z"),
       reinterpret_cast<void*>(&EglWaitNative)},
  };
  if (!Register(env, "com/google/android/gles_jni/EGLImpl", egl_methods,
                static_cast<jint>(std::size(egl_methods)))) {
    return false;
  }
  JNINativeMethod gl_impl_methods[] = {
      {const_cast<char*>("_nativeClassInit"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&GlNativeClassInit)},
  };
  if (!Register(env, "com/google/android/gles_jni/GLImpl", gl_impl_methods,
                static_cast<jint>(std::size(gl_impl_methods)))) {
    return false;
  }
  JNINativeMethod gles20_methods[] = {
      {const_cast<char*>("_nativeClassInit"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&GlNativeClassInit)},
      {const_cast<char*>("glPixelStorei"), const_cast<char*>("(II)V"),
       reinterpret_cast<void*>(&GlesPixelStoreI)},
      {const_cast<char*>("glDisable"), const_cast<char*>("(I)V"),
       reinterpret_cast<void*>(&GlesDisable)},
      {const_cast<char*>("glDepthMask"), const_cast<char*>("(Z)V"),
       reinterpret_cast<void*>(&GlesDepthMask)},
      {const_cast<char*>("glClearColor"), const_cast<char*>("(FFFF)V"),
       reinterpret_cast<void*>(&GlesClearColor)},
      {const_cast<char*>("glClear"), const_cast<char*>("(I)V"),
       reinterpret_cast<void*>(&GlesClear)},
      {const_cast<char*>("glViewport"), const_cast<char*>("(IIII)V"),
       reinterpret_cast<void*>(&GlesViewport)},
      {const_cast<char*>("glIsTexture"), const_cast<char*>("(I)Z"),
       reinterpret_cast<void*>(&GlesIsTexture)},
      {const_cast<char*>("glGenTextures"), const_cast<char*>("(I[II)V"),
       reinterpret_cast<void*>(&GlesGenTextures)},
      {const_cast<char*>("glActiveTexture"), const_cast<char*>("(I)V"),
       reinterpret_cast<void*>(&GlesActiveTexture)},
      {const_cast<char*>("glBindTexture"), const_cast<char*>("(II)V"),
       reinterpret_cast<void*>(&GlesBindTexture)},
      {const_cast<char*>("glTexParameteri"), const_cast<char*>("(III)V"),
       reinterpret_cast<void*>(&GlesTexParameterI)},
      {const_cast<char*>("glGetError"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(&GlesGetError)},
      {const_cast<char*>("glDeleteTextures"), const_cast<char*>("(I[II)V"),
       reinterpret_cast<void*>(&GlesDeleteTextures)},
  };
  if (!Register(env, "android/opengl/GLES20", gles20_methods,
                static_cast<jint>(std::size(gles20_methods)))) {
    return false;
  }
  JNINativeMethod gl_utils_methods[] = {
      {const_cast<char*>("native_getInternalFormat"),
       const_cast<char*>("(Landroid/graphics/Bitmap;)I"),
       reinterpret_cast<void*>(&GlUtilsGetInternalFormat)},
      {const_cast<char*>("native_getType"),
       const_cast<char*>("(Landroid/graphics/Bitmap;)I"),
       reinterpret_cast<void*>(&GlUtilsGetType)},
      {const_cast<char*>("native_texImage2D"),
       const_cast<char*>("(IIILandroid/graphics/Bitmap;II)I"),
       reinterpret_cast<void*>(&GlUtilsTexImage2D)},
      {const_cast<char*>("native_texSubImage2D"),
       const_cast<char*>("(IIIILandroid/graphics/Bitmap;II)I"),
       reinterpret_cast<void*>(&GlUtilsTexSubImage2D)},
  };
  return Register(env, "android/opengl/GLUtils", gl_utils_methods,
                  static_cast<jint>(std::size(gl_utils_methods)));
}

}  // namespace darwin_art
