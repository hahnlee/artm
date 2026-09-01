#include "metal_composer.h"

#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>

namespace {

struct MetalComposerState {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLRenderPipelineState> pipeline = nil;

  ~MetalComposerState() {
    [pipeline release];
    [queue release];
    [device release];
  }
};

std::mutex& ComposerMutex() {
  static std::mutex mutex;
  return mutex;
}

MetalComposerState& ComposerState() {
  static MetalComposerState state;
  return state;
}

bool EnsurePipeline(id<MTLDevice> device, MetalComposerState& state) {
  if (state.device == device && state.queue != nil && state.pipeline != nil)
    return true;
  [state.pipeline release];
  [state.queue release];
  [state.device release];
  state = {};

  static NSString* const source = @R"metal(
#include <metal_stdlib>
using namespace metal;
struct VertexIn { float2 position; float2 texcoord; };
struct VertexOut { float4 position [[position]]; float2 texcoord; };
vertex VertexOut darwin_art_composer_vertex(
    uint vertex_id [[vertex_id]], constant VertexIn* vertices [[buffer(0)]]) {
  VertexOut out;
  out.position = float4(vertices[vertex_id].position, 0.0, 1.0);
  out.texcoord = vertices[vertex_id].texcoord;
  return out;
}
fragment float4 darwin_art_composer_fragment(
    VertexOut in [[stage_in]], texture2d<float> source [[texture(0)]],
    constant float& alpha [[buffer(0)]]) {
  constexpr sampler linear_sampler(coord::normalized, address::clamp_to_edge,
                                   filter::linear);
  return source.sample(linear_sampler, in.texcoord) * alpha;
}
)metal";

  NSError* error = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:source
                                                options:nil
                                                  error:&error];
  if (library == nil) {
    std::fprintf(stderr, "ART Metal Composer: shader compile failed: %s\n",
                 error == nil ? "unknown" : error.localizedDescription.UTF8String);
    return false;
  }
  id<MTLFunction> vertex =
      [library newFunctionWithName:@"darwin_art_composer_vertex"];
  id<MTLFunction> fragment =
      [library newFunctionWithName:@"darwin_art_composer_fragment"];
  MTLRenderPipelineDescriptor* descriptor =
      [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vertex;
  descriptor.fragmentFunction = fragment;
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  descriptor.colorAttachments[0].blendingEnabled = YES;
  descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
  descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
  descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
  descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
  descriptor.colorAttachments[0].destinationRGBBlendFactor =
      MTLBlendFactorOneMinusSourceAlpha;
  descriptor.colorAttachments[0].destinationAlphaBlendFactor =
      MTLBlendFactorOneMinusSourceAlpha;
  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  [descriptor release];
  [fragment release];
  [vertex release];
  [library release];
  if (pipeline == nil) {
    std::fprintf(stderr, "ART Metal Composer: pipeline creation failed: %s\n",
                 error == nil ? "unknown" : error.localizedDescription.UTF8String);
    return false;
  }
  id<MTLCommandQueue> queue = [device newCommandQueue];
  if (queue == nil) {
    [pipeline release];
    return false;
  }
  state.device = [device retain];
  state.queue = queue;
  state.pipeline = pipeline;
  return true;
}

struct Vertex {
  float position[2];
  float texcoord[2];
};

}  // namespace

