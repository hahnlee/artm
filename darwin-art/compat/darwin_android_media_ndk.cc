#include "darwin_android_media_ndk.h"

#include "darwin_angle_egl.h"
#include "darwin_art_bionic_socket_broker.h"
#include "darwin_vp9_decoder.h"

#include <android/hardware_buffer.h>
#include <CoreMedia/CoreMedia.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <VideoToolbox/VideoToolbox.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/types.h>

struct AMediaFormat {
  std::unordered_map<std::string, int32_t> integers;
  std::unordered_map<std::string, int64_t> wide_integers;
  std::unordered_map<std::string, float> floats;
  std::unordered_map<std::string, std::string> strings;
  std::unordered_map<std::string, std::vector<uint8_t>> buffers;
};

struct AMediaCodec {
  std::mutex mutex;
  std::string name;
  std::string mime;
  bool configured = false;
  bool started = false;
  bool input_owned = false;
  bool output_owned = false;
  bool output_format_pending = false;
  bool eos = false;
  int32_t width = 0;
  int32_t height = 0;
  darwin_art::Vp9Decoder vp9;
  std::vector<uint8_t> input;
  // The dequeued slot retains full capacity across zero-sized EOS buffers.
  // Android reports valid bytes in BufferInfo, not in getOutputBuffer capacity.
  std::vector<uint8_t> output_buffer;
  std::deque<darwin_art::DecodedVideoFrame> output;
  CMVideoFormatDescriptionRef format = nullptr;
  VTDecompressionSessionRef session = nullptr;
};
struct AMediaCodecBufferInfo {
  int32_t offset;
  int32_t size;
  int64_t presentationTimeUs;
  uint32_t flags;
};
struct AMediaCodecOnAsyncNotifyCallback {
  void (*on_async_input_available)(AMediaCodec*, void*, int32_t);
  void (*on_async_output_available)(AMediaCodec*, void*, int32_t, void*);
  void (*on_async_format_changed)(AMediaCodec*, void*, AMediaFormat*);
  void (*on_async_error)(AMediaCodec*, void*, int32_t, int32_t, const char*);
};
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
MEDIA_KEY(AMEDIAFORMAT_KEY_AAC_PROFILE, "aac-profile");
MEDIA_KEY(AMEDIAFORMAT_KEY_CHANNEL_COUNT, "channel-count");
MEDIA_KEY(AMEDIAFORMAT_KEY_CHANNEL_MASK, "channel-mask");
MEDIA_KEY(AMEDIAFORMAT_KEY_DURATION, "durationUs");
MEDIA_KEY(AMEDIAFORMAT_KEY_FLAC_COMPRESSION_LEVEL, "flac-compression-level");
MEDIA_KEY(AMEDIAFORMAT_KEY_IS_ADTS, "is-adts");
MEDIA_KEY(AMEDIAFORMAT_KEY_IS_AUTOSELECT, "is-autoselect");
MEDIA_KEY(AMEDIAFORMAT_KEY_IS_DEFAULT, "is-default");
MEDIA_KEY(AMEDIAFORMAT_KEY_IS_FORCED_SUBTITLE, "is-forced-subtitle");
MEDIA_KEY(AMEDIAFORMAT_KEY_LANGUAGE, "language");
MEDIA_KEY(AMEDIAFORMAT_KEY_MAX_HEIGHT, "max-height");
MEDIA_KEY(AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, "max-input-size");
MEDIA_KEY(AMEDIAFORMAT_KEY_MAX_WIDTH, "max-width");
MEDIA_KEY(AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP, "push-blank-buffers-on-shutdown");
MEDIA_KEY(AMEDIAFORMAT_KEY_REPEAT_PREVIOUS_FRAME_AFTER, "repeat-previous-frame-after");
MEDIA_KEY(AMEDIAFORMAT_KEY_SAMPLE_RATE, "sample-rate");
MEDIA_KEY(AMEDIAFORMAT_KEY_ROTATION, "rotation-degrees");
#undef MEDIA_KEY

}  // namespace

extern "C" AMediaFormat* AMediaFormat_new() { return new AMediaFormat; }

