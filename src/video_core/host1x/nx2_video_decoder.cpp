// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cstring>
#include <optional>
#include <set>

#ifdef __ANDROID__
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#endif

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/host1x/ffmpeg.h"
#include "video_core/host1x/h264_packet.h"
#include "video_core/host1x/nx2_video_decoder.h"

extern "C" {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif

#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

namespace Tegra::Host1x {

namespace {

std::optional<NvdecCommon::VideoCodec> CodecFromMime(std::string_view mime) {
    if (mime.find("vp9") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::VP9;
    }
    if (mime.find("vp8") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::VP8;
    }
    // "video/av01" or "video/av1". FFmpeg decodes AV1 with libdav1d.
    if (mime.find("av01") != std::string_view::npos || mime.find("av1") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::AV1;
    }
    if (mime.find("avc") != std::string_view::npos || mime.find("264") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::H264;
    }
    return std::nullopt;
}

#ifdef __ANDROID__
/// AV1 on Android: the bundled FFmpeg has no AV1 decoder, so the SoC's MediaCodec decodes it.
/// Output is copied into NV12 (P010 for 10-bit) AVFrames, which Render converts as for FFmpeg.
class MediaCodecAv1 {
public:
    MediaCodecAv1(u32 width, u32 height) {
        codec = AMediaCodec_createDecoderByType("video/av01");
        if (codec == nullptr) {
            return;
        }
        AMediaFormat* format = AMediaFormat_new();
        AMediaFormat_setString(format, "mime", "video/av01");
        AMediaFormat_setInt32(format, "width", static_cast<s32>(width ? width : 1920));
        AMediaFormat_setInt32(format, "height", static_cast<s32>(height ? height : 1080));
        AMediaFormat_setInt32(format, "color-format", ColorFormatYuv420SemiPlanar);
        AMediaFormat_setInt32(format, "max-input-size", 4 << 20);
        AMediaFormat_setInt32(format, "low-latency", 1);
        const bool started = AMediaCodec_configure(codec, format, nullptr, nullptr, 0) == AMEDIA_OK &&
                             AMediaCodec_start(codec) == AMEDIA_OK;
        AMediaFormat_delete(format);
        if (!started) {
            AMediaCodec_delete(codec);
            codec = nullptr;
        }
    }

    ~MediaCodecAv1() {
        if (codec != nullptr) {
            AMediaCodec_stop(codec);
            AMediaCodec_delete(codec);
        }
    }

    bool IsOpen() const {
        return codec != nullptr;
    }

    void Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures) {
        if (Queue(data, pts, 0, pictures)) {
            Receive(pictures, FirstOutputWaitUs);
        }
    }

    void Drain(std::vector<Nx2Picture>& pictures) {
        eos_seen = false;
        if (Queue({}, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM, pictures)) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (!eos_seen && std::chrono::steady_clock::now() < deadline) {
                Receive(pictures, 20'000);
            }
        }
        Reset();
    }

    void Reset() {
        if (codec != nullptr) {
            AMediaCodec_flush(codec);
        }
        eos_seen = false;
    }

private:
    static constexpr s32 ColorFormatYuv420Planar = 19;
    static constexpr s32 ColorFormatYuv420SemiPlanar = 21;
    static constexpr s32 ColorFormatYuvP010 = 54;
    static constexpr s64 FirstOutputWaitUs = 4'000;

    struct OutputFormat {
        s32 stride{};
        s32 slice_height{};
        s32 color_format{ColorFormatYuv420SemiPlanar};
        s32 crop_left{};
        s32 crop_top{};
        s32 width{};
        s32 height{};
        s32 standard{};
        s32 range{};
    };

    // Hands one input to the codec, taking pictures out while its input buffers are all busy.
    bool Queue(std::span<const u8> data, u64 pts, u32 flags, std::vector<Nx2Picture>& pictures) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
        while (true) {
            const ssize_t index = AMediaCodec_dequeueInputBuffer(codec, 0);
            if (index >= 0) {
                size_t capacity{};
                u8* const buffer = AMediaCodec_getInputBuffer(codec, static_cast<size_t>(index),
                                                              &capacity);
                const size_t size = buffer != nullptr && data.size() <= capacity ? data.size() : 0;
                if (size != data.size()) {
                    LOG_ERROR(HW_GPU, "nx2 video: AV1 frame of {} bytes exceeds the {} byte input buffer",
                              data.size(), capacity);
                }
                if (size != 0) {
                    std::memcpy(buffer, data.data(), size);
                }
                return AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(index), 0, size,
                                                    pts, flags) == AMEDIA_OK;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                LOG_ERROR(HW_GPU, "nx2 video: MediaCodec took no AV1 input for 500 ms");
                return false;
            }
            Receive(pictures, 10'000);
        }
    }

    void Receive(std::vector<Nx2Picture>& pictures, s64 timeout_us) {
        while (true) {
            AMediaCodecBufferInfo info{};
            const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, timeout_us);
            timeout_us = 0;
            if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                ReadOutputFormat();
                continue;
            }
            if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                continue;
            }
            if (index < 0) {
                return;
            }
            size_t capacity{};
            const u8* const buffer =
                AMediaCodec_getOutputBuffer(codec, static_cast<size_t>(index), &capacity);
            if (buffer != nullptr && info.size > 0 && info.offset >= 0 &&
                static_cast<size_t>(info.offset) <= capacity) {
                if (auto picture = MakePicture(buffer + info.offset, capacity - info.offset,
                                               info.presentationTimeUs)) {
                    pictures.push_back(std::move(*picture));
                }
            }
            eos_seen |= (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
            AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(index), false);
            if (eos_seen) {
                return;
            }
        }
    }

    void ReadOutputFormat() {
        AMediaFormat* format = AMediaCodec_getOutputFormat(codec);
        const auto get = [format](const char* key, s32 fallback) {
            s32 value{};
            return AMediaFormat_getInt32(format, key, &value) ? value : fallback;
        };
        out.width = get("width", 0);
        out.height = get("height", 0);
        out.stride = get("stride", out.width);
        out.slice_height = get("slice-height", out.height);
        out.color_format = get("color-format", ColorFormatYuv420SemiPlanar);
        out.crop_left = get("crop-left", 0);
        out.crop_top = get("crop-top", 0);
        // crop-right and crop-bottom are the last visible column and row
        out.width = get("crop-right", out.width + out.crop_left - 1) - out.crop_left + 1;
        out.height = get("crop-bottom", out.height + out.crop_top - 1) - out.crop_top + 1;
        out.standard = get("color-standard", 0);
        out.range = get("color-range", 0);
        LOG_INFO(HW_GPU,
                 "nx2 video: MediaCodec AV1 output {}x{} (stride {}, slice height {}, crop {},{}), "
                 "color format {}",
                 out.width, out.height, out.stride, out.slice_height, out.crop_left, out.crop_top,
                 out.color_format);
        AMediaFormat_delete(format);
    }

    std::optional<Nx2Picture> MakePicture(const u8* data, size_t size, s64 pts) {
        const bool planar = out.color_format == ColorFormatYuv420Planar;
        const bool p010 = out.color_format == ColorFormatYuvP010;
        const s32 bytes = p010 ? 2 : 1;
        if (out.width <= 0 || out.height <= 0 || out.stride < out.width * bytes ||
            out.slice_height < out.height + out.crop_top ||
            size < static_cast<size_t>(out.stride) * static_cast<size_t>(out.slice_height) * 3 / 2) {
            LOG_ERROR(HW_GPU,
                      "nx2 video: unusable MediaCodec AV1 output ({} bytes for {}x{}, stride {}, "
                      "slice height {})",
                      size, out.width, out.height, out.stride, out.slice_height);
            return std::nullopt;
        }
        auto frame = std::make_shared<FFmpeg::Frame>();
        AVFrame* const f = frame->GetFrame();
        f->format = planar ? AV_PIX_FMT_YUV420P : p010 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
        f->width = out.width;
        f->height = out.height;
        if (av_frame_get_buffer(f, 0) < 0) {
            return std::nullopt;
        }
        const size_t stride = static_cast<size_t>(out.stride);
        const u8* const luma = data + stride * static_cast<size_t>(out.crop_top) +
                               static_cast<size_t>(out.crop_left * bytes);
        for (s32 y = 0; y < out.height; ++y) {
            std::memcpy(f->data[0] + y * f->linesize[0], luma + stride * static_cast<size_t>(y),
                        static_cast<size_t>(out.width * bytes));
        }
        const s32 chroma_rows = (out.height + 1) / 2;
        const u8* const chroma_base = data + stride * static_cast<size_t>(out.slice_height);
        if (planar) {
            // U then V, each half the stride wide and half the slice height tall
            const size_t chroma_stride = stride / 2;
            const size_t plane_size = chroma_stride * static_cast<size_t>(out.slice_height / 2);
            for (s32 p = 1; p <= 2; ++p) {
                const u8* const plane = chroma_base + plane_size * static_cast<size_t>(p - 1) +
                                        chroma_stride * static_cast<size_t>(out.crop_top / 2) +
                                        static_cast<size_t>(out.crop_left / 2);
                for (s32 y = 0; y < chroma_rows; ++y) {
                    std::memcpy(f->data[p] + y * f->linesize[p],
                                plane + chroma_stride * static_cast<size_t>(y),
                                static_cast<size_t>((out.width + 1) / 2));
                }
            }
        } else {
            const u8* const plane = chroma_base + stride * static_cast<size_t>(out.crop_top / 2) +
                                    static_cast<size_t>((out.crop_left & ~1) * bytes);
            for (s32 y = 0; y < chroma_rows; ++y) {
                std::memcpy(f->data[1] + y * f->linesize[1],
                            plane + stride * static_cast<size_t>(y),
                            static_cast<size_t>(((out.width + 1) / 2) * 2 * bytes));
            }
        }
        f->pts = pts;
        f->best_effort_timestamp = pts;
        // MediaFormat.COLOR_STANDARD_* and COLOR_RANGE_*; unset stays unspecified
        f->colorspace = out.standard == 1   ? AVCOL_SPC_BT709
                        : out.standard == 6 ? AVCOL_SPC_BT2020_NCL
                        : out.standard == 2 || out.standard == 4 ? AVCOL_SPC_SMPTE170M
                                                                 : AVCOL_SPC_UNSPECIFIED;
        f->color_range = out.range == 1   ? AVCOL_RANGE_JPEG
                         : out.range == 2 ? AVCOL_RANGE_MPEG
                                          : AVCOL_RANGE_UNSPECIFIED;
        return Nx2Picture{static_cast<u32>(out.width), static_cast<u32>(out.height),
                          static_cast<u64>(pts), std::move(frame)};
    }

    AMediaCodec* codec{};
    OutputFormat out;
    bool eos_seen{};
};
#endif

} // namespace

