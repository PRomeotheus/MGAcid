// Tests for the .cube colour grading table reader.
//
// Worth having as runnable tests rather than a compile check: every one of
// these is a file the reader has to either understand exactly or refuse for a
// stated reason, and "refuses the ones it should" is not something reading the
// code tells you. The half-texel sampling that goes with this lives in
// post.frag and is not covered here -- it needs a GPU.
//
//   cmake --build out/mga --target mga_colour_lut_tests
//   ctest --test-dir out/mga -R colour_lut --output-on-failure
//
// Two things in here are deliberate and were not, in the first version:
//
// The scratch file goes in std::filesystem::temp_directory_path(), not in
// /tmp. The first version hardcoded /tmp, which does not exist on Windows, so
// every load failed there -- and then every check below indexed into the empty
// vector that came back, which is undefined behaviour. It read harmless
// garbage under GCC and segfaulted under MSVC. The tests "passed" on Linux
// while testing nothing.
//
// And nothing here indexes a vector directly. at() returns a sentinel past the
// end instead, so a reader that returns less than it should makes a test FAIL
// rather than making the test binary crash. A test that crashes tells you
// almost nothing; a test that fails names the case.

#include "gpu/colour_lut.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace mga::gpu;

namespace {

std::filesystem::path scratch_path() {
    std::error_code code;
    std::filesystem::path dir = std::filesystem::temp_directory_path(code);
    if (code) dir = std::filesystem::current_path();
    return dir / "mga_colour_lut_test.cube";
}

// True only when the bytes are actually on disk afterwards.
bool write_file(const std::filesystem::path &path, const char *text) {
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file << text;
        if (!file) return false;
    }
    std::error_code code;
    return std::filesystem::exists(path, code);
}

// Past the end returns a value no real entry can hold: every entry has alpha
// 0xFF, so 0 is unmistakable and turns an out-of-range read into a failed
// comparison instead of undefined behaviour.
std::uint32_t at(const ColourLut &lut, std::size_t index) {
    return index < lut.texels().size() ? lut.texels()[index] : 0u;
}

} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char *what) {
        if (!ok) {
            std::printf("FAIL %s\n", what);
            ++failures;
        }
    };

    const std::filesystem::path scratch = scratch_path();
    if (!write_file(scratch, "probe")) {
        std::printf("FAIL cannot write %s -- the tests below would all be meaningless\n",
                    scratch.string().c_str());
        return 1;
    }
    std::printf("scratch file: %s\n\n", scratch.string().c_str());

    // The identity table, which needs no file at all.
    ColourLut identity;
    identity.make_identity(17);
    check(identity.size() == 17, "identity size");
    check(identity.texels().size() == 17u * 17u * 17u, "identity entry count");
    check(at(identity, 0) == 0xFF000000u, "identity black");
    check(at(identity, 17u * 17u * 17u - 1u) == 0xFFFFFFFFu, "identity white");
    // Red varies fastest: entry 1 is (1/16, 0, 0), which is 16 of 255.
    check((at(identity, 1) & 0xFFu) == 16u, "red varies fastest");
    check(((at(identity, 1) >> 8) & 0xFFu) == 0u, "green does not move with red");
    // One step of green is a whole row of red away.
    check(((at(identity, 17) >> 8) & 0xFFu) == 16u, "green is the second axis");
    check((at(identity, 17) & 0xFFu) == 0u, "red resets at the end of a row");

    // A 2-cube that swaps red and blue, written the way a grading tool writes
    // one: a title, a domain, comments and blank lines.
    check(write_file(scratch,
                     "# a comment\n"
                     "TITLE \"swap red and blue\"\n"
                     "LUT_3D_SIZE 2\n"
                     "DOMAIN_MIN 0.0 0.0 0.0\n"
                     "DOMAIN_MAX 1.0 1.0 1.0\n"
                     "\n"
                     "0 0 0\n"
                     "0 0 1\n"   // red in  -> blue out
                     "0 1 0\n"
                     "0 1 1\n"
                     "1 0 0\n"   // blue in -> red out
                     "1 0 1\n"
                     "1 1 0\n"
                     "1 1 1\n"),
          "wrote the 2-cube");
    ColourLut lut;
    std::string error;
    check(lut.load(scratch, error), "loads a 2-cube");
    check(lut.size() == 2, "2-cube size");
    check(lut.texels().size() == 8u, "2-cube entry count");
    // RGBA8 with red in the low byte, so blue is 0x00FF0000 under 0xFF alpha.
    check(at(lut, 1) == 0xFFFF0000u, "entry 1 is blue");
    check(at(lut, 4) == 0xFF0000FFu, "entry 4 is red");

    // Out-of-range values clamp rather than wrapping. A .cube may hold them;
    // eight bits may not.
    check(write_file(scratch, "LUT_3D_SIZE 2\n"
                              "-0.5 0 0\n4.0 0 0\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n"),
          "wrote the out-of-range cube");
    ColourLut hdr;
    check(hdr.load(scratch, error), "loads out-of-range values");
    check((at(hdr, 0) & 0xFFu) == 0u, "below zero clamps to black");
    check((at(hdr, 1) & 0xFFu) == 255u, "above one clamps to white");

    // The refusals. Each has to be refused, has to say why, and has to leave
    // the table empty rather than half-read.
    const auto refuses = [&](const char *body, const char *what) {
        if (!write_file(scratch, body)) {
            std::printf("FAIL could not write the case for %s\n", what);
            ++failures;
            return;
        }
        ColourLut refused;
        std::string why;
        if (refused.load(scratch, why)) {
            std::printf("FAIL %s: accepted\n", what);
            ++failures;
        } else if (!refused.empty()) {
            std::printf("FAIL %s: refused but left data behind\n", what);
            ++failures;
        } else if (why.empty()) {
            std::printf("FAIL %s: refused with no reason\n", what);
            ++failures;
        } else {
            std::printf("   refused %-24s %s\n", what, why.c_str());
        }
    };
    refuses("LUT_1D_SIZE 32\n0 0 0\n", "a 1D table");
    refuses("LUT_3D_SIZE 2\n0 0 0\n0 0 1\n", "a truncated table");
    refuses("LUT_3D_SIZE 2\n0 0 0\n0 0 1\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n1 1 1\n",
            "too many entries");
    refuses("LUT_3D_SIZE 0\n", "size 0");
    refuses("LUT_3D_SIZE 128\n", "size 128");
    refuses("0 0 0\n", "data before the size");
    refuses("LUT_3D_SIZE 2\nDOMAIN_MAX 4 4 4\n0 0 0\n0 0 1\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n",
            "a log domain");
    refuses("LUT_3D_SIZE 2\n0 0 zero\n", "a word where a number goes");
    refuses("nothing here\n", "a file that is not a cube");

    // A path that does not exist is refused, not crashed on.
    {
        ColourLut missing;
        std::string why;
        check(!missing.load(scratch.parent_path() / "mga_no_such_grade.cube", why), "refuses a missing file");
        check(!why.empty(), "says why a missing file was refused");
        check(missing.empty(), "a missing file leaves the table empty");
    }

    std::error_code code;
    std::filesystem::remove(scratch, code);

    std::printf(failures != 0 ? "\n%d failed\n" : "\nall colour LUT tests pass\n", failures);
    return failures != 0 ? 1 : 0;
}
