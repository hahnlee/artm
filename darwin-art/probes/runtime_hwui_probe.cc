#include "runtime_hwui_probe.h"

#if defined(DARWIN_ART_REAL_GRAPHICS)

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#ifdef HIDDEN
#undef HIDDEN
#endif
#include "hwui/Canvas.h"
#define private public
#define protected public
#include "AnimationContext.h"
#include "Animator.h"
#include "AnimatorManager.h"
#include "RenderNode.h"
#undef protected
#undef private
#include "darwin_surface_bridge.h"
#include "pipeline/skia/RenderNodeDrawable.h"
#include "runtime_frame_probe.h"

namespace {

class DarwinHwuiTreeObserver final : public android::uirenderer::TreeObserver {
 public:
  void onMaybeRemovedFromTree(android::uirenderer::RenderNode* node) override {
    node->onRemovedFromTree(nullptr);
  }
};

}  // namespace

namespace darwin_art_hwui {

size_t sync_recorded_render_node_tree(android::uirenderer::RenderNode* node) {
  DarwinHwuiTreeObserver observer;
  if (node == nullptr) return 0;
  size_t synchronized = 0;
  if (node->mDirtyPropertyFields != 0) {
    node->mDirtyPropertyFields = 0;
    node->syncProperties();
  }
  if (node->mNeedsDisplayListSync) {
    node->mNeedsDisplayListSync = false;
    node->syncDisplayList(observer, nullptr);
    ++synchronized;
  }
  if (node->mDisplayList) {
    node->mDisplayList.updateChildren(
        [&](android::uirenderer::RenderNode* child) {
          synchronized += sync_recorded_render_node_tree(child);
        });
  }
  return synchronized;
}

void animate_node_with_context(android::uirenderer::RenderNode* node,
                               android::uirenderer::AnimationContext& context) {
  if (node == nullptr) return;
  auto& manager = node->mAnimatorManager;
  const bool debug_animation = std::getenv("DARWIN_ART_DEBUG_ANIMATION") != nullptr;
  if (debug_animation) {
    std::cerr << "ART HWUI animation pulse node=" << node
              << " new=" << manager.mNewAnimators.size()
              << " active=" << manager.mAnimators.size()
              << " handle=" << manager.mAnimationHandle
              << " frame_ms=" << context.frameTimeMs() << "\n";
  }
  if (manager.mAnimationHandle == nullptr) return;
  manager.pushStaging();
  auto new_end = std::remove_if(
      manager.mAnimators.begin(), manager.mAnimators.end(),
      [&context](android::sp<android::uirenderer::BaseRenderNodeAnimator>& animator) {
        const bool finished = animator->animate(context);
        if (finished) animator->detach();
        return finished;
      });
  manager.mAnimators.erase(new_end, manager.mAnimators.end());
  auto* handle = manager.mAnimationHandle;
  node->mProperties.updateMatrix();
  handle->notifyAnimationsRan();
  if (debug_animation) {
    std::cerr << "ART HWUI animation pulse result active="
              << manager.mAnimators.size() << " handle="
              << manager.mAnimationHandle << "\n";
    for (const auto& animator : manager.mAnimators) {
      std::cerr << "ART HWUI animator duration=" << animator->duration()
                << " remaining=" << animator->getRemainingPlayTime()
                << " final=" << animator->finalValue() << "\n";
    }
  }
}

bool node_subtree_has_animators(android::uirenderer::RenderNode* node) {
  if (node == nullptr) return false;
  if (!node->mAnimatorManager.mNewAnimators.empty() ||
      !node->mAnimatorManager.mAnimators.empty()) {
    return true;
  }
  bool found = false;
  if (node->mDisplayList) {
    node->mDisplayList.updateChildren(
        [&](android::uirenderer::RenderNode* child) {
          found = found || node_subtree_has_animators(child);
        });
  }
  return found;
}

void register_node_subtree_animators(
    android::uirenderer::RenderNode* node,
    android::uirenderer::AnimationContext& context) {
  if (node == nullptr) return;
  if ((!node->mAnimatorManager.mNewAnimators.empty() ||
       !node->mAnimatorManager.mAnimators.empty()) &&
      !node->animators().hasAnimationHandle()) {
    context.addAnimatingRenderNode(*node);
  }
  if (node->mDisplayList) {
    node->mDisplayList.updateChildren(
        [&](android::uirenderer::RenderNode* child) {
          register_node_subtree_animators(child, context);
        });
  }
}

bool render_node_to_surface(
    JNIEnv* env, jobject render_node, DarwinArtSurface* surface, jint width,
    jint height, bool overlay_active, jfloat overlay_x, jfloat overlay_y,
    std::chrono::steady_clock::time_point overlay_started) {
  if (env == nullptr || render_node == nullptr || surface == nullptr) {
    return false;
  }
  jclass render_node_class = env->FindClass("android/graphics/RenderNode");
  jfieldID native_render_node =
      render_node_class == nullptr
          ? nullptr
          : env->GetFieldID(render_node_class, "mNativeRenderNode", "J");
  auto* node = native_render_node == nullptr
                   ? nullptr
                   : reinterpret_cast<android::uirenderer::RenderNode*>(
                         static_cast<std::uintptr_t>(env->GetLongField(
                             render_node, native_render_node)));
  if (node == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    env->DeleteLocalRef(render_node_class);
    return false;
  }
  DarwinArtGpuFrame* frame = darwin_art_surface_gpu_begin(surface);
  if (frame == nullptr) {
    env->DeleteLocalRef(render_node_class);
    return false;
  }
  auto* canvas = static_cast<SkCanvas*>(darwin_art_surface_gpu_canvas(frame));
  if (canvas == nullptr) {
    darwin_art_surface_gpu_end(surface, frame);
    env->DeleteLocalRef(render_node_class);
    return false;
  }
  canvas->clear(SK_ColorTRANSPARENT);
  android::uirenderer::skiapipeline::RenderNodeDrawable drawable(
      node, canvas, false);
  drawable.forceDraw(canvas);
  if (overlay_active) {
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() -
                                  overlay_started)
                                  .count();
    const float progress = std::clamp(static_cast<float>(elapsed_ms / 2200.0),
                                      0.0f, 1.0f);
    SkPaint ripple_paint;
    ripple_paint.setAntiAlias(true);
    ripple_paint.setColor(SkColorSetARGB(
        static_cast<U8CPU>(24.0f + (1.0f - progress) * 64.0f), 30, 30, 30));
    canvas->save();
    canvas->clipRect(SkRect::MakeLTRB(104.0f, 298.0f, 256.0f, 342.0f));
    canvas->drawCircle(overlay_x, overlay_y, 8.0f + progress * 76.0f,
                       ripple_paint);
    canvas->restore();
  }
  const DarwinArtSurfaceResult result =
      darwin_art_surface_gpu_end(surface, frame);
  env->DeleteLocalRef(render_node_class);
  if (result != DARWIN_ART_SURFACE_OK) return false;
  darwin_art_frame_probe::record_dimensions(width, height);
  return true;
}

}  // namespace darwin_art_hwui

#endif  // defined(DARWIN_ART_REAL_GRAPHICS)