bool IsH264Stream(std::span<const u8> data) {
    if (data.size() >= 4) {
        // Annex-B 3-byte start code: 00 00 01 <nal_header>
        if (data[0] == 0 && data[1] == 0 && data[2] == 1) {
            const u8 nal_type = data[3] & 0x1F;
            if ((data[3] & 0x80) == 0 && nal_type >= 1 && nal_type <= 23) {
                return true;
            }
        }
    }
    if (data.size() >= 5) {
        // Annex-B 4-byte start code: 00 00 00 01 <nal_header>
        if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
            const u8 nal_type = data[4] & 0x1F;
            if ((data[4] & 0x80) == 0 && nal_type >= 1 && nal_type <= 23) {
                return true;
            }
        }
        // avcC / MP4 length-prefixed NAL unit: [u32 be length] <nal_header>
        const u32 nalu_len = (static_cast<u32>(data[0]) << 24) |
                             (static_cast<u32>(data[1]) << 16) |
                             (static_cast<u32>(data[2]) << 8) |
                             static_cast<u32>(data[3]);
        if (nalu_len > 0 && nalu_len <= data.size() - 4) {
            const u8 nal_type = data[4] & 0x1F;
            if ((data[4] & 0x80) == 0 && nal_type >= 1 && nal_type <= 23) {
                return true;
            }
        }
    }
    if (data.size() >= 7) {
        // avcC configuration record: version=1, profile, compat, level, lengthSizeMinusOne | 0xFC, numSPS | 0xE0
        if (data[0] == 1 && (data[4] & 0xFC) == 0xFC && (data[5] & 0xE0) == 0xE0 && (data[5] & 0x1F) > 0) {
            return true;
        }
    }
    return false;
}

