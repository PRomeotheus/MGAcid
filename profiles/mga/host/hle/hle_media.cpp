// sceDisplay, sceCtrl, sceGe_user, sceAudio and sceSasCore. These keep guest
// timing and callbacks behaving like hardware (vblank-paced input reads, GE
// list completion callbacks, blocking audio output); the drawing and the
// mixing themselves live under gpu/ and audio/.
#include "hle_common.hpp"
#include "install/user_data.hpp"
#include "state/save_state.hpp"
#include "kernel/scene.hpp"

#include "overlays.hpp"

#include "audio/audio_sink.hpp"
#include "audio/sas_core.hpp"

#include "psprecomp/common.hpp"

#include "gpu/ge_state.hpp"
#include "perf/frame_stats.hpp"
#if defined(MGA_HAS_RENDERER)
#include "gpu/frame_record.hpp"
#include "gpu/vulkan_renderer.hpp"
#include "ui/ui.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

namespace mga {
namespace {

constexpr std::uint32_t kEdramBase = 0x04000000u;
constexpr std::uint32_t kEdramSize = 0x00200000u;
constexpr std::uint32_t kAudioSampleRate = 44'100u;

// MGA_TRACE_DISPLAY=1: how the game paces itself, once a second. Frames the
// host presents are only as meaningful as the guest calls behind them, and
// those are what say whether a flip is a frame.
struct ClockSample {
    std::uint64_t virtual_us{};
    std::uint64_t vblanks{};
};

#if defined(MGA_HAS_RENDERER)
// The frame being recorded. File scope rather than a MediaState member on
// purpose: MediaState is written into save states field by field, and this is
// host-side scratch that means nothing in another session.
gpu::FrameRecord &frame_record() {
    static gpu::FrameRecord record;
    return record;
}

// The frame before the one being recorded. The extra frame is built from this
// and the current one, so it has to outlive its own present.
gpu::FrameRecord &previous_frame() {
    static gpu::FrameRecord record;
    return record;
}

// The frame the game never drew, built at the end of one game frame and shown
// on the vblank in the middle of the next.
gpu::FrameRecord &extra_frame() {
    static gpu::FrameRecord record;
    return record;
}

// Half a frame past the newer of the two real frames. Not the midpoint
// between them: see build_blend for why forwards rather than between.
constexpr float kExtraFrameAt = 1.5f;

// What the extra present needs, captured while a game frame is being
// presented and used on the vblank after it. Guest memory is not touched
// between those two moments -- every guest thread is waiting on that vblank --
// so the pointer is as good then as it was here.
struct ExtraFrame {
    const psprecomp::GuestMemory *memory{};
    std::uint32_t address{};
    bool built{};
};

ExtraFrame &extra() {
    static ExtraFrame state;
    return state;
}

// MGA_SMOOTH: show the window more often than the game computes frames.
//
// Ac!d paces itself to every second vblank and takes exactly one simulation
// step per frame (see the comment on kVBlanksPerFrame), so it cannot be made
// to produce 60 frames a second -- forcing it to try runs the game at double
// speed. The frames in between have to be made here instead.
//
//   off    what the game does, one present per frame.
//   double the same frame shown twice. Not smoother; it proves the second
//          present can happen at all, from where it has to happen, without
//          disturbing the game.
//   lerp   an image built from the two real frames either side.
enum class Smoothing { Off, Double, Lerp };

Smoothing smoothing() {
    // MGA_SMOOTH=double is a diagnostic rather than a setting: it shows the
    // same frame twice, which proves the extra present is happening without
    // changing anything about what is on it. Every other value, and the
    // setting, mean the real thing. Read every frame, so the menu's toggle
    // takes effect on the next one.
    static const bool same_frame_twice = [] {
        const char *text = std::getenv("MGA_SMOOTH");
        return text != nullptr && std::string(text) == "double";
    }();
    if (same_frame_twice) return Smoothing::Double;
    return settings::current().frame_smoothing ? Smoothing::Lerp : Smoothing::Off;
}

// Set when a game frame has been presented and the extra present that follows
// it has not happened yet. The game flips once every two vblanks, so without
// this the vblank after the extra present would present a third time.
bool &extra_present_due() {
    static bool due = false;
    return due;
}

#endif

struct DisplayTrace {
    std::uint64_t set_frame_buf{};   // sceDisplaySetFrameBuf calls
    std::uint64_t set_immediate{};   // ... of them with sync = 0
    std::uint64_t address_changed{}; // ... that named a different buffer
    std::uint64_t vblank_waits{};    // sceDisplayWaitVblankStart(+CB)
    // Which guest code each vblank wait was called from (by return address).
    // Two waits per presented frame can be one call site reached twice -- a
    // loop pacing the game to every second vblank -- or two different places
    // each waiting once. Those need opposite fixes, and the return address is
    // the only thing that tells them apart.
    std::map<std::uint32_t, std::uint64_t> vblank_sites;
    std::uint64_t extra_presents{};  // presents between the game's own frames
    std::uint64_t extra_blended{};   // ... that showed a frame built from two real ones
    gpu::FrameRecord::MatchReport match;    // how well frames line up with their predecessor
    gpu::FrameRecord::MotionReport motion;  // and how much actually moved between them
    std::uint64_t ctrl_reads{};      // sceCtrlReadBufferPositive calls
    std::uint64_t ctrl_blocks{};     // ... of them that had to wait for a sample
    std::uint64_t vblank_base{};
    ClockSample clock_base{};
    std::chrono::steady_clock::time_point since{};
};

DisplayTrace &display_trace() {
    static DisplayTrace trace;
    return trace;
}

bool trace_display() {
    static const bool enabled = std::getenv("MGA_TRACE_DISPLAY") != nullptr;
    return enabled;
}

// Called once a frame; prints a line a second.
void report_display_trace() {
    if (!trace_display()) return;
    using Clock = std::chrono::steady_clock;
    DisplayTrace &trace = display_trace();
    const Clock::time_point now = Clock::now();
    if (trace.since == Clock::time_point{}) {
        trace.since = now;
        trace.vblank_base = kernel().vblank_count();
        trace.clock_base = {kernel().now_us(), kernel().vblank_count()};
        return;
    }
    const double seconds = std::chrono::duration<double>(now - trace.since).count();
    if (seconds < 1.0) return;
    std::cout << "[display] per second: setframebuf " << static_cast<double>(trace.set_frame_buf) / seconds
              << " (immediate " << static_cast<double>(trace.set_immediate) / seconds << ", new address "
              << static_cast<double>(trace.address_changed) / seconds << ") | vblank waits "
              << static_cast<double>(trace.vblank_waits) / seconds << " | vblanks "
              << static_cast<double>(kernel().vblank_count() - trace.vblank_base) / seconds << " | pad reads "
              << static_cast<double>(trace.ctrl_reads) / seconds << " (blocked "
              << static_cast<double>(trace.ctrl_blocks) / seconds << ") | extra presents "
              << static_cast<double>(trace.extra_presents) / seconds << " (blended "
              << static_cast<double>(trace.extra_blended) / seconds << ")" << [&] {
                     using Record = gpu::FrameRecord;
                     const Record::MatchReport &m = trace.match;
                     const std::size_t total = m.compared;
                     std::ostringstream out;
                     out << " | draws " << static_cast<double>(total) / seconds << ", matched "
                         << (total != 0u ? 100.0 * static_cast<double>(m.matched) / static_cast<double>(total) : 0.0)
                         << "%";
                     if (m.unpaired != 0u)
                         out << " (" << static_cast<double>(m.unpaired) / seconds << " unpaired)";
                     // Only the reasons that actually fired, biggest first, so
                     // a poor rate names the part of the key that is wrong.
                     std::vector<std::pair<std::size_t, const char *>> reasons;
                     for (std::size_t i = 0; i < m.reasons.size(); ++i)
                         if (m.reasons[i] != 0u)
                             reasons.emplace_back(m.reasons[i], Record::mismatch_name(static_cast<Record::Mismatch>(i)));
                     std::sort(reasons.begin(), reasons.end(), [](const auto &a, const auto &b) { return a > b; });
                     for (const auto &[count, name] : reasons)
                         out << " | " << name << " " << static_cast<double>(count) / seconds;
                     // What actually moved. Averaged over the draws that were
                     // paired, not over time, so it reads the same whatever
                     // the frame rate is: pixels of the PSP's own screen that
                     // a draw shifted from one frame to the next.
                     const gpu::FrameRecord::MotionReport &mo = trace.motion;
                     const auto part = [&out](const char *what, double pixels, std::size_t draws,
                                              std::size_t moving, double most) {
                         out << " | " << what << " ";
                         if (draws == 0u) {
                             out << "none";
                             return;
                         }
                         out << pixels / static_cast<double>(draws) << " px avg, "
                             << 100.0 * static_cast<double>(moving) / static_cast<double>(draws) << "% moving, max "
                             << most;
                     };
                     part("motion 3D", mo.blended_pixels, mo.blended_draws, mo.blended_moving, mo.blended_max);
                     part("2D", mo.screen_pixels, mo.screen_draws, mo.screen_moving, mo.screen_max);
                     return out.str();
                 }() << [&] {
                     std::ostringstream sites;
                     sites << " | wait sites";
                     for (const auto &[ra, count] : trace.vblank_sites)
                         sites << " " << std::hex << std::showbase << ra << std::dec << std::noshowbase << "x"
                               << static_cast<double>(count) / seconds;
                     return sites.str();
                 }()
#if defined(MGA_HAS_RENDERER)
              << " | arena peak "
              << (active_renderer() != nullptr ? active_renderer()->arena_peak_bytes() / 1024u : 0u) << " KB of "
              << (active_renderer() != nullptr ? active_renderer()->arena_bytes() / 1024u : 0u)
              << " KB, draws dropped "
              << (active_renderer() != nullptr ? active_renderer()->dropped_draws() : 0u) << ", record overflow "
              << frame_record().overflow() << " | blobs "
              << (active_renderer() != nullptr ? active_renderer()->blob_report() : std::string())
#endif
              << " | cpu scale " << kernel().cpu_scale() << ", charged work "
              << static_cast<double>(kernel().last_work_us()) / 1000.0 << " ms/frame, emulated "
              << static_cast<double>(kernel().now_us() - trace.clock_base.virtual_us) / 1000.0 / seconds
              << " ms per real s | idle " << kernel().describe_idle_time() << "\n";
    trace.set_frame_buf = trace.set_immediate = trace.address_changed = trace.vblank_waits = 0u;
    trace.ctrl_reads = trace.ctrl_blocks = trace.extra_presents = trace.extra_blended = 0u;
    trace.match = {};
    trace.motion = {};
    trace.vblank_sites.clear();
    trace.vblank_base = kernel().vblank_count();
    trace.clock_base = {kernel().now_us(), kernel().vblank_count()};
    trace.since = now;
}

// MGA_PAD_SCRIPT: presses buttons on a schedule, with no window and no pad.
//
//   MGA_PAD_SCRIPT="6:START,8:CROSS,9.5:CROSS+CIRCLE"
//
// Each entry is a time in emulated seconds and the buttons held for 200 ms
// from it. It exists so a headless run can get past the title screen and into
// the part of the game a bug lives in, which a diagnostic run otherwise cannot
// reach at all.
std::uint32_t scripted_buttons() {
    struct Press {
        std::uint64_t at_us{};
        std::uint32_t buttons{};
    };
    static const std::vector<Press> script = [] {
        std::vector<Press> presses;
        const char *spec = std::getenv("MGA_PAD_SCRIPT");
        if (spec == nullptr) return presses;
        static const std::pair<const char *, std::uint32_t> kNames[] = {
            {"SELECT", 0x1u},    {"START", 0x8u},    {"UP", 0x10u},      {"RIGHT", 0x20u},
            {"DOWN", 0x40u},     {"LEFT", 0x80u},    {"L", 0x100u},      {"R", 0x200u},
            {"TRIANGLE", 0x1000u}, {"CIRCLE", 0x2000u}, {"CROSS", 0x4000u}, {"SQUARE", 0x8000u},
        };
        const std::string list = spec;
        std::size_t start = 0u;
        while (start < list.size()) {
            const std::size_t end = std::min(list.find(',', start), list.size());
            const std::string item = list.substr(start, end - start);
            start = end + 1u;
            const std::size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            Press press;
            press.at_us = static_cast<std::uint64_t>(std::strtod(item.c_str(), nullptr) * 1e6);
            std::size_t from = colon + 1u;
            while (from <= item.size()) {
                const std::size_t plus = std::min(item.find('+', from), item.size());
                const std::string name = item.substr(from, plus - from);
                from = plus + 1u;
                for (const auto &[text, bit] : kNames)
                    if (name == text) press.buttons |= bit;
            }
            presses.push_back(press);
            std::cout << "[pad] scripted " << psprecomp::hex32(press.buttons) << " at "
                      << press.at_us / 1000u << " ms\n";
        }
        return presses;
    }();
    constexpr std::uint64_t kHoldUs = 200'000u;
    const std::uint64_t now = kernel().now_us();
    std::uint32_t buttons = 0u;
    for (const Press &press : script)
        if (now >= press.at_us && now < press.at_us + kHoldUs) buttons |= press.buttons;
    return buttons;
}

struct DisplayState {
    std::uint32_t mode{};
    std::uint32_t width{480u};
    std::uint32_t height{272u};
    std::uint32_t framebuffer{};
    std::uint32_t buffer_width{};
    std::uint32_t pixel_format{};
    // What the last present put on screen, and whether anything has been
    // drawn since. Metal Gear Ac!d calls sceDisplaySetFrameBuf about three
    // times for every frame it draws, naming two different buffers and one of
    // them twice. Presenting on each of those costs two extra waits for vsync
    // a frame, and vsync is what the game's frame rate ends up being.
    std::uint32_t presented{0xFFFFFFFFu};
    bool drawn_since_present{};
    std::chrono::steady_clock::time_point last_present{};
};

struct GeCallback {
    std::uint32_t signal_function{};
    std::uint32_t signal_argument{};
    std::uint32_t finish_function{};
    std::uint32_t finish_argument{};
};

struct GeList {
    std::uint32_t pc{};
    std::uint32_t stall{};
    std::int32_t callback{-1};
    bool done{};
};

struct AudioChannel {
    bool reserved{};
    std::uint32_t samples{};
    std::uint32_t format{};
    // Virtual time at which everything handed to this channel has finished
    // playing, and the host ring position its next frames are mixed at.
    std::uint64_t queued_until_us{};
    std::uint64_t cursor{};
};

struct MediaState {
    DisplayState display;
    std::uint32_t ctrl_cycle{};
    std::uint32_t ctrl_mode{};
    // Which sample sceCtrlReadBufferPositive last handed out; see the comment
    // there for why a read is only ever blocked when this is the current one.
    std::uint64_t ctrl_read_sample{};
    std::map<std::int32_t, GeCallback> ge_callbacks;
    std::int32_t next_ge_callback{};
    std::map<std::uint32_t, GeList> ge_lists;
    std::uint32_t next_ge_list{1u};
    std::array<AudioChannel, 8> audio{};
    gpu::GeState ge;
#if defined(MGA_HAS_RENDERER)
    std::unique_ptr<gpu::VulkanRenderer> renderer;
#endif
};

MediaState &media() {
    static MediaState state;
    return state;
}

// Runs a display list: the GE state machine produces draw calls for the
// renderer and raises the guest's signal/finish callbacks in interrupt context.
// A GE block transfer, row by row. A source in a framebuffer the renderer drew
// is written back to guest memory first: the game copies the last frame of a
// hunt this way and textures the quest reward screen's background from it.
void block_transfer(Runtime &rt, const gpu::BlockTransfer &transfer) {
    psprecomp::GuestMemory &memory = rt.memory();
#if defined(MGA_HAS_RENDERER)
    if (media().renderer && media().renderer->available())
        media().renderer->read_back_framebuffer(transfer.source, memory);
#endif
    const std::size_t row_bytes = static_cast<std::size_t>(transfer.width) * transfer.bytes_per_pixel;
    for (std::uint32_t row = 0; row < transfer.height; ++row) {
        const std::uint32_t from =
            transfer.source +
            ((transfer.source_y + row) * transfer.source_stride + transfer.source_x) * transfer.bytes_per_pixel;
        const std::uint32_t to = transfer.destination + ((transfer.destination_y + row) * transfer.destination_stride +
                                                          transfer.destination_x) *
                                                             transfer.bytes_per_pixel;
        const std::uint8_t *source = memory.raw_pointer(from, row_bytes);
        std::uint8_t *destination = memory.raw_pointer(to, row_bytes);
        if (source == nullptr || destination == nullptr) {
            log_once("ge-transfer-range", "[ge] block transfer outside guest memory skipped");
            return;
        }
        std::memmove(destination, source, row_bytes);
    }
#if defined(MGA_HAS_RENDERER)
    // Textures already looked up in this list may have changed.
    if (media().renderer && media().renderer->available()) frame_record().begin_list();
#endif
}

void run_ge_list(Runtime &rt, std::uint32_t id) {
    auto found = media().ge_lists.find(id);
    if (found == media().ge_lists.end()) return;
    const perf::Clock::time_point start = perf::Clock::now();
    GeList &list = found->second;
    const GeCallback *callback = nullptr;
    if (const auto cb = media().ge_callbacks.find(list.callback); cb != media().ge_callbacks.end())
        callback = &cb->second;

    media().ge.set_signal_sink([callback](std::uint32_t signal, std::uint32_t pc) {
        if (callback == nullptr) return;
        const bool finish = (signal & 0x10000u) != 0u;
        const std::uint32_t function = finish ? callback->finish_function : callback->signal_function;
        if (function == 0u) return;
        InterruptCall call{};
        call.function = function;
        call.arguments = {signal & 0xFFFFu, finish ? callback->finish_argument : callback->signal_argument, pc, 0u};
        kernel().queue_interrupt(std::move(call));
    });
#if defined(MGA_HAS_RENDERER)
    if (media().renderer && media().renderer->available()) {
        frame_record().begin_list();
        media().ge.set_draw_sink([](const gpu::DrawCall &call) { frame_record().add(call); });
    }
#endif
    media().ge.set_transfer_sink([&rt](const gpu::BlockTransfer &transfer) { block_transfer(rt, transfer); });

    bool finished = false;
    try {
        list.pc = media().ge.execute(rt.memory(), list.pc, list.stall, finished);
    } catch (const psprecomp::Error &error) {
        // A malformed list must not take the whole run down: drop it and carry on.
        log_once("ge-list-error", std::string("[ge] display list aborted: ") + error.what());
        finished = true;
    }
    list.done = finished;
    media().display.drawn_since_present = true;
    media().ge.set_draw_sink(nullptr);
    media().ge.set_transfer_sink(nullptr);
    perf::add_render_time(perf::Clock::now() - start);
}

// Whether this sceDisplaySetFrameBuf is a frame, rather than one of the calls
// the game makes around it that leave the screen as it is. Presenting costs a
// wait for vsync, so an extra one is a third of the game's frame rate.
bool should_present(DisplayState &display, std::uint32_t previous_framebuffer) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point now = Clock::now();
    const bool changed = display.framebuffer != previous_framebuffer;
    // A frame the GE drew; a frame the CPU composed in main memory (the movie
    // player's sceJpegCsc draws no display list, so a new buffer is all there
    // is to go on); or nothing new for long enough that the window has to be
    // serviced anyway, since input and the pause menu ride on presenting.
    constexpr auto kIdlePresent = std::chrono::milliseconds(100);
    const bool cpu_frame = changed && (display.framebuffer & 0x1F000000u) != kEdramBase &&
                           (display.framebuffer & 0xFF000000u) != 0u;
    const bool idle = display.last_present == Clock::time_point{} || now - display.last_present >= kIdlePresent;
    if (!display.drawn_since_present && !cpu_frame && !idle) return false;
    display.presented = display.framebuffer;
    display.drawn_since_present = false;
    display.last_present = now;
    return true;
}

#if defined(MGA_HAS_RENDERER)
// Hands the renderer the characters to put a shadow under, once a frame.
//
// The game's own shadows are cast by the scenery; the characters have none,
// and the renderer cannot work out where they are for itself, because they
// are skinned on the CPU and submitted pre-transformed at the origin. The
// game's records know, so the kernel reads them and passes them on.
void publish_shadow_casters(Runtime &rt) {
    // MGA_NO_SHADOWS stops the scene from being read at all, which is the way
    // to tell a fault in that read from one anywhere else.
    static const bool disabled = std::getenv("MGA_NO_SHADOWS") != nullptr;
    if (disabled) return;
    if (!media().renderer || !media().renderer->available()) return;
    static std::vector<gpu::ShadowCaster> casters;
    const scene::View &view = scene::read(rt);
    casters.clear();
    if (view.valid) {
        // A character's own position sits about half a cell above the surface
        // it stands on -- at ground level the records read y ~= 985 while the
        // floor is at 0, and a cell is 2000 units.
        //
        // The floor used to be taken from the character's cell instead, which
        // held only while everyone stood on the stage's base level: a
        // character up on a platform inside the same cell got a shadow down
        // at the cell's base, buried inside the platform and invisible. The
        // character's own height tracks them up and down, so it is used
        // instead. The cost is that the animation moves the shadow with the
        // character, by about twelve units out of two thousand.
        //
        // That cost turned out to be the whole of the flicker. Twelve units of
        // animated bob around a floor the disc is otherwise flush with means the
        // disc crosses the floor plane and back on every step: on the down beat
        // it is behind the floor, the depth test rejects it, and it disappears
        // for those frames. Standing still it is steady, which is why the
        // flicker only showed up while walking.
        //
        // So the disc is lifted clear by more than the bob can reach. Thirty
        // units is one and a half percent of a cell -- far too little to see as
        // a gap under a character, and comfortably more than twelve.
        constexpr float kFootDrop = 1000.0f;
        constexpr float kGroundLift = 30.0f;
        constexpr float kRadius = 620.0f;
        for (const scene::Character &character : view.characters)
            casters.push_back({character.position, character.position[1] - kFootDrop + kGroundLift, kRadius});
    }
    media().renderer->set_shadow_casters(view.world_to_clip, casters);
}
#endif

void present_frame(Runtime &rt) {
    // Overlays are swapped between frames; re-check before drawing the next one.
    revalidate_overlays(rt);
#if defined(MGA_HAS_RENDERER)
    publish_shadow_casters(rt);
    if (!media().renderer || !media().renderer->available()) {
        frame_record().clear();
        perf::end_frame(kernel().now_us());
        kernel().calibrate_cpu_scale();
        report_display_trace();
        return;
    }
    gpu::VulkanRenderer &renderer = *media().renderer;
    // The guest passes a VRAM offset when the high byte is zero.
    const std::uint32_t address = (media().display.framebuffer & 0xFF000000u) == 0u
                                      ? (media().display.framebuffer | 0x04000000u)
                                      : media().display.framebuffer;
    const perf::Clock::time_point present_start = perf::Clock::now();
    // The GE only draws into VRAM, so a framebuffer in main memory was
    // written by the CPU (the movie player's sceJpegCsc) and has to be shown
    // from memory.
    constexpr std::uint32_t kPixelFormat8888 = 3u;
    const DisplayState &display = media().display;
    if ((address & 0x1F000000u) != kEdramBase && display.pixel_format == kPixelFormat8888) {
        const std::size_t bytes = static_cast<std::size_t>(display.buffer_width) * display.height * 4u;
        renderer.upload_frame(address, rt.memory().raw_pointer(address, bytes), display.width, display.height,
                              display.buffer_width);
    }
    // The frame's draws were recorded rather than drawn as they arrived; this
    // is where they are actually drawn. Nothing about the picture changes --
    // same draws, same order, same list boundaries -- but the frame now still
    // exists after it has been shown, which is what an in-between frame has to
    // be built from.
    frame_record().replay(renderer, rt.memory());
    ui::draw_over_game();
    renderer.write_back_frame(rt.memory());
    renderer.present(address);
    // How much of this frame was also in the one before it. Counted before the
    // swap, while the two are still this frame and the one before.
    {
        static gpu::FrameRecord::Alignment alignment;
        gpu::FrameRecord::align(previous_frame(), frame_record(), alignment);
        DisplayTrace &trace = display_trace();
        trace.match.compared += alignment.report.compared;
        trace.match.matched += alignment.report.matched;
        trace.match.unpaired += alignment.report.unpaired;
        for (std::size_t i = 0; i < trace.match.reasons.size(); ++i)
            trace.match.reasons[i] += alignment.report.reasons[i];
        gpu::FrameRecord::measure_motion(previous_frame(), frame_record(), alignment, trace.motion);
    }
    // Built here, while both real frames are still in hand; drawn on the
    // vblank after this one.
    extra().memory = &rt.memory();
    extra().address = address;
    extra().built = smoothing() == Smoothing::Lerp &&
                    gpu::FrameRecord::build_blend(previous_frame(), frame_record(), kExtraFrameAt, extra_frame());
    extra_present_due() = true;
    // This frame becomes the previous one; what was the previous one becomes
    // the pool the next frame records into, so neither allocates again.
    previous_frame().swap(frame_record());
    frame_record().clear();
    perf::add_render_time(perf::Clock::now() - present_start);
    // A frame ends when its image has been handed to the swapchain.
    perf::end_frame(kernel().now_us());
    kernel().calibrate_cpu_scale();
    report_display_trace();

    // Optional frame capture, independent of the window.
    static const char *screenshot_dir = std::getenv("MGA_SCREENSHOT_DIR");
    static const std::uint64_t screenshot_every = [] {
        const char *text = std::getenv("MGA_SCREENSHOT_EVERY");
        return text != nullptr ? std::strtoull(text, nullptr, 10) : 60ull;
    }();
    if (screenshot_dir != nullptr && screenshot_every != 0u &&
        renderer.frames_presented() % screenshot_every == 0u) {
        const std::string path = std::string(screenshot_dir) + "/frame_" +
                                 std::to_string(renderer.frames_presented()) + ".bmp";
        if (renderer.capture_frame(path))
            std::cout << "[render] frame " << renderer.frames_presented() << " (" << renderer.draws_submitted()
                      << " draws) -> " << path << "\n";
    }
    // Fast forward follows the key every frame rather than being switched on and
    // off, so letting go is as immediate as pressing and nothing can be left in
    // the wrong state.
    kernel().set_fast_forward(ui::fast_forward_held());
    if (ui::screenshot_requested()) {
        std::error_code code;
        const auto folder = install::user_data_directory() / "screenshots";
        std::filesystem::create_directories(folder, code);
        if (code) {
            std::cout << "[render] cannot make a screenshots folder: " << code.message() << "\n";
        } else {
            // Numbered by the frame rather than the clock: two screenshots in the
            // same second would otherwise be one file.
            const auto name = "shot_" + std::to_string(renderer.frames_presented()) + ".bmp";
            renderer.capture_window(install::path_to_utf8(folder / name));
            std::cout << "[render] screenshot -> " << install::path_to_utf8(folder / name) << "\n";
        }
    }

    // A function key asked for a save state. Recorded on the way past rather
    // than carried out: this runs inside present_frame, and the call that owns
    // the guest context is the one above it. It performs the request the moment
    // this returns, which is the dispatch boundary a state needs.
    {
        unsigned slot = 0u;
        bool load = false;
        if (ui::state_hotkey(slot, load))
            state::request(load ? state::Request::Load : state::Request::Save, slot);
    }
    if (!renderer.pump_events()) {
        rt.stop("window closed");
    } else if (ui::take_quit_request()) {
        rt.stop("quit from the menu");
    } else if (!ui::menu_over_game() && ui::menu_requested()) {
        if (ui::menu_pauses()) {
            // The menu pauses the game: guest code and emulated time stand
            // still while it runs in here, and the device stops playing.
            audio::AudioSink::instance().set_paused(true);
            const bool keep_playing = ui::run_menu();
            audio::AudioSink::instance().set_paused(false);
            // Resume at normal speed rather than racing to make up the pause,
            // and keep the pause out of the frame statistics.
            kernel().resync_real_time();
            perf::restart_measurement();
            if (!keep_playing) rt.stop("quit from the menu");
        } else {
            // The game keeps running, sound and pacing included; the menu is
            // drawn over each frame and takes all input until it closes.
            ui::open_menu_over_game();
        }
    }
#else
    (void)rt;
    perf::end_frame(kernel().now_us());
    kernel().calibrate_cpu_scale();
    report_display_trace();
#endif
}

// MGA_60FPS=1: lift the game's own 30 fps cap.
//
// Ac!d paces itself in the loop at 0x0887B308, which reads as:
//
//     sceDisplayWaitVblankStart();
//     elapsed = *kVBlanksThisFrame;                     // 0x089B7508
//     while (elapsed < *kVBlanksPerFrame)               // 0x089E111C
//         sceDisplayWaitVblankStart();
//     *kVBlanksPerFrame = clamp(elapsed, 2, 6);
//     *kVBlanksThisFrame = 0;
//
// So the cadence for the next frame is whatever the last one cost, in whole
// vblanks, and that clamp is the frame rate: a floor of two vblanks is 30 fps
// and can never be anything else, and a ceiling of six means a frame that
// overruns falls to 10 rather than tearing. Nothing else in the game reads
// either variable -- they belong to this loop alone -- so writing the floor
// down to one vblank changes the game's pacing and nothing else about it.
//
// What that does NOT settle is whether the simulation advances by a fixed step
// per frame or by elapsed time. If it is fixed, this runs the game at double
// speed rather than at 60 fps, and the honest fix is a much larger one. The
// clamp's own shape hints at fixed: a game scaling by elapsed time would have
// no reason to prefer a steady slow cadence over a variable one. Cheaper to
// try it than to argue about it.
constexpr std::uint32_t kVBlanksPerFrame = 0x089E111Cu;

void raise_frame_rate_cap(Runtime &rt) {
    static const bool enabled = std::getenv("MGA_60FPS") != nullptr;
    if (!enabled) return;
    // Written on the way into every vblank wait, which is always just before
    // the loop reads it, so the value the game wrote a moment ago never gets
    // the chance to take effect.
    rt.memory().store32(kVBlanksPerFrame, 1u);
}

void register_display_ctrl(HleRegistrar &hle) {
    hle.add("sceDisplay", "sceDisplaySetMode", [](Runtime &, AllegrexContext &ctx) {
        media().display.mode = arg(ctx, 0);
        media().display.width = arg(ctx, 1);
        media().display.height = arg(ctx, 2);
        kernel().finish(ctx, 0u);
    });
    // The guest flipping the framebuffer is the end of a frame.
    hle.add("sceDisplay", "sceDisplaySetFrameBuf", [](Runtime &rt, AllegrexContext &ctx) {
        scene::trace(rt);
        if (trace_display()) {
            DisplayTrace &trace = display_trace();
            ++trace.set_frame_buf;
            if (arg(ctx, 3) == 0u) ++trace.set_immediate;
            if (arg(ctx, 0) != media().display.framebuffer) ++trace.address_changed;
        }
        DisplayState &display = media().display;
        const std::uint32_t previous = display.framebuffer;
        display.framebuffer = arg(ctx, 0);
        display.buffer_width = arg(ctx, 1);
        display.pixel_format = arg(ctx, 2);
        if (should_present(display, previous)) present_frame(rt);
        // The one place a save state is taken or put back. The display call is
        // reached from the outer dispatch loop and holds the live context, so
        // between this call and the next the host stack carries no guest frames
        // -- which is the only kind of moment the whole of the guest is in its
        // memory and its contexts and nowhere else.
        //
        // A load returns true: `ctx` now belongs to the restored session, and
        // finishing this call on it would set a return value on a thread that
        // never made the call. The dispatcher carries on from the restored
        // program counter instead.
        if (state::run_pending(rt, ctx)) return;
        kernel().finish(ctx, 0u);
    });
    // Early titles pace themselves on the vblank instead of on SetFrameBuf.
    hle.add("sceDisplay", "sceDisplayGetFrameBuf", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        if (arg(ctx, 0) != 0u) memory.store32(arg(ctx, 0), media().display.framebuffer);
        if (arg(ctx, 1) != 0u) memory.store32(arg(ctx, 1), media().display.buffer_width);
        if (arg(ctx, 2) != 0u) memory.store32(arg(ctx, 2), media().display.pixel_format);
        kernel().finish(ctx, 0u);
    });
    hle.add("sceDisplay", "sceDisplayWaitVblankStart", [](Runtime &rt, AllegrexContext &ctx) {
        raise_frame_rate_cap(rt);
        ++display_trace().vblank_waits;
        ++display_trace().vblank_sites[ctx.gpr[31]];
        // Once a frame is the right cadence to watch the scene change.
        scene::trace(rt);
        WaitState wait{};
        wait.type = WaitType::VBlank;
        kernel().block(ctx, wait, 0u);
    });
    hle.add("sceDisplay", "sceDisplayWaitVblankStartCB", [](Runtime &rt, AllegrexContext &ctx) {
        raise_frame_rate_cap(rt);
        ++display_trace().vblank_waits;
        ++display_trace().vblank_sites[ctx.gpr[31]];
        scene::trace(rt);
        (void)kernel().deliver_callbacks();
        WaitState wait{};
        wait.type = WaitType::VBlank;
        kernel().block(ctx, wait, 0u);
    });
    // 286 lines per frame on the PSP's LCD timing.
    hle.add("sceDisplay", "sceDisplayGetAccumulatedHcount", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, static_cast<std::uint32_t>(kernel().vblank_count() * 286u));
    });

    hle.add("sceCtrl", "sceCtrlSetSamplingCycle", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t previous = media().ctrl_cycle;
        media().ctrl_cycle = arg(ctx, 0);
        kernel().finish(ctx, previous);
    });
    hle.add("sceCtrl", "sceCtrlSetSamplingMode", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t previous = media().ctrl_mode;
        media().ctrl_mode = arg(ctx, 0);
        // The mode is recorded but not acted on: the game subtracts 128 from Lx
        // unconditionally, so reporting the neutral 0x80 is right in both modes.
        if (std::getenv("MGA_TRACE_PAD") != nullptr && media().ctrl_mode != previous)
            std::cout << "[pad] sceCtrlSetSamplingMode " << media().ctrl_mode << "\n";
        kernel().finish(ctx, previous);
    });
    // Reading the controller buffer takes whatever the sampler has put there
    // since the last read, and blocks only when there is nothing -- that is,
    // when the caller has got ahead of the sampler by reading twice inside one
    // vblank. It is not a "wait for the next sample" call.
    //
    // Blocking on every read looks equivalent for a game that reads once a
    // frame, and is what this did until now. It is not equivalent: that game
    // then spends one vblank in the read and a second in its own
    // sceDisplayWaitVblankStart, so two vblanks pass per frame and it runs at
    // half the refresh rate however little work it has to do. That was the
    // whole of this port's 30 fps -- the profile showed the CPU idle for 22 of
    // every 33 ms and the main thread never sleeping once.
    hle.add("sceCtrl", "sceCtrlReadBufferPositive", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t address = arg(ctx, 0);
        const std::uint32_t count = std::clamp<std::uint32_t>(arg(ctx, 1), 1u, 64u);
        std::uint32_t buttons = 0u;
        std::uint8_t analog_x = 0x80u;
        std::uint8_t analog_y = 0x80u;
        std::uint8_t right_x = 0x80u;
        std::uint8_t right_y = 0x80u;
        buttons |= scripted_buttons();
#if defined(MGA_HAS_RENDERER)
        if (media().renderer && media().renderer->available()) {
            const gpu::PadState pad = media().renderer->pad();
            buttons |= pad.buttons;
            analog_x = pad.analog_x;
            analog_y = pad.analog_y;
            right_x = pad.right_x;
            right_y = pad.right_y;
        }
#endif
        auto &memory = rt.memory();
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t entry = address + i * 16u;
            memory.store32(entry, static_cast<std::uint32_t>(kernel().now_us()));
            memory.store32(entry + 4u, buttons);
            memory.store8(entry + 8u, analog_x);
            memory.store8(entry + 9u, analog_y);
            // Bytes 10 and 11 are the HD release's second stick, not padding.
            // Leaving them zero reads as a full diagonal deflection and turns
            // the camera every frame; 0x80 is the centre the guest tests for.
            memory.store8(entry + 10u, right_x);
            memory.store8(entry + 11u, right_y);
            for (std::uint32_t j = 12u; j < 16u; ++j) memory.store8(entry + j, 0u);
        }
        ++display_trace().ctrl_reads;
        // One sample per vblank, so the vblank counter is the sample counter.
        // Not carried in a save state: it is a cursor into a stream of samples
        // that no longer exists on the other side of a load, and starting it
        // fresh costs one read a single vblank, once.
        const std::uint64_t sample = kernel().vblank_count();
        if (sample != media().ctrl_read_sample) {
            media().ctrl_read_sample = sample;
            kernel().finish(ctx, count);
            return;
        }
        ++display_trace().ctrl_blocks;
        WaitState wait{};
        wait.type = WaitType::VBlank;
        kernel().block(ctx, wait, count);
    });
}

