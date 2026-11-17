#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace tow2 {

// Decodes the private Atrac stream embedded in the title's PSMF container.
// The guest receives interleaved 16-bit stereo PCM through sceMpegAtracDecode.
class AtracPlayer {
public:
    AtracPlayer();
    ~AtracPlayer();

    AtracPlayer(const AtracPlayer &) = delete;
    AtracPlayer &operator=(const AtracPlayer &) = delete;

    bool open(const std::string &path, std::string &error);
    void close();
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] std::uint64_t decoded_frames() const;

    // Copies up to `bytes` bytes of PCM. Missing data is zero-filled so the
    // guest's fixed-size PCM buffer is always fully defined.
    std::size_t read_pcm(std::uint8_t *destination, std::size_t bytes);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tow2