struct Nx2VideoDecoder::Impl {
    std::string name;
    std::optional<FFmpeg::Decoder> decoder;
    std::optional<FFmpeg::HardwareContext> hardware;
    std::optional<FFmpeg::DecoderContext> context;
#ifdef __ANDROID__
    std::optional<MediaCodecAv1> mediacodec;
#endif
    SwsContext* scaler{};
    bool open{};
    H264PacketConverter h264_packets;
    /// Timestamps of submitted frames not yet output. Frames copied back from a host GPU decoder
    /// lose theirs and take the earliest one instead.
    std::multiset<u64> pending_pts;
    std::vector<u8> packet; ///< FFmpeg reads up to AV_INPUT_BUFFER_PADDING_SIZE past the data

    ~Impl() {
        sws_freeContext(scaler);
        context.reset();
        hardware.reset();
    }

    void Receive(std::vector<Nx2Picture>& pictures) {
        while (auto frame = context->ReceiveFrame()) {
            const AVFrame* f = frame->GetFrame();
            const s64 stamp = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp
                                                                         : f->pts;
            u64 pts{};
            if (stamp != AV_NOPTS_VALUE) {
                pts = static_cast<u64>(stamp);
                if (const auto it = pending_pts.find(pts); it != pending_pts.end()) {
                    pending_pts.erase(it);
                }
            } else if (!pending_pts.empty()) {
                pts = *pending_pts.begin();
                pending_pts.erase(pending_pts.begin());
            }
            pictures.push_back({static_cast<u32>(f->width), static_cast<u32>(f->height), pts,
                                std::move(frame)});
        }
    }
};

