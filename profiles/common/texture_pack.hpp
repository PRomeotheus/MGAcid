#pragma once

// Replacement textures, and the dumps that let someone make them.
//
// The renderer already gives every guest texture a 64-bit key, derived from its
// bytes rather than its address, so the same image gets the same key wherever
// the game happens to have loaded it. That key is all a texture pack needs:
//
//   <data>/textures/<key>.png        a replacement, loaded in place of the
//                                    guest's own texture
//   <data>/textures/dump/<key>.png   what the guest actually drew, written out
//                                    when dumping is on
//   <data>/textures/dump/index.txt   one line per dump: key, size, and when it
//                                    was first seen
//
// A dump and a replacement share the same name deliberately: the workflow is to
// dump, enlarge or repaint the file in an editor, and drop it into the parent
// folder. Nothing has to be renamed and no manifest has to be edited.
//
// A replacement may be any size -- that is the point of it -- and it overrides
// the renderer's own upscaling, which would otherwise enlarge an image someone
// has already drawn at a higher resolution.
//
// PNG is read and written through libavcodec, which the port already links for
// the movie player, so this costs no new dependency. Without it the class still
// compiles and simply reports that nothing is available.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace psp::gpu {

// RGBA8, in the order the renderer uploads: one texel per uint32, red in the
// lowest byte.
struct PackedTexture {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint32_t> pixels;
};

class TexturePack {
public:
    // Points the pack at a data directory. Replacements are looked for under
    // `<root>/textures`, dumps written to `<root>/textures/dump`. Loading is
    // lazy: this only notes where to look and whether the folder exists.
    void open(const std::filesystem::path &root);

    // True when a textures folder exists with at least one file in it.
    [[nodiscard]] bool available() const noexcept { return available_; }
    // True when PNG can be read and written at all.
    [[nodiscard]] static bool supported();

    // The replacement for this texture, or null. The result is cached, missing
    // files included, so a texture the game uses every frame costs one failed
    // lookup in total rather than one per frame.
    [[nodiscard]] const PackedTexture *find(std::uint64_t key);

    // Turns dumping on. Every texture decoded from then on is written once.
    void set_dumping(bool on);
    [[nodiscard]] bool dumping() const noexcept { return dumping_; }

    // Writes one texture out, if dumping is on and this key has not been
    // written yet. `pixels` is RGBA8 as above.
    void dump(std::uint64_t key, std::uint32_t width, std::uint32_t height, const std::uint32_t *pixels);

    // How many replacements have been loaded and how many dumps written, for
    // the interface to report.
    [[nodiscard]] std::size_t loaded() const noexcept { return loaded_; }
    [[nodiscard]] std::size_t dumped() const noexcept { return dumped_.size(); }

    // The key as it appears in a file name: sixteen lowercase hex digits.
    [[nodiscard]] static std::string key_name(std::uint64_t key);

private:
    std::filesystem::path replacements_;
    std::filesystem::path dumps_;
    bool available_{};
    bool dumping_{};
    std::size_t loaded_{};
    // Absent value means "looked and found nothing", which is what stops the
    // lookup being repeated every frame.
    std::map<std::uint64_t, std::optional<PackedTexture>> cache_;
    std::map<std::uint64_t, bool> dumped_;
};

} // namespace psp::gpu
