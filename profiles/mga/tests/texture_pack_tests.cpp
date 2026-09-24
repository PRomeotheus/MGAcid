// Replacement textures: that a dump can be read back exactly, and that every
// PNG layout an image editor produces is understood.
//
// The round trip is the test that matters. The workflow a pack author follows is
// dump, edit, drop back in, so a dump that cannot be read again -- or that comes
// back with its channels swapped or its alpha lost -- makes the whole feature
// useless in a way no amount of looking at a screenshot would reveal.
//
// The fixtures are written here rather than kept as files, because the point is
// to cover the layouts rather than any particular picture, and a folder of tiny
// binaries in the repository would not say which layout each one was.

#include "gpu/texture_pack.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

namespace fs = std::filesystem;
using mga::gpu::PackedTexture;
using mga::gpu::TexturePack;

namespace {

int failures = 0;

void check(bool ok, const std::string &what) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
    if (!ok) ++failures;
}

// A 4x4 test picture in the requested layout, written as a PNG. Returns false
// when this build of FFmpeg cannot encode that layout, which is not a failure
// of the code under test.
bool write_fixture(const fs::path &path, AVPixelFormat format) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (codec == nullptr) return false;
    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (context == nullptr) return false;
    context->width = 4;
    context->height = 4;
    context->pix_fmt = format;
    context->time_base = {1, 1};
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    bool ok = false;
    if (frame != nullptr && packet != nullptr && avcodec_open2(context, codec, nullptr) >= 0) {
        frame->format = format;
        frame->width = context->width;
        frame->height = context->height;
        if (av_frame_get_buffer(frame, 0) >= 0) {
            // Whatever the layout, fill every byte of it with something that
            // varies, so a misread shows up as wrong values rather than zeros.
            for (int plane = 0; plane < AV_NUM_DATA_POINTERS && frame->data[plane] != nullptr; ++plane) {
                const int height = plane == 0 ? frame->height : 1;
                const int bytes = plane == 0 ? frame->linesize[0] : 256 * 4;
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < bytes; ++x)
                        frame->data[plane][static_cast<std::ptrdiff_t>(y) * frame->linesize[plane] + x] =
                            static_cast<std::uint8_t>((x * 7 + y * 31 + plane * 11) & 0xFF);
            }
            if (avcodec_send_frame(context, frame) >= 0 && avcodec_receive_packet(context, packet) >= 0) {
                std::ofstream file(path, std::ios::binary);
                file.write(reinterpret_cast<const char *>(packet->data), packet->size);
                ok = file.good();
            }
        }
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&context);
    return ok;
}

} // namespace

