#include "gpu/texture_pack.hpp"

#include <fstream>
#include <iostream>

#if defined(MGA_HAS_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}
#endif

namespace mga::gpu {
namespace {

// A texture pack is meant to raise resolution, not to be a memory leak. Four
// thousand pixels an edge is already far past anything a PSP texture needs.
constexpr std::uint32_t kMaxEdge = 4096u;

#if defined(MGA_HAS_FFMPEG)

// libavcodec's PNG decoder hands back whichever layout the file used, and which
// one that is depends on how whoever made the file saved it. All of the layouts
// an image editor produces for a PNG are handled -- colour and greyscale, with
// and without alpha, paletted, and sixteen bits a channel. Anything else is
// refused by name rather than silently drawn wrong.
[[nodiscard]] bool convert_to_rgba(const AVFrame &frame, PackedTexture &out, std::string &error) {
    const int width = frame.width;
    const int height = frame.height;
    if (width <= 0 || height <= 0 || static_cast<std::uint32_t>(width) > kMaxEdge ||
        static_cast<std::uint32_t>(height) > kMaxEdge) {
        error = "unreasonable size";
        return false;
    }
    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);
    out.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0u);

    const auto pack = [](std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
        return static_cast<std::uint32_t>(r) | static_cast<std::uint32_t>(g) << 8u |
               static_cast<std::uint32_t>(b) << 16u | static_cast<std::uint32_t>(a) << 24u;
    };
    const std::uint8_t *rows = frame.data[0];
    const int stride = frame.linesize[0];
    for (int y = 0; y < height; ++y) {
        const std::uint8_t *row = rows + static_cast<std::ptrdiff_t>(y) * stride;
        std::uint32_t *out_row = out.pixels.data() + static_cast<std::size_t>(y) * out.width;
        switch (frame.format) {
        case AV_PIX_FMT_RGBA:
            for (int x = 0; x < width; ++x) out_row[x] = pack(row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]);
            break;
        case AV_PIX_FMT_RGB24:
            for (int x = 0; x < width; ++x) out_row[x] = pack(row[x * 3], row[x * 3 + 1], row[x * 3 + 2], 0xFFu);
            break;
        case AV_PIX_FMT_GRAY8:
            for (int x = 0; x < width; ++x) out_row[x] = pack(row[x], row[x], row[x], 0xFFu);
            break;
        case AV_PIX_FMT_YA8:  // greyscale with alpha
            for (int x = 0; x < width; ++x)
                out_row[x] = pack(row[x * 2], row[x * 2], row[x * 2], row[x * 2 + 1]);
            break;
        case AV_PIX_FMT_PAL8: {
            // The palette is 256 entries of native-endian ARGB in the second
            // plane, which is why it is read as words rather than bytes.
            const auto *palette = reinterpret_cast<const std::uint32_t *>(frame.data[1]);
            if (palette == nullptr) {
                error = "is paletted but carries no palette";
                return false;
            }
            for (int x = 0; x < width; ++x) {
                const std::uint32_t entry = palette[row[x]];
                out_row[x] = pack(static_cast<std::uint8_t>((entry >> 16u) & 0xFFu),
                                  static_cast<std::uint8_t>((entry >> 8u) & 0xFFu),
                                  static_cast<std::uint8_t>(entry & 0xFFu),
                                  static_cast<std::uint8_t>((entry >> 24u) & 0xFFu));
            }
            break;
        }
        // Sixteen bits a channel, which an editor writes when asked for high
        // colour depth. The extra precision is no use to an 8-bit upload, so
        // the high byte of each channel is taken. PNG is big-endian, and the
        // decoder reports the matching format.
        case AV_PIX_FMT_RGB48BE:
        case AV_PIX_FMT_RGB48LE: {
            const int high = frame.format == AV_PIX_FMT_RGB48BE ? 0 : 1;
            for (int x = 0; x < width; ++x)
                out_row[x] = pack(row[x * 6 + high], row[x * 6 + 2 + high], row[x * 6 + 4 + high], 0xFFu);
            break;
        }
        case AV_PIX_FMT_RGBA64BE:
        case AV_PIX_FMT_RGBA64LE: {
            const int high = frame.format == AV_PIX_FMT_RGBA64BE ? 0 : 1;
            for (int x = 0; x < width; ++x)
                out_row[x] = pack(row[x * 8 + high], row[x * 8 + 2 + high], row[x * 8 + 4 + high],
                                  row[x * 8 + 6 + high]);
            break;
        }
        case AV_PIX_FMT_GRAY16BE:
        case AV_PIX_FMT_GRAY16LE: {
            const int high = frame.format == AV_PIX_FMT_GRAY16BE ? 0 : 1;
            for (int x = 0; x < width; ++x) {
                const std::uint8_t value = row[x * 2 + high];
                out_row[x] = pack(value, value, value, 0xFFu);
            }
            break;
        }
        default:
            error = std::string("unsupported pixel layout ") +
                    (av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame.format)) != nullptr
                         ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame.format))
                         : "?") +
                    " (save it as 8-bit RGB or RGBA)";
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool read_png(const std::filesystem::path &path, PackedTexture &out, std::string &error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot be opened";
        return false;
    }
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (bytes.empty()) {
        error = "is empty";
        return false;
    }

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
    if (codec == nullptr) {
        error = "no PNG decoder in this build of FFmpeg";
        return false;
    }
    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (context == nullptr) {
        error = "out of memory";
        return false;
    }
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    bool ok = false;
    if (packet != nullptr && frame != nullptr && avcodec_open2(context, codec, nullptr) >= 0) {
        // The decoder reads past the end of a packet while scanning, so the
        // buffer has to carry FFmpeg's padding.
        if (av_new_packet(packet, static_cast<int>(bytes.size())) >= 0) {
            std::copy(bytes.begin(), bytes.end(), packet->data);
            if (avcodec_send_packet(context, packet) >= 0 && avcodec_receive_frame(context, frame) >= 0)
                ok = convert_to_rgba(*frame, out, error);
            else
                error = "is not a PNG this decoder accepts";
        } else {
            error = "out of memory";
        }
    } else {
        error = "the PNG decoder would not start";
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    return ok;
}

