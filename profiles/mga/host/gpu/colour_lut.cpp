#include "colour_lut.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <string_view>

namespace mga::gpu {
namespace {

// 2 is the smallest cube that is a cube at all. 64 is where a table stops
// being worth its memory: 64^3 texels at four bytes is a megabyte, and the
// grades people actually export are 17, 25, 32 or 33 a side, where trilinear
// interpolation between the entries is doing most of the work anyway.
constexpr std::uint32_t kMinSize = 2;
constexpr std::uint32_t kMaxSize = 64;

std::string_view trim(std::string_view text) {
    // A .cube may carry # comments; they are not part of any value.
    const auto hash = text.find('#');
    if (hash != std::string_view::npos) text = text.substr(0, hash);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

// The next whitespace-separated word, consumed from `text`.
std::string_view word(std::string_view &text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    std::size_t end = 0;
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
    const std::string_view result = text.substr(0, end);
    text.remove_prefix(end);
    return result;
}

bool parse_float(std::string_view text, float &out) {
    if (text.empty()) return false;
    // from_chars does not accept a leading '+', which some writers emit.
    if (text.front() == '+') text.remove_prefix(1);
    const auto *first = text.data();
    const auto *last = first + text.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr == last && std::isfinite(out);
}

bool parse_uint(std::string_view text, std::uint32_t &out) {
    const auto *first = text.data();
    const auto *last = first + text.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr == last;
}

bool equals_ignoring_case(std::string_view a, const char *b) {
    std::string_view other{b};
    if (a.size() != other.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(other[i])))
            return false;
    }
    return true;
}

std::uint32_t pack(float r, float g, float b) {
    // RGBA8 with red in the low byte, which is what VK_FORMAT_R8G8B8A8_UNORM
    // reads on a little-endian host.
    //
    // A .cube may hold values above 1 or below 0 -- the format allows it, and
    // grades meant for a log or scene-referred pipeline use it. Eight bits
    // cannot, so they clamp here. That is a real loss and it is silent, but
    // the alternative is refusing every HDR-authored file, and the clamped
    // result is still the grade over the range this renderer works in, which
    // is display values between black and white.
    const auto quantise = [](float value) -> std::uint32_t {
        const float scaled = std::clamp(value, 0.0f, 1.0f) * 255.0f;
        return static_cast<std::uint32_t>(scaled + 0.5f);
    };
    return quantise(r) | (quantise(g) << 8) | (quantise(b) << 16) | (0xFFu << 24);
}

} // namespace

void ColourLut::make_identity(std::uint32_t size) {
    size = std::clamp(size, kMinSize, kMaxSize);
    size_ = size;
    texels_.clear();
    texels_.reserve(static_cast<std::size_t>(size) * size * size);
    const float last = static_cast<float>(size - 1);
    // Red varies fastest, then green, then blue: the order a .cube lists its
    // entries in, and the order a 3D texture upload wants its rows in.
    for (std::uint32_t b = 0; b < size; ++b) {
        for (std::uint32_t g = 0; g < size; ++g) {
            for (std::uint32_t r = 0; r < size; ++r) {
                texels_.push_back(pack(static_cast<float>(r) / last,
                                       static_cast<float>(g) / last,
                                       static_cast<float>(b) / last));
            }
        }
    }
}

