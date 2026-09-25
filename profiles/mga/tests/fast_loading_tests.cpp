#include "kernel/fast_loading.hpp"
#include <cstdio>
#include <string>
using namespace mga::fast_loading;
static int failures = 0;
static void expect(bool ok, const std::string &what) {
    if (!ok) { std::printf("  FAIL %s\n", what.c_str()); ++failures; }
}
int main() {
    const Guards on{true, false, false, false, false};
    {   // a silent disc read runs fast; reads stopping ends it
        Detector d; d.disc_read(1'000'000);
        expect(d.update(1'000'000, on), "reads + silence -> fast");
        expect(d.update(1'400'000, on), "still fast inside the read window");
        expect(!d.update(1'600'000, on), "past the window -> real time");
        expect(d.reason() == Reason::NotLoading, "reason is reads stopped");
    }
    {   // one audible sample ends it at once and is played
        Detector d; d.disc_read(1'000'000); d.update(1'000'000, on);
        expect(d.audio(1'000'000, 0), "silence while fast is dropped");
        expect(!d.audio(1'010'000, 1), "one non-zero sample is played");
        expect(!d.fast() && d.reason() == Reason::Sound, "sound ends it at once");
        d.disc_read(1'100'000);
        expect(!d.update(1'100'000, on), "still quiet-blocked just after sound");
        expect(d.update(1'270'000, on), "fast again once quiet long enough");
    }
    {   // every guard holds real time on its own
        struct { const char *name; Guards g; Reason want; } cases[] = {
            {"disabled", {false,false,false,false,false}, Reason::Disabled},
            {"button",   {true, true, false,false,false}, Reason::Buttons},
            {"movie",    {true, false,true, false,false}, Reason::Movie},
            {"online",   {true, false,false,true, false}, Reason::Online},
            {"menu",     {true, false,false,false,true }, Reason::Menu},
        };
        for (auto &c : cases) {
            Detector d; d.disc_read(1'000'000);
            expect(!d.update(1'000'000, c.g), std::string(c.name) + " keeps real time");
            expect(d.reason() == c.want, std::string(c.name) + " reports itself");
        }
    }
    {   // never fast before the game has read anything
        Detector d;
        expect(!d.update(5'000'000, on), "no read yet -> real time");
        expect(!d.audio(5'000'000, 0), "silence before any read is played");
    }
    std::printf(failures ? "\n%d failures\n" : "\nall fast-loading tests pass\n", failures);
    return failures != 0;
}
