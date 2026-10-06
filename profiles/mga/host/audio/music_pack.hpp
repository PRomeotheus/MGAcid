#pragma once

// Replacement music: a higher-quality recording played in place of a cue's
// ATRAC3plus stream.
//
//   <data>/music/<key>.wav     a replacement, played instead of the guest's
//                              own compressed audio
//
// The naming follows the texture pack: a 64-bit key derived from the cue's own
// bytes, so nothing has to be renamed and no manifest has to be edited.
//
// What the key is made of
// -----------------------
// The header fields that describe the stream, plus the first kMusicKeyBytes of
// the compressed audio. Both halves are needed. The header alone is not enough
// because Ac!d ships cues with identical length, loop points and block size but
// different music -- B504_nekal and B505_info are one such pair, B722/B724/B725
// another -- and keying on the header would quietly play one cue's replacement
// over another's. The audio alone would be enough, but including the header
// costs nothing and catches a file that was re-encoded rather than replaced.
//
// Cues that are genuinely byte-identical -- Ac!d has five such demo stingers --
// land on one key and share a replacement, which is what we want.
//
// Why the file needs no offset beside it
// --------------------------------------
// A replacement is stored already lined up with the stream it stands in for:
// sample 0 of the file is sample 0 of the DECODED stream, the same timeline
// load_frame indexes, including the encoder and decoder delay the real stream
// carries before its first playable sample.
//
// That is what lets the game keep its own loop points. The runtime goes on
// computing positions, loop ends and remain-frame counts from the .a3p header
// exactly as it did; only the samples come from somewhere else. Lining the file
// up is the job of tools/build_music_pack.py, which measures the offset by
// correlation and bakes it in, so nothing here has to know that a replacement
// came from a differently-mastered recording.
//
// Playback streams from disk through a read-ahead window rather than loading
// whole tracks. A two-minute replacement is about 21 MB at 44.1 kHz stereo, and
// reading that during a stage change is the kind of stall this port has had to
// hunt down before; a 256 KB window turns it into one read every thirty-odd
// frames.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mga::audio {

// How much of the compressed stream goes into the key. Comfortably inside the
// 4 KB the header parser already copies out of the guest buffer: the data chunk
// of every cue in the game starts before offset 200.
constexpr std::size_t kMusicKeyBytes = 1024u;

// `audio` points at the first kMusicKeyBytes of the data chunk. A short buffer
// yields 0, meaning "no identity": the caller then plays the guest's own audio
// rather than risk matching the wrong replacement.
[[nodiscard]] std::uint64_t music_key(std::uint32_t file_size, std::uint32_t data_size,
                                      std::uint32_t channels, std::uint32_t block_align,
                                      std::uint32_t codec, const std::uint8_t *audio,
                                      std::size_t audio_bytes);

[[nodiscard]] std::string music_key_name(std::uint64_t key);

// One replacement, read on demand. 16-bit stereo at 44.1 kHz, which is what
// every cue in the game decodes to; anything else is refused at open() because
// the sample timeline is what keeps the loop points meaningful.
class MusicTrack {
public:
    [[nodiscard]] bool open(const std::filesystem::path &path);

    // Writes `frames` interleaved stereo frames starting at decoded-stream
    // frame `at`. Reads past the end, and before the start, give silence --
    // a replacement shorter than the cue it stands in for fades out rather
    // than reading someone else's memory.
    void read(std::int64_t at, std::int16_t *out, std::size_t frames);

    [[nodiscard]] std::int64_t frames() const noexcept { return frames_; }

private:
    bool fill(std::int64_t at);

    std::ifstream file_;
    std::int64_t data_offset_{};
    std::int64_t frames_{};
    std::vector<std::int16_t> window_;
    std::int64_t window_at_{-1};
    std::int64_t window_frames_{};
};

class MusicPack {
public:
    // Points the pack at the folder the replacements are in. Nothing is read
    // here beyond the directory listing. Named open_folder for the same reason
    // TexturePack's is: it used to derive the folder from the data directory.
    void open_folder(const std::filesystem::path &folder);

    [[nodiscard]] bool available() const noexcept { return available_; }

    // The replacement for this cue, or null. Each cue is opened once per call;
    // a key with no file is remembered so a cue that restarts every few seconds
    // does not keep re-testing the filesystem.
    [[nodiscard]] std::unique_ptr<MusicTrack> open_for(std::uint64_t key);

private:
    std::filesystem::path folder_;
    std::unordered_map<std::uint64_t, bool> known_;
    bool available_{false};
};

[[nodiscard]] MusicPack &music_pack();

} // namespace mga::audio
