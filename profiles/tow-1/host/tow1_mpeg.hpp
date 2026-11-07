#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tow1 {

// Host-side PSMF/MPEG playback.  The game owns the MPEG stream and ringbuffer
// bookkeeping; this class only supplies decoded video frames on demand.
class MpegPlayer {
public:
    MpegPlayer();
    ~MpegPlayer();

    MpegPlayer(const MpegPlayer &) = delete;
    MpegPlayer &operator=(const MpegPlayer &) = delete;

    bool open(const std::string &path, std::string &error);
    void close();
    [[nodiscard]] bool is_open() const;

    // Decodes the next video frame into tightly packed RGBA8 (alpha 255).
    // Returns false at end of stream or on decode failure.
    bool decode_next(std::vector<std::uint8_t> &rgba, int &width, int &height);

    [[nodiscard]] std::uint64_t decoded_frames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tow1
