#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "darwin_surface_internal.h"
#include "darwin_android_native_window.h"

#include "include/core/SkColorSpace.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/SkImageGanesh.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendContext.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendSurface.h"
#include "include/gpu/ganesh/mtl/GrMtlDirectContext.h"

#include <cstdlib>
#include <iostream>
#include <new>

struct DarwinArtGpuFrame {
  sk_sp<SkSurface> surface;
  id<CAMetalDrawable> drawable = nil;
};

thread_local SkCanvas* g_active_gpu_canvas = nullptr;

namespace {

struct DarwinArtGpuState {
  sk_sp<GrDirectContext> context;
  sk_sp<SkImage> embedded_image;
  id<MTLTexture> embedded_texture = nil;
  sk_sp<SkImage> native_window_image;
  uint64_t native_window_generation = 0;
};

DarwinArtGpuState* State(DarwinArtSurface* surface) {
  return surface == nullptr
             ? nullptr
             : static_cast<DarwinArtGpuState*>(surface->gpu_state);
}

DarwinArtGpuState* EnsureState(DarwinArtSurface* surface) {
  if (surface == nullptr) return nullptr;
  if (auto* state = State(surface)) return state;
  auto* state = new (std::nothrow) DarwinArtGpuState();
  if (state == nullptr) return nullptr;
  GrMtlBackendContext backend = {};
  backend.fDevice.retain((__bridge GrMTLHandle)surface->device);
  backend.fQueue.retain((__bridge GrMTLHandle)surface->command_queue);
  state->context = GrDirectContexts::MakeMetal(backend);
  if (!state->context) {
    delete state;
    return nullptr;
  }
  surface->gpu_state = state;
  return state;
}

}  // namespace

DarwinArtGpuFrame* darwin_art_surface_gpu_begin(DarwinArtSurface* surface) {
  // The CAMetalLayer/window is created and serviced on AppKit's main thread,
  // but the drawable is owned by the Android RenderThread equivalent.  The
  // GPU path must therefore be callable from that renderer thread; unlike the
  // IOSurface producer APIs it never touches AppKit view state or CPU memory.
  if (surface == nullptr || surface->producer_mapped) {
    return nullptr;
  }
  @autoreleasepool {
    DarwinArtGpuState* state = EnsureState(surface);
    if (state == nullptr) return nullptr;
    id<CAMetalDrawable> drawable = [surface->view.metalLayer nextDrawable];
    if (drawable == nil || drawable.texture.width < surface->width ||
        drawable.texture.height < surface->height) {
      return nullptr;
    }
    GrMtlTextureInfo texture_info = {};
    texture_info.fTexture.retain((__bridge GrMTLHandle)drawable.texture);
    GrBackendRenderTarget target = GrBackendRenderTargets::MakeMtl(
        surface->width, surface->height, texture_info);
    sk_sp<SkSurface> sk_surface = SkSurfaces::WrapBackendRenderTarget(
        state->context.get(), target, kTopLeft_GrSurfaceOrigin,
        kBGRA_8888_SkColorType, SkColorSpace::MakeSRGB(), nullptr);
    if (sk_surface == nullptr) return nullptr;
    auto* frame = new (std::nothrow) DarwinArtGpuFrame();
    if (frame == nullptr) return nullptr;
    frame->surface = std::move(sk_surface);
    frame->drawable = drawable;
    g_active_gpu_canvas = frame->surface->getCanvas();
    return frame;
  }
}

void* darwin_art_surface_gpu_canvas(DarwinArtGpuFrame* frame) {
  return frame == nullptr || frame->surface == nullptr
             ? nullptr
             : static_cast<void*>(frame->surface->getCanvas());
}

void* darwin_art_surface_gpu_active_canvas(void) {
  return static_cast<void*>(g_active_gpu_canvas);
}

