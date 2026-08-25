#include <android/bitmap.h>

#include <hwui/Bitmap.h>

// Keep this provider independent of GraphicsJNI.h: that umbrella header also
// pulls Canvas and the complete androidfw dependency graph. The method is
// implemented by AOSP's Bitmap.cpp already linked into the graphics runtime.
class GraphicsJNI {
 public:
  static android::Bitmap* getNativeBitmap(JNIEnv*, jobject);
};

namespace {

int32_t BitmapFormat(const SkImageInfo& info) {
  switch (info.colorType()) {
    case kN32_SkColorType:
      return ANDROID_BITMAP_FORMAT_RGBA_8888;
    case kRGB_565_SkColorType:
      return ANDROID_BITMAP_FORMAT_RGB_565;
    case kARGB_4444_SkColorType:
      return ANDROID_BITMAP_FORMAT_RGBA_4444;
    case kAlpha_8_SkColorType:
      return ANDROID_BITMAP_FORMAT_A_8;
    case kRGBA_F16_SkColorType:
      return ANDROID_BITMAP_FORMAT_RGBA_F16;
    case kRGBA_1010102_SkColorType:
      return ANDROID_BITMAP_FORMAT_RGBA_1010102;
    default:
      return ANDROID_BITMAP_FORMAT_NONE;
  }
}

uint32_t BitmapFlags(const android::Bitmap& bitmap) {
  uint32_t flags = ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE;
  switch (bitmap.info().alphaType()) {
    case kPremul_SkAlphaType:
      flags = ANDROID_BITMAP_FLAGS_ALPHA_PREMUL;
      break;
    case kUnpremul_SkAlphaType:
      flags = ANDROID_BITMAP_FLAGS_ALPHA_UNPREMUL;
      break;
    case kOpaque_SkAlphaType:
    case kUnknown_SkAlphaType:
      break;
  }
  if (bitmap.isHardware()) flags |= ANDROID_BITMAP_FLAGS_IS_HARDWARE;
  return flags;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) int AndroidBitmap_getInfo(
    JNIEnv* env, jobject object, AndroidBitmapInfo* info) {
  if (env == nullptr || object == nullptr) {
    return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  }
  android::Bitmap* bitmap = GraphicsJNI::getNativeBitmap(env, object);
  if (bitmap == nullptr) return ANDROID_BITMAP_RESULT_JNI_EXCEPTION;
  if (info != nullptr) {
    info->width = bitmap->info().width();
    info->height = bitmap->info().height();
    info->stride = static_cast<uint32_t>(bitmap->rowBytes());
    info->format = BitmapFormat(bitmap->info());
    info->flags = BitmapFlags(*bitmap);
  }
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

extern "C" __attribute__((visibility("default"))) int AndroidBitmap_lockPixels(
    JNIEnv* env, jobject object, void** address) {
  if (env == nullptr || object == nullptr) {
    return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  }
  android::Bitmap* bitmap = GraphicsJNI::getNativeBitmap(env, object);
  if (bitmap == nullptr || bitmap->isHardware()) {
    return ANDROID_BITMAP_RESULT_JNI_EXCEPTION;
  }
  void* pixels = bitmap->pixels();
  if (pixels == nullptr) return ANDROID_BITMAP_RESULT_ALLOCATION_FAILED;
  bitmap->ref();
  if (address != nullptr) *address = pixels;
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

extern "C" __attribute__((visibility("default"))) int AndroidBitmap_unlockPixels(
    JNIEnv* env, jobject object) {
  if (env == nullptr || object == nullptr) {
    return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  }
  android::Bitmap* bitmap = GraphicsJNI::getNativeBitmap(env, object);
  if (bitmap == nullptr || bitmap->isHardware()) {
    return ANDROID_BITMAP_RESULT_JNI_EXCEPTION;
  }
  bitmap->notifyPixelsChanged();
  bitmap->unref();
  return ANDROID_BITMAP_RESULT_SUCCESS;
}
