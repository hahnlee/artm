#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "darwin_surface_internal.h"

#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendContext.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendSurface.h"
#include "include/gpu/ganesh/mtl/GrMtlDirectContext.h"

#include <new>

struct DarwinArtGpuFrame {
  sk_sp<SkSurface> surface;
  id<CAMetalDrawable> drawable = nil;
};

thread_local SkCanvas* g_active_gpu_canvas = nullptr;

namespace {

struct DarwinArtGpuState {
  sk_sp<GrDirectContext> context;
};

bool IsMainThread() {
  return [NSThread isMainThread];
}

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
        kBGRA_8888_SkColorType, nullptr, nullptr);
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
    surface->last_command_buffer = command_buffer;
  }
  g_active_gpu_canvas = nullptr;
  delete frame;
  return DARWIN_ART_SURFACE_OK;
}

DarwinArtSurface* darwin_art_surface_active_gpu(void) {
  return g_active_gpu_surface;
}

void darwin_art_surface_set_active_gpu(DarwinArtSurface* surface) {
  g_active_gpu_surface = surface;
}

void darwin_art_surface_gpu_forget(DarwinArtSurface* surface) {
  if (surface == nullptr) return;
  auto* state = State(surface);
  if (state != nullptr) {
    state->context->flushAndSubmit();
    state->context->abandonContext();
    delete state;
    surface->gpu_state = nullptr;
  }
}