DarwinArtSurfaceResult darwin_art_surface_gpu_end(
    DarwinArtSurface* surface, DarwinArtGpuFrame* frame) {
  DarwinArtGpuState* state = State(surface);
  if (surface == nullptr || state == nullptr || frame == nullptr ||
      frame->surface == nullptr || frame->drawable == nil) {
    delete frame;
    return DARWIN_ART_SURFACE_INVALID_ARGUMENT;
  }
  @autoreleasepool {
    state->context->flushAndSubmit();
    id<MTLCommandBuffer> command_buffer =
        [surface->command_queue commandBuffer];
    if (command_buffer == nil) {
      delete frame;
      return DARWIN_ART_SURFACE_GPU_SUBMISSION_FAILED;
    }
    [command_buffer presentDrawable:frame->drawable];
    [command_buffer commit];
    {
      std::lock_guard<std::mutex> lock(surface->backing_mutex);
      surface->last_gpu_command_buffer = command_buffer;
    }
  }
  g_active_gpu_canvas = nullptr;
  delete frame;
  return DARWIN_ART_SURFACE_OK;
}

DarwinArtSurface* darwin_art_surface_active_gpu(void) {
  return g_active_gpu_surface.load(std::memory_order_acquire);
}

void darwin_art_surface_set_active_gpu(DarwinArtSurface* surface) {
  g_active_gpu_surface.store(surface, std::memory_order_release);
}

