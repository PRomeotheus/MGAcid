#pragma once

// A colour grade, as a .cube lookup table.
//
// The grade that was here is a contrast and saturation nudge with two
// constants in it: enough to lift a picture authored for a small dim screen,
// and nothing like what a grade actually is. A lookup table is the form the
// rest of the world uses -- every colour grading tool exports one -- so a
// grade can be made in something that shows you the picture while you turn the
// knobs, saved, and dropped into the data folder.
//
// The file is Adobe's .cube format, which is plain text and universally
// written:
//
//     TITLE "something"          optional
//     LUT_3D_SIZE 33             the cube is 33 x 33 x 33
//     DOMAIN_MIN 0 0 0           optional, defaults to 0 0 0
//     DOMAIN_MAX 1 1 1           optional, defaults to 1 1 1
//     0.0 0.0 0.0                size^3 lines of r g b, red varying fastest
//     ...
//
// 1D tables (LUT_1D_SIZE) are not read. They cannot express a grade that moves
// hues, which is most of what a grade is for, and a file that silently did
// less than it looked like it was doing would be worse than one that is
// refused.
//
// Sampling is left to the hardware: a 3D texture with a linear filter is
// exactly a trilinear lookup, which is what a .cube is meant to be read with.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mga::gpu {

class ColourLut {
public:
    // Reads a .cube. False with `error` set when the file is missing or not
    // one; the table is left empty and the caller grades with nothing.
    bool load(const std::filesystem::path &path, std::string &error);

    // An identity table of this size, for when there is no file: sampling it
    // returns the colour that went in, so the same shader path can run with
    // and without a grade and the only difference is the numbers.
    void make_identity(std::uint32_t size);

    [[nodiscard]] bool empty() const noexcept { return texels_.empty(); }
    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    // RGBA8, size^3 of them, red varying fastest: the order a 3D texture
    // upload wants.
    [[nodiscard]] const std::vector<std::uint32_t> &texels() const noexcept { return texels_; }

private:
    std::uint32_t size_{};
    std::vector<std::uint32_t> texels_;
};

} // namespace mga::gpu
