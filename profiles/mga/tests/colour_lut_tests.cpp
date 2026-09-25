// Tests for the .cube colour grading table reader.
//
// Worth having as runnable tests rather than a compile check: every one of
// these is a file the reader has to either understand exactly or refuse for a
// stated reason, and "refuses the ones it should" is not something reading the
// code tells you. The half-texel sampling that goes with this lives in
// post.frag and is not covered here -- it needs a GPU.
//
//   g++ -std=c++20 -Iprofiles/mga/host profiles/mga/tests/colour_lut_tests.cpp
//       profiles/mga/host/gpu/colour_lut.cpp -o out/colour_lut_tests
//
// or, as the build has it: cmake --build out/mga --target mga_colour_lut_tests
#include "gpu/colour_lut.hpp"
#include <cstdio>
#include <fstream>
#include <cassert>
using namespace mga::gpu;

static void write(const char *p, const char *s) { std::ofstream f(p); f << s; }

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char *what) {
        if (!ok) { std::printf("FAIL %s\n", what); ++failures; }
    };

    // identity
    ColourLut id; id.make_identity(17);
    check(id.size() == 17 && id.texels().size() == 17u*17*17, "identity size");
    check(id.texels().front() == 0xFF000000u, "identity black");
    check(id.texels().back() == 0xFFFFFFFFu, "identity white");
    // red fastest: texel 1 is (1/16, 0, 0) -> 16
    check((id.texels()[1] & 0xFF) == 16u && ((id.texels()[1] >> 8) & 0xFF) == 0u, "red varies fastest");
    // one step of green is 17 texels in
    check(((id.texels()[17] >> 8) & 0xFF) == 16u && (id.texels()[17] & 0xFF) == 0u, "green is the second axis");

    // a real 2x2x2 cube, written the way a tool writes one
    write("/tmp/t.cube",
        "# a comment\n"
        "TITLE \"swap red and blue\"\n"
        "LUT_3D_SIZE 2\n"
        "DOMAIN_MIN 0.0 0.0 0.0\n"
        "DOMAIN_MAX 1.0 1.0 1.0\n"
        "\n"
        "0 0 0\n"
        "0 0 1\n"        // r=1 -> blue
        "0 1 0\n"
        "0 1 1\n"
        "1 0 0\n"        // b=1 -> red
        "1 0 1\n"
        "1 1 0\n"
        "1 1 1\n");
    ColourLut lut; std::string err;
    check(lut.load("/tmp/t.cube", err), "loads a 2-cube");
    check(lut.size() == 2 && lut.texels().size() == 8, "2-cube size");
    check(lut.texels()[1] == 0xFFFF0000u, "entry 1 is blue");   // b=255 in bits 16..23
    check(lut.texels()[4] == 0xFF0000FFu, "entry 4 is red");

    // refusals
    auto refuses = [&](const char *body, const char *what) {
        write("/tmp/t.cube", body);
        ColourLut l; std::string e;
        if (l.load("/tmp/t.cube", e)) { std::printf("FAIL %s: accepted\n", what); ++failures; }
        else if (!l.empty()) { std::printf("FAIL %s: left data behind\n", what); ++failures; }
        else std::printf("   refused %-22s %s\n", what, e.c_str());
    };
    refuses("LUT_1D_SIZE 32\n0 0 0\n", "a 1D table");
    refuses("LUT_3D_SIZE 2\n0 0 0\n0 0 1\n", "a truncated table");
    refuses("LUT_3D_SIZE 2\n" "0 0 0\n0 0 1\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n1 1 1\n", "too many entries");
    refuses("LUT_3D_SIZE 0\n", "size 0");
    refuses("LUT_3D_SIZE 128\n", "size 128");
    refuses("0 0 0\n", "data with no size");
    refuses("LUT_3D_SIZE 2\nDOMAIN_MAX 4 4 4\n0 0 0\n0 0 1\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n", "a log domain");
    refuses("LUT_3D_SIZE 2\n0 0 zero\n", "a word where a number goes");
    refuses("nothing here\n", "a file that is not a cube");

    // an HDR value clamps rather than wrapping
    write("/tmp/t.cube", "LUT_3D_SIZE 2\n"
        "-0.5 0 0\n4.0 0 0\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n");
    ColourLut hdr; check(hdr.load("/tmp/t.cube", err), "loads out-of-range values");
    check((hdr.texels()[0] & 0xFF) == 0u && (hdr.texels()[1] & 0xFF) == 255u, "out-of-range clamps");

    std::printf(failures ? "\n%d failed\n" : "\nall colour LUT tests pass\n", failures);
    return failures != 0;
}
