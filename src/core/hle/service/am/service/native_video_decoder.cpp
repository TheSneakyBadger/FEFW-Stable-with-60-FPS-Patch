// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/kernel/fefw_wait_stats.h"
#include "core/core.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <optional>
#include <stop_token>
#include "common/thread.h"
#include <vector>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <limits>
#include "core/memory.h"
#include "core/hle/service/nvdrv/devices/nvhost_as_gpu.h"
#include "video_core/gpu.h"
#include "video_core/textures/decoders.h"
#include <mutex>
#include <string>
#include <fmt/format.h>
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "video_core/host1x/host1x.h"
#include "core/hle/service/am/service/native_video_layout.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/service/kernel_helpers.h"
#include "gav1/decoder.h"
#ifdef FEFW_HAVE_DAV1D
#include <dav1d/dav1d.h>
#endif
#include "video_core/host1x/ffmpeg.h"
#include <map>
#include <string_view>
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/service/am/service/graphics_supervisor_protocol.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/service/am/service/graphics_session.h"
#include "core/hle/service/am/service/native_video_decoder.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/service.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/kernel/kernel.h"

namespace {
// Threads libgav1 decodes AV1 with. Fortune's Weave's 1440p cutscenes took up to ~40 ms a frame
// in their busiest scenes, over the 33 ms a 30 FPS video has, and the game's presentation falls
// to a few FPS whenever its video falls behind. Their frames are one tile each, so more threads
// only help with frame-parallel decoding (several frames at once; libgav1 uses it when there are
// more than 3 threads per tile, about half as many frames as threads). All logical processors but
// 4 (4 to 12); EDEN_AV1_THREADS=N (1-32) overrides it.
int Av1DecodeThreads() {
    static const int threads = [] {
        int count = static_cast<int>(std::thread::hardware_concurrency()) - 4;
        count = (std::clamp)(count, 4, 12);
        if (const char* const value = std::getenv("EDEN_AV1_THREADS")) {
            const int requested = std::atoi(value);
            if (requested >= 1 && requested <= 32) {
                count = requested;
            }
        }
        return count;
    }();
    return threads;
}

// Fortune's Eden: AV1 frames dav1d decodes at once (frame threading). Fortune's Weave's 1440p
// cutscenes are one tile per frame: one frame at a time, dav1d could only share the filtering
// among its threads, and heavy frames (fire, fast camera moves) took 30-43 ms each, over the
// 33 ms a 30 FPS video has, so the movie fell behind and the game faded the dialogue (#22).
// Several at once, a frame still takes as long but one is finished every 10-15 ms.
// Off by default (one at a time, as before): with 4 at once the decode thread stopped inside
// dav1d partway through a fire scene on a Ryzen 9 + RTX 4090 and the movie froze (still being
// looked into). EDEN_AV1_FRAME_DELAY=2-8 turns it on for testing.
int Av1FrameDelay() {
    static const int delay = [] {
        int value = 1;
        if (const char* const env = std::getenv("EDEN_AV1_FRAME_DELAY")) {
            const int requested = std::atoi(env);
            if (requested >= 1 && requested <= 8) {
                value = requested;
            }
        }
        return value;
    }();
    return delay;
}

// EDEN_VIDEO_LOOKAHEAD=N (2-8; off by default) lets the game submit up to N AV1 frames before a
// submission waits for a decoded frame. Each submission normally waits for the oldest frame
// (when the game has an image free), so libgav1 only ever has about one frame to work on and
// frame-parallel decoding can't help: heavy 1440p frames (~50 ms each, one tile) then set the
// cutscene's pace. Experimental (issue #22): fetches and image hand-backs still wait as before.
size_t VideoLookahead() {
    static const size_t lookahead = [] {
        size_t value = 0;
        if (const char* const env = std::getenv("EDEN_VIDEO_LOOKAHEAD")) {
            const int requested = std::atoi(env);
            if (requested >= 2 && requested <= 8) {
                value = static_cast<size_t>(requested);
            }
        }
        if (value != 0) {
            LOG_INFO(Service_AM, "Native video: submissions decode up to {} frames ahead", value);
        }
        return value;
    }();
    return lookahead;
}

// EDEN_VIDEO_ASYNC_OUTPUT=0 writes each AV1 frame inside the call that hands its image back, as
// before. By default an output thread writes it: the game's rendering thread waited in
// ReleaseImage for the next frame to be decoded, converted and written (~26 ms, up to 170 ms in
// Fortune's Weave's heavy 1440p cutscenes), which held presentation to 15-19 FPS (issue #22).
bool AsyncVideoOutput() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_VIDEO_ASYNC_OUTPUT");
        const bool on = value == nullptr || value[0] != '0';
        LOG_INFO(Service_AM, "Native video frames are written {}",
                 on ? "by their own thread" : "in the call that hands back the image");
        return on;
    }();
    return enabled;
}

// Which AV1 decoder runs the cutscenes: dav1d, unless EDEN_AV1_DECODER=libgav1. Fortune's Weave's
// 1440p cutscenes are one tile per frame, and libgav1 only splits work across tiles or whole
// frames, so a heavy frame took ~50 ms on one thread whatever the thread count (issue #22). dav1d
// also splits one tile's frame across threads (superblock rows) and has AVX2/AVX-512 assembly.
bool UseDav1d() {
#ifdef FEFW_HAVE_DAV1D
    static const bool use = [] {
        const char* const value = std::getenv("EDEN_AV1_DECODER");
        const bool dav1d = value == nullptr || std::string_view{value} != "libgav1";
        LOG_INFO(Service_AM, "Native AV1 decoder: {} with {} threads, {} frames at once",
                 dav1d ? "dav1d" : "libgav1", Av1DecodeThreads(), dav1d ? Av1FrameDelay() : 0);
        return dav1d;
    }();
    return use;
#else
    return false;
#endif
}

// EDEN_AV1_FRAME_PARALLEL=0 decodes one frame at a time, as before.
bool Av1FrameParallel() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_AV1_FRAME_PARALLEL");
        const bool on = value == nullptr || value[0] != '0';
        LOG_INFO(Service_AM, "Native AV1 decoding with {} threads{}", Av1DecodeThreads(),
                 on ? ", frame-parallel" : ", one frame at a time");
        return on;
    }();
    return enabled;
}
} // Anonymous namespace

namespace Service::AM {
struct NativeVideoImagePool {
    ~NativeVideoImagePool() {
        auto& nvmap = module->GetContainer().GetNvMapFile();
        nvmap.UnpinHandle(memory_id);
        const auto info = nvmap.FreeHandle(memory_id, true);
        if (info && info->can_unlock && info->address) {
            process->GetPageTable().UnlockForDeviceAddressSpace(info->address, info->size);
        }
    }
    std::shared_ptr<Nvidia::Module> module;
    std::shared_ptr<void> memory_owner;
    Kernel::KProcess* process;
    NativeVideoImageLayout layout;
    u32 first_image_id;
    u32 count;
    u32 memory_id;
    VAddr address;
    DAddr device_address;
    u32 packed_dimensions;
};

struct NativeVideoMapping {
    ~NativeVideoMapping() { address_space->UnmapNative4K(address, size); }
    std::shared_ptr<Nvidia::Devices::nvhost_as_gpu> address_space;
    std::shared_ptr<NativeVideoImagePool> pool;
    u64 address;
    u64 size;
    u32 image_id;
    u32 references{1};
};

// The decoder sessions' own server, on a host thread (made with the first decoder, then kept for
// the rest of the boot). AM's services all run on one emulated core thread, and the decoder's calls
// wait for decoding and convert 1440p frames: in Fortune's Weave's heavy cutscenes they kept that
// thread busy almost all the time (ReleaseImage and Submit ~10 s of every 10 s), so the game's
// rendering thread queued its nvdrv:gsv GPU calls behind them (up to 180 ms each) and presentation
// fell to 5-7 FPS while the movie itself kept up (issue #22).
struct NativeVideoServer {
    explicit NativeVideoServer(Core::System& system_) : system{system_} {}
    Service::ServerManager* Get() {
        std::call_once(once, [this] {
            auto manager = std::make_unique<Service::ServerManager>(system);
            server = manager.get();
            auto holder = std::make_shared<std::unique_ptr<Service::ServerManager>>(std::move(manager));
            system.Kernel()
                .RunOnHostCoreProcess("NativeVideo",
                                      [&system = system, holder] { system.RunServer(std::move(*holder)); })
                .detach();
            LOG_INFO(Service_AM, "Native video decoder calls are served on their own host thread");
        });
        return server;
    }
    Core::System& system;
    std::once_flag once;
    Service::ServerManager* server{};
};

std::shared_ptr<NativeVideoServer> MakeNativeVideoServer(Core::System& system) {
    return std::make_shared<NativeVideoServer>(system);
}

struct NativeVideoState {
    Core::System& system;
    std::shared_ptr<Nvidia::Module> module;
    Kernel::KProcess* process;
    std::shared_ptr<void> memory_owner;
    std::shared_ptr<NativeVideoServer> server;
    std::mutex mutex;
    std::vector<std::shared_ptr<NativeVideoImagePool>> pools;
    std::vector<std::shared_ptr<NativeVideoMapping>> mappings;
};

namespace {
class INativeVideoCodec final : public ServiceFramework<INativeVideoCodec> {
public:
    INativeVideoCodec(Core::System& system_, std::shared_ptr<NativeVideoState> state_, NativeVideoKind kind_)
        : ServiceFramework{system_, kind_ == NativeVideoKind::Encoder ? "INativeVideoEncoder" : "INativeVideoDecoder"}, process{state_->process}, kind{kind_}, context{system_, "NativeVideoCodec"}, state{std::move(state_)} {
        process->Open(kernel);
        static const FunctionInfo functions[] = {
            {3, &INativeVideoCodec::GetSubmissionEvent, "GetSubmissionEvent"},
            {6, &INativeVideoCodec::Start, "Start"},
            {8, &INativeVideoCodec::Flush, "Flush"},
            {207, &INativeVideoCodec::Stop, "Stop"},
            {212, &INativeVideoCodec::UnregisterImage, "UnregisterImage"},
            {311, &INativeVideoCodec::RegisterImage, "RegisterImage"},
            {314, &INativeVideoCodec::ReleaseImage, "ReleaseImage"},
            {213, &INativeVideoCodec::GetOutputProperties, "GetOutputProperties"},
            {101, &INativeVideoCodec::Configure, "Configure"},
            {109, &INativeVideoCodec::Submit, "Submit"},
            {115, &INativeVideoCodec::Finalize, "Finalize"},
            {200, &INativeVideoCodec::InitializeByTransferMemory, "InitializeByTransferMemory200"},
            {201, &INativeVideoCodec::InitializeByTransferMemory, "InitializeByTransferMemory"},
        };
        RegisterHandlersTipc(functions);
    }
    ~INativeVideoCodec() override {
        std::scoped_lock codec_lock{codec_mutex};
        Reset();
        if (submission_event) context.CloseEvent(submission_event);
        process->Close(kernel);
    }
private:
    void Reset() {
        // Joins the output and decode workers (requests stop first), so nothing below races with
        // them. The output worker never waits for codec_mutex (held here), so it sees the stop.
        StopOutputWorker();
        decode_worker = std::jthread{};
        deferred_output_error.reset();
        decode_busy = false;
        decode_error.reset();
        pending_inputs.clear();
        pending_input_bytes = 0;
        output_properties.clear();
        decoded_frames.clear();
        Av1Destroy();
        DropAv1InFlight();
        h264_context.reset();
        h264_decoder.reset();
        h264_timestamps.clear();
        if (transfer_memory) transfer_memory->Close(kernel);
        transfer_memory = nullptr;
        configuration.reset();
        registered_images.clear();
        if (operation_syncpoint) {
            state->module->GetContainer().GetSyncpointManager().FreeSyncpoint(*operation_syncpoint);
            operation_syncpoint.reset();
        }
        started = false;
        flushed = false;
        ResetMovieClock();
        if (submission_event) submission_event->Clear(kernel);
    }

