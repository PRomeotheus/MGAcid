#include "gpu/frame_record.hpp"

#include "gpu/vulkan_renderer.hpp"

#include <algorithm>
#include <cmath>

namespace mga::gpu {

void FrameRecord::begin_list() {
    // A boundary with no draws since the last one is not a boundary: replaying
    // it would re-hash textures for nothing, and two in a row cannot be told
    // apart from one anyway.
    if (!list_starts_.empty() && list_starts_.back() == count_) return;
    list_starts_.push_back(static_cast<std::uint32_t>(count_));
}

void FrameRecord::add(const DrawCall &call) {
    if (count_ >= kMaxDraws) {
        ++overflow_;
        return;
    }
    if (count_ == draws_.size()) draws_.push_back(call);
    else draws_[count_] = call;
    ++count_;
}

void FrameRecord::clear() noexcept {
    count_ = 0u;
    list_starts_.clear();
}

void FrameRecord::replay(VulkanRenderer &renderer, const GuestMemory &memory) const {
    std::size_t next = 0u;
    for (std::size_t i = 0; i < count_; ++i) {
        // <= rather than ==: a boundary recorded at a draw the cap refused
        // still has to be put back, and it lands on the next draw that exists.
        while (next < list_starts_.size() && list_starts_[next] <= i) {
            renderer.begin_display_list();
            ++next;
        }
        renderer.submit(draws_[i], memory);
    }
}

FrameRecord::DrawKey FrameRecord::key_of(const DrawCall &call) noexcept {
    DrawKey key;
    key.primitive = static_cast<std::uint32_t>(call.primitive);
    key.vertex_count = static_cast<std::uint32_t>(call.vertices.size());
    key.index_count = static_cast<std::uint32_t>(call.indices.size());
    key.vertex_type = call.vertex_type;
    key.texture = call.texture.enabled ? call.texture.address : 0u;
    key.through = call.through;
    key.clear_mode = call.clear_mode;
    return key;
}

namespace {

// Which field of the key differs. One reason per unpaired draw keeps the
// totals honest: they add up to the draws that were not paired.
using Mismatch = FrameRecord::Mismatch;
[[nodiscard]] Mismatch first_difference(const FrameRecord::DrawKey &a, const FrameRecord::DrawKey &b) noexcept {
    if (a.primitive != b.primitive) return Mismatch::Primitive;
    if (a.vertex_count != b.vertex_count) return Mismatch::VertexCount;
    if (a.vertex_type != b.vertex_type) return Mismatch::VertexType;
    if (a.index_count != b.index_count) return Mismatch::IndexCount;
    if (a.texture != b.texture) return Mismatch::Texture;
    if (a.through != b.through) return Mismatch::Through;
    return Mismatch::ClearMode;
}

} // namespace

void FrameRecord::align(const FrameRecord &earlier, const FrameRecord &later, Alignment &into) {
    into.partner.assign(later.size(), kUnpaired);
    into.report = {};
    into.report.compared = later.size();

    std::size_t i = 0u;  // into earlier
    std::size_t j = 0u;  // into later
    while (i < earlier.size() && j < later.size()) {
        const DrawKey a = key_of(earlier[i]);
        const DrawKey b = key_of(later[j]);
        if (a == b) {
            into.partner[j] = static_cast<std::uint32_t>(i);
            ++into.report.matched;
            ++i;
            ++j;
            continue;
        }
        // Out of step. Look for the nearest place the two line up again,
        // taking the closest candidate in either direction so that an
        // insertion and a removal cost the same short search.
        bool resynced = false;
        for (std::size_t d = 1u; d <= kAlignWindow && !resynced; ++d) {
            // This draw appears further along in `later`: something new was
            // drawn before it.
            if (j + d < later.size() && a == key_of(later[j + d])) {
                j += d;
                resynced = true;
            // This draw appears further along in `earlier`: something that was
            // drawn last frame is gone.
            } else if (i + d < earlier.size() && key_of(earlier[i + d]) == b) {
                i += d;
                resynced = true;
            }
        }
        if (resynced) continue;
        // Genuinely different. Leave this draw unpaired and carry on with the
        // next of each, rather than letting one odd draw end the frame.
        ++into.report.reasons[static_cast<std::size_t>(first_difference(a, b))];
        ++i;
        ++j;
    }
    into.report.unpaired = into.report.compared - into.report.matched;
}

namespace {

// Column-major, matching the renderer: element [column * 4 + row].
[[nodiscard]] std::array<float, 16> multiply(const std::array<float, 16> &a, const std::array<float, 16> &b) noexcept {
    std::array<float, 16> result{};
    for (std::size_t column = 0; column < 4u; ++column)
        for (std::size_t row = 0; row < 4u; ++row) {
            float sum = 0.0f;
            for (std::size_t k = 0; k < 4u; ++k) sum += a[k * 4u + row] * b[column * 4u + k];
            result[column * 4u + row] = sum;
        }
    return result;
}

// Where a draw sits on screen, as the average of its vertices carried through
// to pixels. An average is crude for a long thin thing, and it is the right
// crudeness here: the question is whether a draw moved, not what shape it is.
// False when the draw is behind the camera, where the divide is meaningless.
[[nodiscard]] bool screen_centre(const DrawCall &call, float &x, float &y) noexcept {
    if (call.vertices.empty()) return false;
    std::array<float, 4> sum{};
    for (const Vertex &v : call.vertices)
        for (std::size_t c = 0; c < 3u; ++c) sum[c] += v.position[c];
    const float count = static_cast<float>(call.vertices.size());
    for (std::size_t c = 0; c < 3u; ++c) sum[c] /= count;
    sum[3] = 1.0f;

    if (call.through) {
        // Already in screen space, in pixels.
        x = sum[0];
        y = sum[1];
        return std::isfinite(x) && std::isfinite(y);
    }
    const std::array<float, 16> clip = multiply(call.projection, multiply(call.view, call.world));
    std::array<float, 4> out{};
    for (std::size_t row = 0; row < 4u; ++row) {
        float total = 0.0f;
        for (std::size_t k = 0; k < 4u; ++k) total += clip[k * 4u + row] * sum[k];
        out[row] = total;
    }
    if (!(out[3] > 1e-4f)) return false;
    // Normalised device coordinates out to half the PSP's screen either way,
    // so the number that comes out is pixels of the screen the game was made
    // for rather than of whatever this window happens to be.
    x = out[0] / out[3] * (480.0f / 2.0f);
    y = out[1] / out[3] * (272.0f / 2.0f);
    return std::isfinite(x) && std::isfinite(y);
}

} // namespace

void FrameRecord::measure_motion(const FrameRecord &earlier, const FrameRecord &later, const Alignment &alignment,
                                 MotionReport &into) {
    for (std::size_t j = 0; j < later.size(); ++j) {
        const std::uint32_t partner = alignment.partner[j];
        if (partner == kUnpaired) continue;
        const DrawCall &b = later[j];
        if (b.clear_mode) continue;
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        if (!screen_centre(earlier[partner], ax, ay) || !screen_centre(b, bx, by)) continue;
        const double moved = std::hypot(static_cast<double>(bx - ax), static_cast<double>(by - ay));
        // Something that jumps most of the screen in a frame is a cut or a
        // reused draw slot, not motion, and averaging it in would drown out
        // everything this is trying to measure.
        if (moved > 480.0) continue;
        if (b.through) {
            into.screen_pixels += moved;
            ++into.screen_draws;
            if (moved >= 1.0) ++into.screen_moving;
            into.screen_max = std::max(into.screen_max, moved);
        } else {
            into.blended_pixels += moved;
            ++into.blended_draws;
            if (moved >= 1.0) ++into.blended_moving;
            into.blended_max = std::max(into.blended_max, moved);
        }
    }
}

const char *FrameRecord::mismatch_name(Mismatch reason) noexcept {
    switch (reason) {
    case Mismatch::Primitive: return "primitive";
    case Mismatch::VertexCount: return "vertex count";
    case Mismatch::VertexType: return "vertex type";
    case Mismatch::IndexCount: return "index count";
    case Mismatch::Texture: return "texture";
    case Mismatch::Through: return "through";
    case Mismatch::ClearMode: return "clear mode";
    case Mismatch::Count: break;
    }
    return "?";
}

namespace {

[[nodiscard]] float mix(float a, float b, float t) noexcept { return a + (b - a) * t; }

// Vertex colours carry fades and flashes, so they move between frames like
// everything else. Each channel is blended on its own and clamped, since
// carrying motion forward can aim past the ends of the range.
[[nodiscard]] std::uint32_t mix_colour(std::uint32_t a, std::uint32_t b, float t) noexcept {
    std::uint32_t out = 0u;
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        const float channel = mix(static_cast<float>((a >> shift) & 0xFFu), static_cast<float>((b >> shift) & 0xFFu), t);
        out |= static_cast<std::uint32_t>(std::clamp(channel, 0.0f, 255.0f)) << shift;
    }
    return out;
}

void mix_matrix(const std::array<float, 16> &a, const std::array<float, 16> &b, float t,
                std::array<float, 16> &into) noexcept {
    // Element by element, which is not how rotations should be blended: a
    // matrix part way between two rotations this way is slightly shrunk, by
    // an amount that grows with the angle between them. Over half a frame of
    // a game running at thirty that angle is small enough not to show, and
    // the alternative -- decomposing every matrix in every draw into rotation,
    // scale and translation and recomposing it -- costs far more than the
    // error it removes. If something visibly breathes as it turns, this is
    // where it comes from.
    for (std::size_t i = 0; i < a.size(); ++i) into[i] = mix(a[i], b[i], t);
}

} // namespace

