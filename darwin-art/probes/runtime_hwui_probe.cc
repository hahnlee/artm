#include "runtime_hwui_probe.h"

#if defined(DARWIN_ART_REAL_GRAPHICS)

#include <algorithm>
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

}  // namespace darwin_art_hwui

#endif  // defined(DARWIN_ART_REAL_GRAPHICS)
