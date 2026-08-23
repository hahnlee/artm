#pragma once

#include <cstddef>
#include <chrono>

#include <jni.h>

struct DarwinArtSurface;

#if defined(DARWIN_ART_REAL_GRAPHICS)

namespace android::uirenderer {
class AnimationContext;
class RenderNode;
}  // namespace android::uirenderer

namespace darwin_art_hwui {

void hide_skottie_backing_views(JNIEnv* env, jobject root_view);

size_t sync_recorded_render_node_tree(android::uirenderer::RenderNode* node);

void animate_node_with_context(android::uirenderer::RenderNode* node,
                               android::uirenderer::AnimationContext& context);

bool node_subtree_has_animators(android::uirenderer::RenderNode* node);

void register_node_subtree_animators(android::uirenderer::RenderNode* node,
                                     android::uirenderer::AnimationContext& context);

bool render_node_to_surface(
    JNIEnv* env, jobject root_view, jobject render_node, DarwinArtSurface* surface, jint width,
    jint height, bool overlay_active, jfloat overlay_x, jfloat overlay_y,
    std::chrono::steady_clock::time_point overlay_started);

}  // namespace darwin_art_hwui

#endif  // defined(DARWIN_ART_REAL_GRAPHICS)
