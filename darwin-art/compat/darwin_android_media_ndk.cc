#include "darwin_android_media_ndk.h"

#include "darwin_angle_egl.h"
#include "darwin_art_bionic_socket_broker.h"

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

struct AMediaFormat {
  std::unordered_map<std::string, int32_t> integers;
  std::unordered_map<std::string, float> floats;
  std::unordered_map<std::string, std::string> strings;
};

struct AMediaCodec;
struct ANativeWindow;
struct AMediaCrypto;

struct AImage {
  int32_t width = 0;
  int32_t height = 0;
  AHardwareBuffer* buffer = nullptr;
};

struct AImageReader {
  int32_t width = 0;
  int32_t height = 0;
  int32_t format = 0;
  uint64_t usage = 0;
  int32_t max_images = 0;
  ANativeWindow* window = nullptr;
  AImageReader_ImageListener listener{};
};

struct AMediaCodecOnAsyncNotifyCallback {
  void (*on_async_input_available)(AMediaCodec*, void*, int32_t);
  void (*on_async_output_available)(AMediaCodec*, void*, int32_t, void*);
  void (*on_async_format_changed)(AMediaCodec*, void*, AMediaFormat*);
  void (*on_async_error)(AMediaCodec*, void*, int32_t, int32_t, const char*);
};

namespace {

constexpr int32_t kMediaErrorUnsupported = -1010;
constexpr int32_t kMediaErrorInvalidObject = -10000;

#define MEDIA_KEY(name, value) extern "C" const char* name = value
MEDIA_KEY(AMEDIAFORMAT_KEY_BITRATE_MODE, "bitrate-mode");
MEDIA_KEY(AMEDIAFORMAT_KEY_BIT_RATE, "bitrate");
MEDIA_KEY(AMEDIAFORMAT_KEY_COLOR_FORMAT, "color-format");
MEDIA_KEY(AMEDIAFORMAT_KEY_COLOR_RANGE, "color-range");
MEDIA_KEY(AMEDIAFORMAT_KEY_COLOR_STANDARD, "color-standard");
MEDIA_KEY(AMEDIAFORMAT_KEY_COLOR_TRANSFER, "color-transfer");
MEDIA_KEY(AMEDIAFORMAT_KEY_FRAME_RATE, "frame-rate");
MEDIA_KEY(AMEDIAFORMAT_KEY_HEIGHT, "height");
MEDIA_KEY(AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, "i-frame-interval");
MEDIA_KEY(AMEDIAFORMAT_KEY_LATENCY, "latency");
MEDIA_KEY(AMEDIAFORMAT_KEY_LEVEL, "level");
MEDIA_KEY(AMEDIAFORMAT_KEY_MIME, "mime");
MEDIA_KEY(AMEDIAFORMAT_KEY_PRIORITY, "priority");
MEDIA_KEY(AMEDIAFORMAT_KEY_PROFILE, "profile");
MEDIA_KEY(AMEDIAFORMAT_KEY_SLICE_HEIGHT, "slice-height");
MEDIA_KEY(AMEDIAFORMAT_KEY_STRIDE, "stride");
MEDIA_KEY(AMEDIAFORMAT_KEY_TEMPORAL_LAYERING, "ts-schema");
MEDIA_KEY(AMEDIAFORMAT_KEY_WIDTH, "width");
#undef MEDIA_KEY

}  // namespace

extern "C" AMediaFormat* AMediaFormat_new() { return new AMediaFormat; }

extern "C" int32_t AMediaFormat_delete(AMediaFormat* format) {
  delete format;
  return 0;
}

extern "C" bool AMediaFormat_getInt32(AMediaFormat* format,
                                       const char* name,
                                       int32_t* output) {
  if (format == nullptr || name == nullptr || output == nullptr) return false;
  const auto found = format->integers.find(name);
  if (found == format->integers.end()) return false;
  *output = found->second;
  return true;
}

extern "C" void AMediaFormat_setInt32(AMediaFormat* format,
                                       const char* name,
                                       int32_t value) {
  if (format != nullptr && name != nullptr) format->integers[name] = value;
}

extern "C" void AMediaFormat_setFloat(AMediaFormat* format,
                                       const char* name,
                                       float value) {
  if (format != nullptr && name != nullptr) format->floats[name] = value;
}

extern "C" void AMediaFormat_setString(AMediaFormat* format,
                                        const char* name,
                                        const char* value) {
  if (format != nullptr && name != nullptr && value != nullptr) {
    format->strings[name] = value;
  }
}

extern "C" AMediaCodec* AMediaCodec_createCodecByName(const char*) {
  // Chromium can fall back to its software decoder when no Android codec is
  // advertised. A VideoToolbox-backed codec will replace this explicit
  // unsupported result without changing the exported NDK boundary.
  return nullptr;
}