    struct DecodedFrame {
        std::array<std::vector<u8>, 3> planes;
        std::array<u32, 3> widths;
        std::array<u32, 3> heights;
        u32 bitdepth;
        u32 format;
        u32 matrix;
        bool full_range;
        bool eos{};
        u64 submission_id;
        u64 timestamp;
        std::chrono::steady_clock::duration decode_time{};
    };

    // 3, or EDEN_VIDEO_LOOKAHEAD when higher, so the game can keep submitting while libgav1 works.
    static size_t MaximumPendingInputs() {
        return (std::max)(size_t{3}, VideoLookahead());
    }
    // AV1 frames the decode worker may hold decoded ahead of the images the game hands back.
    static constexpr size_t MaximumDecodedAhead = 2;

    struct PendingInput {
        std::vector<u8> bytes;
        u64 submission_id;
        u64 timestamp;
        bool eos;
    };

    void Submit(HLERequestContext& ctx) {
        std::scoped_lock codec_lock{codec_mutex};
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        u64 submission_id{};
        u64 consumed_size{};
        if (packet[0] == 0x10007d && packet[1] == 3 && (packet[7] & 0xff) <= 1) {
            result = Kernel::ResultInvalidState;
            if (started && Initialized() && configuration) {
                const auto input = (!ctx.BufferDescriptorA().empty() && ctx.BufferDescriptorA()[0].Size()) ? ctx.ReadBuffer() : std::span<const u8>{};
                result = Kernel::ResultInvalidSize;
                if (input.size() <= configuration->input_capacity && (input.empty() == bool(packet[7] & 0xff))) {
                    result = Result{0x1c266f};
                    std::unique_lock lock{decode_mutex};
                    if (pending_inputs.size() < MaximumPendingInputs() &&
                        pending_input_bytes + input.size() <= transfer_memory->GetSize()) {
                        submission_id = next_submission_id++;
                        const u64 timestamp = u64{packet[5]} | (u64{packet[6]} << 32);
                        consumed_size = input.size();
                        pending_input_bytes += input.size();
                        pending_inputs.push_back({std::vector<u8>{input.begin(), input.end()}, submission_id, timestamp, bool(packet[7] & 0xff)});
                        lock.unlock();
                        decode_cv.notify_all();
                        result = PumpOutput(/*from_submit=*/true, /*no_wait=*/OutputAsync());
                        RequestOutput();
                    } else {
                        ++submissions_refused;
                        // The game retries while the queue is full (about 30 a second during a
                        // movie): logged only when testing frame threading or timing.
                        if ((Av1FrameDelay() > 1 || VideoTimingEnabled()) &&
                            (submissions_refused == 1 || submissions_refused % 100 == 0)) {
                            LOG_WARNING(Service_AM,
                                        "Native video: submission refused ({} bytes; {} so far): {}",
                                        input.size(), submissions_refused, DecodeState(true));
                        }
                        lock.unlock();
                        if (submission_event) submission_event->Clear(kernel);
                    }
                }
            }
        }
        LOG_DEBUG(Service_AM, "native video submit id={}, pending_bytes={}, result={:#x}", submission_id, pending_input_bytes, result.raw);
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(result.raw);
        rb.Push(result.IsSuccess() ? consumed_size : u64{0});
    }

    bool Initialized() const {
        return Av1Open() || h264_context;
    }

    // H.264 input is accepted as Annex B, or as 4-byte length-prefixed units which are
    // rewritten to start codes.
    static std::vector<u8> ToAnnexB(const std::vector<u8>& data) {
        const bool annex_b = (data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) ||
                             (data.size() >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1);
        if (annex_b) return data;
        std::vector<u8> out;
        size_t pos = 0;
        while (pos + 4 <= data.size()) {
            const size_t length = (size_t{data[pos]} << 24) | (size_t{data[pos + 1]} << 16) |
                                  (size_t{data[pos + 2]} << 8) | size_t{data[pos + 3]};
            pos += 4;
            if (length == 0 || length > data.size() - pos) return data;
            out.insert(out.end(), {0, 0, 0, 1});
            out.insert(out.end(), data.begin() + pos, data.begin() + pos + length);
            pos += length;
        }
        return pos == data.size() ? out : data;
    }

    void CollectH264Frames() {
        while (auto received = h264_context->ReceiveFrame()) {
            const AVFrame* av = received->GetFrame();
            if (av->format != AV_PIX_FMT_YUV420P && av->format != AV_PIX_FMT_YUVJ420P) {
                LOG_ERROR(Service_AM, "Unsupported native H.264 output pixel format {}", av->format);
                continue;
            }
            DecodedFrame frame{};
            frame.bitdepth = 8;
            frame.format = 0;
            frame.matrix = 1;
            frame.full_range = av->format == AV_PIX_FMT_YUVJ420P || av->color_range == AVCOL_RANGE_JPEG;
            for (u32 plane = 0; plane < 3; ++plane) {
                frame.widths[plane] = plane ? (av->width + 1) / 2 : av->width;
                frame.heights[plane] = plane ? (av->height + 1) / 2 : av->height;
                frame.planes[plane].resize(size_t{frame.widths[plane]} * frame.heights[plane]);
                for (u32 row = 0; row < frame.heights[plane]; ++row)
                    std::memcpy(frame.planes[plane].data() + size_t{row} * frame.widths[plane],
                                av->data[plane] + ptrdiff_t{row} * av->linesize[plane], frame.widths[plane]);
            }
            frame.submission_id = static_cast<u64>(av->pts);
            if (const auto it = h264_timestamps.find(frame.submission_id); it != h264_timestamps.end()) {
                frame.timestamp = it->second;
                h264_timestamps.erase(it);
            }
            LOG_DEBUG(Service_AM, "native H.264 decoded submission={}, dimensions={}x{}", frame.submission_id,
                      frame.widths[0], frame.heights[0]);
            decoded_frames.push_back(std::move(frame));
        }
    }

    void DrainH264() {
        avcodec_send_packet(h264_context->GetCodecContext(), nullptr);
        CollectH264Frames();
        avcodec_flush_buffers(h264_context->GetCodecContext());
    }

    Result DecodePendingH264() {
        const auto& input = pending_inputs.front();
        if (input.eos) {
            DrainH264();
            DecodedFrame frame{};
            frame.eos = true;
            frame.submission_id = input.submission_id;
            frame.timestamp = input.timestamp;
            decoded_frames.push_back(std::move(frame));
            pending_inputs.pop_front();
            R_SUCCEED();
        }
        auto bytes = ToAnnexB(input.bytes);
        const size_t size = bytes.size();
        bytes.resize(size + AV_INPUT_BUFFER_PADDING_SIZE);
        FFmpeg::Packet packet(std::span<const u8>{bytes.data(), size});
        packet.GetPacket()->pts = static_cast<s64>(input.submission_id);
        packet.GetPacket()->dts = static_cast<s64>(input.submission_id);
        h264_timestamps[input.submission_id] = input.timestamp;
        LOG_DEBUG(Service_AM, "native H.264 input submission={}, bytes={}, head={:02x}{:02x}{:02x}{:02x}{:02x}",
                  input.submission_id, input.bytes.size(), input.bytes.size() > 0 ? input.bytes[0] : 0,
                  input.bytes.size() > 1 ? input.bytes[1] : 0, input.bytes.size() > 2 ? input.bytes[2] : 0,
                  input.bytes.size() > 3 ? input.bytes[3] : 0, input.bytes.size() > 4 ? input.bytes[4] : 0);
        if (!h264_context->SendPacket(packet)) {
            LOG_ERROR(Service_AM, "Native H.264 packet rejected");
            R_THROW(Kernel::ResultInvalidArgument);
        }
        CollectH264Frames();
        pending_input_bytes -= input.bytes.size();
        pending_inputs.pop_front();
        R_SUCCEED();
    }

    // H.264 decodes synchronously, from PumpOutput. AV1 decodes on the decode worker.
    Result DecodePendingInput() {
        if (pending_inputs.empty()) R_SUCCEED();
        return DecodePendingH264();
    }

    // Copies a frame libgav1 decoded (valid until the next DequeueFrame).
    static DecodedFrame CopyAv1Frame(const libgav1::DecoderBuffer& output, u64 timestamp) {
        DecodedFrame frame{};
        frame.bitdepth = output.bitdepth;
        frame.format = static_cast<u32>(output.image_format);
        frame.matrix = static_cast<u32>(output.matrix_coefficients);
        frame.full_range = output.color_range == kLibgav1ColorRangeFull;
        frame.submission_id = output.user_private_data;
        frame.timestamp = timestamp;
        for (u32 plane = 0; plane < 3; ++plane) {
            frame.widths[plane] = output.displayed_width[plane];
            frame.heights[plane] = output.displayed_height[plane];
            const size_t row_bytes = frame.widths[plane] * (frame.bitdepth > 8 ? 2U : 1U);
            frame.planes[plane].resize(row_bytes * frame.heights[plane]);
            for (u32 row = 0; row < frame.heights[plane]; ++row)
                std::memcpy(frame.planes[plane].data() + row * row_bytes,
                    output.plane[plane] + row * output.stride[plane], row_bytes);
        }
        LOG_DEBUG(Service_AM, "native AV1 decoded submission={}, dimensions={}x{}, depth={}, format={}, matrix={}, range={}",
                  frame.submission_id, frame.widths[0], frame.heights[0], frame.bitdepth, frame.format, frame.matrix, frame.full_range);
        return frame;
    }

#ifdef FEFW_HAVE_DAV1D
    // Copies a picture dav1d decoded into the same frame layout as libgav1's (format 0 is 4:2:0
    // there, matrix and range as in the sequence header), then releases the picture.
    static DecodedFrame CopyDav1dPicture(Dav1dPicture& picture, u64 timestamp) {
        DecodedFrame frame{};
        frame.bitdepth = static_cast<u32>(picture.p.bpc);
        switch (picture.p.layout) {
        case DAV1D_PIXEL_LAYOUT_I420: frame.format = 0; break;
        case DAV1D_PIXEL_LAYOUT_I422: frame.format = 1; break;
        case DAV1D_PIXEL_LAYOUT_I444: frame.format = 2; break;
        default: frame.format = 3; break; // monochrome
        }
        frame.matrix = picture.seq_hdr ? static_cast<u32>(picture.seq_hdr->mtrx) : 2U;
        frame.full_range = picture.seq_hdr && picture.seq_hdr->color_range;
        frame.submission_id = static_cast<u64>(picture.m.timestamp);
        frame.timestamp = timestamp;
        const u32 planes = picture.p.layout == DAV1D_PIXEL_LAYOUT_I400 ? 1U : 3U;
        const u32 ss_x = picture.p.layout == DAV1D_PIXEL_LAYOUT_I420 || picture.p.layout == DAV1D_PIXEL_LAYOUT_I422;
        const u32 ss_y = picture.p.layout == DAV1D_PIXEL_LAYOUT_I420;
        for (u32 plane = 0; plane < 3; ++plane) {
            if (plane >= planes) {
                frame.widths[plane] = frame.heights[plane] = 0;
                continue;
            }
            const u32 width = static_cast<u32>(picture.p.w), height = static_cast<u32>(picture.p.h);
            frame.widths[plane] = plane == 0 ? width : (width + ss_x) >> ss_x;
            frame.heights[plane] = plane == 0 ? height : (height + ss_y) >> ss_y;
            const size_t row_bytes = frame.widths[plane] * (frame.bitdepth > 8 ? 2U : 1U);
            const ptrdiff_t stride = picture.stride[plane == 0 ? 0 : 1];
            const auto* source = static_cast<const u8*>(picture.data[plane]);
            frame.planes[plane].resize(row_bytes * frame.heights[plane]);
            for (u32 row = 0; row < frame.heights[plane]; ++row)
                std::memcpy(frame.planes[plane].data() + row * row_bytes, source + row * stride, row_bytes);
        }
        dav1d_picture_unref(&picture);
        LOG_DEBUG(Service_AM, "native AV1 (dav1d) decoded submission={}, dimensions={}x{}, depth={}, format={}, matrix={}, range={}",
                  frame.submission_id, frame.widths[0], frame.heights[0], frame.bitdepth, frame.format, frame.matrix, frame.full_range);
        return frame;
    }
#endif

