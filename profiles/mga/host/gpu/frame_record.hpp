#pragma once

// A frame's draw calls, kept so that the frame can be drawn more than once.
//
// The GE hands draws to a sink as it walks a display list, and until now each
// one went straight to the renderer and was then forgotten. A frame drawn only
// once needs nothing more than that. Showing the game at a higher rate than it
// computes does: the in-between images are not frames the game ever produced,
// so they have to be built from the two real frames either side of them, and
// that means the earlier of the two has to still exist when the later one
// lands.
//
// What is recorded is the DrawCall itself, vertices and all. Keeping a
// reference would be far cheaper and does not work: the GE resolves vertices
// out of guest memory as it walks the list, and the game is free to overwrite
// that memory the moment the list is done.
//
// Display list boundaries are recorded alongside the draws. The renderer
// hashes texture contents once per list rather than once per draw -- guest
// memory cannot change while a list is walked -- so replaying a frame without
// its boundaries would let a texture looked up in one list be reused for the
// whole frame.
//
// One thing a replay cannot reproduce: the *contents* of guest memory as they
// were when the draw first happened. A texture the game overwrites later in
// the frame is sampled at its final value rather than the value it had at the
// draw. Preserving that would mean decoding every texture at record time,
// which costs more than the frames are worth. Whether it matters in practice
// is a question about this game rather than about the design, and the answer
// is what the first build of this is for.

#include "gpu/ge_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mga::gpu {

class VulkanRenderer;

class FrameRecord {
public:
    // A new display list is about to be walked.
    void begin_list();
    void add(const DrawCall &call);
    // Empties the frame without giving up the memory its draws hold: the next
    // frame is very nearly the same size as this one, and the vertex vectors
    // inside each draw are the expensive part.
    void clear() noexcept;

    [[nodiscard]] bool empty() const noexcept { return count_ == 0u; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] const DrawCall &operator[](std::size_t index) const { return draws_[index]; }
    // Draws refused because the frame hit the cap below.
    [[nodiscard]] std::uint64_t overflow() const noexcept { return overflow_; }

    // Hands every recorded draw to the renderer, in the order it arrived and
    // with the list boundaries put back.
    void replay(VulkanRenderer &renderer, const GuestMemory &memory) const;

    void swap(FrameRecord &other) noexcept;

    // What identifies a draw across two frames.
    //
    // Interpolating between two frames means knowing which draw in one is the
    // same thing a frame later, and the GE gives nothing to identify a draw by
    // -- no handle, no name, nothing that survives the list being rewritten.
    // What it does give is a shape: the same object drawn a frame later has
    // the same primitive, the same number of vertices laid out the same way,
    // and the same texture. Its *position* is what changed, which is exactly
    // what must not be part of the key.
    //
    // Two different objects can of course share a shape -- two identical cards
    // side by side -- so this is paired with position in the list rather than
    // used alone. Between consecutive frames of a game that builds its list
    // the same way every frame, the pair is a strong identity; where it is
    // wrong it is wrong for one frame and the draw simply is not interpolated.
    struct DrawKey {
        std::uint32_t primitive{};
        std::uint32_t vertex_count{};
        std::uint32_t index_count{};
        std::uint32_t vertex_type{};
        std::uint32_t texture{};  // 0 when the draw is untextured
        bool through{};
        bool clear_mode{};
        friend bool operator==(const DrawKey &, const DrawKey &) = default;
    };

    // Not in the key, though it looks like it belongs there: which buffer the
    // draw targets. The game double-buffers, so the target address alternates
    // every frame and including it makes every draw look new every frame --
    // which is what it did, and the match rate was a flat zero everywhere,
    // menus included. The position of the draw in the list already separates
    // one pass from another, which is what the target was meant to do.
    [[nodiscard]] static DrawKey key_of(const DrawCall &call) noexcept;

    // Why two draws in the same position were not the same thing. Recorded so
    // that a disappointing match rate says which part of the key is wrong,
    // rather than only that something is.
    enum class Mismatch { Primitive, VertexCount, VertexType, IndexCount, Texture, Through, ClearMode, Count };
    struct MatchReport {
        std::size_t compared{};  // draws in `later`
        std::size_t matched{};   // ... that were found in `earlier` too
        std::size_t unpaired{};
        std::array<std::size_t, static_cast<std::size_t>(Mismatch::Count)> reasons{};
    };