bool ColourLut::load(const std::filesystem::path &path, std::string &error) {
    size_ = 0;
    texels_.clear();

    std::ifstream file(path);
    if (!file) {
        error = "cannot open " + path.string();
        return false;
    }

    std::uint32_t size = 0;
    float domain_min[3] = {0.0f, 0.0f, 0.0f};
    float domain_max[3] = {1.0f, 1.0f, 1.0f};
    std::vector<std::uint32_t> texels;
    std::size_t expected = 0;
    std::size_t line_number = 0;
    std::string line;

    while (std::getline(file, line)) {
        ++line_number;
        std::string_view rest = trim(line);
        if (rest.empty()) continue;

        const std::string_view keyword = word(rest);

        if (equals_ignoring_case(keyword, "TITLE")) continue;

        if (equals_ignoring_case(keyword, "LUT_1D_SIZE")) {
            // Refused rather than approximated. A 1D table is three separate
            // curves; it cannot move a hue, which is most of what a grade is
            // for. Reading one and applying it would look like the grade had
            // been applied when most of it had not.
            error = "this is a 1D lookup table; only LUT_3D_SIZE is read";
            return false;
        }

        if (equals_ignoring_case(keyword, "LUT_3D_SIZE")) {
            if (size != 0) {
                error = "LUT_3D_SIZE given twice (line " + std::to_string(line_number) + ")";
                return false;
            }
            if (!parse_uint(word(rest), size) || size < kMinSize || size > kMaxSize) {
                error = "LUT_3D_SIZE must be between " + std::to_string(kMinSize) + " and " +
                        std::to_string(kMaxSize) + " (line " + std::to_string(line_number) + ")";
                return false;
            }
            expected = static_cast<std::size_t>(size) * size * size;
            texels.reserve(expected);
            continue;
        }

        const bool is_min = equals_ignoring_case(keyword, "DOMAIN_MIN");
        if (is_min || equals_ignoring_case(keyword, "DOMAIN_MAX")) {
            float *target = is_min ? domain_min : domain_max;
            for (int i = 0; i < 3; ++i) {
                if (!parse_float(word(rest), target[i])) {
                    error = std::string(is_min ? "DOMAIN_MIN" : "DOMAIN_MAX") +
                            " needs three numbers (line " + std::to_string(line_number) + ")";
                    return false;
                }
            }
            continue;
        }

        // Anything else has to be a data row, and the first word of it has
        // already been taken off.
        float rgb[3];
        if (size == 0) {
            // Whether this is a data row before its size line or not a .cube
            // at all is told apart by whether it reads as one, so that a file
            // picked by mistake says so rather than being reported as a
            // malformed table.
            float ignored;
            error = parse_float(keyword, ignored)
                        ? "table data before LUT_3D_SIZE (line " + std::to_string(line_number) + ")"
                        : "no LUT_3D_SIZE line; this does not look like a .cube";
            return false;
        }
        if (!parse_float(keyword, rgb[0]) || !parse_float(word(rest), rgb[1]) ||
            !parse_float(word(rest), rgb[2])) {
            error = "expected three numbers (line " + std::to_string(line_number) + ")";
            return false;
        }
        if (!trim(rest).empty()) {
            error = "extra text after the three numbers (line " + std::to_string(line_number) + ")";
            return false;
        }
        if (texels.size() == expected) {
            error = "more entries than LUT_3D_SIZE " + std::to_string(size) + " calls for";
            return false;
        }
        texels.push_back(pack(rgb[0], rgb[1], rgb[2]));
    }

    if (size == 0) {
        error = "no LUT_3D_SIZE line; this does not look like a .cube";
        return false;
    }
    if (texels.size() != expected) {
        error = "LUT_3D_SIZE " + std::to_string(size) + " calls for " + std::to_string(expected) +
                " entries, found " + std::to_string(texels.size());
        return false;
    }

    // The shader samples this table with the colour itself as the texture
    // coordinate, which is only the right thing when the table covers 0..1.
    // A file with a wider domain is a grade for a log or scene-referred
    // picture, and applying it here would apply the wrong part of it to every
    // colour. Refused for the same reason a 1D table is: the failure would
    // otherwise be invisible.
    for (int i = 0; i < 3; ++i) {
        if (std::abs(domain_min[i]) > 1e-6f || std::abs(domain_max[i] - 1.0f) > 1e-6f) {
            error = "domain is not 0..1; this grade is for a log or scene-referred picture, "
                    "and this renderer grades display values";
            return false;
        }
    }

    size_ = size;
    texels_ = std::move(texels);
    return true;
}

} // namespace mga::gpu