void register_ge(HleRegistrar &hle) {
    hle.add("sceGe_user", "sceGeEdramGetAddr", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, kEdramBase); });
    hle.add("sceGe_user", "sceGeEdramGetSize", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, kEdramSize); });
    hle.add("sceGe_user", "sceGeEdramSetAddrTranslation", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
    hle.add("sceGe_user", "sceGeSetCallback", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t data = arg(ctx, 0);
        auto &memory = rt.memory();
        const std::int32_t id = media().next_ge_callback++;
        media().ge_callbacks[id] = GeCallback{memory.load32(data), memory.load32(data + 4u), memory.load32(data + 8u),
                                              memory.load32(data + 12u)};
        kernel().finish(ctx, static_cast<std::uint32_t>(id));
    });
    hle.add("sceGe_user", "sceGeUnsetCallback", [](Runtime &, AllegrexContext &ctx) {
        media().ge_callbacks.erase(static_cast<std::int32_t>(arg(ctx, 0)));
        kernel().finish(ctx, 0u);
    });
    const auto enqueue = [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = media().next_ge_list++;
        GeList list{};
        list.pc = arg(ctx, 0) & 0x0FFFFFFFu;
        list.stall = arg(ctx, 1) & 0x0FFFFFFFu;
        list.callback = static_cast<std::int32_t>(arg(ctx, 2));
        media().ge_lists[id] = std::move(list);
        perf::count_display_list();
        run_ge_list(rt, id);
        kernel().finish(ctx, id);
    };
    hle.add("sceGe_user", "sceGeListEnQueue", enqueue);
    hle.add("sceGe_user", "sceGeListEnQueueHead", enqueue);
    hle.add("sceGe_user", "sceGeListUpdateStallAddr", [](Runtime &rt, AllegrexContext &ctx) {
        auto found = media().ge_lists.find(arg(ctx, 0));
        if (found == media().ge_lists.end()) {
            kernel().finish(ctx, error::kIllegalArgument);
            return;
        }
        found->second.stall = arg(ctx, 1) & 0x0FFFFFFFu;
        run_ge_list(rt, arg(ctx, 0));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceGe_user", "sceGeListSync", [](Runtime &, AllegrexContext &ctx) {
        auto found = media().ge_lists.find(arg(ctx, 0));
        // 0 = completed, 2 = still stalled (drawing).
        const std::uint32_t state = found == media().ge_lists.end() || found->second.done ? 0u : 2u;
        kernel().finish(ctx, state);
    });
    hle.add("sceGe_user", "sceGeDrawSync", [](Runtime &, AllegrexContext &ctx) {
        std::erase_if(media().ge_lists, [](const auto &item) { return item.second.done; });
        kernel().finish(ctx, 0u);
    });
    hle.add("sceGe_user", "sceGeBreak", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
    hle.add("sceGe_user", "sceGeContinue", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
}

// sceAudioOutput*Blocking returns once the previously submitted buffer has
// drained, which is what paces the guest's audio thread: the channel's queue
// is tracked in virtual time and the thread waits on the kernel clock, never
// on the host. The samples themselves go straight to the sink.
void audio_output(Runtime &rt, AllegrexContext &ctx) {
    const std::uint32_t channel = arg(ctx, 0);
    if (channel >= media().audio.size()) {
        kernel().finish(ctx, 0x80260002u);
        return;
    }
    AudioChannel &state = media().audio[channel];
    const std::uint32_t left = arg(ctx, 1);
    const std::uint32_t right = arg(ctx, 2);
    const std::uint32_t buffer = arg(ctx, 3);
    const std::uint32_t frames = state.samples;
    // Format 0x10 is mono: one sample per frame instead of a stereo pair.
    const bool mono = (state.format & 0x10u) != 0u;
    const std::size_t words = frames * (mono ? 1u : 2u);

    if (buffer != 0u && frames != 0u) {
        static std::vector<std::int16_t> staging;
        staging.resize(frames * 2u);
        if (const std::uint8_t *source = rt.memory().raw_pointer(buffer, words * 2u)) {
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                const std::size_t index = mono ? frame : frame * 2u;
                const auto sample = static_cast<std::int16_t>(source[index * 2u] | (source[index * 2u + 1u] << 8));
                staging[frame * 2u] = sample;
                staging[frame * 2u + 1u] = mono ? sample
                                                : static_cast<std::int16_t>(source[(index + 1u) * 2u] |
                                                                            (source[(index + 1u) * 2u + 1u] << 8));
            }
            audio::AudioSink::instance().mix(state.cursor, staging.data(), frames, left, right);
        } else {
            log_once("audio-buffer", "[audio] output buffer is not a single mapped range; dropping it");
        }
    }

    const std::uint64_t now = kernel().now_us();
    if (state.queued_until_us < now) state.queued_until_us = now;
    const std::uint64_t previous_end = state.queued_until_us;
    state.queued_until_us += static_cast<std::uint64_t>(frames) * 1'000'000u / kAudioSampleRate;
    if (previous_end > now)
        kernel().delay_current(ctx, previous_end - now, frames);
    else
        kernel().finish(ctx, frames);
}