extern "C" int32_t AMediaFormat_delete(AMediaFormat* format) {
  delete format;
  return 0;
}

extern "C" const char* AMediaFormat_toString(AMediaFormat* format) {
  static thread_local std::string description;
  description.clear();
  if (format == nullptr) return description.c_str();
  description = "AMediaFormat{";
  bool first = true;
  for (const auto& entry : format->strings) {
    if (!first) description += ", ";
    first = false;
    description += entry.first;
    description += "=";
    description += entry.second;
  }
  description += "}";
  return description.c_str();
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

extern "C" bool AMediaFormat_getInt64(AMediaFormat* format,
                                       const char* name, int64_t* output) {
  if (format == nullptr || name == nullptr || output == nullptr) return false;
  const auto found = format->wide_integers.find(name);
  if (found == format->wide_integers.end()) return false;
  *output = found->second;
  return true;
}

extern "C" bool AMediaFormat_getFloat(AMediaFormat* format,
                                       const char* name, float* output) {
  if (format == nullptr || name == nullptr || output == nullptr) return false;
  const auto found = format->floats.find(name);
  if (found == format->floats.end()) return false;
  *output = found->second;
  return true;
}

extern "C" bool AMediaFormat_getSize(AMediaFormat* format,
                                      const char* name, size_t* output) {
  int64_t value = 0;
  if (!AMediaFormat_getInt64(format, name, &value) || value < 0) return false;
  *output = static_cast<size_t>(value);
  return true;
}

extern "C" bool AMediaFormat_getString(AMediaFormat* format,
                                        const char* name,
                                        const char** output) {
  if (format == nullptr || name == nullptr || output == nullptr) return false;
  const auto found = format->strings.find(name);
  if (found == format->strings.end()) return false;
  *output = found->second.c_str();
  return true;
}

extern "C" void AMediaFormat_setInt32(AMediaFormat* format,
                                       const char* name,
                                       int32_t value) {
  if (format != nullptr && name != nullptr) format->integers[name] = value;
}

extern "C" void AMediaFormat_setInt64(AMediaFormat* format,
                                       const char* name, int64_t value) {
  if (format != nullptr && name != nullptr) format->wide_integers[name] = value;
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

extern "C" bool AMediaFormat_getBuffer(AMediaFormat* format, const char* name,
                                         void** output, size_t* size) {
  if (format == nullptr || name == nullptr || output == nullptr || size == nullptr)
    return false;
  auto found = format->buffers.find(name);
  if (found == format->buffers.end()) return false;
  *output = found->second.data();
  *size = found->second.size();
  return true;
}

extern "C" void AMediaFormat_setBuffer(AMediaFormat* format, const char* name,
                                         const void* data, size_t size) {
  if (format == nullptr || name == nullptr || data == nullptr) return;
  const auto* bytes = static_cast<const uint8_t*>(data);
  format->buffers[name] = std::vector<uint8_t>(bytes, bytes + size);
}

#include "darwin_media_extractor_ndk.inc"

std::vector<uint8_t> StripCodecStartCode(const std::vector<uint8_t>& value) {
  size_t offset = 0;
  if (value.size() >= 4 && value[0] == 0 && value[1] == 0 && value[2] == 0 &&
      value[3] == 1) offset = 4;
  else if (value.size() >= 3 && value[0] == 0 && value[1] == 0 && value[2] == 1)
    offset = 3;
  return std::vector<uint8_t>(value.begin() + offset, value.end());
}

void NdkDecodeCallback(void* refcon, void*, OSStatus status, VTDecodeInfoFlags,
                       CVImageBufferRef image, CMTime pts, CMTime) {
  auto* codec = static_cast<AMediaCodec*>(refcon);
  if (codec == nullptr || status != noErr || image == nullptr) return;
  auto pixel = static_cast<CVPixelBufferRef>(image);
  CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
  const size_t width = CVPixelBufferGetWidth(pixel);
  const size_t height = CVPixelBufferGetHeight(pixel);
  std::vector<uint8_t> frame(width * height * 3 / 2);
  if (CVPixelBufferGetPlaneCount(pixel) >= 2) {
    auto* y = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel, 0));
    auto* uv = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel, 1));
    const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 0);
    const size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 1);
    for (size_t row = 0; row < height; ++row)
      std::memcpy(frame.data() + row * width, y + row * y_stride, width);
    for (size_t row = 0; row < height / 2; ++row)
      std::memcpy(frame.data() + width * height + row * width,
                  uv + row * uv_stride, width);
  } else {
    frame.clear();
  }
  CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
  if (!frame.empty()) {
    std::lock_guard<std::mutex> lock(codec->mutex);
    darwin_art::DecodedVideoFrame decoded;
    decoded.bytes = std::move(frame);
    decoded.width = width;
    decoded.height = height;
    decoded.pts_us = CMTIME_IS_VALID(pts) ? CMTimeGetSeconds(pts) * 1000000 : 0;
    codec->output.push_back(std::move(decoded));
  }
}