extern "C" int32_t AMediaCodec_delete(AMediaCodec*) { return 0; }
extern "C" int32_t AMediaCodec_configure(AMediaCodec* codec,
                                          const AMediaFormat*,
                                          ANativeWindow*,
                                          AMediaCrypto*,
                                          uint32_t) {
  return codec == nullptr ? kMediaErrorInvalidObject : kMediaErrorUnsupported;
}
extern "C" int32_t AMediaCodec_start(AMediaCodec* codec) {
  return codec == nullptr ? kMediaErrorInvalidObject : kMediaErrorUnsupported;
}
extern "C" int32_t AMediaCodec_stop(AMediaCodec* codec) {
  return codec == nullptr ? kMediaErrorInvalidObject : kMediaErrorUnsupported;
}
extern "C" int32_t AMediaCodec_setAsyncNotifyCallback(
    AMediaCodec* codec, AMediaCodecOnAsyncNotifyCallback, void*) {
  return codec == nullptr ? kMediaErrorInvalidObject : kMediaErrorUnsupported;
}
extern "C" int32_t AMediaCodec_setParameters(AMediaCodec* codec,
                                              const AMediaFormat*) {
  return codec == nullptr ? kMediaErrorInvalidObject : kMediaErrorUnsupported;
}
extern "C" uint8_t* AMediaCodec_getInputBuffer(AMediaCodec*, size_t, size_t*) {
  return nullptr;
}
extern "C" uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec*, size_t, size_t*) {
  return nullptr;
}
extern "C" AMediaFormat* AMediaCodec_getInputFormat(AMediaCodec*) {
  return nullptr;
}
extern "C" AMediaFormat* AMediaCodec_getBufferFormat(AMediaCodec*, size_t) {
  return nullptr;
}
extern "C" int32_t AMediaCodec_queueInputBuffer(AMediaCodec* codec,
                                                 size_t,
                                                 size_t,
                                                 size_t,
                                                 uint64_t,
                                                 uint32_t) {
  return codec == nullptr ? kMediaErrorInvalidObject : kMediaErrorUnsupported;
}
extern "C" int32_t AMediaCodec_releaseOutputBuffer(AMediaCodec* codec,
                                                    size_t,
                                                    bool) {
  return codec == nullptr ? kMediaErrorInvalidObject : kMediaErrorUnsupported;
}
extern "C" int32_t AMediaCodec_getName(AMediaCodec* codec, char** name) {
  if (name != nullptr) *name = nullptr;
  return codec == nullptr ? kMediaErrorInvalidObject : kMediaErrorUnsupported;
}
extern "C" void AMediaCodec_releaseName(AMediaCodec*, char*) {}

extern "C" media_status_t AImageReader_newWithUsage(
    int32_t width, int32_t height, int32_t format, uint64_t usage,
    int32_t max_images, AImageReader** out) {
  if (out == nullptr || width <= 0 || height <= 0 || max_images <= 0)
    return AMEDIA_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  auto* reader = new (std::nothrow) AImageReader();
  if (reader == nullptr) return AMEDIA_ERROR_UNKNOWN;
  reader->width = width;
  reader->height = height;
  reader->format = format;
  reader->usage = usage;
  reader->max_images = max_images;
  reader->window = static_cast<ANativeWindow*>(
      darwin_art_android_ANativeWindow_create(width, height, format));
  if (reader->window == nullptr) {
    delete reader;
    return AMEDIA_ERROR_UNKNOWN;
  }
  *out = reader;
  return AMEDIA_OK;
}

extern "C" void AImageReader_delete(AImageReader* reader) {
  if (reader == nullptr) return;
  darwin_art_android_ANativeWindow_release(reader->window);
  delete reader;
}