// __sceSasCore renders one grain into a guest buffer; the guest then hands that
// buffer to a sceAudio channel itself, so nothing here reaches the sink.
void sas_render(Runtime &rt, std::uint32_t core, std::uint32_t output, bool mix,
                std::uint32_t left_volume, std::uint32_t right_volume) {
    audio::SasCore &sas = audio::sas_core(core);
    const std::size_t frames = sas.grain();
    static std::vector<std::int16_t> staging;
    staging.resize(frames * 2u);
    sas.render(rt.memory(), staging.data(), frames);

    std::uint8_t *destination = rt.memory().raw_pointer(output, frames * 4u);
    if (destination == nullptr) {
        log_once("sas-output", "[sas] output buffer is not a single mapped range; dropping the grain");
        return;
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t side = 0; side < 2u; ++side) {
            const std::size_t index = frame * 2u + side;
            std::int32_t value = staging[index];
            if (mix) {
                const std::uint32_t gain = side == 0u ? left_volume : right_volume;
                value = (value * static_cast<std::int32_t>(std::min(gain, 0x1000u))) >> 12;
                value += static_cast<std::int16_t>(destination[index * 2u] | (destination[index * 2u + 1u] << 8));
                value = std::clamp(value, -32768, 32767);
            }
            destination[index * 2u] = static_cast<std::uint8_t>(value);
            destination[index * 2u + 1u] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(value) >> 8u);
        }
    }
}