    // Which draw in `earlier` each draw in `later` is the same thing as.
    //
    // Position in the list is what identifies a draw, but it is not fixed: a
    // game that draws one more thing than it did last frame pushes everything
    // after it along by one, and comparing strictly by index then finds a
    // mismatch at every draw from there to the end of the frame. Measured, that
    // is not a hypothetical -- in play the rate fell from 99% to the seventies
    // whenever the scene gained or lost something, and it was a cascade from
    // one insertion rather than a frame full of genuinely new draws.
    //
    // So the two lists are walked together and allowed to slip: where the keys
    // disagree, a short look ahead in each finds where they line up again. The
    // window is deliberately small. A long one would start pairing a draw with
    // some unrelated thing of the same shape elsewhere in the frame, which is
    // worse than not pairing it at all -- an unpaired draw is simply left where
    // the game put it, while a wrongly paired one is dragged across the screen.
    static constexpr std::uint32_t kUnpaired = 0xFFFFFFFFu;
    struct Alignment {
        std::vector<std::uint32_t> partner;  // one per draw in `later`
        MatchReport report;
    };
    static void align(const FrameRecord &earlier, const FrameRecord &later, Alignment &into);

    // How far things actually moved between two frames, in pixels of the
    // PSP's own 480x272 screen.
    //
    // This exists to answer a question the frame rate cannot: whether there is
    // any motion to smooth. An extra frame built between two frames that are
    // nearly identical is itself nearly identical, and no amount of showing it
    // will look smoother. It splits the answer in two because the blend
    // deliberately leaves screen-space draws alone -- if the motion a player
    // actually watches in a card game turns out to live there, then excluding
    // it was the wrong call and the smoothing has been diligently blending a
    // scene that barely moves.
    struct MotionReport {
        double blended_pixels{};  // summed over paired draws that are blended
        std::size_t blended_draws{};
        std::size_t blended_moving{};  // ... that moved at least a pixel
        double blended_max{};
        double screen_pixels{};  // the same, for the screen-space draws left alone
        std::size_t screen_draws{};
        std::size_t screen_moving{};
        double screen_max{};
    };
    static void measure_motion(const FrameRecord &earlier, const FrameRecord &later, const Alignment &alignment,
                               MotionReport &into);
    [[nodiscard]] static const char *mismatch_name(Mismatch reason) noexcept;

    // Builds a frame the game never drew, out of the two either side of where
    // it belongs, and puts it in `into`.
    //
    // `t` is where the new frame sits on the line through the two: 0 is
    // `earlier`, 1 is `later`, 0.5 the midpoint between them, and 1.5 half a
    // frame *past* `later` -- which is the one this port actually wants. The
    // midpoint would be the more accurate image, and it cannot be used: to
    // show it the real frame after it would have to be held back a frame,
    // which costs latency, and it would consume the interface overlay that
    // the real frame needs. Carrying the motion forward instead leaves the
    // game's own frame untouched and merely adds an image after it.
    //
    // The cost of going forward rather than between is that motion is
    // predicted rather than known, so anything that changes direction
    // overshoots by half a frame before the next real frame corrects it.
    //
    // False when the two frames are too unalike to blend -- a cut, a load, a
    // scene change. The caller should show the real frame again instead: one
    // frame of judder is a far smaller fault than half a frame of smear.
    static bool build_blend(const FrameRecord &earlier, const FrameRecord &later, float t, FrameRecord &into);

private:
    // How far out of step the two lists are allowed to get before a draw is
    // given up on. Eight is a couple of objects' worth of draws.
    static constexpr std::size_t kAlignWindow = 8u;

    // How far a draw may have moved and still be blended, in pixels of the
    // PSP's 480x272 screen.
    //
    // Two draws can carry the same key and not be the same object -- two
    // identical cards, the same effect on a different target -- and a wrong
    // pairing shows up as a draw that apparently crossed the screen in a
    // thirtieth of a second. Blending that drags it half way across an image
    // it was never in. Genuine motion this fast is a blur to the eye anyway,
    // so there is nothing to lose by leaving it where the game put it.
    static constexpr float kMaxBlendPixels = 80.0f;

    // Enough of a frame has to be recognisable for the blend to mean
    // anything. Measured at 99.9% in play and 100% in menus, against 18% on
    // the frame that cuts into a battle, so anywhere in between separates the
    // two cleanly.
    static constexpr std::size_t kBlendMatchPercent = 80u;

    void copy_from(const FrameRecord &other);

public:

private:
    // A run of lists that never presents -- a loading screen drawing into a
    // buffer nothing flips to -- would otherwise grow this without limit. Ac!d
    // draws a few hundred times in a frame, so reaching this means something
    // has run away, not that a scene is busy.
    static constexpr std::size_t kMaxDraws = 16384u;

    // draws_ is a pool rather than a list: it keeps every DrawCall ever
    // recorded, and count_ says how many of them belong to the current frame.
    // Assigning over a draw that is already there reuses its vertex vector's
    // buffer, where clearing and pushing would free and reallocate one per
    // draw per frame.
    std::vector<DrawCall> draws_;
    std::size_t count_{};
    std::vector<std::uint32_t> list_starts_;
    std::uint64_t overflow_{};
};

} // namespace mga::gpu