    // The decode worker's three calls, on dav1d or libgav1. Enqueue: 0 taken, 1 try again after a
    // dequeue, -1 rejected. Dequeue waits for the oldest frame: false on failure, and no frame when
    // the input showed none. Reset drops everything the decoder holds (end of stream, flush).
    int Av1Enqueue(const PendingInput& input) {
#ifdef FEFW_HAVE_DAV1D
        if (dav1d) {
            // dav1d starts decoding the frame here, on its frame threads alongside the frames
            // before it (Av1FrameDelay). It refuses new data while a picture waits to be taken:
            // the rest of the input is then kept and sent again from Av1Dequeue.
            if (dav1d_pending.sz) return 1;
            Dav1dData data{};
            u8* const bytes = dav1d_data_create(&data, input.bytes.size());
            if (!bytes) return -1;
            std::memcpy(bytes, input.bytes.data(), input.bytes.size());
            data.m.timestamp = static_cast<int64_t>(input.submission_id);
            const int sent = dav1d_send_data(dav1d, &data);
            if (sent == 0 || sent == DAV1D_ERR(EAGAIN)) {
                if (data.sz) {
                    dav1d_pending = data;
                } else {
                    dav1d_data_unref(&data);
                }
                return 0;
            }
            dav1d_data_unref(&data);
            LOG_ERROR(Service_AM, "dav1d rejected an AV1 packet: {}", sent);
            return -1;
        }
#endif
        const auto enqueue = decoder->EnqueueFrame(input.bytes.data(), input.bytes.size(),
                                                   static_cast<int64_t>(input.submission_id), nullptr);
        if (enqueue == libgav1::kStatusTryAgain) return 1;
        if (enqueue != libgav1::kStatusOk) {
            LOG_ERROR(Service_AM, "Native AV1 packet rejected: enqueue={}", static_cast<int>(enqueue));
            return -1;
        }
        return 0;
    }
    bool Av1Dequeue(std::optional<DecodedFrame>& frame, u64 timestamp) {
#ifdef FEFW_HAVE_DAV1D
        if (dav1d) {
            // With frame threading, the first "try again" only says no picture is finished yet:
            // asked again with no new data, dav1d waits for the oldest frame in progress (its
            // drain), and says "try again" only when it has none.
            bool drained = Av1FrameDelay() <= 1;
            while (true) {
                Dav1dPicture picture{};
                const int got = dav1d_get_picture(dav1d, &picture);
                if (got == 0) {
                    frame = CopyDav1dPicture(picture, timestamp);
                    return true;
                }
                if (got != DAV1D_ERR(EAGAIN)) {
                    LOG_ERROR(Service_AM, "dav1d failed to decode an AV1 frame: {}", got);
                    return false;
                }
                if (!dav1d_pending.sz) {
                    if (!drained) {
                        drained = true;
                        continue;
                    }
                    return true; // no frame from what it was given
                }
                drained = Av1FrameDelay() <= 1;
                const int sent = dav1d_send_data(dav1d, &dav1d_pending);
                if (sent != 0 && sent != DAV1D_ERR(EAGAIN)) {
                    dav1d_data_unref(&dav1d_pending);
                    LOG_ERROR(Service_AM, "dav1d rejected an AV1 packet: {}", sent);
                    return false;
                }
                if (!dav1d_pending.sz) dav1d_data_unref(&dav1d_pending);
            }
        }
#endif
        const libgav1::DecoderBuffer* output{};
        const auto dequeue = decoder->DequeueFrame(&output);
        if (dequeue != libgav1::kStatusOk) {
            LOG_ERROR(Service_AM, "Native AV1 packet rejected: dequeue={}", static_cast<int>(dequeue));
            return false;
        }
        if (output) frame = CopyAv1Frame(*output, timestamp);
        return true;
    }
    bool Av1Reset() {
#ifdef FEFW_HAVE_DAV1D
        if (dav1d) {
            dav1d_data_unref(&dav1d_pending);
            dav1d_flush(dav1d);
            return true;
        }
#endif
        return decoder->SignalEOS() == libgav1::kStatusOk;
    }
    void Av1Destroy() {
#ifdef FEFW_HAVE_DAV1D
        if (dav1d) {
            dav1d_data_unref(&dav1d_pending);
            dav1d_close(&dav1d);
        }
#endif
        decoder.reset();
    }
    bool Av1Open() const {
#ifdef FEFW_HAVE_DAV1D
        if (dav1d) return true;
#endif
        return decoder != nullptr;
    }

    static void ReleaseAv1Input(void*, void*) {
        // The worker keeps each input in av1_in_flight until its frame is dequeued, which is
        // after libgav1 releases it (or after SignalEOS / the decoder's destruction drops it).
    }

    void ReleaseInFlightBytes(u64 bytes) {
        pending_input_bytes = pending_input_bytes >= bytes ? pending_input_bytes - bytes : 0;
    }

    // A line for the log when something has waited a while (decode_mutex held). The images
    // only from the service's calls, which own them.
    std::string DecodeState(bool with_images = false) const {
        std::string text = fmt::format(
            "pending {} ({} bytes), in decoder {}, decoded {}, decoder full {}, busy {}, eos {}, "
            "frames at once {}, submissions refused {}",
            pending_inputs.size(), pending_input_bytes, av1_in_flight.size(),
            decoded_frames.size(), av1_decoder_full, decode_busy, eos_in_progress,
            Av1FrameDelay(), submissions_refused);
        if (with_images) {
            text += fmt::format(", images handed back {} of {}",
                                std::ranges::count_if(registered_images,
                                                      [](const auto& image) { return image.available; }),
                                registered_images.size());
        }
        return text;
    }

    // AV1 decode worker, the only user of the libgav1 decoder while decode_busy (Stop and Flush
    // wait for it to be idle before touching the decoder). It hands libgav1 every input it will
    // take, so frame-parallel decoding has several frames to work on at once, then takes the
    // decoded frames in order, up to MaximumDecodedAhead ahead of the images the game hands back,
    // so the game's IPC calls only convert and write a frame that is already decoded.
    // Fortune's Weave's 1440p cutscenes are one tile per frame: decoding one frame at a time
    // used one thread for most of each frame (up to ~40 ms in the busiest scenes, over the 33 ms
    // a 30 FPS video has, whatever the thread count).
    void DecodeWorker(std::stop_token stop) {
        Common::SetCurrentThreadName("NativeVideoDecode");
        std::unique_lock lock{decode_mutex};
        const auto can_enqueue = [&] {
            return !pending_inputs.empty() && !pending_inputs.front().eos && !av1_decoder_full;
        };
        const auto can_dequeue = [&] {
            return !av1_in_flight.empty() && decoded_frames.size() < MaximumDecodedAhead;
        };
        const auto can_end_stream = [&] {
            return !pending_inputs.empty() && pending_inputs.front().eos && av1_in_flight.empty() &&
                   decoded_frames.size() < MaximumDecodedAhead;
        };
        const auto ready = [&] {
            return !decode_error && (can_enqueue() || can_dequeue() || can_end_stream());
        };
        while (true) {
            // Waiting over 2 s mid-movie: say for what (issue #22's frame threading).
            if (!decode_cv.wait_for(lock, stop, std::chrono::seconds{2}, ready)) {
                if (stop.stop_requested()) break;
                if (!pending_inputs.empty() || !av1_in_flight.empty() || !decoded_frames.empty() ||
                    (Av1FrameDelay() > 1 && submissions_refused != 0 &&
                     std::chrono::steady_clock::now() - last_frame_out < std::chrono::seconds{60})) {
                    LOG_WARNING(Service_AM, "Native video decode worker waiting 2 s: {}",
                                DecodeState());
                }
                continue;
            }
            if (stop.stop_requested()) break;
            if (can_enqueue()) {
                PendingInput input = std::move(pending_inputs.front());
                pending_inputs.pop_front();
                decode_busy = true;
                lock.unlock();
                const int enqueue = Av1Enqueue(input);
                lock.lock();
                decode_busy = false;
                if (enqueue == 1) {
                    // The decoder's queue is full: take a frame out first
                    av1_decoder_full = true;
                    pending_inputs.push_front(std::move(input));
                } else if (enqueue != 0) {
                    ReleaseInFlightBytes(input.bytes.size());
                    decode_error = Kernel::ResultInvalidArgument;
                } else {
                    // The decoder has its own copy: the game's transfer memory is free again.
                    // Counted until the frame came out, several heavy frames in progress at once
                    // (2 MB each in the fire scenes) filled it, the game's next submission was
                    // refused, and the movie stopped (frame threading, issue #22).
                    ReleaseInFlightBytes(input.bytes.size());
                    av1_in_flight.push_back(std::move(input));
                }
                decode_cv.notify_all();
                continue;
            }
            if (can_dequeue()) {
                const u64 timestamp = av1_in_flight.front().timestamp;
                decode_busy = true;
                lock.unlock();
                // Waits for the oldest frame only; libgav1 keeps decoding the others meanwhile.
                const auto wait_start = std::chrono::steady_clock::now();
                std::optional<DecodedFrame> frame;
                const bool dequeued = Av1Dequeue(frame, timestamp);
                const auto waited = std::chrono::steady_clock::now() - wait_start;
                if (frame) {
                    frame->decode_time = waited;
                }
                lock.lock();
                if (waited > std::chrono::seconds{1}) {
                    LOG_WARNING(Service_AM, "Native video: taking a frame out took {} ms ({}): {}",
                                std::chrono::duration_cast<std::chrono::milliseconds>(waited).count(),
                                frame ? "a frame" : "no frame", DecodeState());
                }
                decode_busy = false;
                av1_decoder_full = false;
                if (!dequeued) {
                    // The decoder dropped every frame it had after a failure
                    av1_in_flight.clear();
                    decode_error = Kernel::ResultInvalidArgument;
                } else if (!frame) {
                    av1_in_flight.pop_front();
                } else {
                    // The frame's own input (by its submission ID; dav1d decodes several at once),
                    // and any before it that showed no frame, are done.
                    const auto own = std::ranges::find(av1_in_flight, frame->submission_id,
                                                       &PendingInput::submission_id);
                    if (own != av1_in_flight.end()) {
                        frame->timestamp = own->timestamp;
                        while (!av1_in_flight.empty()) {
                            const bool last = &av1_in_flight.front() == &*own;
                            av1_in_flight.pop_front();
                            if (last) {
                                break;
                            }
                        }
                    } else {
                        av1_in_flight.pop_front();
                    }
                    decoded_frames.push_back(std::move(*frame));
                    last_frame_out = std::chrono::steady_clock::now();
                }
                decode_cv.notify_all();
                continue;
            }
            // End of stream, once every frame before it has been taken out
            PendingInput input = std::move(pending_inputs.front());
            pending_inputs.pop_front();
            decode_busy = true;
            eos_in_progress = true;
            lock.unlock();
            const bool reset = Av1Reset();
            lock.lock();
            decode_busy = false;
            eos_in_progress = false;
            av1_decoder_full = false;
            if (!reset) {
                decode_error = Kernel::ResultInvalidState;
            } else {
                DecodedFrame frame{};
                frame.eos = true;
                frame.submission_id = input.submission_id;
                frame.timestamp = input.timestamp;
                decoded_frames.push_back(std::move(frame));
            }
            decode_cv.notify_all();
        }
    }

