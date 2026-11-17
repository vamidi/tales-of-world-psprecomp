#include "tow2_atrac.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace tow2 {
namespace {

std::uint16_t read_be16(const std::vector<std::uint8_t> &data, std::size_t at) {
    if (at + 2u > data.size()) return 0u;
    return static_cast<std::uint16_t>((data[at] << 8u) | data[at + 1u]);
}

void extract_private_payloads(const std::vector<std::uint8_t> &data,
                              std::vector<std::uint8_t> &audio) {
    for (std::size_t pos = 0u; pos + 6u <= data.size();) {
        if (data[pos] != 0x00u || data[pos + 1u] != 0x00u ||
            data[pos + 2u] != 0x01u) {
            ++pos;
            continue;
        }
        const std::uint8_t code = data[pos + 3u];
        if (code != 0xBDu) {
            if (code == 0xBAu) {
                // Pack header: rely on the next start-code scan.
                pos += 4u;
            } else {
                const std::uint16_t length = read_be16(data, pos + 4u);
                if (length == 0u) {
                    pos += 4u;
                } else {
                    pos += 6u + length;
                }
            }
            continue;
        }

        const std::uint16_t length = read_be16(data, pos + 4u);
        const std::size_t end = std::min<std::size_t>(
            data.size(), pos + 6u + static_cast<std::size_t>(length));
        std::size_t at = pos + 6u;
        if (at + 2u > end) {
            pos += 4u;
            continue;
        }

        int c = data[at++];
        while (c == 0xFF && at < end) c = data[at++];
        if ((c & 0xC0) == 0x40) {
            if (at + 1u >= end) break;
            ++at;
            c = data[at++];
        }
        if ((c & 0xC0) == 0x80) {
            if (at + 2u > end) break;
            const int flags = data[at++];
            int header_length = data[at++];
            if ((flags & 0x80) != 0) {
                at += 5u;
                header_length -= 5;
                if ((flags & 0x40) != 0) {
                    at += 5u;
                    header_length -= 5;
                }
            }
            if ((flags & 0x01) != 0 && at < end) {
                const int extension = data[at++];
                --header_length;
                int skip = (extension >> 4) & 0x0B;
                skip += skip & 0x09;
                if ((extension & 0x40) != 0 || skip > header_length)
                    skip = 0;
                at += static_cast<std::size_t>(skip);
                header_length -= skip;
                if ((extension & 0x01) != 0 && at < end) {
                    const int extended_length = data[at++];
                    --header_length;
                    if ((extended_length & 0x7F) != 0 && at < end) {
                        ++at;
                        --header_length;
                    }
                }
            }
            if (header_length > 0) at += static_cast<std::size_t>(header_length);
        }

        // Private stream 1: channel byte, then a 3-byte (or 4-byte for
        // 0xB0..0xBF) audio sub-header, matching MpegDemux.
        if (at >= end) {
            pos = end;
            continue;
        }
        const std::uint8_t channel = data[at++];
        at += 3u;
        if (channel >= 0xB0u && channel <= 0xBFu) ++at;
        if (at < end)
            audio.insert(audio.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                         data.begin() + static_cast<std::ptrdiff_t>(end));
        pos = end;
    }
}

} // namespace

struct AtracPlayer::Impl {
    AVCodecContext *codec{};
    AVFrame *frame{};
    AVPacket *packet{};
    SwrContext *resampler{};
    std::vector<std::int16_t> pcm;
    std::size_t pcm_position{};
    std::uint64_t frames{};

    void release() {
        if (resampler != nullptr) swr_free(&resampler);
        if (frame != nullptr) av_frame_free(&frame);
        if (packet != nullptr) av_packet_free(&packet);
        if (codec != nullptr) avcodec_free_context(&codec);
        pcm.clear();
        pcm_position = 0u;
        frames = 0u;
    }

    bool append_frame(AVFrame *decoded) {
        if (decoded->sample_rate <= 0 || decoded->ch_layout.nb_channels <= 0)
            return false;
        AVChannelLayout out_layout{};
        av_channel_layout_default(&out_layout, 2);
        if (resampler == nullptr) {
            const int result = swr_alloc_set_opts2(
                &resampler, &out_layout, AV_SAMPLE_FMT_S16, decoded->sample_rate,
                &decoded->ch_layout, static_cast<AVSampleFormat>(decoded->format),
                decoded->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&out_layout);
            if (result < 0 || resampler == nullptr) return false;
            if (swr_init(resampler) < 0) return false;
        } else {
            av_channel_layout_uninit(&out_layout);
        }

        const int max_samples = static_cast<int>(av_rescale_rnd(
            swr_get_delay(resampler, decoded->sample_rate) + decoded->nb_samples,
            decoded->sample_rate, decoded->sample_rate, AV_ROUND_UP));
        const std::size_t old_size = pcm.size();
        pcm.resize(old_size + static_cast<std::size_t>(max_samples) * 2u);
        std::uint8_t *output = reinterpret_cast<std::uint8_t *>(pcm.data() + old_size);
        const int converted = swr_convert(
            resampler, &output, max_samples,
            const_cast<const std::uint8_t **>(decoded->extended_data),
            decoded->nb_samples);
        if (converted <= 0) {
            pcm.resize(old_size);
            return false;
        }
        pcm.resize(old_size + static_cast<std::size_t>(converted) * 2u);
        ++frames;
        return true;
    }

