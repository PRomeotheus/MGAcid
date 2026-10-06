#include "music_pack.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <sstream>

namespace mga::audio {
namespace {

// The window traded against a stall: 64Ki frames is 256 KB, about thirty-two
// 2048-sample ATRAC3plus frames, so playback touches the disk roughly once a
// second instead of once a frame.
constexpr std::int64_t kWindowFrames = 64 * 1024;

std::uint64_t fnv1a(std::uint64_t hash, const std::uint8_t *bytes, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

std::uint64_t fnv1a_u32(std::uint64_t hash, std::uint32_t value) {
    const std::array<std::uint8_t, 4> bytes{
        static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8u),
        static_cast<std::uint8_t>(value >> 16u), static_cast<std::uint8_t>(value >> 24u)};
    return fnv1a(hash, bytes.data(), bytes.size());
}

template <std::size_t N>
std::uint32_t read_le32(const std::array<std::uint8_t, N> &bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

template <std::size_t N>
std::uint32_t read_le16(const std::array<std::uint8_t, N> &bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u);
}

} // namespace

std::uint64_t music_key(std::uint32_t file_size, std::uint32_t data_size, std::uint32_t channels,
                        std::uint32_t block_align, std::uint32_t codec, const std::uint8_t *audio,
                        std::size_t audio_bytes) {
    if (audio == nullptr || audio_bytes < kMusicKeyBytes) return 0u;
    std::uint64_t hash = 0xcbf29ce484222325ull;
    hash = fnv1a_u32(hash, file_size);
    hash = fnv1a_u32(hash, data_size);
    hash = fnv1a_u32(hash, channels);
    hash = fnv1a_u32(hash, block_align);
    hash = fnv1a_u32(hash, codec);
    return fnv1a(hash, audio, kMusicKeyBytes);
}

std::string music_key_name(std::uint64_t key) {
    std::ostringstream name;
    name << std::hex << std::nouppercase;
    name.width(16);
    name.fill('0');
    name << key;
    return name.str();
}

bool MusicTrack::open(const std::filesystem::path &path) {
    file_.open(path, std::ios::binary);
    if (!file_) return false;

    std::array<std::uint8_t, 12> riff{};
    file_.read(reinterpret_cast<char *>(riff.data()), static_cast<std::streamsize>(riff.size()));
    if (!file_ || std::memcmp(riff.data(), "RIFF", 4) != 0 || std::memcmp(riff.data() + 8, "WAVE", 4) != 0)
        return false;

    bool have_format = false;
    std::int64_t offset = 12;
    while (true) {
        file_.seekg(offset);
        std::array<std::uint8_t, 8> chunk{};
        file_.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        if (!file_) return false;
        const std::uint32_t id = read_le32(chunk, 0u);
        const std::uint32_t size = read_le32(chunk, 4u);
        const std::int64_t body = offset + 8;
        if (id == 0x20746D66u) {  // "fmt "
            if (size < 16u) return false;
            std::array<std::uint8_t, 16> head{};
            file_.read(reinterpret_cast<char *>(head.data()), static_cast<std::streamsize>(head.size()));
            if (!file_) return false;
            const std::uint32_t tag = read_le16(head, 0u);
            const std::uint32_t channels = read_le16(head, 2u);
            const std::uint32_t rate = read_le32(head, 4u);
            const std::uint32_t bits = read_le16(head, 14u);
            // The sample timeline is the whole point: a replacement at another
            // rate would move every loop point the game still believes in. The
            // depth matters just as much -- frames_ below counts four bytes a
            // frame, so a 24-bit or float file would play as noise.
            if (tag != 1u || channels != 2u || rate != 44100u || bits != 16u) return false;
            have_format = true;
        } else if (id == 0x61746164u) {  // "data"
            data_offset_ = body;
            frames_ = static_cast<std::int64_t>(size) / 4;
            break;
        }
        offset = body + size + (size & 1u);
    }
    if (!have_format || frames_ <= 0) return false;
    window_.assign(static_cast<std::size_t>(kWindowFrames) * 2u, 0);
    window_at_ = -1;
    window_frames_ = 0;
    return true;
}

bool MusicTrack::fill(std::int64_t at) {
    const std::int64_t want = std::min<std::int64_t>(kWindowFrames, frames_ - at);
    if (want <= 0) return false;
    file_.clear();
    file_.seekg(data_offset_ + at * 4);
    file_.read(reinterpret_cast<char *>(window_.data()), static_cast<std::streamsize>(want) * 4);
    const std::int64_t got = static_cast<std::int64_t>(file_.gcount()) / 4;
    if (got <= 0) return false;
    window_at_ = at;
    window_frames_ = got;
    return true;
}

void MusicTrack::read(std::int64_t at, std::int16_t *out, std::size_t frames) {
    std::size_t done = 0;
    while (done < frames) {
        const std::int64_t want = at + static_cast<std::int64_t>(done);
        if (want < 0 || want >= frames_) break;
        if (window_at_ < 0 || want < window_at_ || want >= window_at_ + window_frames_) {
            if (!fill(want)) break;
        }
        const std::int64_t inside = want - window_at_;
        const std::size_t run = std::min<std::size_t>(frames - done,
                                                      static_cast<std::size_t>(window_frames_ - inside));
        std::memcpy(out + done * 2u, window_.data() + static_cast<std::size_t>(inside) * 2u, run * 4u);
        done += run;
    }
    if (done < frames) std::memset(out + done * 2u, 0, (frames - done) * 4u);
}

void MusicPack::open_folder(const std::filesystem::path &folder) {
    known_.clear();
    folder_ = folder;
    std::error_code code;
    available_ = false;
    if (!std::filesystem::is_directory(folder_, code)) return;
    for (const auto &entry : std::filesystem::directory_iterator(folder_, code)) {
        if (entry.is_regular_file(code)) {
            available_ = true;
            break;
        }
    }
}

std::unique_ptr<MusicTrack> MusicPack::open_for(std::uint64_t key) {
    if (!available_ || key == 0u) return nullptr;
    const auto seen = known_.find(key);
    if (seen != known_.end() && !seen->second) return nullptr;

    const std::filesystem::path path = folder_ / (music_key_name(key) + ".wav");
    std::error_code code;
    if (!std::filesystem::is_regular_file(path, code)) {
        known_[key] = false;
        return nullptr;
    }
    auto track = std::make_unique<MusicTrack>();
    if (!track->open(path)) {
        known_[key] = false;
        std::cout << "[music] " << music_key_name(key)
                  << ".wav is not 16-bit stereo 44.1 kHz PCM; playing the game's own audio\n";
        return nullptr;
    }
    if (seen == known_.end())
        std::cout << "[music] " << music_key_name(key) << " replaced by "
                  << track->frames() / 44100 << "s of audio\n";
    known_[key] = true;
    return track;
}

MusicPack &music_pack() {
    static MusicPack pack;
    return pack;
}

} // namespace mga::audio