    // After SignalEOS (or destroying the decoder) under QuiesceAv1Decoding's lock: libgav1 holds
    // none of the inputs it was given any more.
    void DropAv1InFlight() {
        av1_in_flight.clear();
        av1_decoder_full = false;
    }

    // Stops AV1 decoding in flight: drops queued inputs and waits for the worker to finish the
    // frame it is on, so the caller may use the libgav1 decoder. Holds the decode lock.
    std::unique_lock<std::mutex> QuiesceAv1Decoding() {
        std::unique_lock lock{decode_mutex};
        pending_inputs.clear();
        pending_input_bytes = 0;
        decode_cv.wait(lock, [&] { return !decode_busy; });
        decode_error.reset();
        return lock;
    }

    // Movie pacing (ported from Eglantine 1f52ac7e2e). A game that shows one decoded frame per
    // frame it draws, and never skips one, falls further behind the movie's sound with every frame
    // while it runs below the movie's frame rate. The movie clock follows the frames the game
    // fetches (GetOutputProperties): decoded frames that a later decoded frame has already
    // overtaken on that clock are dropped before they are converted and written, so the next
    // image holds the frame the sound is at. Frames are only dropped, never held back, never the
    // end-of-stream frame, and not at all while the game keeps up. A gap of over 1 s between
    // fetches (a pause) or a lag of over 3 s (a jump) restarts the clock instead, as do Flush,
    // Stop, Configure and Finalize. Streams whose timestamps don't rise are left alone.
    // EDEN_VIDEO_PACE=0 turns it off.
    static constexpr s64 MoviePauseGapUs = 1'000'000;
    static constexpr s64 MovieMaxCatchUpUs = 3'000'000;

    static bool MoviePacingEnabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_VIDEO_PACE");
            return value == nullptr || value[0] != '0';
        }();
        return enabled;
    }

    static s64 HostTimeUs() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void ResetMovieClock() {
        movie_clock_offset.reset();
    }

    // Starts or corrects the movie clock from a frame the game just fetched. The clock never runs
    // behind a frame the game already has.
    void AnchorMovieClock(u64 pts_us) {
        const s64 now = HostTimeUs();
        const s64 offset = now - static_cast<s64>(pts_us);
        if (!movie_clock_offset || now - movie_last_fetch > MoviePauseGapUs ||
            pts_us < movie_last_fetched_pts || offset < *movie_clock_offset ||
            offset - *movie_clock_offset > MovieMaxCatchUpUs) {
            movie_clock_offset = offset;
        }
        movie_last_fetch = now;
        movie_last_fetched_pts = pts_us;
    }

    // Drops decoded frames the movie clock has already passed the next one of (decode_mutex held).
    void DropOvertakenFrames() {
        if (!MoviePacingEnabled() || !movie_clock_offset || decoded_frames.size() < 2) {
            return;
        }
        const s64 now = HostTimeUs();
        if (now - movie_last_fetch > MoviePauseGapUs) {
            return; // Paused: the next fetch restarts the clock.
        }
        const s64 due = now - *movie_clock_offset;
        while (decoded_frames.size() >= 2) {
            const DecodedFrame& front = decoded_frames[0];
            const DecodedFrame& next = decoded_frames[1];
            if (front.eos || next.eos || next.timestamp <= front.timestamp ||
                static_cast<s64>(next.timestamp) > due) {
                break;
            }
            timing.decode += front.decode_time;
            decoded_frames.pop_front();
            ++timing.dropped;
            ++movie_frames_dropped;
        }
    }

    Result PumpOutput(bool from_submit = false, bool no_wait = false, bool from_worker = false) {
        while (true) {
            auto image = registered_images.end();
            for (auto it = registered_images.begin(); it != registered_images.end(); ++it)
                if (it->available && (image == registered_images.end() || it->completion_value < image->completion_value)) image = it;
            if (image == registered_images.end()) break;
            DecodedFrame frame;
            if (h264_context) {
                if (decoded_frames.empty()) {
                    if (pending_inputs.empty()) break;
                    R_TRY(DecodePendingInput());
                    continue;
                }
                frame = std::move(decoded_frames.front());
                decoded_frames.pop_front();
            } else {
                // The worker normally has the next frame ready. If it is still decoding it (or has
                // inputs queued), wait: an image the game hands back must get its frame within this
                // call, since the game then waits on the image's fence and may make no other call.
                std::unique_lock lock{decode_mutex};
                if (from_submit && VideoLookahead() != 0 && decoded_frames.empty() &&
                    !decode_error) {
                    // Lookahead: return to the game while fewer than N frames are submitted, so
                    // it can hand libgav1 more to decode at once (never with an EOS queued).
                    const bool eos_queued = std::ranges::any_of(
                        pending_inputs, [](const PendingInput& input) { return input.eos; });
                    if (!eos_queued && !eos_in_progress &&
                        pending_inputs.size() + av1_in_flight.size() < VideoLookahead()) {
                        break;
                    }
                }
                // The output thread (and calls made while it runs) only write frames already decoded:
                // it writes the rest as they come.
                if (no_wait && decoded_frames.empty() && !decode_error) {
                    break;
                }
                const auto frame_or_idle = [&] {
                    return !decoded_frames.empty() || decode_error ||
                           (pending_inputs.empty() && av1_in_flight.empty() && !decode_busy);
                };
                while (!decode_cv.wait_for(lock, std::chrono::seconds{2}, frame_or_idle)) {
                    LOG_WARNING(Service_AM,
                                "Native video: the game has waited 2 s for a frame ({}): {}",
                                from_submit ? "submitting" : "handing an image back",
                                DecodeState(true));
                }
                if (decode_error) {
                    const Result error = *decode_error;
                    decode_error.reset();
                    decode_cv.notify_all();
                    return error;
                }
                if (decoded_frames.empty()) break;
                DropOvertakenFrames();
                frame = std::move(decoded_frames.front());
                decoded_frames.pop_front();
                lock.unlock();
                decode_cv.notify_all(); // Room to decode ahead again.
            }
            timing.decode += frame.decode_time;
            const auto& pool = *image->pool;
            const u32 pool_width = (pool.packed_dimensions & 0xffffU) + 1;
            const u32 pool_height = (pool.packed_dimensions >> 16) + 1;
            if (!frame.eos && (frame.bitdepth != 8 || frame.format != 0 ||
                (!pool.layout.nv12 && frame.matrix != 1) ||
                frame.widths[0] > pool_width || frame.heights[0] > pool_height)) {
                LOG_ERROR(Service_AM, "Unsupported native AV1 output: depth={}, format={}, matrix={}, dimensions={}x{}",
                          frame.bitdepth, frame.format, frame.matrix, frame.widths[0], frame.heights[0]);
                R_THROW(Kernel::ResultNotImplemented);
            }
            if (!frame.eos && pool.layout.nv12) {
                // NV12 pool: copy the planes as decoded, with the interleaved chroma plane after
                // the luma plane; colour conversion is left to the game.
                std::vector<u8> nv12(pool.layout.image_bytes);
                const u32 pitch = pool.layout.row_bytes;
                for (u32 y = 0; y < frame.heights[0]; ++y)
                    std::memcpy(nv12.data() + size_t{y} * pitch, frame.planes[0].data() + size_t{y} * frame.widths[0],
                                frame.widths[0]);
                for (u32 y = 0; y < frame.heights[1]; ++y) {
                    u8* row = nv12.data() + pool.layout.chroma_offset + size_t{y} * pitch;
                    for (u32 x = 0; x < frame.widths[1]; ++x) {
                        row[x * 2] = frame.planes[1][size_t{y} * frame.widths[1] + x];
                        row[x * 2 + 1] = frame.planes[2][size_t{y} * frame.widths[2] + x];
                    }
                }
                const u64 offset = u64{image->index} * pool.layout.image_stride;
                // Invalidate before the write (untracks the pages so the copy is a plain memcpy) and
                // again after it: the GPU thread may re-upload the image while the copy is running,
                // and the second invalidation makes it upload the finished frame.
                system.GPU().InvalidateRegion(pool.device_address + offset, nv12.size());
                R_UNLESS(process->GetMemory().WriteBlock(pool.address + offset, nv12.data(), nv12.size()), Kernel::ResultInvalidAddress);
                system.GPU().InvalidateRegion(pool.device_address + offset, nv12.size());
            } else if (!frame.eos) {
                auto stage_start = std::chrono::steady_clock::now();
                const auto lap = [&](std::chrono::steady_clock::duration& total) {
                    const auto now = std::chrono::steady_clock::now();
                    total += now - stage_start;
                    stage_start = now;
                };
                // The RGBA and tiled buffers are kept between frames (two ~15 MB buffers per 1440p
                // frame were allocated, zeroed and page-faulted in every time). They are cleared
                // only when the frame or pool size changes, so padding stays zero as before.
                const u32 row_bytes = pool.layout.row_bytes;
                const size_t rgba_bytes = size_t{row_bytes} * pool_height;
                const std::array<u32, 3> buffer_shape{frame.widths[0], frame.heights[0], pool.layout.image_bytes};
                if (rgba_buffer.size() != rgba_bytes || tiled_buffer.size() != pool.layout.image_bytes ||
                    buffer_shape != rgba_buffer_shape) {
                    rgba_buffer.assign(rgba_bytes, 0);
                    tiled_buffer.assign(pool.layout.image_bytes, 0);
                    rgba_buffer_shape = buffer_shape;
                }
                std::vector<u8>& rgba = rgba_buffer;
                std::vector<u8>& tiled = tiled_buffer;
                const NativeVideoYuv709Table table{frame.full_range};
                const auto convert_rows = [&](u32 first, u32 last) {
                    for (u32 y = first; y < last; ++y) {
                        const u8* luma = frame.planes[0].data() + size_t{y} * frame.widths[0];
                        const u8* cb = frame.planes[1].data() + size_t{y / 2} * frame.widths[1];
                        const u8* cr = frame.planes[2].data() + size_t{y / 2} * frame.widths[2];
                        u8* out = rgba.data() + size_t{y} * row_bytes;
                        for (u32 x = 0; x < frame.widths[0]; ++x)
                            table.Convert(luma[x], cb[x / 2], cr[x / 2], out + size_t{x} * 4);
                    }
                };
                // Block-linear rows come in blocks of 8 << 4 = 128 rows, and each block row is
                // contiguous in the tiled image (row_bytes * 128 bytes), so bands of whole block
                // rows convert and swizzle independently: each worker converts its rows and then
                // swizzles them, and the swizzle no longer runs on one thread. Byte-identical to
                // swizzling the whole image (checked for several sizes and thread counts).
                constexpr u32 ConversionThreads = 4;
                constexpr u32 BlockHeightLog2 = 4;
                constexpr u32 RowsPerBlock = 8U << BlockHeightLog2;
                const u32 block_rows = (pool_height + RowsPerBlock - 1) / RowsPerBlock;
                const size_t block_row_bytes = size_t{row_bytes} * RowsPerBlock;
                const bool banded = row_bytes % Tegra::Texture::GOB_SIZE_X == 0 &&
                                    size_t{block_rows} * block_row_bytes <= tiled.size();
                const u32 blocks_per_worker = (block_rows + ConversionThreads - 1) / ConversionThreads;
                const auto convert_and_swizzle = [&](u32 first_block, u32 last_block) {
                    if (first_block >= last_block) return;
                    const u32 first = first_block * RowsPerBlock;
                    const u32 last = (std::min)(last_block * RowsPerBlock, pool_height);
                    convert_rows((std::min)(first, frame.heights[0]), (std::min)(last, frame.heights[0]));
                    Tegra::Texture::SwizzleTexture(
                        std::span<u8>{tiled}.subspan(first_block * block_row_bytes),
                        std::span<const u8>{rgba}.subspan(size_t{first} * row_bytes, size_t{last - first} * row_bytes),
                        4, row_bytes / 4, last - first, 1, BlockHeightLog2, 0, 1, true);
                };
                if (banded) {
                    std::array<std::jthread, ConversionThreads - 1> workers;
                    for (u32 i = 1; i < ConversionThreads; ++i) {
                        workers[i - 1] = std::jthread{convert_and_swizzle,
                                                      (std::min)(i * blocks_per_worker, block_rows),
                                                      (std::min)((i + 1) * blocks_per_worker, block_rows)};
                    }
                    convert_and_swizzle(0, (std::min)(blocks_per_worker, block_rows));
                } else {
                    const u32 band = (frame.heights[0] + ConversionThreads - 1) / ConversionThreads;
                    {
                        std::array<std::jthread, ConversionThreads - 1> workers;
                        for (u32 i = 1; i < ConversionThreads; ++i) {
                            const u32 first = (std::min)(i * band, frame.heights[0]);
                            const u32 last = (std::min)(first + band, frame.heights[0]);
                            workers[i - 1] = std::jthread{convert_rows, first, last};
                        }
                        convert_rows(0, (std::min)(band, frame.heights[0]));
                    }
                    Tegra::Texture::SwizzleTexture(tiled, rgba, 4, row_bytes / 4, pool_height, 1,
                                                   BlockHeightLog2, 0, 1, true);
                }
                lap(timing.convert);
                const u64 offset = u64{image->index} * pool.layout.image_stride;
                // As above: invalidate before the write for a fast copy, and after it so a GPU upload
                // made during the copy does not keep a half-written frame.
                system.GPU().InvalidateRegion(pool.device_address + offset, tiled.size());
                R_UNLESS(process->GetMemory().WriteBlock(pool.address + offset, tiled.data(), tiled.size()), Kernel::ResultInvalidAddress);
                system.GPU().InvalidateRegion(pool.device_address + offset, tiled.size());
                lap(timing.write);
            }
            if (!frame.eos && ++timing.frames == 60) {
                using ms = std::chrono::duration<double, std::milli>;
                const auto now = std::chrono::steady_clock::now();
                LOG_INFO(Service_AM,
                         "Native video {}x{}: {:.1f} ms/frame wall, decode {:.1f}, convert+swizzle {:.1f}, "
                         "write {:.1f} (averages over 60 frames), {} late frames dropped ({} in all)",
                         frame.widths[0], frame.heights[0],
                         ms(now - timing.window_start).count() / 60, ms(timing.decode).count() / 60,
                         ms(timing.convert).count() / 60, ms(timing.write).count() / 60,
                         timing.dropped, movie_frames_dropped);
                timing = {};
                timing.window_start = now;
            }
            std::array<u32, 16> properties{};
            properties[0] = image->id;
            properties[1] = configuration->width;
            properties[2] = configuration->height;
            properties[3] = frame.widths[0];
            properties[4] = frame.heights[0];
            const u64 timestamp_ns = frame.timestamp * 1000;
            properties[6] = static_cast<u32>(timestamp_ns);
            properties[7] = static_cast<u32>(timestamp_ns >> 32);
            properties[8] = frame.eos ? 1 : 0;
            output_properties.push_back(properties);
            image->available = false;
            system.Host1x().GetSyncpointManager().IncrementGuest(*operation_syncpoint);
            system.Host1x().GetSyncpointManager().IncrementHost(*operation_syncpoint);
            LOG_DEBUG(Service_AM, "native AV1 output written image={}, dimensions={}x{}, threshold={}",
                      image->id, frame.widths[0], frame.heights[0], image->completion_value);
        }
        if (!from_worker) {
            UpdateSubmissionEvent();
        }
        R_SUCCEED();
    }

