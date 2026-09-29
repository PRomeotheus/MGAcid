#include "texture_decode.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace mga::gpu {
namespace {

std::uint32_t expand_5650(std::uint16_t value) {
    const std::uint32_t r = (value & 0x1Fu) * 255u / 31u;
    const std::uint32_t g = ((value >> 5u) & 0x3Fu) * 255u / 63u;
    const std::uint32_t b = ((value >> 11u) & 0x1Fu) * 255u / 31u;
    return 0xFF000000u | (b << 16u) | (g << 8u) | r;
}

std::uint32_t expand_5551(std::uint16_t value) {
    const std::uint32_t r = (value & 0x1Fu) * 255u / 31u;
    const std::uint32_t g = ((value >> 5u) & 0x1Fu) * 255u / 31u;
    const std::uint32_t b = ((value >> 10u) & 0x1Fu) * 255u / 31u;
    const std::uint32_t a = ((value >> 15u) & 1u) * 255u;
    return (a << 24u) | (b << 16u) | (g << 8u) | r;
}

std::uint32_t expand_4444(std::uint16_t value) {
    const std::uint32_t r = (value & 0xFu) * 17u;
    const std::uint32_t g = ((value >> 4u) & 0xFu) * 17u;
    const std::uint32_t b = ((value >> 8u) & 0xFu) * 17u;
    const std::uint32_t a = ((value >> 12u) & 0xFu) * 17u;
    return (a << 24u) | (b << 16u) | (g << 8u) | r;
}

// Bits per texel for the format, or 0 for the block-compressed ones.
std::uint32_t bits_per_texel(TextureFormat format) {
    switch (format) {
    case TextureFormat::Clut4: return 4u;
    case TextureFormat::Clut8: return 8u;
    case TextureFormat::Rgba5650:
    case TextureFormat::Rgba5551:
    case TextureFormat::Rgba4444:
    case TextureFormat::Clut16: return 16u;
    case TextureFormat::Rgba8888:
    case TextureFormat::Clut32: return 32u;
    default: return 0u;
    }
}

// Swizzled textures are stored as 16-byte wide, 8-row blocks.
void unswizzle(std::vector<std::uint8_t> &data, std::uint32_t row_bytes, std::uint32_t rows) {
    if (row_bytes % 16u != 0u || rows % 8u != 0u) return;
    std::vector<std::uint8_t> source = data;
    const std::uint32_t block_columns = row_bytes / 16u;
    std::size_t offset = 0u;
    for (std::uint32_t block_row = 0; block_row < rows / 8u; ++block_row) {
        for (std::uint32_t block_column = 0; block_column < block_columns; ++block_column) {
            for (std::uint32_t row = 0; row < 8u; ++row) {
                const std::size_t destination = static_cast<std::size_t>(block_row * 8u + row) * row_bytes +
                                                static_cast<std::size_t>(block_column) * 16u;
                if (offset + 16u > source.size() || destination + 16u > data.size()) return;
                std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(offset), 16u,
                            data.begin() + static_cast<std::ptrdiff_t>(destination));
                offset += 16u;
            }
        }
    }
}

std::uint32_t read_clut(const GuestMemory &memory, const TextureState &texture, std::uint32_t index) {
    const std::uint32_t entry = ((index >> texture.clut_shift) & texture.clut_mask) | texture.clut_offset << 4u;
    switch (texture.clut_format) {
    case 0u: return expand_5650(memory.load16(texture.clut_address + entry * 2u));
    case 1u: return expand_5551(memory.load16(texture.clut_address + entry * 2u));
    case 2u: return expand_4444(memory.load16(texture.clut_address + entry * 2u));
    default: return memory.load32(texture.clut_address + entry * 4u);
    }
}

// A DXT endpoint colour: RGB565 with red in the top bits, the block formats'
// own order, unlike the GE's 5650 texels, which keep red in the low bits.
std::uint32_t expand_dxt_565(std::uint16_t value) {
    const std::uint32_t r = ((value >> 11u) & 0x1Fu) * 255u / 31u;
    const std::uint32_t g = ((value >> 5u) & 0x3Fu) * 255u / 63u;
    const std::uint32_t b = (value & 0x1Fu) * 255u / 31u;
    return (b << 16u) | (g << 8u) | r;
}