    bool decode_payload(const std::uint8_t *payload, int size) {
        av_packet_unref(packet);
        packet->data = const_cast<std::uint8_t *>(payload);
        packet->size = size;
        const int sent = avcodec_send_packet(codec, packet);
        av_packet_unref(packet);
        if (sent < 0 && sent != AVERROR(EAGAIN)) return false;
        while (true) {
            const int received = avcodec_receive_frame(codec, frame);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
            if (received < 0) return false;
            const bool appended = append_frame(frame);
            av_frame_unref(frame);
            if (!appended) return false;
        }
        return true;
    }
};

AtracPlayer::AtracPlayer() : impl_(std::make_unique<Impl>()) {}

AtracPlayer::~AtracPlayer() {
    close();
}

bool AtracPlayer::open(const std::string &path, std::string &error) {
    close();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open Atrac source: " + path;
        return false;
    }
    std::vector<std::uint8_t> data(
        (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.empty()) {
        error = "empty Atrac source";
        return false;
    }

    std::vector<std::uint8_t> audio;
    audio.reserve(data.size() / 4u);
    extract_private_payloads(data, audio);
    if (audio.empty()) {
        error = "no private audio stream found";
        return false;
    }

    int block_align = 0;
    for (std::size_t position = 0u; position + 4u < audio.size(); ++position) {
        if (audio[position] != 0x0Fu || audio[position + 1u] != 0xD0u) continue;
        const std::uint8_t code1 = audio[position + 2u];
        const std::uint8_t code2 = audio[position + 3u];
        const int frame_size = (((code1 & 0x03u) << 8u) | (code2 * 8u)) + 0x10;
        if (frame_size > 0x10 &&
            position + static_cast<std::size_t>(frame_size) <= audio.size()) {
            block_align = frame_size;
        }
        break;
    }

    const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
    if (decoder == nullptr) {
        error = "FFmpeg has no ATRAC3+ decoder";
        return false;
    }
    impl_->codec = avcodec_alloc_context3(decoder);
    impl_->frame = av_frame_alloc();
    impl_->packet = av_packet_alloc();
    if (impl_->codec == nullptr || impl_->frame == nullptr || impl_->packet == nullptr) {
        error = "ATRAC3+ allocation failed";
        close();
        return false;
    }
    impl_->codec->thread_count = 1;
    // PSMF movie audio is 44.1 kHz stereo.  The raw private stream carries no
    // container parameters, so provide them before opening the decoder.
    impl_->codec->sample_rate = 44100;
    impl_->codec->block_align = block_align;
    av_channel_layout_default(&impl_->codec->ch_layout, 2);
    if (avcodec_open2(impl_->codec, decoder, nullptr) < 0) {
        error = "ATRAC3+ decoder open failed";
        close();
        return false;
    }

    std::size_t position = 0u;
    while (position + 4u < audio.size()) {
        if (audio[position] != 0x0Fu || audio[position + 1u] != 0xD0u) {
            ++position;
            continue;
        }
        const std::uint8_t code1 = audio[position + 2u];
        const std::uint8_t code2 = audio[position + 3u];
        const int frame_size = (((code1 & 0x03u) << 8u) | (code2 * 8u)) + 0x10;
        if (frame_size <= 0x10 ||
            position + static_cast<std::size_t>(frame_size) > audio.size()) {
            ++position;
            continue;
        }
        (void)impl_->decode_payload(audio.data() + position + 8u, frame_size - 8);
        position += static_cast<std::size_t>(frame_size);
    }
    if (impl_->pcm.empty()) {
        error = "no Atrac frames decoded";
        close();
        return false;
    }
    return true;
}

void AtracPlayer::close() {
    if (impl_ != nullptr) impl_->release();
}

bool AtracPlayer::is_open() const {
    return impl_ != nullptr && impl_->codec != nullptr;
}

std::uint64_t AtracPlayer::decoded_frames() const {
    return impl_ != nullptr ? impl_->frames : 0u;
}

std::size_t AtracPlayer::read_pcm(std::uint8_t *destination, std::size_t bytes) {
    if (destination == nullptr || bytes == 0u) return 0u;
    std::size_t copied = 0u;
    if (impl_ != nullptr) {
        const std::size_t available =
            (impl_->pcm.size() - impl_->pcm_position) * sizeof(std::int16_t);
        copied = std::min(bytes, available);
        if (copied != 0u) {
            std::memcpy(destination, impl_->pcm.data() + impl_->pcm_position, copied);
            impl_->pcm_position += copied / sizeof(std::int16_t);
        }
    }
    if (copied < bytes)
        std::memset(destination + copied, 0, bytes - copied);
    return copied;
}

} // namespace tow2
