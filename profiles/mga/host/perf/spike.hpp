#pragma once

// MGA_STUTTER: say what a long frame was spent on.
//
// The average frame time says nothing about a stutter, because a stutter is
// one frame in a hundred and the average hides it. This times the stages of
// the texture miss path -- the only work in this port that is unbounded, done
// on the render thread, and triggered by content appearing for the first time
// -- and prints a line when a frame runs long:
//
//   MGA_STUTTER=20 ./MGAcid        # report any frame over 20 ms
//
//   [stutter] frame 1483  118.4 ms  31 new textures: decode 9.2, key 2.1,
//             pack 94.7 (28 loaded), scale 7.8, upload 4.1  2 pipelines 61.3
//
// The numbers are milliseconds inside that frame. Anything the stages do not
// account for is guest code, the GPU or the kernel, and shows as the gap
// between the frame time and their sum.
//
// Costs two clock reads per texture miss, and a miss already decodes and
// uploads an image, so it is left in rather than compiled out. With
// MGA_STUTTER unset nothing is printed and the accumulators are still
// summed, which is a handful of additions per frame.

#include <chrono>
#include <cstdint>

namespace mga::perf::spike {

using Clock = std::chrono::steady_clock;

enum class Stage {
    Decode,   // guest texels -> RGBA (decode_texture, including unswizzle and CLUT)
    Key,      // content_key: one pass over the decoded texels
    Pack,     // TexturePack::find, which decodes a PNG from disk on a hit
    Scale,    // the renderer's own upscaling (texture_scale)
    Upload,   // create_texture: staging buffer, copy, descriptor set
    Pipeline, // vkCreateGraphicsPipelines for a blend/depth state not seen before
    FileIo,   // sceIoRead and friends, which read the disc image
    Print,    // the game's own debug output, one unbuffered write per line
    Count
};

// Adds to this frame's total for `stage`.
void add(Stage stage, Clock::duration duration);
// One texture missed the cache this frame; `loaded` when the pack replaced it.
void count_miss(bool loaded);

// Times a stage for as long as it is alive.
class Scope {
public:
    explicit Scope(Stage stage) : stage_(stage), start_(Clock::now()) {}
    ~Scope() { add(stage_, Clock::now() - start_); }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    Stage stage_;
    Clock::time_point start_;
};

// One pipeline was compiled this frame.
void count_pipeline();
// Bytes the guest read from the disc this frame, and how many calls.
void count_read(std::uint64_t bytes);
// One line of guest debug output.
void count_print();

// Frames of silence the audio device played because the ring was empty.
void count_silence(std::uint64_t frames);

// Closes the frame: prints a line when it ran longer than MGA_STUTTER says,
// then clears the accumulators. Called where the guest flips.
//
// `virtual_us` is the kernel's clock. Printed against the real time the frame
// took, because the two coming apart is its own kind of fault: the guest can
// only generate as much audio as its clock says has passed, so a burst that
// costs a second of real time and advances the guest by twenty milliseconds
// starves the sink no matter how often its threads are scheduled.
void end_frame(std::uint64_t virtual_us);

} // namespace mga::perf::spike