extern "C" media_status_t AImageReader_getWindow(AImageReader* reader,
                                                   ANativeWindow** out) {
  if (reader == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  *out = reader->window;
  return AMEDIA_OK;
}

extern "C" media_status_t AImageReader_setImageListener(
    AImageReader* reader, AImageReader_ImageListener* listener) {
  if (reader == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  reader->listener = listener == nullptr ? AImageReader_ImageListener{} : *listener;
  return AMEDIA_OK;
}

media_status_t AcquireEmptyImage(AImageReader* reader, AImage** out,
                                 int* fence) {
  if (reader == nullptr || out == nullptr || fence == nullptr)
    return AMEDIA_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  *fence = -1;
  return AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE;
}

extern "C" media_status_t AImageReader_acquireNextImageAsync(
    AImageReader* reader, AImage** out, int* fence) {
  return AcquireEmptyImage(reader, out, fence);
}
extern "C" media_status_t AImageReader_acquireLatestImageAsync(
    AImageReader* reader, AImage** out, int* fence) {
  return AcquireEmptyImage(reader, out, fence);
}

extern "C" void AImage_delete(AImage* image) {
  if (image == nullptr) return;
  if (image->buffer != nullptr) AHardwareBuffer_release(image->buffer);
  delete image;
}
extern "C" void AImage_deleteAsync(AImage* image, int fence) {
  if (fence >= 0) darwin_art_bionic_socket_broker_close(fence);
  if (image != nullptr) AImage_delete(image);
}
extern "C" media_status_t AImage_getWidth(const AImage* image, int32_t* out) {
  if (image == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  *out = image->width; return AMEDIA_OK;
}
extern "C" media_status_t AImage_getHeight(const AImage* image, int32_t* out) {
  if (image == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  *out = image->height; return AMEDIA_OK;
}
extern "C" media_status_t AImage_getCropRect(const AImage* image,
                                              AImageCropRect* out) {
  if (image == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  *out = AImageCropRect{0, 0, image->width, image->height};
  return AMEDIA_OK;
}
extern "C" media_status_t AImage_getHardwareBuffer(const AImage* image,
                                                     AHardwareBuffer** out) {
  if (image == nullptr || out == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  *out = image->buffer;
  return image->buffer == nullptr ? AMEDIA_ERROR_INVALID_OBJECT : AMEDIA_OK;
}

void* darwin_art_android_media_ndk_symbol(const char* symbol) {
  if (symbol == nullptr) return nullptr;
#define MEDIA_FUNCTION(name)                         \
  if (std::strcmp(symbol, #name) == 0) {             \
    return reinterpret_cast<void*>(&name);           \
  }
  MEDIA_FUNCTION(AMediaCodec_configure)
  MEDIA_FUNCTION(AMediaCodec_createCodecByName)
  MEDIA_FUNCTION(AMediaCodec_delete)
  MEDIA_FUNCTION(AMediaCodec_getBufferFormat)
  MEDIA_FUNCTION(AMediaCodec_getInputBuffer)
  MEDIA_FUNCTION(AMediaCodec_getInputFormat)
  MEDIA_FUNCTION(AMediaCodec_getName)
  MEDIA_FUNCTION(AMediaCodec_getOutputBuffer)
  MEDIA_FUNCTION(AMediaCodec_queueInputBuffer)
  MEDIA_FUNCTION(AMediaCodec_releaseName)
  MEDIA_FUNCTION(AMediaCodec_releaseOutputBuffer)
  MEDIA_FUNCTION(AMediaCodec_setAsyncNotifyCallback)
  MEDIA_FUNCTION(AMediaCodec_setParameters)
  MEDIA_FUNCTION(AMediaCodec_start)
  MEDIA_FUNCTION(AMediaCodec_stop)
  MEDIA_FUNCTION(AMediaFormat_delete)
  MEDIA_FUNCTION(AMediaFormat_getInt32)
  MEDIA_FUNCTION(AMediaFormat_new)
  MEDIA_FUNCTION(AMediaFormat_setFloat)
  MEDIA_FUNCTION(AMediaFormat_setInt32)
  MEDIA_FUNCTION(AMediaFormat_setString)
  MEDIA_FUNCTION(AImageReader_acquireLatestImageAsync)
  MEDIA_FUNCTION(AImageReader_acquireNextImageAsync)
  MEDIA_FUNCTION(AImageReader_delete)
  MEDIA_FUNCTION(AImageReader_getWindow)
  MEDIA_FUNCTION(AImageReader_newWithUsage)
  MEDIA_FUNCTION(AImageReader_setImageListener)
  MEDIA_FUNCTION(AImage_delete)
  MEDIA_FUNCTION(AImage_deleteAsync)
  MEDIA_FUNCTION(AImage_getCropRect)
  MEDIA_FUNCTION(AImage_getHardwareBuffer)
  MEDIA_FUNCTION(AImage_getHeight)
  MEDIA_FUNCTION(AImage_getWidth)
#undef MEDIA_FUNCTION
#define MEDIA_DATA(name)                             \
  if (std::strcmp(symbol, #name) == 0) {             \
    return reinterpret_cast<void*>(&name);           \
  }
  MEDIA_DATA(AMEDIAFORMAT_KEY_BITRATE_MODE)
  MEDIA_DATA(AMEDIAFORMAT_KEY_BIT_RATE)
  MEDIA_DATA(AMEDIAFORMAT_KEY_COLOR_FORMAT)
  MEDIA_DATA(AMEDIAFORMAT_KEY_COLOR_RANGE)
  MEDIA_DATA(AMEDIAFORMAT_KEY_COLOR_STANDARD)
  MEDIA_DATA(AMEDIAFORMAT_KEY_COLOR_TRANSFER)
  MEDIA_DATA(AMEDIAFORMAT_KEY_FRAME_RATE)
  MEDIA_DATA(AMEDIAFORMAT_KEY_HEIGHT)
  MEDIA_DATA(AMEDIAFORMAT_KEY_I_FRAME_INTERVAL)
  MEDIA_DATA(AMEDIAFORMAT_KEY_LATENCY)
  MEDIA_DATA(AMEDIAFORMAT_KEY_LEVEL)
  MEDIA_DATA(AMEDIAFORMAT_KEY_MIME)
  MEDIA_DATA(AMEDIAFORMAT_KEY_PRIORITY)
  MEDIA_DATA(AMEDIAFORMAT_KEY_PROFILE)
  MEDIA_DATA(AMEDIAFORMAT_KEY_SLICE_HEIGHT)
  MEDIA_DATA(AMEDIAFORMAT_KEY_STRIDE)
  MEDIA_DATA(AMEDIAFORMAT_KEY_TEMPORAL_LAYERING)
  MEDIA_DATA(AMEDIAFORMAT_KEY_WIDTH)
#undef MEDIA_DATA
  return nullptr;
}
