#pragma once

#include <cstddef>

#if defined(DARWIN_ART_REAL_GRAPHICS)

namespace android::uirenderer {
class AnimationContext;
class RenderNode;
}  // namespace android::uirenderer

namespace darwin_art_hwui {

size_t sync_recorded_render_node_tree(android::uirenderer::RenderNode* node);

void animate_node_with_context(android::uirenderer::RenderNode* node,
                               android::uirenderer::AnimationContext& context);

bool node_subtree_has_animators(android::uirenderer::RenderNode* node);

void register_node_subtree_animators(android::uirenderer::RenderNode* node,
                                     android::uirenderer::AnimationContext& context);

}  // namespace darwin_art_hwui

#endif  // defined(DARWIN_ART_REAL_GRAPHICS)