bool darwin_art_surface_gpu_composite_embedded(
    DarwinArtSurface* surface, void* sk_canvas) {
  auto* state = State(surface);
  auto* canvas = static_cast<SkCanvas*>(sk_canvas);
  if (std::getenv("DARWIN_ART_DEBUG_RESIZE") != nullptr) {
    static std::atomic<uint32_t> calls{0};
    const uint32_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call <= 4) {
      std::cerr << "ART Android GPU composite: entry pid=" << getpid()
                << " surface=" << surface << " state=" << state
                << " canvas=" << canvas << "\n";
    }
  }
  if (surface == nullptr || state == nullptr || canvas == nullptr) {
    return false;
  }
  DarwinArtAndroidNativeWindowFrame native_frame{};
  if (darwin_art_android_ANativeWindow_acquire_frame(&native_frame)) {
    if (native_frame.generation != state->native_window_generation) {
      const SkColorType color_type = native_frame.format == 4
                                         ? kRGB_565_SkColorType
                                         : kRGBA_8888_SkColorType;
      const SkAlphaType alpha_type = native_frame.format == 1
                                         ? kPremul_SkAlphaType
                                         : kOpaque_SkAlphaType;
      const SkImageInfo info = SkImageInfo::Make(
          static_cast<int>(native_frame.width),
          static_cast<int>(native_frame.height), color_type, alpha_type,
          SkColorSpace::MakeSRGB());
      auto release_frame = [](const void*, void* context) {
        auto* held = static_cast<DarwinArtAndroidNativeWindowFrame*>(context);
        darwin_art_android_ANativeWindow_release_frame(held);
        delete held;
      };
      auto* held = new (std::nothrow)
          DarwinArtAndroidNativeWindowFrame(native_frame);
      if (held != nullptr) {
        const size_t bytes_per_pixel = native_frame.format == 4 ? 2u : 4u;
        sk_sp<SkData> data = SkData::MakeWithProc(
            native_frame.pixels, native_frame.size, release_frame, held);
        native_frame.owner = nullptr;
        state->native_window_image = SkImages::RasterFromData(
            info, std::move(data),
            static_cast<size_t>(native_frame.stride_pixels) * bytes_per_pixel);
        if (state->native_window_image != nullptr) {
          state->native_window_generation = native_frame.generation;
        }
      }
    }
    darwin_art_android_ANativeWindow_release_frame(&native_frame);
  }
  if (!darwin_art_surface_gpu_is_iosurface_composition_active(
          surface->io_surface)) {
    return false;
  }
  const uint32_t width =
      surface->embedded_surface_width.load(std::memory_order_relaxed);
  const uint32_t height =
      surface->embedded_surface_height.load(std::memory_order_acquire);
  if (width == 0 || height == 0) {
    return false;
  }
  const int32_t x =
      surface->embedded_surface_x.load(std::memory_order_relaxed);
  const int32_t y =
      surface->embedded_surface_y.load(std::memory_order_relaxed);
  if (std::getenv("DARWIN_ART_DEBUG_RESIZE") != nullptr) {
    static uint32_t reported_width = 0;
    static uint32_t reported_height = 0;
    if (reported_width != width || reported_height != height) {
      reported_width = width;
      reported_height = height;
      const SkIRect clip = canvas->getDeviceClipBounds();
      std::cerr << "ART Android GPU composite: geometry=" << x << "," << y
                << " " << width << "x" << height << " backing="
                << surface->width << "x" << surface->height << " clip="
                << clip.left() << "," << clip.top() << "-" << clip.right()
                << "," << clip.bottom() << "\n";
    }
  }
  if (state->native_window_image != nullptr) {
    const SkRect source = SkRect::MakeWH(
        state->native_window_image->width(), state->native_window_image->height());
    const SkRect destination = SkRect::MakeXYWH(
        static_cast<SkScalar>(x), static_cast<SkScalar>(y),
        static_cast<SkScalar>(width), static_cast<SkScalar>(height));
    // A locked ANativeWindow is a CPU producer buffer.  Its row zero follows
    // Android's software Canvas convention, so preserve it as-is; the GLES
    // origin conversion belongs to the ANGLE/EGL IOSurface path below.
    canvas->drawImageRect(state->native_window_image, source, destination,
                          SkSamplingOptions(SkFilterMode::kLinear), nullptr,
                          SkCanvas::kStrict_SrcRectConstraint);
    return true;
  }
  if (surface->embedded_surface_frame.load(std::memory_order_acquire) == 0 ||
      surface->io_surface_texture == nil) {
    return false;
  }
  if (state->embedded_image == nullptr ||
      state->embedded_texture != surface->io_surface_texture) {
    GrMtlTextureInfo texture_info = {};
    texture_info.fTexture.retain(
        (__bridge GrMTLHandle)surface->io_surface_texture);
    const GrBackendTexture backend = GrBackendTextures::MakeMtl(
        surface->width, surface->height, skgpu::Mipmapped::kNo, texture_info,
        "Darwin ART SurfaceView IOSurface");
    state->embedded_image = SkImages::BorrowTextureFrom(
        state->context.get(), backend, kTopLeft_GrSurfaceOrigin,
        kBGRA_8888_SkColorType, kPremul_SkAlphaType,
        SkColorSpace::MakeSRGB());
    if (state->embedded_image == nullptr) return false;
    state->embedded_texture = surface->io_surface_texture;
  }
  const uint32_t buffer_height =
      surface->embedded_buffer_height.load(std::memory_order_acquire);
  const uint32_t buffer_width =
      surface->embedded_buffer_width.load(std::memory_order_relaxed);
  const SkRect source = SkRect::MakeWH(
      std::min<uint32_t>(buffer_width == 0 ? width : buffer_width,
                         surface->width),
      std::min<uint32_t>(buffer_height == 0 ? height : buffer_height,
                         surface->height));
  const SkRect destination = SkRect::MakeXYWH(
      static_cast<SkScalar>(x), static_cast<SkScalar>(y),
      static_cast<SkScalar>(width), static_cast<SkScalar>(height));
  // SurfaceFlinger has already composited producer buffers into this display
  // IOSurface in Android's top-left coordinate space.  Scan that completed
  // display image into the HWUI/Metal canvas unchanged.  Applying the raw
  // GLES framebuffer-origin conversion here flips the whole Chrome child
  // surface a second time while leaving the HWUI popup drawn above it upright.
  canvas->drawImageRect(state->embedded_image, source, destination,
                        SkSamplingOptions(SkFilterMode::kLinear), nullptr,
                        SkCanvas::kStrict_SrcRectConstraint);
  return true;
}

void darwin_art_surface_gpu_forget(DarwinArtSurface* surface) {
  if (surface == nullptr) return;
  auto* state = State(surface);
  if (state != nullptr) {
    state->context->flushAndSubmit();
    state->embedded_image.reset();
    state->embedded_texture = nil;
    state->native_window_image.reset();
    state->context->abandonContext();
    delete state;
    surface->gpu_state = nullptr;
  }
}