extern "C" AMediaCodec* AMediaCodec_createCodecByName(const char* name) {
  if (name == nullptr || (std::strcmp(name, "c2.darwin.avc.decoder") != 0 &&
                          std::strcmp(name, "c2.darwin.hevc.decoder") != 0 &&
                          std::strcmp(name, "c2.darwin.vp9.decoder") != 0))
    return nullptr;
  auto* codec = new (std::nothrow) AMediaCodec();
  if (codec != nullptr) codec->name = name;
  return codec;
}

extern "C" AMediaCodec* AMediaCodec_createDecoderByType(const char* mime) {
  if (mime == nullptr) return nullptr;
  if (std::strcmp(mime, "video/hevc") == 0)
    return AMediaCodec_createCodecByName("c2.darwin.hevc.decoder");
  if (std::strcmp(mime, "video/avc") == 0)
    return AMediaCodec_createCodecByName("c2.darwin.avc.decoder");
  if (std::strcmp(mime, "video/x-vnd.on2.vp9") == 0)
    return AMediaCodec_createCodecByName("c2.darwin.vp9.decoder");
  return nullptr;
}

extern "C" AMediaCodec* AMediaCodec_createEncoderByType(const char*) {
  return nullptr;
}

extern "C" int32_t AMediaCodec_delete(AMediaCodec* codec) {
  if (codec == nullptr) return kMediaErrorInvalidObject;
  if (codec->session != nullptr) {
    VTDecompressionSessionWaitForAsynchronousFrames(codec->session);
    VTDecompressionSessionInvalidate(codec->session);
    CFRelease(codec->session);
  }
  if (codec->format != nullptr) CFRelease(codec->format);
  delete codec;
  return 0;
}
extern "C" int32_t AMediaCodec_configure(AMediaCodec* codec,
                                          const AMediaFormat* format,
                                          ANativeWindow* surface,
                                          AMediaCrypto* crypto,
                                          uint32_t flags) {
  if (codec == nullptr || format == nullptr) return kMediaErrorInvalidObject;
  if (surface != nullptr || crypto != nullptr || flags != 0 || codec->configured) {
    return kMediaErrorUnsupported;
  }
  auto mime = format->strings.find("mime");
  if (mime == format->strings.end()) return kMediaErrorUnsupported;
  auto width = format->integers.find("width");
  auto height = format->integers.find("height");
  if (width == format->integers.end() || height == format->integers.end() ||
      width->second <= 0 || height->second <= 0 ||
      width->second > 16384 || height->second > 16384) return kMediaErrorUnsupported;
  codec->width = width->second;
  codec->height = height->second;
  codec->mime = mime->second;
  if (codec->mime == "video/x-vnd.on2.vp9") {
    if (!codec->vp9.Initialize(codec->width, codec->height)) return kMediaErrorUnsupported;
    codec->input.resize(4 * 1024 * 1024);
    codec->output_buffer.resize(size_t(codec->width) * codec->height +
        2 * size_t((codec->width + 1) / 2) * ((codec->height + 1) / 2));
    codec->configured = true;
    if (std::getenv("DARWIN_ART_DEBUG_MEDIA_CODEC")) {
      fprintf(stderr, "ART NDK MediaCodec: VP9 configured %dx%d\n", codec->width, codec->height);
    }
    return 0;
  }
  auto sps_it = format->buffers.find("csd-0");
  auto pps_it = format->buffers.find("csd-1");
  auto vps_it = format->buffers.find("csd-2");
  const bool hevc = codec->name.find("hevc") != std::string::npos;
  if (mime == format->strings.end() || sps_it == format->buffers.end() ||
      pps_it == format->buffers.end() || (hevc && vps_it == format->buffers.end()))
    return kMediaErrorUnsupported;
  codec->mime = mime->second;
  const auto sps = StripCodecStartCode(sps_it->second);
  const auto pps = StripCodecStartCode(pps_it->second);
  OSStatus status = noErr;
  if (!hevc) {
    const uint8_t* sets[] = {sps.data(), pps.data()};
    size_t sizes[] = {sps.size(), pps.size()};
    status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
        kCFAllocatorDefault, 2, sets, sizes, 4, &codec->format);
  } else {
    const auto vps = StripCodecStartCode(vps_it->second);
    const uint8_t* sets[] = {vps.data(), sps.data(), pps.data()};
    size_t sizes[] = {vps.size(), sps.size(), pps.size()};
    status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
        kCFAllocatorDefault, 3, sets, sizes, 4, nullptr, &codec->format);
  }
  if (status != noErr) return kMediaErrorUnsupported;
  VTDecompressionOutputCallbackRecord callback = {
      .decompressionOutputCallback = &NdkDecodeCallback,
      .decompressionOutputRefCon = codec};
  status = VTDecompressionSessionCreate(kCFAllocatorDefault, codec->format,
                                         nullptr, nullptr, &callback,
                                         &codec->session);
  if (status != noErr) return kMediaErrorUnsupported;
  codec->input.resize(4 * 1024 * 1024);
  codec->output_buffer.resize(size_t(codec->width) * codec->height * 3 / 2);
  codec->configured = true;
  return 0;
}
extern "C" int32_t AMediaCodec_start(AMediaCodec* codec) {
  if (codec == nullptr) return kMediaErrorInvalidObject;
  if (!codec->configured) return kMediaErrorUnsupported;
  codec->started = true;
  codec->output_format_pending = true;
  return 0;
}
extern "C" int32_t AMediaCodec_stop(AMediaCodec* codec) {
  if (codec == nullptr) return kMediaErrorInvalidObject;
  codec->started = false;
  return 0;
}
extern "C" int32_t AMediaCodec_flush(AMediaCodec* codec) {
  if (codec == nullptr) return kMediaErrorInvalidObject;
  if (codec->session) {
    VTDecompressionSessionFinishDelayedFrames(codec->session);
    VTDecompressionSessionWaitForAsynchronousFrames(codec->session);
  }
  std::lock_guard<std::mutex> lock(codec->mutex);
  codec->output.clear();
  codec->input_owned = codec->output_owned = codec->eos = false;
  if (codec->mime == "video/x-vnd.on2.vp9" && !codec->vp9.Initialize(codec->width, codec->height)) {
    return kMediaErrorUnsupported;
  }
  return 0;
}
extern "C" int32_t AMediaCodec_setAsyncNotifyCallback(
    AMediaCodec* codec, AMediaCodecOnAsyncNotifyCallback, void*) {
  return codec == nullptr ? kMediaErrorInvalidObject : kMediaErrorUnsupported;
}
extern "C" int32_t AMediaCodec_setParameters(AMediaCodec* codec,
                                              const AMediaFormat*) {
  return codec == nullptr ? kMediaErrorInvalidObject : kMediaErrorUnsupported;
}
extern "C" uint8_t* AMediaCodec_getInputBuffer(AMediaCodec* codec, size_t index,
                                                 size_t* size) {
  if (size != nullptr) *size = 0;
  if (codec == nullptr || index != 0 || !codec->started) return nullptr;
  if (size != nullptr) *size = codec->input.size();
  return codec->input.data();
}
extern "C" uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec* codec, size_t index,
                                                  size_t* size) {
  if (size != nullptr) *size = 0;
  if (codec == nullptr || index != 0) return nullptr;
  std::lock_guard<std::mutex> lock(codec->mutex);
  if (!codec->output_owned || codec->output.empty()) return nullptr;
  if (size != nullptr) *size = codec->output_buffer.size();
  return codec->output_buffer.data();
}
extern "C" AMediaFormat* AMediaCodec_getInputFormat(AMediaCodec*) {
  return AMediaFormat_new();
}
extern "C" AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec* codec) {
  if (!codec) return nullptr;
  std::lock_guard<std::mutex> lock(codec->mutex);
  auto* format = AMediaFormat_new();
  format->strings["mime"] = "video/raw";
  format->integers = {{"width", codec->width}, {"height", codec->height},
      {"stride", codec->width}, {"slice-height", codec->height},
      {"color-format", codec->mime == "video/x-vnd.on2.vp9" ? 0x13 : 0x15},
      {"crop-left", 0}, {"crop-top", 0}, {"crop-right", codec->width - 1},
      {"crop-bottom", codec->height - 1}, {"color-range", 2},
      {"color-standard", 1}, {"color-transfer", 3}, {"rotation-degrees", 0}};
  return format;
}
extern "C" AMediaFormat* AMediaCodec_getBufferFormat(AMediaCodec* codec, size_t) {
  return AMediaCodec_getOutputFormat(codec);
}
extern "C" ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec* codec, int64_t) {
  if (!codec) return -1;
  std::lock_guard<std::mutex> lock(codec->mutex);
  if (!codec->started || codec->input_owned || codec->eos || codec->output.size() >= 4) return -1;
  codec->input_owned = true;
  return 0;
}
extern "C" ssize_t AMediaCodec_dequeueOutputBuffer(
    AMediaCodec* codec, AMediaCodecBufferInfo* info, int64_t) {
  if (codec == nullptr || info == nullptr) return -1;
  std::lock_guard<std::mutex> lock(codec->mutex);
  if (!codec->started || codec->output_owned || codec->output.empty()) return -1;
  const auto& next = codec->output.front();
  if (next.width > 0 && next.height > 0 &&
      (next.width != codec->width || next.height != codec->height)) {
    codec->width = next.width;
    codec->height = next.height;
    codec->output_format_pending = true;
  }
  if (codec->output_format_pending) {
    codec->output_format_pending = false;
    return -2;
  }
  codec->output_owned = true;
  info->offset = 0;
  info->size = static_cast<int32_t>(codec->output.front().bytes.size());
  info->presentationTimeUs = codec->output.front().pts_us;
  info->flags = codec->output.front().flags;
  if (!codec->output.front().bytes.empty()) {
    codec->output_buffer.swap(codec->output.front().bytes);
  }
  if (std::getenv("DARWIN_ART_DEBUG_MEDIA_CODEC")) {
    fprintf(stderr, "ART NDK MediaCodec: output size=%d pts=%lld flags=%u\n",
            info->size, static_cast<long long>(info->presentationTimeUs), info->flags);
  }
  return 0;
}
extern "C" int32_t AMediaCodec_queueInputBuffer(AMediaCodec* codec,
                                                 size_t index,
                                                 size_t offset,
                                                 size_t size,
                                                 uint64_t pts,
                                                 uint32_t flags) {
  if (codec == nullptr) return kMediaErrorInvalidObject;
  if (index != 0 || !codec->started || offset > codec->input.size() ||
      size > codec->input.size() - offset) return kMediaErrorUnsupported;
  {
    std::lock_guard<std::mutex> lock(codec->mutex);
    if (!codec->input_owned || codec->eos) return kMediaErrorUnsupported;
    codec->input_owned = false;
    codec->eos = (flags & 4) != 0;
    if (codec->mime == "video/x-vnd.on2.vp9") {
      if (!codec->vp9.Decode(codec->input.data() + offset, size, pts, &codec->output)) {
        return kMediaErrorUnsupported;
      }
      if (codec->eos) {
        darwin_art::DecodedVideoFrame eos;
        eos.pts_us = pts;
        eos.flags = 4;
        codec->output.emplace_back(std::move(eos));
      }
      return 0;
    }
  }
  if (!codec->session) return kMediaErrorUnsupported;
  CMBlockBufferRef block = nullptr;
  OSStatus status = CMBlockBufferCreateWithMemoryBlock(
      kCFAllocatorDefault, nullptr, size, kCFAllocatorDefault, nullptr, 0,
      size, 0, &block);
  if (status == kCMBlockBufferNoErr)
    status = CMBlockBufferReplaceDataBytes(codec->input.data() + offset, block,
                                            0, size);
  if (status != kCMBlockBufferNoErr) return kMediaErrorUnsupported;
  CMSampleTimingInfo timing = {kCMTimeInvalid, CMTimeMake(pts, 1000000),
                               kCMTimeInvalid};
  CMSampleBufferRef sample = nullptr;
  status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, codec->format,
                                      1, 1, &timing, 0, nullptr, &sample);
  if (status == noErr) {
    VTDecompressionSessionDecodeFrame(codec->session, sample, 0, nullptr,
                                      nullptr);
    CFRelease(sample);
  }
  CFRelease(block);
  return status == noErr ? 0 : kMediaErrorUnsupported;
}
extern "C" int32_t AMediaCodec_releaseOutputBuffer(AMediaCodec* codec,
                                                    size_t index,
                                                    bool) {
  if (codec == nullptr) return kMediaErrorInvalidObject;
  if (index != 0) return kMediaErrorUnsupported;
  std::lock_guard<std::mutex> lock(codec->mutex);
  if (!codec->output_owned || codec->output.empty()) return kMediaErrorUnsupported;
  codec->output.pop_front();
  codec->output_owned = false;
  return 0;
}
extern "C" int32_t AMediaCodec_releaseOutputBufferAtTime(AMediaCodec* codec,
                                                          size_t index, int64_t) {
  return AMediaCodec_releaseOutputBuffer(codec, index, true);
}
extern "C" int32_t AMediaCodec_getName(AMediaCodec* codec, char** name) {
  if (name == nullptr) return kMediaErrorInvalidObject;
  *name = nullptr;
  if (codec == nullptr) return kMediaErrorInvalidObject;
  *name = static_cast<char*>(std::malloc(codec->name.size() + 1));
  if (*name == nullptr) return -12;
  std::memcpy(*name, codec->name.c_str(), codec->name.size() + 1);
  return 0;
}
extern "C" void AMediaCodec_releaseName(AMediaCodec*, char* name) {
  std::free(name);
}

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
  MEDIA_FUNCTION(AMediaCodec_createDecoderByType)
  MEDIA_FUNCTION(AMediaCodec_createCodecByName)
  MEDIA_FUNCTION(AMediaCodec_createEncoderByType)
  MEDIA_FUNCTION(AMediaCodec_delete)
  MEDIA_FUNCTION(AMediaCodec_dequeueInputBuffer)
  MEDIA_FUNCTION(AMediaCodec_dequeueOutputBuffer)
  MEDIA_FUNCTION(AMediaCodec_flush)
  MEDIA_FUNCTION(AMediaCodec_getBufferFormat)
  MEDIA_FUNCTION(AMediaCodec_getInputBuffer)
  MEDIA_FUNCTION(AMediaCodec_getInputFormat)
  MEDIA_FUNCTION(AMediaCodec_getName)
  MEDIA_FUNCTION(AMediaCodec_getOutputBuffer)
  MEDIA_FUNCTION(AMediaCodec_getOutputFormat)
  MEDIA_FUNCTION(AMediaCodec_queueInputBuffer)
  MEDIA_FUNCTION(AMediaCodec_releaseName)
  MEDIA_FUNCTION(AMediaCodec_releaseOutputBuffer)
  MEDIA_FUNCTION(AMediaCodec_releaseOutputBufferAtTime)
  MEDIA_FUNCTION(AMediaCodec_setAsyncNotifyCallback)
  MEDIA_FUNCTION(AMediaCodec_setParameters)
  MEDIA_FUNCTION(AMediaCodec_start)
  MEDIA_FUNCTION(AMediaCodec_stop)
  MEDIA_FUNCTION(AMediaFormat_delete)
  MEDIA_FUNCTION(AMediaFormat_getInt32)
  MEDIA_FUNCTION(AMediaFormat_getInt64)
  MEDIA_FUNCTION(AMediaFormat_getFloat)
  MEDIA_FUNCTION(AMediaFormat_getSize)
  MEDIA_FUNCTION(AMediaFormat_getString)
  MEDIA_FUNCTION(AMediaFormat_getBuffer)
  MEDIA_FUNCTION(AMediaFormat_new)
  MEDIA_FUNCTION(AMediaFormat_setFloat)
  MEDIA_FUNCTION(AMediaFormat_setInt32)
  MEDIA_FUNCTION(AMediaFormat_setInt64)
  MEDIA_FUNCTION(AMediaFormat_setBuffer)
  MEDIA_FUNCTION(AMediaFormat_setString)
  MEDIA_FUNCTION(AMediaFormat_toString)
  MEDIA_FUNCTION(AMediaExtractor_new)
  MEDIA_FUNCTION(AMediaExtractor_delete)
  MEDIA_FUNCTION(AMediaExtractor_setDataSourceFd)
  MEDIA_FUNCTION(AMediaExtractor_setDataSource)
  MEDIA_FUNCTION(AMediaExtractor_setDataSourceCustom)
  MEDIA_FUNCTION(AMediaExtractor_getTrackCount)
  MEDIA_FUNCTION(AMediaExtractor_getTrackFormat)
  MEDIA_FUNCTION(AMediaExtractor_selectTrack)
  MEDIA_FUNCTION(AMediaExtractor_unselectTrack)
  MEDIA_FUNCTION(AMediaExtractor_readSampleData)
  MEDIA_FUNCTION(AMediaExtractor_getSampleFlags)
  MEDIA_FUNCTION(AMediaExtractor_getSampleTrackIndex)
  MEDIA_FUNCTION(AMediaExtractor_getSampleTime)
  MEDIA_FUNCTION(AMediaExtractor_advance)
  MEDIA_FUNCTION(AMediaExtractor_seekTo)
  MEDIA_FUNCTION(AMediaDataSource_new)
  MEDIA_FUNCTION(AMediaDataSource_delete)
  MEDIA_FUNCTION(AMediaDataSource_setUserdata)
  MEDIA_FUNCTION(AMediaDataSource_setReadAt)
  MEDIA_FUNCTION(AMediaDataSource_setGetSize)
  MEDIA_FUNCTION(AMediaDataSource_setClose)
  MEDIA_FUNCTION(AMediaDataSource_setGetAvailableSize)
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
  MEDIA_DATA(AMEDIAFORMAT_KEY_AAC_PROFILE)
  MEDIA_DATA(AMEDIAFORMAT_KEY_CHANNEL_COUNT)
  MEDIA_DATA(AMEDIAFORMAT_KEY_CHANNEL_MASK)
  MEDIA_DATA(AMEDIAFORMAT_KEY_DURATION)
  MEDIA_DATA(AMEDIAFORMAT_KEY_FLAC_COMPRESSION_LEVEL)
  MEDIA_DATA(AMEDIAFORMAT_KEY_IS_ADTS)
  MEDIA_DATA(AMEDIAFORMAT_KEY_IS_AUTOSELECT)
  MEDIA_DATA(AMEDIAFORMAT_KEY_IS_DEFAULT)
  MEDIA_DATA(AMEDIAFORMAT_KEY_IS_FORCED_SUBTITLE)
  MEDIA_DATA(AMEDIAFORMAT_KEY_LANGUAGE)
  MEDIA_DATA(AMEDIAFORMAT_KEY_MAX_HEIGHT)
  MEDIA_DATA(AMEDIAFORMAT_KEY_MAX_INPUT_SIZE)
  MEDIA_DATA(AMEDIAFORMAT_KEY_MAX_WIDTH)
  MEDIA_DATA(AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP)
  MEDIA_DATA(AMEDIAFORMAT_KEY_REPEAT_PREVIOUS_FRAME_AFTER)
  MEDIA_DATA(AMEDIAFORMAT_KEY_SAMPLE_RATE)
  MEDIA_DATA(AMEDIAFORMAT_KEY_ROTATION)
#undef MEDIA_DATA
  return nullptr;
}