// One 4x4 block as the PSP stores it, which is not the PC's DXT layout:
//
//   colour part, 8 bytes:  4 bytes of 2-bit indices, one byte per row with the
//                          leftmost texel in the low bits, then the two
//                          RGB565 endpoints
//   DXT3, 8 more bytes:    4-bit alphas, one 16-bit word per row
//   DXT5, 8 more bytes:    48 bits of 3-bit alpha indices, then the two
//                          alpha endpoints
//
// The colour part comes first and the alpha part after it, and within the
// colour part the indices come before the endpoints. Read as PC blocks, with
// the endpoints first, a DXT texture comes out as coloured 4x4 noise.
//
// Taken from upstream (TeamGDB/Yakumo 453f843), where it was traced against
// Monster Hunter's own DXT1 textures. Metal Gear Ac!d may draw no DXT texture
// at all -- every one of the 9,933 in the .qar archives is CLUT4 or CLUT8 --
// so this may be fixing something the game never reaches. It is still the
// right layout, and a decoder that is wrong only where nothing looks is worse
// than one that is right, because the day something does look, nobody thinks
// of the decoder.
void decode_dxt_block(const std::uint8_t *block, TextureFormat format, std::uint32_t *out, std::uint32_t stride) {
    const std::uint8_t *indices = block;
    const std::uint8_t *alphas = block + 8u;
    const auto color0 = static_cast<std::uint16_t>(block[4] | (block[5] << 8));
    const auto color1 = static_cast<std::uint16_t>(block[6] | (block[7] << 8));
    std::array<std::uint32_t, 4> palette{};
    palette[0] = expand_dxt_565(color0);
    palette[1] = expand_dxt_565(color1);
    const auto lerp = [](std::uint32_t a, std::uint32_t b, std::uint32_t numerator, std::uint32_t denominator) {
        std::uint32_t result = 0u;
        for (std::uint32_t shift = 0; shift < 24u; shift += 8u) {
            const std::uint32_t left = (a >> shift) & 0xFFu;
            const std::uint32_t right = (b >> shift) & 0xFFu;
            result |= (((left * (denominator - numerator)) + right * numerator) / denominator) << shift;
        }
        return result;
    };
    const bool one_bit_alpha = format == TextureFormat::Dxt1 && color0 <= color1;
    palette[2] = one_bit_alpha ? lerp(palette[0], palette[1], 1u, 2u) : lerp(palette[0], palette[1], 1u, 3u);
    palette[3] = one_bit_alpha ? 0u : lerp(palette[0], palette[1], 2u, 3u);

    for (std::uint32_t row = 0; row < 4u; ++row) {
        const std::uint8_t bits = indices[row];
        for (std::uint32_t column = 0; column < 4u; ++column) {
            const std::uint32_t selector = (bits >> (column * 2u)) & 3u;
            std::uint32_t alpha = 0xFFu;
            if (format == TextureFormat::Dxt1) {
                if (one_bit_alpha && selector == 3u) alpha = 0u;
            } else if (format == TextureFormat::Dxt3) {
                const std::uint32_t nibble_index = row * 4u + column;
                const std::uint8_t byte = alphas[nibble_index / 2u];
                alpha = ((nibble_index & 1u) != 0u ? (byte >> 4u) : (byte & 0xFu)) * 17u;
            } else {  // DXT5
                const std::uint32_t alpha0 = alphas[6];
                const std::uint32_t alpha1 = alphas[7];
                std::uint64_t codes = 0u;
                for (std::uint32_t i = 0; i < 6u; ++i) codes |= static_cast<std::uint64_t>(alphas[i]) << (i * 8u);
                const std::uint32_t code = static_cast<std::uint32_t>((codes >> ((row * 4u + column) * 3u)) & 7u);
                if (code == 0u) alpha = alpha0;
                else if (code == 1u) alpha = alpha1;
                else if (alpha0 > alpha1) alpha = ((8u - code) * alpha0 + (code - 1u) * alpha1) / 7u;
                else if (code < 6u) alpha = ((6u - code) * alpha0 + (code - 1u) * alpha1) / 5u;
                else alpha = code == 6u ? 0u : 255u;
            }
            out[row * stride + column] = (palette[selector] & 0x00FFFFFFu) | (alpha << 24u);
        }
    }
}

} // namespace