    // Kernel events are signalled from the service's own thread only (not the output thread).
    void UpdateSubmissionEvent() {
        if (submission_event) {
            std::unique_lock lock{decode_mutex};
            const bool room = transfer_memory && pending_inputs.size() < MaximumPendingInputs() &&
                              pending_input_bytes < transfer_memory->GetSize();
            lock.unlock();
            if (room) submission_event->Signal(kernel);
            else submission_event->Clear(kernel);
        }
    }

    bool OutputAsync() const {
        return output_worker.joinable();
    }

    // The output worker writes decoded frames to the images the game handed back, as soon as
    // both are there, so ReleaseImage returns at once. It takes codec_mutex only with try_lock:
    // Reset holds it while joining the worker.
    void OutputWorker() {
        Common::SetCurrentThreadName("NativeVideoOutput");
        std::unique_lock lock{decode_mutex};
        while (true) {
            decode_cv.wait(lock, [&] {
                return output_stop ||
                       (output_wanted && (!decoded_frames.empty() || decode_error.has_value()));
            });
            if (output_stop) {
                return;
            }
            lock.unlock();
            std::unique_lock codec_lock{codec_mutex, std::try_to_lock};
            if (!codec_lock.owns_lock()) {
                std::this_thread::sleep_for(std::chrono::microseconds{200});
                lock.lock();
                continue;
            }
            const Result result = PumpOutput(false, /*no_wait=*/true, /*from_worker=*/true);
            if (result.IsError() && !deferred_output_error) {
                deferred_output_error = result;
            }
            const bool images_waiting =
                result.IsSuccess() &&
                std::ranges::any_of(registered_images, &RegisteredImage::available);
            lock.lock();
            output_wanted = images_waiting;
            codec_lock.unlock();
        }
    }

    void StartOutputWorker() {
        if (!AsyncVideoOutput() || h264_context || output_worker.joinable()) {
            return;
        }
        {
            std::scoped_lock lock{decode_mutex};
            output_stop = false;
            output_wanted = false;
        }
        output_worker = std::thread{[this] { OutputWorker(); }};
    }

    void StopOutputWorker() {
        if (!output_worker.joinable()) {
            return;
        }
        {
            std::scoped_lock lock{decode_mutex};
            output_stop = true;
        }
        decode_cv.notify_all();
        output_worker.join();
        output_wanted = false;
    }

    // Wakes the output thread to fill any images handed back (it clears this when none are).
    // Images also come back without ReleaseImage (Flush, Stop), so every call that pumps asks.
    void RequestOutput() {
        if (!OutputAsync()) {
            return;
        }
        {
            std::scoped_lock lock{decode_mutex};
            output_wanted = true;
        }
        decode_cv.notify_all();
    }

    // Hands an image back for the output thread to fill, and returns an error it met since.
    Result QueueOutput() {
        RequestOutput();
        UpdateSubmissionEvent();
        if (deferred_output_error) {
            const Result error = *deferred_output_error;
            deferred_output_error.reset();
            return error;
        }
        R_SUCCEED();
    }

