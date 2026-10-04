#include "perf/spike.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace mga::perf::spike {
namespace {

constexpr std::size_t kStages = static_cast<std::size_t>(Stage::Count);

struct Frame {
    std::array<Clock::duration, kStages> stages{};
    std::uint32_t misses{};
    std::uint32_t loaded{};
    std::uint32_t pipelines{};
    std::uint32_t reads{};
    std::uint64_t read_bytes{};
    std::uint32_t prints{};
    Clock::time_point began{Clock::now()};
};

Frame &frame() {
    static Frame current;
    return current;
}

// Milliseconds a frame must exceed to be reported; 0 when MGA_STUTTER is unset
// or unreadable, which turns reporting off.
double threshold_ms() {
    static const double value = [] {
        const char *text = std::getenv("MGA_STUTTER");
        if (text == nullptr || *text == '\0') return 0.0;
        try {
            const double ms = std::stod(text);
            return ms > 0.0 ? ms : 0.0;
        } catch (...) {
            std::cout << "[stutter] MGA_STUTTER is not a number of milliseconds: " << text << "\n";
            return 0.0;
        }
    }();
    return value;
}

double ms(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

} // namespace

void add(Stage stage, Clock::duration duration) {
    frame().stages[static_cast<std::size_t>(stage)] += duration;
}

void count_miss(bool loaded) {
    ++frame().misses;
    if (loaded) ++frame().loaded;
}

void count_pipeline() { ++frame().pipelines; }

void count_read(std::uint64_t bytes) {
    ++frame().reads;
    frame().read_bytes += bytes;
}

void count_print() { ++frame().prints; }

void end_frame() {
    static std::uint64_t index = 0u;
    ++index;
    Frame &f = frame();
    const Clock::time_point now = Clock::now();
    const double total = ms(now - f.began);
    const double limit = threshold_ms();
    if (limit > 0.0 && total > limit) {
        std::cout << "[stutter] frame " << index << "  " << total << " ms  " << f.misses << " new textures";
        if (f.misses != 0u) {
            std::cout << ": decode " << ms(f.stages[0]) << ", key " << ms(f.stages[1]) << ", pack "
                      << ms(f.stages[2]) << " (" << f.loaded << " loaded), scale " << ms(f.stages[3])
                      << ", upload " << ms(f.stages[4]);
        }
        if (f.pipelines != 0u)
            std::cout << "  " << f.pipelines << " pipelines " << ms(f.stages[5]);
        if (f.reads != 0u)
            std::cout << "  io " << ms(f.stages[6]) << " (" << f.reads << " reads, "
                      << (f.read_bytes / 1024u) << " KiB)";
        if (f.prints != 0u)
            std::cout << "  print " << ms(f.stages[7]) << " (" << f.prints << " lines)";
        // What the stages did not account for: guest code, the GPU, the kernel.
        double accounted = 0.0;
        for (const Clock::duration stage : f.stages) accounted += ms(stage);
        std::cout << "  [elsewhere " << (total - accounted) << "]\n";
    }
    f = Frame{};
    f.began = now;
}

} // namespace mga::perf::spike