void register_audio(HleRegistrar &hle) {
    audio::AudioSink::instance().initialize();

    hle.add("sceAudio", "sceAudioChReserve", [](Runtime &, AllegrexContext &ctx) {
        auto channel = static_cast<std::int32_t>(arg(ctx, 0));
        auto &channels = media().audio;
        if (channel < 0) {
            channel = -1;
            for (std::size_t i = 0; i < channels.size(); ++i) {
                if (!channels[i].reserved) {
                    channel = static_cast<std::int32_t>(i);
                    break;
                }
            }
        }
        if (channel < 0 || channel >= static_cast<std::int32_t>(channels.size()) || channels[static_cast<std::size_t>(channel)].reserved) {
            kernel().finish(ctx, 0x80260002u);
            return;
        }
        channels[static_cast<std::size_t>(channel)] = AudioChannel{true, arg(ctx, 1), arg(ctx, 2), 0u, 0u};
        kernel().finish(ctx, static_cast<std::uint32_t>(channel));
    });
    hle.add("sceAudio", "sceAudioChRelease", [](Runtime &, AllegrexContext &ctx) {
        if (arg(ctx, 0) < media().audio.size()) media().audio[arg(ctx, 0)].reserved = false;
        kernel().finish(ctx, 0u);
    });
    hle.add("sceAudio", "sceAudioSetChannelDataLen", [](Runtime &, AllegrexContext &ctx) {
        if (arg(ctx, 0) < media().audio.size()) media().audio[arg(ctx, 0)].samples = arg(ctx, 1);
        kernel().finish(ctx, 0u);
    });
    hle.add("sceAudio", "sceAudioChangeChannelConfig", [](Runtime &, AllegrexContext &ctx) {
        if (arg(ctx, 0) < media().audio.size()) media().audio[arg(ctx, 0)].format = arg(ctx, 1);
        kernel().finish(ctx, 0u);
    });
    hle.add("sceAudio", "sceAudioChangeChannelVolume", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
    // How much of what the channel was given is still to play, in samples.
    hle.add("sceAudio", "sceAudioGetChannelRestLength", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t channel = arg(ctx, 0);
        if (channel >= media().audio.size()) {
            kernel().finish(ctx, 0x80260002u);
            return;
        }
        const std::uint64_t now = kernel().now_us();
        const AudioChannel &state = media().audio[channel];
        const std::uint64_t remaining = state.queued_until_us > now ? state.queued_until_us - now : 0u;
        kernel().finish(ctx, static_cast<std::uint32_t>(remaining * kAudioSampleRate / 1'000'000u));
    });
    hle.add("sceAudio", "sceAudioOutputPannedBlocking", audio_output);
    hle.try_add("sceAudio", "sceAudioOutputPanned", audio_output);
    // The one-volume forms of the first firmware: (channel, volume, buffer).
    const auto one_volume = [](Runtime &rt, AllegrexContext &ctx) {
        ctx.set_gpr(7, ctx.gpr[6]);
        ctx.set_gpr(6, ctx.gpr[5]);
        audio_output(rt, ctx);
    };
    hle.try_add("sceAudio", "sceAudioOutputBlocking", one_volume);
    hle.try_add("sceAudio", "sceAudioOutput", one_volume);
    hle.try_add("sceAudio", "sceAudioGetChannelRestLen", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t channel = arg(ctx, 0);
        if (channel >= media().audio.size()) {
            kernel().finish(ctx, 0x80260002u);
            return;
        }
        const std::uint64_t now = kernel().now_us();
        const AudioChannel &state = media().audio[channel];
        const std::uint64_t remaining = state.queued_until_us > now ? state.queued_until_us - now : 0u;
        kernel().finish(ctx, static_cast<std::uint32_t>(remaining * kAudioSampleRate / 1'000'000u));
    });

    hle.add("sceSasCore", "__sceSasInit", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).init(arg(ctx, 1), arg(ctx, 2), arg(ctx, 3));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetVoice", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).set_voice(arg(ctx, 1), arg(ctx, 2), arg(ctx, 3), arg(ctx, 4) != 0u);
        kernel().finish(ctx, 0u);
    });
    hle.try_add("sceSasCore", "__sceSasSetVoicePCM", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).set_voice_pcm(arg(ctx, 1), arg(ctx, 2), arg(ctx, 3),
                                                   static_cast<std::int32_t>(arg(ctx, 4)));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetPitch", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).set_pitch(arg(ctx, 1), arg(ctx, 2));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetVolume", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).set_volume(arg(ctx, 1), static_cast<std::int32_t>(arg(ctx, 2)),
                                                static_cast<std::int32_t>(arg(ctx, 3)));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetSimpleADSR", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).set_simple_adsr(arg(ctx, 1), arg(ctx, 2), arg(ctx, 3));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetKeyOn", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).key_on(arg(ctx, 1));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetKeyOff", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).key_off(arg(ctx, 1));
        kernel().finish(ctx, 0u);
    });
    hle.try_add("sceSasCore", "__sceSasSetPause", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).set_pause(arg(ctx, 1), arg(ctx, 2) != 0u);
        kernel().finish(ctx, 0u);
    });
    hle.try_add("sceSasCore", "__sceSasGetEnvelopeHeight", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, static_cast<std::uint32_t>(audio::sas_core(arg(ctx, 0)).envelope_height(arg(ctx, 1))));
    });
    // Full ADSR control and the noise generator are not modelled yet: accepted
    // so the sound driver runs, with the simple envelope still in effect.
    for (const char *name : {"__sceSasSetADSR", "__sceSasSetADSRmode", "__sceSasSetSL", "__sceSasSetNoise"}) {
        const std::string key = std::string("sas-") + name;
        hle.try_add("sceSasCore", name, [key](Runtime &, AllegrexContext &ctx) {
            log_once(key, "[sas] " + key.substr(4) + " is accepted but not modelled");
            kernel().finish(ctx, 0u);
        });
    }
    // Reverb is not modelled, so the sends are accepted and dropped.
    for (const char *name : {"__sceSasRevType", "__sceSasRevParam", "__sceSasRevEVOL", "__sceSasRevVON"})
        hle.add("sceSasCore", name, [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
    hle.add("sceSasCore", "__sceSasGetOutputmode", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, audio::sas_core(arg(ctx, 0)).output_mode());
    });
    hle.add("sceSasCore", "__sceSasGetEndFlag", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, audio::sas_core(arg(ctx, 0)).end_flag());
    });
    hle.add("sceSasCore", "__sceSasCore", [](Runtime &rt, AllegrexContext &ctx) {
        sas_render(rt, arg(ctx, 0), arg(ctx, 1), false, 0u, 0u);
        kernel().finish(ctx, 0u);
    });
    hle.try_add("sceSasCore", "__sceSasCoreWithMix", [](Runtime &rt, AllegrexContext &ctx) {
        sas_render(rt, arg(ctx, 0), arg(ctx, 1), true, arg(ctx, 2), arg(ctx, 3));
        kernel().finish(ctx, 0u);
    });
}

} // namespace