    static bool VideoTimingEnabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_VIDEO_TIMING");
            return value != nullptr && value[0] == '1';
        }();
        return enabled;
    }

    // One fetch: `pts_us` is the frame the game got, or nullopt when it asked and none was ready.
    void RecordFetch(std::optional<u64> pts_us) {
        if (!VideoTimingEnabled()) {
            return;
        }
        auto& t = fetch_timing;
        const s64 now = HostTimeUs();
        if (!pts_us) {
            if (t.active) {
                ++t.empty;
            }
            return;
        }
        const u64 pts = *pts_us;
        // A new run of frames: the first fetch, a pause of over 1 s, or the timestamps going back.
        if (!t.active || now - t.last_fetch > MoviePauseGapUs || pts < t.last_pts) {
            if (t.active) {
                LOG_INFO(Service_AM, "Movie timing: new run (gap {:.0f} ms, pts {} -> {} us)",
                         static_cast<double>(now - t.last_fetch) / 1000.0, t.last_pts, pts);
            }
            t = {};
            t.active = true;
            t.start_host = t.window_start = t.last_fetch = now;
            t.start_pts = t.last_pts = t.first_pts_in_window = pts;
            t.dropped_at_start = movie_frames_dropped;
            return;
        }
        // Lateness: how far behind real time (since the run began) this frame's timestamp is.
        const s64 lateness = (now - t.start_host) - static_cast<s64>(pts - t.start_pts);
        if (t.fetched == 0) {
            t.lateness_min = t.lateness_max = lateness;
        }
        t.lateness_min = std::min(t.lateness_min, lateness);
        t.lateness_max = std::max(t.lateness_max, lateness);
        t.fetch_gap_max = std::max(t.fetch_gap_max, now - t.last_fetch);
        t.pts_step_max = std::max(t.pts_step_max, static_cast<s64>(pts - t.last_pts));
        ++t.fetched;
        t.last_fetch = now;
        t.last_pts = pts;
        if (now - t.window_start >= 2'000'000) {
            LOG_INFO(Service_AM,
                     "Movie timing: {:.1f} s in, pts {:.2f}-{:.2f} s, {} frames fetched, {} asks "
                     "with no frame ready, behind real time {:.0f} to {:.0f} ms, longest gap "
                     "between fetches {:.0f} ms, largest pts step {:.0f} ms, {} frames dropped "
                     "by pacing",
                     static_cast<double>(now - t.start_host) / 1e6,
                     static_cast<double>(t.first_pts_in_window) / 1e6,
                     static_cast<double>(pts) / 1e6, t.fetched, t.empty,
                     static_cast<double>(t.lateness_min) / 1000.0,
                     static_cast<double>(t.lateness_max) / 1000.0,
                     static_cast<double>(t.fetch_gap_max) / 1000.0,
                     static_cast<double>(t.pts_step_max) / 1000.0,
                     movie_frames_dropped - t.dropped_at_start);
            t.window_start = now;
            t.fetched = 0;
            t.empty = 0;
            t.fetch_gap_max = 0;
            t.pts_step_max = 0;
            t.first_pts_in_window = pts;
            t.dropped_at_start = movie_frames_dropped;
        }
    }

    void GetOutputProperties(HLERequestContext& ctx) {
        std::scoped_lock codec_lock{codec_mutex};
        FefwWaitStats::MarkThread(FefwWaitStats::Role::Video, &ctx.GetThread());
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        std::array<u32, 16> properties{};
        if (packet[0] == 0xe5 && packet[1] == 0) {
            result = PumpOutput(false, /*no_wait=*/OutputAsync());
            RequestOutput();
            if (result.IsSuccess() && deferred_output_error) {
                result = *deferred_output_error;
                deferred_output_error.reset();
            }
            if (result.IsSuccess()) result = Result{0x1c286f};
            if (result.raw == 0x1c286f && !output_properties.empty()) {
                properties = output_properties.front();
                output_properties.pop_front();
                if (!properties[8]) {
                    const u64 pts_us = (u64{properties[6]} | (u64{properties[7]} << 32)) / 1000;
                    AnchorMovieClock(pts_us);
                    RecordFetch(pts_us);
                }
                result = ResultSuccess;
            } else if (result.raw == 0x1c286f) {
                RecordFetch(std::nullopt);
            }
        }
        {
            std::scoped_lock lock{decode_mutex};
            LOG_DEBUG(Service_AM, "native video output properties result={:#x}, image={}, eos={}, pending_frames={}, pending_inputs={}",
                      result.raw, properties[0], properties[8], decoded_frames.size(), pending_inputs.size());
        }
        IPC::ResponseBuilder rb{ctx, 18};
        rb.Push(result.raw);
        for (u32 word : properties) rb.Push(word);
    }

    void ReleaseImage(HLERequestContext& ctx) {
        std::scoped_lock codec_lock{codec_mutex};
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        u64 completion_value{};
        if (packet[0] == 0x14a && packet[1] == 8 && packet[3] == 0 && packet[8] == 0 && packet[9] == 0) {
            result = Kernel::ResultInvalidState;
            if (configuration && operation_syncpoint) {
                const auto image = std::ranges::find(registered_images, packet[2], &RegisteredImage::id);
                result = Kernel::ResultNotFound;
                if (image != registered_images.end()) {
                    result = Kernel::ResultNotImplemented;
                    if (!(packet[4] || packet[5] || packet[6] || packet[7])) {
                        result = Kernel::ResultInvalidState;
                        if (!image->available) {
                            completion_value = ++next_completion_value;
                            image->completion_value = completion_value;
                            image->available = true;
                            result = OutputAsync() ? QueueOutput() : PumpOutput();
                        } else {
                            // Yoshi's second H.264 decoder releases images that were never handed out
                            // (after its first Flush), and Fortune's Weave does it in a loop after
                            // Flush/Start. The image already waits for a frame, so hand back the fence it
                            // already has: it is reached once a frame is written to it, and the game's
                            // buffer ring polls it. A fresh value would leave a hole in the sequence (each
                            // written frame advances the syncpoint by one, in completion order), and
                            // repeated idle releases pushed every fence out of reach, freezing the game.
                            completion_value = image->completion_value;
                            result = OutputAsync() ? QueueOutput() : PumpOutput();
                            if ((++idle_releases & 0xfff) == 1) {
                                LOG_WARNING(Service_AM, "native video release of idle image={} (x{}), fence={}",
                                            packet[2], idle_releases, completion_value);
                            }
                            LOG_DEBUG(Service_AM, "native video release of idle image={}", packet[2]);
                        }
                    }
                }
            }
        }
        if (result == Kernel::ResultNotImplemented) {
            // A release that waits on a fence is not handled; make it visible, the game aborts on it.
            LOG_WARNING(Service_AM, "native video release image={} with a fence is unsupported: {:#x} {:#x} {:#x} {:#x}",
                        packet[2], packet[4], packet[5], packet[6], packet[7]);
        }
        LOG_DEBUG(Service_AM, "native video release image={}, threshold={}, result={:#x}", packet[2], completion_value, result.raw);
        IPC::ResponseBuilder rb{ctx, 10};
        rb.Push(result.raw);
        rb.Push<u32>(result.IsSuccess() ? ~*operation_syncpoint : 0);
        rb.Push<u32>(0);
        rb.Push(completion_value);
        rb.Push<u64>(0);
        rb.Push<u64>(0);
    }

    void RegisterImage(HLERequestContext& ctx) {
        std::scoped_lock codec_lock{codec_mutex};
        const auto image_id = GraphicsSupervisorProtocol::DecodeVideoImageRegistration(
            std::span<const u32>{ctx.CommandBuffer(), IPC::COMMAND_BUFFER_LENGTH});
        Result result = Kernel::ResultInvalidCombination;
        u64 completion_value{};
        if (image_id) {
            result = Kernel::ResultInvalidState;
            if (configuration && !started) {
                result = Kernel::ResultOutOfResource;
                if (registered_images.size() < 8) {
                    result = Kernel::ResultInvalidState;
                    if (std::ranges::find(registered_images, *image_id, &RegisteredImage::id) == registered_images.end()) {
                        const std::scoped_lock lock{state->mutex};
                        result = Kernel::ResultNotFound;
                        for (const auto& pool : state->pools) {
                            if (*image_id < pool->first_image_id || *image_id - pool->first_image_id >= pool->count) continue;
                            const u32 width = (pool->packed_dimensions & 0xffffU) + 1;
                            const u32 height = (pool->packed_dimensions >> 16) + 1;
                            result = Kernel::ResultInvalidSize;
                            if (width >= configuration->width && height >= configuration->height) {
                                if (!operation_syncpoint) {
                                    auto& syncpoints = state->module->GetContainer().GetSyncpointManager();
                                    operation_syncpoint = syncpoints.AllocateSyncpoint(true);
                                    const auto current = syncpoints.ReadAllocatedSyncpoint(*operation_syncpoint);
                                    if (!current) {
                                        operation_syncpoint.reset();
                                        result = Kernel::ResultOutOfResource;
                                        break;
                                    }
                                    next_completion_value = *current;
                                }
                                completion_value = ++next_completion_value;
                                registered_images.push_back({*image_id, pool, *image_id - pool->first_image_id, completion_value, true});
                                result = ResultSuccess;
                            }
                            break;
                        }
                    }
                }
            }
        }
        LOG_DEBUG(Service_AM, "native video image register id={}, slots={}, result={:#x}", image_id.value_or(0), registered_images.size(), result.raw);
        IPC::ResponseBuilder rb{ctx, 10};
        rb.Push(result.raw);
        // Acquisition waits until the first decoded frame has been written to this backing.
        rb.Push<u32>(result.IsSuccess() ? ~*operation_syncpoint : 0);
        rb.Push<u32>(0);
        rb.Push(completion_value);
        rb.Push<u64>(0);
        rb.Push<u64>(0);
    }

    void Stop(HLERequestContext& ctx) {
        std::scoped_lock codec_lock{codec_mutex};
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        if (packet[0] == 0xdf && packet[1] == 0) {
            result = Kernel::ResultInvalidState;
            if (configuration && Initialized()) {
                if (h264_context) {
                    DrainH264();
                    result = ResultSuccess;
                } else {
                    const auto lock = QuiesceAv1Decoding();
                    decoded_frames.clear();
                    result = Av1Reset() ? ResultSuccess : Kernel::ResultInvalidState;
                    DropAv1InFlight();
                }
                if (result.IsSuccess()) {
                    // As in Flush: outputs written but not fetched give their image back to the
                    // decoder, at the end of the fence sequence. The game never learns of these
                    // outputs, so it never releases their images, and a decoder started again after
                    // Stop would otherwise run out of images and freeze.
                    for (const auto& properties : output_properties) {
                        const auto image = std::ranges::find(registered_images, properties[0], &RegisteredImage::id);
                        if (image != registered_images.end() && !image->available) {
                            image->completion_value = ++next_completion_value;
                            image->available = true;
                        }
                    }
                    started = false;
                    flushed = false;
                    pending_inputs.clear();
                    pending_input_bytes = 0;
                    decoded_frames.clear();
                    output_properties.clear();
                    ResetMovieClock();
                    if (submission_event) submission_event->Clear(kernel);
                }
            }
        }
        LOG_DEBUG(Service_AM, "native video stop result={:#x}", result.raw);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }

    void UnregisterImage(HLERequestContext& ctx) {
        std::scoped_lock codec_lock{codec_mutex};
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        if (packet[0] == 0xe4 && packet[1] == 8 &&
            std::all_of(packet + 3, packet + 10, [](u32 word) { return word == 0; })) {
            result = Kernel::ResultInvalidState;
            if (!started) {
                const auto image = std::ranges::find(registered_images, packet[2], &RegisteredImage::id);
                result = Kernel::ResultNotFound;
                if (image != registered_images.end()) {
                    registered_images.erase(image);
                    result = ResultSuccess;
                }
            }
        }
        LOG_DEBUG(Service_AM, "native video image unregister id={}, result={:#x}", packet[2], result.raw);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }

    void Flush(HLERequestContext& ctx) {
        std::scoped_lock codec_lock{codec_mutex};
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        if (packet[0] == 0x18 && packet[1] == 0) {
            result = Kernel::ResultInvalidState;
            if (started && configuration) {
                if (h264_context) {
                    // Decode everything submitted, then drain the decoder so no frame stays queued.
                    result = PumpOutput();
                    if (result.IsSuccess()) {
                        DrainH264();
                        result = PumpOutput();
                    }
                } else {
                    // AV1 (Fortune's Weave flushes its character-select clips when one ends or the
                    // character changes, then calls Start). Like MediaCodec.flush(), drop everything
                    // in flight: the game ends a clip with EOS inputs, and handing those EOS frames out
                    // after the following Start made it treat the new clip as finished, stop
                    // submitting and spin releasing images. Outputs written but not fetched give their
                    // image back to the decoder, at the end of the fence sequence (their old fence was
                    // already reached). libgav1 is reset so the next input may start a new sequence.
                    const auto lock = QuiesceAv1Decoding();
                    for (const auto& properties : output_properties) {
                        const auto image = std::ranges::find(registered_images, properties[0], &RegisteredImage::id);
                        if (image != registered_images.end() && !image->available) {
                            image->completion_value = ++next_completion_value;
                            image->available = true;
                        }
                    }
                    LOG_DEBUG(Service_AM, "native AV1 flush drops inputs={}, frames={}, outputs={}",
                              pending_inputs.size(), decoded_frames.size(), output_properties.size());
                    output_properties.clear();
                    decoded_frames.clear();
                    pending_inputs.clear();
                    pending_input_bytes = 0;
                    result = Av1Reset() ? ResultSuccess : Kernel::ResultInvalidState;
                    DropAv1InFlight();
                    if (result.IsSuccess() && submission_event && transfer_memory) {
                        submission_event->Signal(kernel);
                    }
                }
            }
        }
        if (result.IsSuccess()) {
            flushed = true;
            ResetMovieClock();
        }
        if (result.IsError()) {
            LOG_WARNING(Service_AM, "native video flush failed: {:#x}, codec={}, pending_inputs={}",
                        result.raw, h264_context ? "H.264" : "AV1", pending_inputs.size());
        } else {
            LOG_DEBUG(Service_AM, "native video flush result={:#x}", result.raw);
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }

    void Start(HLERequestContext& ctx) {
        std::scoped_lock codec_lock{codec_mutex};
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        if (packet[0] == 0x16 && packet[1] == 0) {
            result = Kernel::ResultInvalidState;
            // Like a MediaCodec flush, a flushed decoder is resumed with Start (Fortune's Weave
            // calls Flush then Start when its character-select clip loops or changes, and aborts
            // on InvalidState). Games that keep submitting after Flush need no Start.
            if (Initialized() && configuration && !registered_images.empty() && (!started || flushed)) {
                started = true;
                flushed = false;
                submission_event->Signal(kernel);
                result = ResultSuccess;
            }
        }
        if (result.IsError()) {
            LOG_WARNING(Service_AM, "native video start failed: {:#x}, started={}, images={}", result.raw,
                        started, registered_images.size());
        } else {
            LOG_DEBUG(Service_AM, "native video start result={:#x}", result.raw);
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }

    void Configure(HLERequestContext& ctx) {
        std::scoped_lock codec_lock{codec_mutex};
        const auto request = GraphicsSupervisorProtocol::DecodeVideoDecoderConfiguration(
            std::span<const u32>{ctx.CommandBuffer(), IPC::COMMAND_BUFFER_LENGTH});
        Result result = Kernel::ResultInvalidCombination;
        if (request) {
            result = Kernel::ResultInvalidState;
            if (Initialized() && !configuration) {
                const auto mime = ctx.ReadBuffer();
                const auto mime_is = [&](std::string_view text) {
                    return mime.size() == text.size() + 1 && std::memcmp(mime.data(), text.data(), text.size()) == 0 &&
                           mime.back() == 0;
                };
                LOG_DEBUG(Service_AM, "native video configure buffer bytes={}, head={:02x}{:02x}{:02x}{:02x}",
                          mime.size(), mime.size() > 0 ? mime[0] : 0, mime.size() > 1 ? mime[1] : 0,
                          mime.size() > 2 ? mime[2] : 0, mime.size() > 3 ? mime[3] : 0);
                // AV1 passes its mime string in the buffer. The H.264 initializer's trailing
                // (pointer, size) pair is codec-specific data (parameter sets) instead; when present
                // it is given to FFmpeg as extradata by reopening the decoder.
                result = Kernel::ResultNotImplemented;
                if (h264_context || mime_is("video/av1")) {
                    result = Kernel::ResultInvalidSize;
                    if (request->input_capacity <= transfer_memory->GetSize()) {
                        if (h264_context && !mime.empty()) {
                            auto reopened = std::make_unique<FFmpeg::DecoderContext>(*h264_decoder);
                            if (reopened->OpenContext(*h264_decoder, mime)) {
                                h264_context = std::move(reopened);
                            } else {
                                LOG_ERROR(Service_AM, "Native H.264 extradata rejected");
                            }
                        }
                        if (!submission_event) submission_event = context.CreateEvent("VideoSubmissionReady");
                        result = submission_event ? ResultSuccess : Kernel::ResultOutOfResource;
                        if (submission_event) {
                            configuration = *request;
                            ResetMovieClock();
                            submission_event->Signal(kernel);
                        }
                    }
                }
            }
            LOG_DEBUG(Service_AM, "native video configure {}x{}, fps={}, input_bytes={:#x}, result={:#x}",
                      request->width, request->height, request->frame_rate, request->input_capacity, result.raw);
        }
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(result.raw);
        rb.Push<u64>(0);
    }

    void GetSubmissionEvent(HLERequestContext& ctx) {
        std::scoped_lock codec_lock{codec_mutex};
        const auto* packet = ctx.CommandBuffer();
        Result result = Kernel::ResultInvalidCombination;
        Kernel::KReadableEvent* readable{};
        if (packet[0] == 0x13 && packet[1] == 0) {
            result = configuration && submission_event ? ResultSuccess : Kernel::ResultInvalidState;
            if (result.IsSuccess()) readable = &submission_event->GetReadableEvent();
        }
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(result.raw);
        ctx.AddCopyObject(readable);
    }

    void Finalize(HLERequestContext& ctx) {
        std::scoped_lock codec_lock{codec_mutex};
        const auto* packet = ctx.CommandBuffer();
        const Result result = packet[0] == 0x83 && packet[1] == 0 ? ResultSuccess : Kernel::ResultInvalidCombination;
        if (result.IsSuccess()) Reset();
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }

    void InitializeByTransferMemory(HLERequestContext& ctx) {
        std::scoped_lock codec_lock{codec_mutex};
        const bool is_200 = ctx.GetCommand() == 200;
        const auto request = is_200
            ? GraphicsSupervisorProtocol::DecodeVideoDecoderInitialization200(
                  std::span<const u32>{ctx.CommandBuffer(), IPC::COMMAND_BUFFER_LENGTH})
            : GraphicsSupervisorProtocol::DecodeVideoDecoderInitialization(
                  std::span<const u32>{ctx.CommandBuffer(), IPC::COMMAND_BUFFER_LENGTH});
        Result result = Kernel::ResultInvalidCombination;
        if (request) {
            result = Kernel::ResultNotImplemented;
            // Output format 3 pairs with NV12 pools (image kind 1) and 2 with RGBA pools (kind 5).
            // Codec 3 is the AV1 decoder, codec 0 the H.264 decoder: nn::codec::InitializeH264Decoder
            // sends codec 0 (hard-coded in the SDK helper) and the AV1 initializer sends 3.
            // The command 200 helper (Kirby Air Riders) sends codec 1, input format 3 and output format 3
            // (NV12 pools): treated as the AV1 decoder, the only decoder whose input format is 3. This
            // pairing is inferred, the codec numbering of the real server was not decoded.
            const bool is_av1 = kind == NativeVideoKind::Decoder &&
                                ((!is_200 && request->codec == 3 && request->output_format == 2) ||
                                 (is_200 && request->codec == 1 && request->output_format == 3)) &&
                                request->input_format == 3 && request->output_mode == 1;
            const bool is_h264 = kind == NativeVideoKind::Decoder && request->codec == 0 &&
                                 request->input_format == 1 && (request->output_format == 2 || request->output_format == 3) &&
                                 request->output_mode == 1;
            if (is_av1 || is_h264) {
                result = Kernel::ResultInvalidState;
                if (!Initialized()) {
                    auto memory = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(ctx.GetCopyHandle(0));
                    result = Kernel::ResultInvalidHandle;
                    if (!memory.IsNull() && memory->GetOwner() == process) {
                        result = Kernel::ResultInvalidSize;
                        if (request->memory_size != 0 && request->memory_size == memory->GetSize()) {
                            result = Kernel::ResultOutOfResource;
                            if (is_h264) {
                                auto ffmpeg_decoder = std::make_unique<FFmpeg::Decoder>(
                                    Tegra::Host1x::NvdecCommon::VideoCodec::H264);
                                if (ffmpeg_decoder->GetCodec()) {
                                    auto ffmpeg_context = std::make_unique<FFmpeg::DecoderContext>(*ffmpeg_decoder);
                                    if (ffmpeg_context->OpenContext(*ffmpeg_decoder)) {
                                        transfer_memory = memory.GetPointerUnsafe();
                                        transfer_memory->Open(kernel);
                                        h264_decoder = std::move(ffmpeg_decoder);
                                        h264_context = std::move(ffmpeg_context);
                                        result = ResultSuccess;
                                    }
                                }
                            } else {
                                bool opened = false;
#ifdef FEFW_HAVE_DAV1D
                                if (UseDav1d()) {
                                    Dav1dSettings settings;
                                    dav1d_default_settings(&settings);
                                    settings.n_threads = Av1DecodeThreads();
                                    settings.max_frame_delay = Av1FrameDelay();
                                    settings.logger.callback = nullptr;
                                    opened = dav1d_open(&dav1d, &settings) == 0;
                                    if (!opened) {
                                        LOG_ERROR(Service_AM, "dav1d failed to open; using libgav1");
                                    }
                                }
#endif
                                if (!opened) {
                                    auto backend = std::make_unique<libgav1::Decoder>();
                                    libgav1::DecoderSettings settings;
                                    settings.threads = Av1DecodeThreads();
                                    settings.frame_parallel = Av1FrameParallel();
                                    settings.blocking_dequeue = true;
                                    settings.release_input_buffer = &INativeVideoCodec::ReleaseAv1Input;
                                    if (backend->Init(&settings) == libgav1::kStatusOk) {
                                        decoder = std::move(backend);
                                        opened = true;
                                    }
                                }
                                if (opened) {
                                    transfer_memory = memory.GetPointerUnsafe();
                                    transfer_memory->Open(kernel);
                                    decode_worker = std::jthread{[this](std::stop_token stop) { DecodeWorker(stop); }};
                                    StartOutputWorker();
                                    result = ResultSuccess;
                                }
                            }
                        }
                    }
                }
            }
            LOG_DEBUG(Service_AM, "native video initialize codec={}, output_format={}, input_format={}, output_mode={}, memory_bytes={:#x}, result={:#x}",
                      request->codec, request->output_format, request->input_format, request->output_mode,
                      request->memory_size, result.raw);
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(result.raw);
    }
    struct RegisteredImage {
        u32 id;
        std::shared_ptr<NativeVideoImagePool> pool;
        u32 index;
        u64 completion_value;
        bool available{};
    };
    struct Timing {
        std::chrono::steady_clock::time_point window_start{std::chrono::steady_clock::now()};
        std::chrono::steady_clock::duration decode{}, convert{}, write{};
        u32 frames{};
        u32 dropped{};
    } timing;
    std::optional<s64> movie_clock_offset; // Host time minus pts (us) of the frames the game fetches
    s64 movie_last_fetch{};                // Host time (us) of the game's last fetch
    u64 movie_last_fetched_pts{};
    u64 movie_frames_dropped{};
    // EDEN_VIDEO_TIMING=1: how the frames the game fetches line up with real time (#22).
    struct FetchTiming {
        s64 start_host{};       // first fetch of this run of frames (host us)
        u64 start_pts{};        // its timestamp (us)
        s64 window_start{};     // host us
        s64 last_fetch{};       // host us
        u64 last_pts{};
        u32 fetched{};
        u32 empty{};            // the game asked and no frame was ready
        u64 dropped_at_start{}; // movie_frames_dropped when the window began
        s64 lateness_min{};
        s64 lateness_max{};
        s64 fetch_gap_max{};
        s64 pts_step_max{};
        u64 first_pts_in_window{};
        bool active{};
    } fetch_timing;
    std::vector<u8> rgba_buffer; // Reused between frames, see PumpOutput.
    std::vector<u8> tiled_buffer;
    std::array<u32, 3> rgba_buffer_shape{};
    std::optional<u32> operation_syncpoint;
    u64 next_completion_value{};
    std::vector<RegisteredImage> registered_images;
    std::deque<std::array<u32, 16>> output_properties;
    std::deque<DecodedFrame> decoded_frames;
    std::deque<PendingInput> pending_inputs;
    u64 pending_input_bytes{};
    u64 submissions_refused{};  ///< Submissions refused for a full queue (the log).
    std::chrono::steady_clock::time_point last_frame_out{};
    u64 next_submission_id{1};
    bool started{};
    bool flushed{}; // Flushed since the last Start: Start may resume decoding.
    u64 idle_releases{}; // Releases of images the decoder already held (diagnostics).
    Kernel::KProcess* process;
    NativeVideoKind kind;
    Kernel::KTransferMemory* transfer_memory{};
    std::unique_ptr<libgav1::Decoder> decoder;
#ifdef FEFW_HAVE_DAV1D
    Dav1dContext* dav1d{};
    // The part of an input dav1d hasn't taken yet (it refuses data while a picture waits).
    Dav1dData dav1d_pending{};
#endif
    std::unique_ptr<FFmpeg::Decoder> h264_decoder;
    std::unique_ptr<FFmpeg::DecoderContext> h264_context;
    std::map<u64, u64> h264_timestamps;
    std::optional<GraphicsSupervisorProtocol::VideoDecoderConfiguration> configuration;
    KernelHelpers::ServiceContext context;
    Kernel::KEvent* submission_event{};
    std::shared_ptr<NativeVideoState> state;
    // AV1 decode worker. decode_mutex guards pending_inputs, pending_input_bytes, decoded_frames,
    // decode_busy and decode_error; the worker is the only user of the libgav1 decoder while busy.
    std::mutex decode_mutex;
    std::condition_variable_any decode_cv;
    bool decode_busy{};
    std::optional<Result> decode_error;
    // AV1 inputs libgav1 has, oldest first (it decodes several at once in frame-parallel mode);
    // each is kept until its frame is taken out.
    std::deque<PendingInput> av1_in_flight;
    bool av1_decoder_full{}; // libgav1 said kStatusTryAgain: take a frame out before enqueuing
    bool eos_in_progress{}; // The worker is ending the stream (between taking EOS and its frame)

    // Held by every call and by the output worker while it writes a frame.
    std::mutex codec_mutex;
    // Output worker (AsyncVideoOutput); output_wanted and output_stop are guarded by decode_mutex,
    // deferred_output_error by codec_mutex.
    bool output_wanted{};
    bool output_stop{};
    std::optional<Result> deferred_output_error;
    std::thread output_worker;

    std::jthread decode_worker; // Last: stopped and joined first (Reset also joins it).
};

class INativeVideoContext final : public ServiceFramework<INativeVideoContext> {
public:
    INativeVideoContext(Core::System& system_, std::shared_ptr<NativeVideoState> state_, NativeVideoKind kind_)
        : ServiceFramework{system_, "INativeVideoContext"}, process{state_->process}, kind{kind_}, state{std::move(state_)} {
        process->Open(kernel);
        static const FunctionInfo functions[] = {
            {1, &INativeVideoContext::CreateDecoder, "CreateDecoder"},
        };
        RegisterHandlersTipc(functions);
    }
    ~INativeVideoContext() override { process->Close(kernel); }
private:
    void CreateDecoder(HLERequestContext& ctx) {
        const auto* packet = ctx.CommandBuffer();
        Kernel::KClientSession* client{};
        Result result = Kernel::ResultInvalidCombination;
        if (packet[0] == 0x11 && packet[1] == 0) {
            // EDEN_VIDEO_OWN_THREAD=0 serves the decoder from AM's thread, as before.
            static const bool own_thread = [] {
                const char* const value = std::getenv("EDEN_VIDEO_OWN_THREAD");
                return value == nullptr || value[0] != '0';
            }();
            auto codec = std::make_shared<INativeVideoCodec>(system, state, kind);
            result = own_thread
                         ? CreateGraphicsSessionOn(system, *state->server->Get(), std::move(codec), &client)
                         : CreateGraphicsSession(system, ctx, std::move(codec), &client);
        }
        LOG_DEBUG(Service_AM, "native video decoder create result={:#x}", result.raw);
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(result.raw);
        ctx.AddMoveObject(client);
    }
    Kernel::KProcess* process;
    NativeVideoKind kind;
    std::shared_ptr<NativeVideoState> state;
};
} // namespace

NativeVideoService::NativeVideoService(Core::System& system, std::shared_ptr<Nvidia::Module> module,
                                       Kernel::KProcess* process, std::shared_ptr<void> memory_owner,
                                       std::shared_ptr<NativeVideoServer> server)
    : state{std::make_shared<NativeVideoState>(system, std::move(module), process, std::move(memory_owner),
                                               std::move(server))} {}

std::shared_ptr<SessionRequestHandler> NativeVideoService::CreateContext(NativeVideoKind kind) {
    return std::make_shared<INativeVideoContext>(state->system, state, kind);
}

Result NativeVideoService::MapGpuImage(std::shared_ptr<Nvidia::Devices::nvhost_as_gpu> address_space,
                                      u32 image_id, u64& address, u64& size) {
    const std::scoped_lock lock{state->mutex};
    for (const auto& mapping : state->mappings) {
        if (mapping->address_space == address_space && mapping->image_id == image_id) {
            R_UNLESS(mapping->references < std::numeric_limits<u32>::max(), Kernel::ResultOutOfResource);
            ++mapping->references;
            address = mapping->address;
            size = mapping->size;
            R_SUCCEED();
        }
    }
    for (const auto& pool : state->pools) {
        if (image_id < pool->first_image_id || image_id - pool->first_image_id >= pool->count) continue;
        const u64 offset = u64{image_id - pool->first_image_id} * pool->layout.image_stride;
        size = pool->layout.image_bytes;
        R_UNLESS(address_space->MapNativeImage(pool->memory_id, offset, size, address) == Nvidia::NvResult::Success,
                 Kernel::ResultOutOfResource);
        auto mapping = std::make_shared<NativeVideoMapping>();
        mapping->address_space = std::move(address_space);
        mapping->pool = pool;
        mapping->address = address;
        mapping->size = size;
        mapping->image_id = image_id;
        state->mappings.push_back(std::move(mapping));
        R_SUCCEED();
    }
    R_THROW(Kernel::ResultNotFound);
}

Result NativeVideoService::UnmapGpuImage(const std::shared_ptr<Nvidia::Devices::nvhost_as_gpu>& address_space,
                                        u32 image_id, u64 address, u64 end) {
    const std::scoped_lock lock{state->mutex};
    const auto mapping = std::ranges::find_if(state->mappings, [&](const auto& entry) {
        return entry->address_space == address_space && entry->image_id == image_id &&
               entry->address == address && end >= address && end - address == entry->size - 1;
    });
    R_UNLESS(mapping != state->mappings.end(), Kernel::ResultNotFound);
    if (--(*mapping)->references == 0) state->mappings.erase(mapping);
    R_SUCCEED();
}

Result NativeVideoService::QueryGpuImage(u32 image_id, std::array<u64, 16>& descriptor) {
    const std::scoped_lock lock{state->mutex};
    for (const auto& pool : state->pools) {
        if (image_id < pool->first_image_id || image_id - pool->first_image_id >= pool->count) continue;
        const u64 offset = u64{image_id - pool->first_image_id} * pool->layout.image_stride;
        R_UNLESS(offset <= std::numeric_limits<u32>::max(), Kernel::ResultInvalidSize);
        descriptor = {};
        descriptor[0] = 1;
        descriptor[1] = pool->packed_dimensions;
        descriptor[4] = 0x101000001ULL;
        descriptor[5] = offset | (u64{pool->layout.image_bytes} << 32);
        R_SUCCEED();
    }
    R_THROW(Kernel::ResultNotFound);
}

Result NativeVideoService::QueryVideoImage(u32 image_id, std::array<u64, 16>& descriptor) {
    const std::scoped_lock lock{state->mutex};
    for (const auto& pool : state->pools) {
        if (image_id < pool->first_image_id || image_id - pool->first_image_id >= pool->count) continue;
        R_UNLESS(pool->layout.nv12, Kernel::ResultNotImplemented);
        const u64 offset = u64{image_id - pool->first_image_id} * pool->layout.image_stride;
        R_UNLESS(offset + pool->layout.image_bytes <= std::numeric_limits<u32>::max(), Kernel::ResultInvalidSize);
        // Layout of the NV12 record returned by gsv (run in emulation over a size grid): dimensions
        // and format 0xa, then luma size and image offset, luma pitch, chroma size and offset,
        // chroma pitch.
        descriptor = {};
        descriptor[0] = (u64{pool->packed_dimensions} << 32) | 0xa;
        descriptor[1] = (u64{pool->layout.luma_bytes} << 32) | offset;
        descriptor[2] = pool->layout.row_bytes;
        descriptor[3] = (u64{pool->layout.chroma_bytes} << 32) | (offset + pool->layout.chroma_offset);
        descriptor[4] = pool->layout.row_bytes;
        R_SUCCEED();
    }
    R_THROW(Kernel::ResultNotFound);
}

Result NativeVideoService::DestroyImagePool(u32 first_image_id) {
    const std::scoped_lock lock{state->mutex};
    const auto it = std::ranges::find_if(state->pools, [&](const auto& pool) {
        return pool->first_image_id == first_image_id;
    });
    R_UNLESS(it != state->pools.end(), Kernel::ResultNotFound);
    state->pools.erase(it);
    R_SUCCEED();
}

Result NativeVideoService::CreateImagePool(std::span<const u32> preference, u32 count, u32 memory_id,
                                         u32 first_image_id) {
    const auto layout = CalculateNativeVideoImageLayout(preference, count);
    R_UNLESS(layout && count <= 1024, Kernel::ResultNotImplemented);
    const std::scoped_lock lock{state->mutex};
    R_UNLESS(state->pools.size() < 1024, Kernel::ResultOutOfResource);
    auto& container = state->module->GetContainer();
    auto& nvmap = container.GetNvMapFile();
    const auto memory = nvmap.GetHandle(memory_id);
    R_UNLESS(memory && memory->allocated, Kernel::ResultNotFound);
    const auto* session = container.GetSession(memory->session_id);
    R_UNLESS(session && session->process == state->process, Kernel::ResultInvalidHandle);
    R_UNLESS(memory->size >= layout->size, Kernel::ResultInvalidSize);
    R_UNLESS(memory->Duplicate(true) == Nvidia::NvResult::Success, Kernel::ResultOutOfResource);
    const DAddr device_address = nvmap.PinHandle(memory_id, false);
    if (!device_address) {
        nvmap.FreeHandle(memory_id, true);
        R_THROW(Kernel::ResultOutOfResource);
    }
    auto pool = std::make_shared<NativeVideoImagePool>();
    pool->module = state->module;
    pool->memory_owner = state->memory_owner;
    pool->process = state->process;
    pool->layout = *layout;
    pool->first_image_id = first_image_id;
    pool->count = count;
    pool->memory_id = memory_id;
    pool->address = memory->address;
    pool->device_address = device_address;
    pool->packed_dimensions = preference[2];
    state->pools.push_back(std::move(pool));
    R_SUCCEED();
}
} // namespace Service::AM
