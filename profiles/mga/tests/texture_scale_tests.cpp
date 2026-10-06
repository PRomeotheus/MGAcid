#include "common/texture_scale.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace psp::gpu;
int main(){
    std::vector<std::uint32_t> p(16*16, 0xFF0000FFu);
    std::uint32_t w=16,h=16;
    assert(!scale_texture(p,w,h,1,TextureScaleMode::Sharp));      // factor 1: no-op
    assert(w==16&&h==16);
    std::vector<std::uint32_t> empty; std::uint32_t zw=0,zh=0;
    assert(!scale_texture(empty,zw,zh,4,TextureScaleMode::Sharp)); // degenerate
    std::vector<std::uint32_t> big(600*600,0u); std::uint32_t bw=600,bh=600;
    assert(!scale_texture(big,bw,bh,4,TextureScaleMode::Sharp));   // over the edge cap
    assert(bw==600);
    // Detector: one colour is pixel art; a gradient is not.
    std::vector<std::uint32_t> flat(64, 0xFF112233u);
    assert(looks_like_pixel_art(flat,8,8));
    std::vector<std::uint32_t> ramp(64);
    for (int i=0;i<64;++i) ramp[i]=0xFF000000u|(std::uint32_t)(i*4);
    assert(!looks_like_pixel_art(ramp,8,8));
    // Fully transparent: nothing to preserve, leave it to the filter.
    std::vector<std::uint32_t> clear(64, 0x00445566u);
    assert(!looks_like_pixel_art(clear,8,8));
    // Pixel mode reproduces the source exactly at an integer factor.
    std::vector<std::uint32_t> art{0xFF0000FFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFF0000FFu};
    std::uint32_t aw=2,ah=2;
    assert(scale_texture(art,aw,ah,4,TextureScaleMode::Pixel));
    assert(aw==8&&ah==8);
    for (std::uint32_t y=0;y<8;++y) for (std::uint32_t x=0;x<8;++x) {
        const std::uint32_t expect = (y/4)*2+(x/4) ? 0u : 0u;  // index below
        (void)expect;
        const std::uint32_t src = ((y/4)*2+(x/4))==0?0xFF0000FFu:
                                  ((y/4)*2+(x/4))==1?0xFFFFFFFFu:
                                  ((y/4)*2+(x/4))==2?0xFFFFFFFFu:0xFF0000FFu;
        assert(art[y*8+x]==src);
    }
    // Colour must not leak out of texels that cannot be seen. A palettised
    // texture keeps a colour under a transparent index, and filtering the
    // channels independently used to pull it into the visible edge.
    {
        std::vector<std::uint32_t> glyph(16 * 16, 0x000000FFu);   // invisible red
        for (int y = 2; y < 14; ++y) glyph[y * 16 + 5] = 0xFFFF0000u;  // opaque blue
        std::uint32_t gw = 16, gh = 16;
        assert(scale_texture(glyph, gw, gh, 4, TextureScaleMode::Smooth));
        int visible = 0, tinted = 0;
        for (std::uint32_t texel : glyph) {
            if (((texel >> 24) & 0xFFu) <= 32u) continue;
            ++visible;
            if ((texel & 0xFFu) > 8u) ++tinted;   // red where only blue belongs
        }
        assert(visible > 0);
        assert(tinted == 0);
    }
    // The two 544x80 title graphics must fit: at 4x they are 2176 wide, which
    // the old 2048 limit refused while enlarging everything around them.
    {
        std::vector<std::uint32_t> wide(544u * 80u, 0xFF203040u);
        std::uint32_t ww = 544, wh = 80;
        assert(scale_texture(wide, ww, wh, 4, TextureScaleMode::Smooth));
        assert(ww == 2176u && wh == 320u);
    }
    std::printf("all guards passed\n");
    return 0;
}
