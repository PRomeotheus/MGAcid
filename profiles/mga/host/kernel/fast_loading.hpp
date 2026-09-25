#pragma once

#include <cstdint>

// Fast loading: emulated time runs ahead of real time while the game loads,
// and only then.
//
// The disc reads themselves are instant here, yet a load takes as long as it
// did on a PSP. The reason is that the game's loader reads, checks and unpacks
// a piece at a time and waits on the emulated clock in between, and the kernel
// holds that clock to real time. While a load is under way the hold is let go,
// so a load takes as long as the host needs for the work rather than as long
// as the hardware would have.
//
// A load is recognised from what the game DOES, not from a timer: it is
// reading the disc, and it is silent. Anything that could be play keeps real
// time -- sound coming out, a button held, a movie, the menu, ad hoc play
// (where other players' time has to stay real), and Game speed Unlimited,
// which already lets time run free.
//
// Ported from upstream (TeamGDB/Yakumo), where it was measured against Monster
// Hunter. The detector below is game-agnostic by construction -- it keys on
// disc reads and silence, not on any file or address -- but the thresholds
// were chosen against that game's loader, and whether Ac!d's loader waits on
// the clock the same way is the open question. Off by default here for that
// reason; upstream ships it on.
namespace mga::fast_loading {

// How long after its last disc read the game still counts as loading, in
// emulated microseconds. A load reads at least every few hundred
// milliseconds; in between, its threads unpack and wait on the clock.
inline constexpr std::uint64_t kReadWindowUs = 500'000u;
// How long the game must have been silent before a load may run fast, so that
// the last sound before it plays out in full.
inline constexpr std::uint64_t kQuietUs = 250'000u;
// The loudest sample, after the channel's volume, that still counts as
// silence: none. A loading screen hands the audio exact zeros, so only buffers
// that would mix to zero are ever dropped, and the first sample of a sound,
// however quiet, ends a fast stretch and is played. Upstream found a threshold
// of 64 cut the first few milliseconds off a fade-in.
inline constexpr int kAudiblePeak = 0;
// Emulated time runs at most this many times faster than real time.
inline constexpr double kMaxSpeed = 16.0;

// What keeps real time regardless of the loading, sampled at each update.
struct Guards {
    bool enabled{};       // the setting is on, Game speed is Normal, there is a window
    bool buttons_held{};  // a button or a D-pad direction is down; sticks do not count
    bool movie{};         // a movie is playing
    bool online{};        // ad hoc networking is on, or a session is going
    bool menu{};          // the port's menu is open over the game
};

// Why fast loading is off at the moment, for the log.
enum class Reason { None, Disabled, NotLoading, Sound, Buttons, Movie, Online, Menu };
[[nodiscard]] const char *reason_name(Reason reason);

// The decision itself, with no host around it, so that it can be tested.
class Detector {
public:
    // The game read from the disc at emulated time `now_us`.
    void disc_read(std::uint64_t now_us);
    // The game handed a buffer to sceAudio whose loudest sample, after the
    // channel's volume, is `peak`. Sound ends fast loading at once. Returns
    // true when the buffer is silence handed over while time runs fast, which
    // the caller drops rather than play faster than real time.
    bool audio(std::uint64_t now_us, int peak);
    // Decides whether emulated time may run ahead now, and returns that.
    bool update(std::uint64_t now_us, const Guards &guards);

    [[nodiscard]] bool fast() const noexcept { return fast_; }
    // Why the last update or audio call left it off; None while fast.
    [[nodiscard]] Reason reason() const noexcept { return reason_; }

private:
    bool read_seen_{};
    std::uint64_t last_read_us_{};
    bool sound_seen_{};
    std::uint64_t last_sound_us_{};
    bool fast_{};
    Reason reason_{Reason::NotLoading};
};

// The running game's detector and what feeds it. All called on the emulation
// thread.
void note_disc_read();
// See Detector::audio. `peak` already has the channel's volume applied.
[[nodiscard]] bool note_audio(int peak);
void note_buttons(bool held);
// Re-evaluated at each vblank.
void update();
// Whether emulated time may run ahead of real time at the moment.
[[nodiscard]] bool active();

} // namespace mga::fast_loading