[[nodiscard]] bool write_png(const std::filesystem::path &path, std::uint32_t width, std::uint32_t height,
                             const std::uint32_t *pixels, std::string &error) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (codec == nullptr) {
        error = "no PNG encoder in this build of FFmpeg";
        return false;
    }
    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (context == nullptr) {
        error = "out of memory";
        return false;
    }
    context->width = static_cast<int>(width);
    context->height = static_cast<int>(height);
    context->pix_fmt = AV_PIX_FMT_RGBA;
    context->time_base = {1, 1};
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    bool ok = false;
    if (frame != nullptr && packet != nullptr && avcodec_open2(context, codec, nullptr) >= 0) {
        frame->format = AV_PIX_FMT_RGBA;
        frame->width = context->width;
        frame->height = context->height;
        if (av_frame_get_buffer(frame, 0) >= 0) {
            for (std::uint32_t y = 0; y < height; ++y) {
                std::uint8_t *row = frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0];
                const std::uint32_t *source = pixels + static_cast<std::size_t>(y) * width;
                for (std::uint32_t x = 0; x < width; ++x) {
                    const std::uint32_t texel = source[x];
                    row[x * 4] = static_cast<std::uint8_t>(texel & 0xFFu);
                    row[x * 4 + 1] = static_cast<std::uint8_t>((texel >> 8u) & 0xFFu);
                    row[x * 4 + 2] = static_cast<std::uint8_t>((texel >> 16u) & 0xFFu);
                    row[x * 4 + 3] = static_cast<std::uint8_t>((texel >> 24u) & 0xFFu);
                }
            }
            if (avcodec_send_frame(context, frame) >= 0 && avcodec_receive_packet(context, packet) >= 0) {
                std::ofstream file(path, std::ios::binary);
                if (file) {
                    file.write(reinterpret_cast<const char *>(packet->data), packet->size);
                    ok = file.good();
                    if (!ok) error = "could not be written";
                } else {
                    error = "could not be created";
                }
            } else {
                error = "the PNG encoder produced nothing";
            }
        } else {
            error = "out of memory";
        }
    } else {
        error = "the PNG encoder would not start";
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&context);
    return ok;
}