Nx2VideoDecoder::Nx2VideoDecoder(std::string_view mime, [[maybe_unused]] u32 width,
                                 [[maybe_unused]] u32 height)
    : impl{std::make_unique<Impl>()} {
    impl->name = std::string{mime};
    const auto start = std::chrono::steady_clock::now();
    const auto codec = CodecFromMime(mime);
    if (!codec) {
        LOG_ERROR(HW_GPU, "nx2 video: no decoder for codec \"{}\"", mime);
        return;
    }
    impl->decoder.emplace(*codec);
    if (impl->decoder->GetCodec() == nullptr) {
#ifdef __ANDROID__
        if (*codec == NvdecCommon::VideoCodec::AV1) {
            impl->mediacodec.emplace(width, height);
            impl->open = impl->mediacodec->IsOpen();
            if (impl->open) {
                LOG_INFO(HW_GPU, "nx2 video: \"{}\" decodes on the GPU with MediaCodec", mime);
            } else {
                LOG_ERROR(HW_GPU, "nx2 video: MediaCodec cannot open an AV1 decoder for \"{}\"",
                          mime);
            }
            return;
        }
#endif
        LOG_ERROR(HW_GPU, "nx2 video: FFmpeg has no decoder for \"{}\"", mime);
        return;
    }
    impl->context.emplace(*impl->decoder);
    AVCodecContext* const context = impl->context->GetCodecContext();
    const std::string_view decoder_name = impl->decoder->GetCodec()->name;
    const bool gpu = Settings::values.nvdec_emulation.GetValue() == Settings::NvdecEmulation::Gpu;
    if (gpu) {
        impl->hardware.emplace();
        impl->hardware->InitializeForDecoder(*impl->context, *impl->decoder);
    }
    impl->open = impl->context->OpenContext(*impl->decoder);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    if (!impl->open) {
        LOG_ERROR(HW_GPU, "nx2 video: {} failed to open for \"{}\"", decoder_name, mime);
    } else if (context->hw_device_ctx) {
        const auto* device =
            reinterpret_cast<const AVHWDeviceContext*>(context->hw_device_ctx->data);
        LOG_INFO(HW_GPU, "nx2 video: \"{}\" decodes on the GPU ({}) with {}, opened in {} ms", mime,
                 av_hwdevice_get_type_name(device->type), decoder_name, ms);
    } else {
        LOG_INFO(HW_GPU,
                 "nx2 video: \"{}\" decodes on the CPU with {}, {} threads ({}), opened in {} ms",
                 mime, decoder_name,
                 context->thread_count ? std::to_string(context->thread_count) : "default",
                 gpu ? "no usable GPU decoder" : "NVDEC emulation is CPU", ms);
    }
}

