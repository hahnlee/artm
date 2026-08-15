#pragma once

#include <cstddef>
#include <cstdint>

// Presents Android-format 0xAARRGGBB pixels in a native AppKit window. A zero
// duration validates the frame ABI without creating visible UI.
bool DarwinPresentArgb(const std::uint32_t* pixels,
                       std::size_t width,
                       std::size_t height,
                       double visible_seconds);