#endif // MGA_HAS_FFMPEG

} // namespace

bool TexturePack::supported() {
#if defined(MGA_HAS_FFMPEG)
    return avcodec_find_decoder(AV_CODEC_ID_PNG) != nullptr;
#else
    return false;
#endif
}

std::string TexturePack::key_name(std::uint64_t key) {
    static const char *digits = "0123456789abcdef";
    std::string name(16u, '0');
    for (int i = 15; i >= 0; --i) {
        name[static_cast<std::size_t>(i)] = digits[key & 0xFu];
        key >>= 4u;
    }
    return name;
}

void TexturePack::open(const std::filesystem::path &root) {
    cache_.clear();
    dumped_.clear();
    loaded_ = 0u;
    replacements_ = root / "textures";
    dumps_ = replacements_ / "dump";
    std::error_code code;
    available_ = false;
    if (!std::filesystem::is_directory(replacements_, code)) return;
    // A folder with nothing but the dump subdirectory in it is not a pack.
    for (const auto &entry : std::filesystem::directory_iterator(replacements_, code)) {
        if (entry.is_regular_file(code)) {
            available_ = true;
            break;
        }
    }
    if (available_ && !supported())
        std::cout << "[textures] a pack is present but this build cannot read PNG\n";
}

const PackedTexture *TexturePack::find(std::uint64_t key) {
    if (!available_) return nullptr;
    const auto cached = cache_.find(key);
    if (cached != cache_.end()) return cached->second ? &*cached->second : nullptr;

#if defined(MGA_HAS_FFMPEG)
    const std::filesystem::path path = replacements_ / (key_name(key) + ".png");
    std::error_code code;
    if (!std::filesystem::is_regular_file(path, code)) {
        cache_.emplace(key, std::nullopt);
        return nullptr;
    }
    PackedTexture texture;
    std::string error;
    if (!read_png(path, texture, error)) {
        std::cout << "[textures] " << path.filename().string() << " " << error << "\n";
        cache_.emplace(key, std::nullopt);
        return nullptr;
    }
    ++loaded_;
    std::cout << "[textures] " << path.filename().string() << " -> " << texture.width << "x" << texture.height << "\n";
    return &*cache_.emplace(key, std::move(texture)).first->second;
#else
    cache_.emplace(key, std::nullopt);
    return nullptr;
#endif
}

void TexturePack::set_dumping(bool on) {
    if (dumping_ == on) return;
    dumping_ = on;
    if (!on) return;
    if (!supported()) {
        std::cout << "[textures] this build cannot write PNG, so nothing will be dumped\n";
        dumping_ = false;
        return;
    }
    std::error_code code;
    std::filesystem::create_directories(dumps_, code);
    if (code) {
        std::cout << "[textures] cannot create " << dumps_.string() << ": " << code.message() << "\n";
        dumping_ = false;
        return;
    }
    std::cout << "[textures] dumping to " << dumps_.string() << "\n";
}

void TexturePack::dump(std::uint64_t key, std::uint32_t width, std::uint32_t height, const std::uint32_t *pixels) {
    if (!dumping_ || pixels == nullptr || width == 0u || height == 0u) return;
    if (dumped_.find(key) != dumped_.end()) return;
    dumped_.emplace(key, true);
#if defined(MGA_HAS_FFMPEG)
    const std::string name = key_name(key);
    std::string error;
    if (!write_png(dumps_ / (name + ".png"), width, height, pixels, error)) {
        std::cout << "[textures] " << name << ".png " << error << "\n";
        return;
    }
    // An index, because a folder of hashes says nothing about what is in it.
    // Appended as each one is written, so it survives a crash mid-session.
    std::ofstream index(dumps_ / "index.txt", std::ios::app);
    if (index) index << name << "  " << width << "x" << height << "\n";
#else
    (void)width;
    (void)height;
#endif
}

} // namespace mga::gpu