void initialize_renderer() {
#if defined(MGA_HAS_RENDERER)
    (void)ensure_renderer();
#else
    std::cout << "Renderer: not built\n";
#endif
}

void register_media(HleRegistrar &hle) {
    initialize_renderer();
    register_display_ctrl(hle);
    register_ge(hle);
    register_audio(hle);
}

#if defined(MGA_HAS_RENDERER)
gpu::VulkanRenderer *active_renderer() {
    return media().renderer && media().renderer->available() ? media().renderer.get() : nullptr;
}

// The extra present, from the vblank between two of the game's own flips.
//
// Why here rather than from a timer or the window thread: this runs inside the
// kernel's vblank, which happens while every guest thread is waiting and
// before any of them is woken. Guest memory cannot be in a half-written state
// at that moment, and no display list is part-walked, which makes it the only
// point in the frame where the renderer can be handed work that did not come
// from the game.
void present_between_frames() {
    if (smoothing() == Smoothing::Off || !extra_present_due()) return;
    extra_present_due() = false;
    if (!media().renderer || !media().renderer->available()) return;
    gpu::VulkanRenderer &renderer = *media().renderer;
    // Deliberately not ui::draw_over_game(): it ticks the input script and
    // advances the menu, and doing that twice per game frame would run both at
    // double speed -- the very thing this whole exercise exists to avoid. So
    // while the interface is up there is no way to put it on this frame, and
    // the frame is not worth showing without it: a menu that appears on every
    // second image is worse than a menu at thirty.
    const bool blended = smoothing() == Smoothing::Lerp && extra().built && extra().memory != nullptr &&
                         !ui::overlay_drawn();
    if (blended) {
        extra_frame().replay(renderer, *extra().memory);
        renderer.present(extra().address);
    } else {
        // Nothing new to show: the same image again. Holds the present rate
        // steady, which matters more than it sounds -- a window that presents
        // 60, 30, 60, 30 judders worse than one that presents 30.
        renderer.present_ui(true);
    }
    ++display_trace().extra_presents;
    if (blended) ++display_trace().extra_blended;
}