bool decode_texture(const GuestMemory &memory, const TextureState &texture, std::vector<std::uint32_t> &out) {
    const std::uint32_t width = texture.width;
    const std::uint32_t height = texture.height;
    if (width == 0u || height == 0u || width > 512u || height > 512u) return false;
    out.assign(static_cast<std::size_t>(width) * height, 0xFF000000u);

    if (texture.format == TextureFormat::Dxt1 || texture.format == TextureFormat::Dxt3 ||
        texture.format == TextureFormat::Dxt5) {
        const std::uint32_t block_bytes = texture.format == TextureFormat::Dxt1 ? 8u : 16u;
        const std::uint32_t blocks_x = (width + 3u) / 4u;
        const std::uint32_t blocks_y = (height + 3u) / 4u;
        std::array<std::uint8_t, 16> block{};
        std::array<std::uint32_t, 16> texels{};
        for (std::uint32_t by = 0; by < blocks_y; ++by) {
            for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
                const std::uint32_t address = texture.address + (by * blocks_x + bx) * block_bytes;
                if (!memory.contains(address, block_bytes)) return false;
                for (std::uint32_t i = 0; i < block_bytes; ++i) block[i] = memory.load8(address + i);
                decode_dxt_block(block.data(), texture.format, texels.data(), 4u);
                for (std::uint32_t row = 0; row < 4u; ++row) {
                    for (std::uint32_t column = 0; column < 4u; ++column) {
                        const std::uint32_t x = bx * 4u + column;
                        const std::uint32_t y = by * 4u + row;
                        if (x < width && y < height) out[static_cast<std::size_t>(y) * width + x] = texels[row * 4u + column];
                    }
                }
            }
        }
        return true;
    }

    const std::uint32_t bits = bits_per_texel(texture.format);
    if (bits == 0u) return false;
    // The row stride is the texture buffer width, which is independent of the
    // sampled size: a 512x512 texture can live in a 480-texel-wide buffer, and
    // sampling past the stride wraps into the next row exactly as on hardware.
    const std::uint32_t stride_texels = texture.buffer_width != 0u ? texture.buffer_width : width;
    const std::uint32_t row_bytes = stride_texels * bits / 8u;
    const std::size_t total = static_cast<std::size_t>(row_bytes) * height;
    if (total == 0u || !memory.contains(texture.address, total)) return false;

    std::vector<std::uint8_t> data(total);
    for (std::size_t i = 0; i < total; ++i) data[i] = memory.load8(texture.address + static_cast<std::uint32_t>(i));
    if (texture.swizzled) unswizzle(data, row_bytes, height);

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t row_offset = static_cast<std::size_t>(y) * row_bytes;
            (void)stride_texels;
            std::uint32_t color = 0xFF000000u;
            switch (texture.format) {
            case TextureFormat::Rgba5650:
            case TextureFormat::Rgba5551:
            case TextureFormat::Rgba4444: {
                const std::size_t at = row_offset + static_cast<std::size_t>(x) * 2u;
                if (at + 1u >= data.size()) break;
                const auto value = static_cast<std::uint16_t>(data[at] | (data[at + 1u] << 8));
                color = texture.format == TextureFormat::Rgba5650 ? expand_5650(value)
                      : texture.format == TextureFormat::Rgba5551 ? expand_5551(value)
                                                                  : expand_4444(value);
                break;
            }
            case TextureFormat::Rgba8888: {
                const std::size_t at = row_offset + static_cast<std::size_t>(x) * 4u;
                if (at + 3u >= data.size()) break;
                color = static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1u]) << 8u) |
                        (static_cast<std::uint32_t>(data[at + 2u]) << 16u) |
                        (static_cast<std::uint32_t>(data[at + 3u]) << 24u);
                break;
            }
            case TextureFormat::Clut4: {
                const std::size_t at = row_offset + static_cast<std::size_t>(x) / 2u;
                if (at >= data.size()) break;
                const std::uint32_t index = (x & 1u) != 0u ? (data[at] >> 4u) : (data[at] & 0xFu);
                color = read_clut(memory, texture, index);
                break;
            }
            case TextureFormat::Clut8: {
                const std::size_t at = row_offset + x;
                if (at >= data.size()) break;
                color = read_clut(memory, texture, data[at]);
                break;
            }
            case TextureFormat::Clut16: {
                const std::size_t at = row_offset + static_cast<std::size_t>(x) * 2u;
                if (at + 1u >= data.size()) break;
                color = read_clut(memory, texture, static_cast<std::uint32_t>(data[at] | (data[at + 1u] << 8)));
                break;
            }
            case TextureFormat::Clut32: {
                const std::size_t at = row_offset + static_cast<std::size_t>(x) * 4u;
                if (at + 3u >= data.size()) break;
                color = read_clut(memory, texture,
                                  static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1u]) << 8u) |
                                      (static_cast<std::uint32_t>(data[at + 2u]) << 16u) |
                                      (static_cast<std::uint32_t>(data[at + 3u]) << 24u));
                break;
            }
            default:
                break;
            }
            out[static_cast<std::size_t>(y) * width + x] = color;
        }
    }
    return true;
}

