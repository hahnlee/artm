#pragma once

#include "metal_composer.h"

#include <cstddef>
#include <cstdint>

// Starts the profile-scoped SurfaceFlinger endpoint in android.system. It is
// idempotent and returns only after the listening socket is published.
extern "C" bool darwin_art_surfaceflinger_service_start();

// Sends one process's complete retained layer snapshot to the central
// SurfaceFlinger service. Producer readiness and central Metal composition
// completion cross the process boundary as Android fence descriptors. Metal
// shared events remain the GPU-queue primitives on both sides; the returned
// asynchronous descriptor is owned by the caller.
extern "C" int darwin_art_surfaceflinger_service_present(
    uint32_t target_iosurface_id, uint32_t target_width,
    uint32_t target_height, uint64_t transaction_id,
    const DarwinArtMetalComposerLayer* layers, size_t layer_count,
    void* producer_event, uint64_t producer_value);