gpu::VulkanRenderer *ensure_renderer() {
    static bool tried = false;
    if (tried) return active_renderer();
    tried = true;
    if (std::getenv("MGA_NO_RENDER") != nullptr) {
        std::cout << "Renderer: disabled by MGA_NO_RENDER\n";
        return nullptr;
    }
    auto renderer = std::make_unique<gpu::VulkanRenderer>();
    std::string error;
    gpu::RendererConfig config;
    // Tells windows apart when several instances run side by side, e.g. two
    // players testing ad hoc play on one machine.
    if (const char *title = std::getenv("MGA_WINDOW_TITLE"); title != nullptr && *title != '\0')
        config.title = title;
    if (!renderer->initialize(config, error)) {
        std::cerr << "Renderer: unavailable (" << error << "); running headless\n";
        return nullptr;
    }
    media().renderer = std::move(renderer);
    ui::attach(*media().renderer);
    kernel().add_vblank_hook(present_between_frames);
    return media().renderer.get();
}
#endif


// ---------------------------------------------------------------------------
// Save states

std::string why_no_media_state() {
    // A display list that has not finished. Lists are run to completion inside
    // the call that starts them, so between frames there is none in flight --
    // but a stalled list is one the game means to add to, and its GE state is
    // part way through rather than at a boundary.
    for (const auto &[id, list] : media().ge_lists) {
        (void)id;
        if (!list.done) return "a display list is still being drawn";
    }
    return {};
}

