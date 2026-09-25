#pragma once

#include "ge_state.hpp"

#include <cstdint>
#include <vector>

namespace mga::gpu {

// Decodes a PSP texture into RGBA8888 (one std::uint32_t per texel, red in the
// low byte). Handles the direct colour formats, 4/8/16/32-bit palettes, DXT1/3/5
// and the swizzled layout. Returns false when the texture cannot be read.
bool decode_texture(const GuestMemory &memory, const TextureState &texture, std::vector<std::uint32_t> &out);

// Key that identifies the decoded contents of a texture for caching.
[[nodiscard]] std::uint64_t texture_key(const GuestMemory &memory, const TextureState &texture);

// Key that identifies a texture by its decoded pixels alone, for matching one
// against a replacement built offline. Unlike texture_key this mixes in no
// address, no layout and no palette, only the size and the RGBA8888 texels, so
// the same art keys the same however the game stored it: swizzled or not,
// deflated or not, whatever palette it was written with. tools/qar.py computes
// the identical value over the PNGs it extracts, and the two agreeing is the
// entire point, so neither side can change without the other.
[[nodiscard]] std::uint64_t content_key(std::uint32_t width, std::uint32_t height,
                                        const std::uint32_t *texels);

} // namespace mga::gpu