std::uint64_t content_key(std::uint32_t width, std::uint32_t height, const std::uint32_t *texels) {
    // Plain FNV-1a over the bytes, one at a time. A wider mix would be faster,
    // but this runs once per texture upload rather than once per draw, and
    // being trivial to reimplement matters more here than speed: the extractor
    // has to arrive at the same number from Python, and it can only do that if
    // there is nothing here to get subtly wrong.
    std::uint64_t key = 0xCBF29CE484222325ull;
    const auto mix = [&key](std::uint32_t value) {
        for (int byte = 0; byte < 4; ++byte) {
            key ^= static_cast<std::uint8_t>(value >> (byte * 8));
            key *= 0x100000001B3ull;
        }
    };
    mix(width);
    mix(height);
    const std::size_t count = static_cast<std::size_t>(width) * height;
    for (std::size_t i = 0; i < count; ++i) mix(texels[i]);
    return key;
}

// Textures up to this size are keyed by all of their contents, larger ones by
// one word in every 256 bytes.
constexpr std::uint32_t kFullKeyBytes = 64u * 1024u;

std::uint64_t texture_key(const GuestMemory &memory, const TextureState &texture) {
    // Address, layout and a sample of the contents: guest textures are often
    // rewritten in place, so the key has to notice changed pixels.
    std::uint64_t key = 0xCBF29CE484222325ull;
    const auto mix = [&key](std::uint64_t value) {
        key ^= value;
        key *= 0x100000001B3ull;
    };
    mix(texture.address);
    mix(static_cast<std::uint64_t>(texture.width) << 16u | texture.height);
    mix(static_cast<std::uint64_t>(texture.format));
    mix(texture.buffer_width);
    mix(texture.swizzled ? 1u : 0u);
    // The palette only for the formats that read one. A direct colour or block
    // texture is drawn with whatever CLUT address the game happened to leave
    // set, and upstream found theirs left at the framebuffer -- whose first
    // word changes every frame, so the key changed every frame and the texture
    // was decoded and uploaded again each time it was shown.
    const bool indexed = texture.format == TextureFormat::Clut4 || texture.format == TextureFormat::Clut8 ||
                         texture.format == TextureFormat::Clut16 || texture.format == TextureFormat::Clut32;
    if (indexed) {
        mix(texture.clut_address);
        mix(texture.clut_format);
        // How the palette is indexed, which is as much a part of what this
        // texture looks like as the palette itself: offset picks a sixteen-
        // entry block, so the same indices through offset 0 and offset 1 are
        // two different pictures. Without these in the key they were one
        // entry in the cache.
        mix(texture.clut_shift);
        mix(texture.clut_mask);
        mix(texture.clut_offset);
    }
    const std::uint32_t bits = bits_per_texel(texture.format);
    // DXT1 blocks hold half a byte per texel, DXT3 and DXT5 blocks a byte.
    const std::uint32_t size = bits != 0u                              ? texture.width * texture.height * bits / 8u
                               : texture.format == TextureFormat::Dxt1 ? texture.width * texture.height / 2u
                                                                       : texture.width * texture.height;
    // Resolve the texture once; this runs for every textured draw.
    if (const std::uint8_t *data = memory.raw_pointer(texture.address, static_cast<std::size_t>(size) + 3u)) {
        // MGA_SAMPLED_TEXTURE_KEYS=1 samples small textures too, as before.
        static const bool sampled = std::getenv("MGA_SAMPLED_TEXTURE_KEYS") != nullptr;
        if (size <= kFullKeyBytes && !sampled) {
            // Small textures are read whole: the game's text atlas gains one
            // glyph at a time, and a sample misses most of them. Four
            // independent lanes keep this cheap.
            std::uint64_t lanes[4] = {key, key ^ 0x9E3779B97F4A7C15ull, key ^ 0xC2B2AE3D27D4EB4Full,
                                      key ^ 0x165667B19E3779F9ull};
            std::uint32_t offset = 0;
            for (; offset + 32u <= size; offset += 32u) {
                for (int lane = 0; lane < 4; ++lane) {
                    std::uint64_t word{};
                    std::memcpy(&word, data + offset + static_cast<std::uint32_t>(lane) * 8u, sizeof(word));
                    lanes[lane] = (lanes[lane] ^ word) * 0x100000001B3ull;
                }
            }
            for (const std::uint64_t lane : lanes) mix(lane);
            for (; offset < size; ++offset) mix(data[offset]);
        } else {
            for (std::uint32_t offset = 0; offset < size; offset += 256u) {
                std::uint32_t word{};
                std::memcpy(&word, data + offset, sizeof(word));
                mix(word);
            }
        }
    } else {
        for (std::uint32_t offset = 0; offset < size; offset += 256u) {
            if (!memory.contains(texture.address + offset, 4u)) break;
            mix(memory.load32(texture.address + offset));
        }
    }
    // The palette's contents, and not just its first word.
    //
    // Only the first word used to go in, and for a paletted texture the
    // palette IS most of the picture: the pixels are indices and say nothing
    // about what colour anything is. Two textures that share index data at one
    // address and differ only in their palette -- which is how a console game
    // gets two versions of a panel out of one image -- came out with identical
    // keys, so the cache returned whichever it had decoded first, with the
    // wrong palette baked into it. On screen that is one surface showing two
    // different pictures, swapping as the cache evicts and re-decodes it.
    // Entry zero is very often transparent or black in both variants, which is
    // exactly the entry the old sample looked at.
    //
    // Only the entries this texture can actually reach are read: read_clut()
    // forms an entry as ((index >> shift) & mask) | offset << 4, so the
    // largest it can produce is mask | offset << 4. The hardware's palette
    // memory is a kilobyte, and a mask asking for more than that is malformed
    // rather than a palette, so it is clamped. This is memoised for the
    // display list being walked, so it is paid once per texture per list
    // rather than once per draw.
    if (indexed && texture.clut_address != 0u) {
        const std::uint32_t entry_bytes = texture.clut_format == 3u ? 4u : 2u;
        // The largest entry this texture can actually reach, which is
        // read_clut()'s own formula applied to the largest index the format
        // can hold. Bounding it by the mask alone was wrong and badly so: a
        // sixteen-colour texture has a thirty-two byte palette, but mask and
        // offset together can claim five hundred entries, so a kilobyte of
        // whatever happened to follow the palette went into the key. That
        // memory changes, so the key changed every frame, every such texture
        // missed every frame, and the cache spent itself re-decoding things
        // that had not changed -- which is geometry dropping out, not just
        // textures flickering.
        const std::uint32_t index_bits = bits_per_texel(texture.format);
        const std::uint32_t max_index = index_bits >= 8u || index_bits == 0u ? 255u : (1u << index_bits) - 1u;
        const std::uint32_t last_entry =
            ((max_index >> texture.clut_shift) & texture.clut_mask) | (texture.clut_offset << 4u);
        const std::uint32_t clut_bytes = std::min((last_entry + 1u) * entry_bytes, 1024u);
        std::uint32_t offset = 0u;
        for (; offset + 4u <= clut_bytes; offset += 4u) {
            if (!memory.contains(texture.clut_address + offset, 4u)) break;
            mix(memory.load32(texture.clut_address + offset));
        }
        if (offset + 2u <= clut_bytes && memory.contains(texture.clut_address + offset, 2u))
            mix(memory.load16(texture.clut_address + offset));
    }
    return key;
}

} // namespace mga::gpu