int main() {
    if (!TexturePack::supported()) {
        std::cout << "  skip  this build has no PNG codec, so there is nothing to test\n";
        return 0;
    }

    const fs::path root = fs::temp_directory_path() / "mga_texture_pack_tests";
    std::error_code code;
    fs::remove_all(root, code);
    fs::create_directories(root / "textures", code);
    if (code) {
        std::cout << "  FAIL cannot create a working directory: " << code.message() << "\n";
        return 1;
    }

    check(TexturePack::key_name(0x0123456789ABCDEFull) == "0123456789abcdef", "a key names a file in lowercase hex");
    check(TexturePack::key_name(0u) == "0000000000000000", "a zero key keeps all sixteen digits");

    // ---- the round trip ----------------------------------------------------
    // Not a power of two, so a stride the encoder pads is not mistaken for the
    // width; and a varying alpha, which is the channel most easily lost.
    constexpr std::uint32_t kWidth = 7u;
    constexpr std::uint32_t kHeight = 5u;
    std::vector<std::uint32_t> source(static_cast<std::size_t>(kWidth) * kHeight);
    for (std::uint32_t y = 0; y < kHeight; ++y)
        for (std::uint32_t x = 0; x < kWidth; ++x)
            source[y * kWidth + x] = (x * 30u + 1u) | (y * 40u + 2u) << 8u | ((x + y) * 20u + 3u) << 16u |
                                     (255u - y * 10u) << 24u;

    constexpr std::uint64_t kKey = 0xFEEDFACECAFEBEEFull;
    const std::string name = TexturePack::key_name(kKey) + ".png";

    // A pack is only a pack once there is a file in the folder.
    { std::ofstream marker(root / "textures" / "readme.txt"); marker << "pack\n"; }

    TexturePack writer;
    writer.open(root);
    check(writer.available(), "a folder with a file in it counts as a pack");
    writer.set_dumping(true);
    check(writer.dumping(), "dumping can be turned on");
    writer.dump(kKey, kWidth, kHeight, source.data());
    writer.dump(kKey, kWidth, kHeight, source.data());
    check(writer.dumped() == 1u, "the same texture is dumped once, not once a frame");

    const fs::path dumped = root / "textures" / "dump" / name;
    check(fs::exists(dumped, code), "the dump was written");
    check(fs::exists(root / "textures" / "dump" / "index.txt", code), "an index was written beside it");

    // The workflow is dump, edit, drop back in -- so a dump has to be usable as
    // a replacement with nothing renamed and nothing converted.
    fs::copy_file(dumped, root / "textures" / name, fs::copy_options::overwrite_existing, code);
    TexturePack reader;
    reader.open(root);
    const PackedTexture *back = reader.find(kKey);
    check(back != nullptr, "a dump is found again as a replacement");
    if (back != nullptr) {
        check(back->width == kWidth && back->height == kHeight, "its size came back unchanged");
        bool identical = back->pixels.size() == source.size();
        for (std::size_t i = 0; identical && i < source.size(); ++i) identical = back->pixels[i] == source[i];
        check(identical, "every texel came back unchanged, alpha included");
    }
    check(reader.loaded() == 1u, "one replacement was counted");

    // ---- a key with no file, looked up twice -------------------------------
    constexpr std::uint64_t kAbsent = 0x1122334455667788ull;
    check(reader.find(kAbsent) == nullptr, "a key with no file finds nothing");
    check(reader.find(kAbsent) == nullptr, "and finds nothing again, from the cache");

    // ---- the layouts an editor produces ------------------------------------
    struct Layout {
        const char *what;
        AVPixelFormat format;
        std::uint64_t key;
    };
    const Layout layouts[] = {
        {"8-bit colour with alpha", AV_PIX_FMT_RGBA, 0xA000000000000001ull},
        {"8-bit colour without alpha", AV_PIX_FMT_RGB24, 0xA000000000000002ull},
        {"8-bit greyscale", AV_PIX_FMT_GRAY8, 0xA000000000000003ull},
        {"greyscale with alpha", AV_PIX_FMT_YA8, 0xA000000000000004ull},
        {"paletted", AV_PIX_FMT_PAL8, 0xA000000000000005ull},
        {"16-bit colour", AV_PIX_FMT_RGB48BE, 0xA000000000000006ull},
        {"16-bit colour with alpha", AV_PIX_FMT_RGBA64BE, 0xA000000000000007ull},
        {"16-bit greyscale", AV_PIX_FMT_GRAY16BE, 0xA000000000000008ull},
    };
    for (const Layout &layout : layouts) {
        const fs::path path = root / "textures" / (TexturePack::key_name(layout.key) + ".png");
        if (!write_fixture(path, layout.format)) {
            std::cout << "  skip  " << layout.what << ": this FFmpeg cannot encode it\n";
            continue;
        }
        TexturePack pack;
        pack.open(root);
        const PackedTexture *read = pack.find(layout.key);
        check(read != nullptr && read->width == 4u && read->height == 4u && read->pixels.size() == 16u,
              std::string("a PNG saved as ") + layout.what + " is read");
    }

    // ---- a file that is not a PNG at all -----------------------------------
    constexpr std::uint64_t kJunk = 0xB000000000000001ull;
    {
        std::ofstream junk(root / "textures" / (TexturePack::key_name(kJunk) + ".png"), std::ios::binary);
        junk << "this is not a PNG";
    }
    TexturePack tolerant;
    tolerant.open(root);
    check(tolerant.find(kJunk) == nullptr, "a file that is not a PNG is refused rather than crashing");

    fs::remove_all(root, code);
    std::cout << (failures == 0 ? "ALL PASS\n" : "FAILURES\n");
    return failures == 0 ? 0 : 1;
}