void write_media_state(psprecomp::SnapshotWriter &out) {
    MediaState &state = media();
    out.u32(state.display.mode);
    out.u32(state.display.width);
    out.u32(state.display.height);
    out.u32(state.display.framebuffer);
    out.u32(state.display.buffer_width);
    out.u32(state.display.pixel_format);
    out.u32(state.display.presented);
    out.boolean(state.display.drawn_since_present);
    // Not display.last_present: it is a host clock reading used to space
    // presents, meaningless in another session and re-established by the next
    // frame.

    out.u32(state.ctrl_cycle);
    out.u32(state.ctrl_mode);

    out.u32(static_cast<std::uint32_t>(state.ge_callbacks.size()));
    for (const auto &[id, callback] : state.ge_callbacks) {
        out.i32(id);
        out.u32(callback.signal_function);
        out.u32(callback.signal_argument);
        out.u32(callback.finish_function);
        out.u32(callback.finish_argument);
    }
    out.i32(state.next_ge_callback);

    out.u32(static_cast<std::uint32_t>(state.ge_lists.size()));
    for (const auto &[id, list] : state.ge_lists) {
        out.u32(id);
        out.u32(list.pc);
        out.u32(list.stall);
        out.i32(list.callback);
        out.boolean(list.done);
    }
    out.u32(state.next_ge_list);

    for (const AudioChannel &channel : state.audio) {
        out.boolean(channel.reserved);
        out.u32(channel.samples);
        out.u32(channel.format);
        out.u64(channel.queued_until_us);
        out.u64(channel.cursor);
    }

    state.ge.write_state(out);
}