Nx2VideoDecoder::~Nx2VideoDecoder() = default;

bool Nx2VideoDecoder::IsOpen() const {
    return impl->open;
}

const std::string& Nx2VideoDecoder::Name() const {
    return impl->name;
}

void Nx2VideoDecoder::Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures) {
    if (!impl->open || data.empty()) {
        return;
    }
#ifdef __ANDROID__
    if (impl->mediacodec) {
        impl->mediacodec->Decode(data, pts, pictures);
        return;
    }
#endif
    impl->packet.assign(data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    std::memcpy(impl->packet.data(), data.data(), data.size());

    // Convert MP4/avcC length-prefixed NALUs to Annex-B start codes for FFmpeg if needed.
    if (impl->name.find("avc") != std::string::npos || impl->name.find("264") != std::string::npos) {
        impl->h264_packets.Convert(std::span<u8>{impl->packet.data(), data.size()});
    }

    FFmpeg::Packet packet{std::span<const u8>{impl->packet.data(), data.size()}};
    packet.GetPacket()->pts = static_cast<s64>(pts);
    // Hidden VP9 frames never come out: keep the set from growing over a long movie.
    if (impl->pending_pts.size() >= 64) {
        impl->pending_pts.erase(impl->pending_pts.begin());
    }
    impl->pending_pts.insert(pts);
    impl->context->SendPacket(packet);
    impl->Receive(pictures);
}

void Nx2VideoDecoder::Drain(std::vector<Nx2Picture>& pictures) {
    if (!impl->open) {
        return;
    }
#ifdef __ANDROID__
    if (impl->mediacodec) {
        impl->mediacodec->Drain(pictures);
        return;
    }
#endif
    avcodec_send_packet(impl->context->GetCodecContext(), nullptr);
    impl->Receive(pictures);
    Reset();
}

void Nx2VideoDecoder::Reset() {
#ifdef __ANDROID__
    if (impl->mediacodec) {
        impl->mediacodec->Reset();
        return;
    }
#endif
    if (impl->open) {
        avcodec_flush_buffers(impl->context->GetCodecContext());
    }
    impl->pending_pts.clear();
    impl->h264_packets.Reset();
}

void Nx2VideoDecoder::Render(const Nx2Picture& picture, Nx2PictureFormat format, u32 width,
                             u32 height, u64 pitch, u64 chroma_offset, std::span<u8> out) {
    const AVFrame* f = picture.frame->GetFrame();
    const u64 needed = format == Nx2PictureFormat::Nv12
                           ? chroma_offset + pitch * ((u64{height} + 1) / 2)
                           : pitch * height;
    const u64 min_pitch = format == Nx2PictureFormat::Nv12 ? ((u64{width} + 1) / 2) * 2 : 0;
    if (width == 0 || height == 0 || pitch < min_pitch || out.size() < needed) {
        return;
    }
    const AVPixelFormat target = format == Nx2PictureFormat::Nv12    ? AV_PIX_FMT_NV12
                                 : format == Nx2PictureFormat::Rgba8 ? AV_PIX_FMT_RGBA
                                                                     : AV_PIX_FMT_BGRA;
    // Frames copied back from a host GPU decoder are already NV12. Copy them as they are:
    // FFmpeg 6.1's swscale leaves the chroma plane of an NV12 to NV12 copy at zero, which shows
    // the movies green.
    if (target == AV_PIX_FMT_NV12 && f->format == AV_PIX_FMT_NV12 &&
        static_cast<u32>(f->width) == width && static_cast<u32>(f->height) == height) {
        for (u32 y = 0; y < height; ++y) {
            std::memcpy(out.data() + y * pitch, f->data[0] + y * f->linesize[0], width);
        }
        for (u32 y = 0; y < (height + 1) / 2; ++y) {
            std::memcpy(out.data() + chroma_offset + y * pitch, f->data[1] + y * f->linesize[1],
                        min_pitch);
        }
        return;
    }
    if (target == AV_PIX_FMT_NV12 && f->format == AV_PIX_FMT_YUV420P &&
        f->color_range != AVCOL_RANGE_JPEG && static_cast<u32>(f->width) == width &&
        static_cast<u32>(f->height) == height) {
        for (u32 y = 0; y < height; ++y) {
            std::memcpy(out.data() + y * pitch, f->data[0] + y * f->linesize[0], width);
        }
        const u32 chroma_width = static_cast<u32>(min_pitch / 2);
        for (u32 y = 0; y < (height + 1) / 2; ++y) {
            const u8* u = f->data[1] + y * f->linesize[1];
            const u8* v = f->data[2] + y * f->linesize[2];
            u8* dst = out.data() + chroma_offset + y * pitch;
            for (u32 x = 0; x < chroma_width; ++x) {
                dst[x * 2] = u[x];
                dst[x * 2 + 1] = v[x];
            }
        }
        return;
    }
    impl->scaler = sws_getCachedContext(impl->scaler, f->width, f->height,
                                        static_cast<AVPixelFormat>(f->format),
                                        static_cast<int>(width), static_cast<int>(height), target,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (impl->scaler == nullptr) {
        LOG_ERROR(HW_GPU, "nx2 video: cannot convert {} to {}",
                  av_get_pix_fmt_name(static_cast<AVPixelFormat>(f->format)),
                  av_get_pix_fmt_name(target));
        return;
    }
    // The stream's matrix and range; untagged HD video is BT.709.
    int space = SWS_CS_ITU601;
    if (f->colorspace == AVCOL_SPC_BT709 ||
        (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720)) {
        space = SWS_CS_ITU709;
    } else if (f->colorspace == AVCOL_SPC_BT2020_NCL || f->colorspace == AVCOL_SPC_BT2020_CL) {
        space = SWS_CS_BT2020;
    }
    const bool full_range_source = f->color_range == AVCOL_RANGE_JPEG ||
                                  f->format == AV_PIX_FMT_YUVJ420P ||
                                  f->format == AV_PIX_FMT_YUVJ422P ||
                                  f->format == AV_PIX_FMT_YUVJ444P ||
                                  f->format == AV_PIX_FMT_YUVJ440P ||
                                  f->format == AV_PIX_FMT_YUVJ411P;
    const int* coefficients = sws_getCoefficients(space);
    sws_setColorspaceDetails(impl->scaler, coefficients, full_range_source, coefficients,
                             format != Nx2PictureFormat::Nv12, 0, 1 << 16, 1 << 16);
    std::array<u8*, 4> planes{out.data(), nullptr, nullptr, nullptr};
    std::array<int, 4> strides{static_cast<int>(pitch), 0, 0, 0};
    if (format == Nx2PictureFormat::Nv12) {
        planes[1] = out.data() + chroma_offset;
        strides[1] = static_cast<int>(pitch);
    }
    sws_scale(impl->scaler, f->data, f->linesize, 0, f->height, planes.data(), strides.data());
}

} // namespace Tegra::Host1x