bool FrameRecord::build_blend(const FrameRecord &earlier, const FrameRecord &later, float t, FrameRecord &into) {
    if (later.empty()) return false;
    static Alignment alignment;
    align(earlier, later, alignment);
    if (alignment.report.compared == 0u) return false;
    if (alignment.report.matched * 100u < alignment.report.compared * kBlendMatchPercent) return false;

    into.copy_from(later);
    for (std::size_t j = 0; j < later.size(); ++j) {
        const std::uint32_t partner = alignment.partner[j];
        if (partner == kUnpaired) continue;
        const DrawCall &a = earlier[partner];
        const DrawCall &b = later[j];
        // Left exactly where the game put them: draws already in screen space
        // -- the interface, text, anything laid out to the pixel -- and the
        // clears. Half a pixel of movement does nothing for those but soften
        // them, and text is the first thing to look wrong when it is not
        // where it was authored to be.
        if (b.through || b.clear_mode) continue;
        // Too far to be the same thing having moved; see kMaxBlendPixels.
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        if (screen_centre(a, ax, ay) && screen_centre(b, bx, by) &&
            std::hypot(bx - ax, by - ay) > kMaxBlendPixels)
            continue;

        DrawCall &dst = into.draws_[j];
        for (std::size_t v = 0; v < b.vertices.size(); ++v) {
            const Vertex &va = a.vertices[v];
            const Vertex &vb = b.vertices[v];
            Vertex &vd = dst.vertices[v];
            // Ac!d skins its characters on the CPU and hands the GE the
            // result, so blending the vertices themselves covers skinning,
            // morphing and every other deformation without this code having
            // to know that any of them exist.
            for (std::size_t c = 0; c < 3u; ++c) vd.position[c] = mix(va.position[c], vb.position[c], t);
            for (std::size_t c = 0; c < 3u; ++c) vd.normal[c] = mix(va.normal[c], vb.normal[c], t);
            // Scrolling texture coordinates are how this game animates water
            // and the panels behind menus.
            for (std::size_t c = 0; c < 2u; ++c) vd.texcoord[c] = mix(va.texcoord[c], vb.texcoord[c], t);
            vd.color = mix_colour(va.color, vb.color, t);
        }
        mix_matrix(a.world, b.world, t, dst.world);
        mix_matrix(a.view, b.view, t, dst.view);
        mix_matrix(a.projection, b.projection, t, dst.projection);
    }
    return true;
}

void FrameRecord::copy_from(const FrameRecord &other) {
    clear();
    for (std::size_t i = 0; i < other.size(); ++i) add(other[i]);
    list_starts_ = other.list_starts_;
}

void FrameRecord::swap(FrameRecord &other) noexcept {
    draws_.swap(other.draws_);
    std::swap(count_, other.count_);
    list_starts_.swap(other.list_starts_);
    std::swap(overflow_, other.overflow_);
}

} // namespace mga::gpu