bool read_media_state(psprecomp::SnapshotReader &in) {
    MediaState &state = media();
    DisplayState display{};
    display.mode = in.u32();
    display.width = in.u32();
    display.height = in.u32();
    display.framebuffer = in.u32();
    display.buffer_width = in.u32();
    display.pixel_format = in.u32();
    display.presented = in.u32();
    display.drawn_since_present = in.boolean();
    display.last_present = std::chrono::steady_clock::now();
    if (!in.ok()) return false;

    const std::uint32_t ctrl_cycle = in.u32();
    const std::uint32_t ctrl_mode = in.u32();

    std::map<std::int32_t, GeCallback> ge_callbacks;
    std::uint32_t count = in.u32();
    if (!in.ok() || count > in.remaining() / sizeof(std::uint32_t)) {
        in.fail();
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::int32_t id = in.i32();
        GeCallback callback{};
        callback.signal_function = in.u32();
        callback.signal_argument = in.u32();
        callback.finish_function = in.u32();
        callback.finish_argument = in.u32();
        ge_callbacks.emplace(id, callback);
    }
    const std::int32_t next_ge_callback = in.i32();

    std::map<std::uint32_t, GeList> ge_lists;
    count = in.u32();
    if (!in.ok() || count > in.remaining() / sizeof(std::uint32_t)) {
        in.fail();
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t id = in.u32();
        GeList list{};
        list.pc = in.u32();
        list.stall = in.u32();
        list.callback = in.i32();
        list.done = in.boolean();
        ge_lists.emplace(id, list);
    }
    const std::uint32_t next_ge_list = in.u32();

    std::array<AudioChannel, 8> audio{};
    for (AudioChannel &channel : audio) {
        channel.reserved = in.boolean();
        channel.samples = in.u32();
        channel.format = in.u32();
        channel.queued_until_us = in.u64();
        channel.cursor = in.u64();
    }
    if (!in.ok()) return false;

    // The GE last, and straight into place: it owns its own failure and there
    // is nothing after it to undo.
    if (!state.ge.read_state(in)) return false;

    state.display = display;
    state.ctrl_cycle = ctrl_cycle;
    state.ctrl_mode = ctrl_mode;
    state.ge_callbacks = std::move(ge_callbacks);
    state.next_ge_callback = next_ge_callback;
    state.ge_lists = std::move(ge_lists);
    state.next_ge_list = next_ge_list;
    state.audio = audio;
    return true;
}

} // namespace mga