extern "C" bool darwin_art_metal_composer_compose(
    void* metal_device, void* target_iosurface, uint32_t target_width,
    uint32_t target_height, const DarwinArtMetalComposerLayer* layers,
    size_t layer_count, void* producer_event, uint64_t producer_value,
    void** completion_event, uint64_t* completion_value) {
  if (completion_event == nullptr || completion_value == nullptr) return false;
  *completion_event = nullptr;
  *completion_value = 0;
  if (metal_device == nullptr || target_iosurface == nullptr ||
      target_width == 0 || target_height == 0 ||
      (layer_count != 0 && layers == nullptr)) {
    return false;
  }
  id<MTLDevice> device = reinterpret_cast<id<MTLDevice>>(metal_device);
  auto* target_surface = reinterpret_cast<IOSurfaceRef>(target_iosurface);
  std::lock_guard<std::mutex> lock(ComposerMutex());
  MetalComposerState& state = ComposerState();
  if (!EnsurePipeline(device, state)) return false;

  MTLTextureDescriptor* target_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:target_width
                                                        height:target_height
                                                     mipmapped:NO];
  target_descriptor.storageMode = MTLStorageModeShared;
  target_descriptor.usage = MTLTextureUsageRenderTarget |
                            MTLTextureUsageShaderRead |
                            MTLTextureUsageShaderWrite;
  id<MTLTexture> target =
      [device newTextureWithDescriptor:target_descriptor
                             iosurface:target_surface
                                 plane:0];
  if (target == nil) return false;
  id<MTLCommandBuffer> command_buffer = [state.queue commandBuffer];
  if (command_buffer == nil) {
    [target release];
    return false;
  }
  if (producer_event != nullptr && producer_value != 0) {
    [command_buffer
        encodeWaitForEvent:reinterpret_cast<id<MTLSharedEvent>>(producer_event)
                     value:producer_value];
  }

  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  id<MTLRenderCommandEncoder> encoder =
      [command_buffer renderCommandEncoderWithDescriptor:pass];
  if (encoder == nil) {
    [target release];
    return false;
  }
  [encoder setRenderPipelineState:state.pipeline];

  for (size_t index = 0; index < layer_count; ++index) {
    const DarwinArtMetalComposerLayer& layer = layers[index];
    if (layer.iosurface == nullptr || layer.width == 0 || layer.height == 0 ||
        layer.source_right <= layer.source_left ||
        layer.source_bottom <= layer.source_top ||
        layer.destination_right <= layer.destination_left ||
        layer.destination_bottom <= layer.destination_top) {
      continue;
    }
    MTLTextureDescriptor* source_descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:layer.width
                                                          height:layer.height
                                                       mipmapped:NO];
    source_descriptor.storageMode = MTLStorageModeShared;
    source_descriptor.usage = MTLTextureUsageShaderRead |
                              MTLTextureUsageShaderWrite |
                              MTLTextureUsageRenderTarget;
    id<MTLTexture> source = [device
        newTextureWithDescriptor:source_descriptor
                       iosurface:reinterpret_cast<IOSurfaceRef>(layer.iosurface)
                           plane:0];
    if (source == nil) continue;

    const float left = 2.0f * static_cast<float>(layer.destination_left) /
                           static_cast<float>(target_width) -
                       1.0f;
    const float right =
        2.0f * static_cast<float>(layer.destination_right) /
            static_cast<float>(target_width) -
        1.0f;
    const float top = 1.0f -
                      2.0f * static_cast<float>(layer.destination_top) /
                          static_cast<float>(target_height);
    const float bottom =
        1.0f - 2.0f * static_cast<float>(layer.destination_bottom) /
                   static_cast<float>(target_height);
    const float u0 = static_cast<float>(layer.source_left) / layer.width;
    const float u1 = static_cast<float>(layer.source_right) / layer.width;
    // The IOSurface handed to SurfaceFlinger is ANGLE's display-space
    // AHardwareBuffer. ANGLE has already resolved the guest GL bottom-left
    // convention when it stores the frame, so Metal samples the same
    // top-left source rectangle that Android's transaction carries. Flipping
    // V here would apply a second transform (the Chrome/WebContents layers
    // visibly become upside-down/180-degree mirrored).
    const float v0 = static_cast<float>(layer.source_top) / layer.height;
    const float v1 = static_cast<float>(layer.source_bottom) / layer.height;
    const std::array<Vertex, 6> vertices{{
        {{left, top}, {u0, v0}},
        {{left, bottom}, {u0, v1}},
        {{right, bottom}, {u1, v1}},
        {{left, top}, {u0, v0}},
        {{right, bottom}, {u1, v1}},
        {{right, top}, {u1, v0}},
    }};
    const float alpha = std::clamp(layer.alpha, 0.0f, 1.0f);
    [encoder setVertexBytes:vertices.data()
                     length:sizeof(vertices)
                    atIndex:0];
    [encoder setFragmentTexture:source atIndex:0];
    [encoder setFragmentBytes:&alpha length:sizeof(alpha) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:vertices.size()];
    [source release];
  }
  [encoder endEncoding];

  id<MTLSharedEvent> event = [device newSharedEvent];
  if (event == nil) {
    [target release];
    return false;
  }
  const uint64_t value = event.signaledValue + 1;
  [command_buffer encodeSignalEvent:event value:value];
  [command_buffer commit];
  [target release];
  *completion_event = reinterpret_cast<void*>(event);
  *completion_value = value;
  return true;
}
