#include "tow1_mpeg.hpp"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

namespace tow1 {
namespace {

std::string av_error_string(int error) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

} // namespace

struct MpegPlayer::Impl {
    AVFormatContext *format{};
    AVCodecContext *codec{};
    SwsContext *scale{};
    AVPacket *packet{};
    AVFrame *frame{};
    int video_stream = -1;
    int width = 0;
    int height = 0;
    AVPixelFormat source_format = AV_PIX_FMT_NONE;
    bool eof = false;
    std::uint64_t frames = 0;

    void release() {
        if (scale != nullptr) {
            sws_freeContext(scale);
            scale = nullptr;
        }
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (codec != nullptr) {
            avcodec_free_context(&codec);
        }
        if (format != nullptr) {
            avformat_close_input(&format);
        }
        video_stream = -1;
        width = 0;
        height = 0;
        source_format = AV_PIX_FMT_NONE;
        eof = false;
        frames = 0;
    }

    bool prepare_scale() {
        const int frame_width = frame->width;
        const int frame_height = frame->height;
        if (frame_width <= 0 || frame_height <= 0) return false;
        if (scale == nullptr || width != frame_width || height != frame_height ||
            source_format != frame->format) {
            scale = sws_getCachedContext(
                scale, frame_width, frame_height,
                static_cast<AVPixelFormat>(frame->format),
                frame_width, frame_height, AV_PIX_FMT_RGBA,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (scale == nullptr) return false;
            width = frame_width;
            height = frame_height;
            source_format = static_cast<AVPixelFormat>(frame->format);
        }
        return true;
    }
};

MpegPlayer::MpegPlayer() : impl_(std::make_unique<Impl>()) {}

MpegPlayer::~MpegPlayer() {
    close();
}

bool MpegPlayer::open(const std::string &path, std::string &error) {
    close();
    int result = avformat_open_input(&impl_->format, path.c_str(), nullptr, nullptr);
    if (result < 0) {
        error = "avformat_open_input: " + av_error_string(result);
        impl_->release();
        return false;
    }
    result = avformat_find_stream_info(impl_->format, nullptr);
    if (result < 0) {
        error = "avformat_find_stream_info: " + av_error_string(result);
        impl_->release();
        return false;
    }

    const AVCodec *decoder = nullptr;
    impl_->video_stream = av_find_best_stream(
        impl_->format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (impl_->video_stream < 0 || decoder == nullptr) {
        error = "no H.264 video stream";
        impl_->release();
        return false;
    }

    impl_->codec = avcodec_alloc_context3(decoder);
    if (impl_->codec == nullptr) {
        error = "avcodec_alloc_context3 failed";
        impl_->release();
        return false;
    }
    result = avcodec_parameters_to_context(
        impl_->codec, impl_->format->streams[impl_->video_stream]->codecpar);
    if (result < 0) {
        error = "avcodec_parameters_to_context: " + av_error_string(result);
        impl_->release();
        return false;
    }
    impl_->codec->thread_count = 1;
    result = avcodec_open2(impl_->codec, decoder, nullptr);
    if (result < 0) {
        error = "avcodec_open2: " + av_error_string(result);
        impl_->release();
        return false;
    }

    impl_->packet = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    if (impl_->packet == nullptr || impl_->frame == nullptr) {
        error = "FFmpeg frame/packet allocation failed";
        impl_->release();
        return false;
    }
    impl_->eof = false;
    return true;
}

void MpegPlayer::close() {
    if (impl_ != nullptr) impl_->release();
}

bool MpegPlayer::is_open() const {
    return impl_ != nullptr && impl_->format != nullptr;
}

std::uint64_t MpegPlayer::decoded_frames() const {
    return impl_ != nullptr ? impl_->frames : 0u;
}

bool MpegPlayer::decode_next(std::vector<std::uint8_t> &rgba, int &width, int &height) {
    if (!is_open()) return false;

    bool have_packet = false;
    while (true) {
        const int receive = avcodec_receive_frame(impl_->codec, impl_->frame);
        if (receive == 0) {
            if (!impl_->prepare_scale()) return false;
            rgba.resize(static_cast<std::size_t>(impl_->width) *
                        static_cast<std::size_t>(impl_->height) * 4u);
            std::uint8_t *destination[4] = {rgba.data(), nullptr, nullptr, nullptr};
            const int destination_stride[4] = {impl_->width * 4, 0, 0, 0};
            sws_scale(impl_->scale, impl_->frame->data, impl_->frame->linesize, 0,
                      impl_->height, destination, destination_stride);
            width = impl_->width;
            height = impl_->height;
            ++impl_->frames;
            av_frame_unref(impl_->frame);
            return true;
        }
        if (receive == AVERROR_EOF) return false;
        if (receive != AVERROR(EAGAIN)) return false;
        if (impl_->eof) return false;

        if (!have_packet) {
            int read = 0;
            do {
                read = av_read_frame(impl_->format, impl_->packet);
                if (read < 0) {
                    impl_->eof = true;
                    avcodec_send_packet(impl_->codec, nullptr);
                    break;
                }
                if (impl_->packet->stream_index == impl_->video_stream) {
                    have_packet = true;
                    break;
                }
                av_packet_unref(impl_->packet);
            } while (true);
            if (!have_packet) continue;
        }

        const int sent = avcodec_send_packet(impl_->codec, impl_->packet);
        if (sent == 0) {
            av_packet_unref(impl_->packet);
            have_packet = false;
        } else if (sent != AVERROR(EAGAIN)) {
            return false;
        }
    }
}

} // namespace tow1
