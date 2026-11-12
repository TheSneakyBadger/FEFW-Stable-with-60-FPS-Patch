// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <utility>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <type_traits>
// TODO: find out which don't require stable iters
#include <unordered_map>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"
#include <vector>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include <queue>

#include "common/common_types.h"
#include "common/hash.h"
#include "common/literals.h"
#include "common/lru_cache.h"
#include <ranges>
#include "common/scratch_buffer.h"
#include "common/slot_vector.h"
#include "video_core/texture_cache/pending_download.h"
#include "common/thread_worker.h"
#include "video_core/compatible_formats.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/delayed_destruction_ring.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/descriptor_table.h"
#include "video_core/texture_cache/dependency_cache.h"
#include "video_core/texture_cache/image_base.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view_base.h"
#include "video_core/texture_cache/render_targets.h"
#include "video_core/texture_cache/texture_bank_reuse.h"
#include "video_core/texture_cache/types.h"
#include "video_core/textures/texture.h"

namespace Tegra {
namespace Control {
struct ChannelState;
}
} // namespace Tegra

namespace VideoCommon {

using Tegra::Texture::TICEntry;
using Tegra::Texture::TSCEntry;
using VideoCore::Surface::PixelFormat;
using namespace Common::Literals;

struct ImageViewInOut {
    u32 index{};
    bool blacklist{};
    ImageViewId id{};
};

/// A compact table's resolved bank as the feedback-loop check sees it: the stamp names exactly
/// these view ids (see texture_bank_reuse.h).
struct FeedbackBank {
    u64 stamp{};
    std::span<const ImageViewId> ids;
};

struct AsyncDecodeContext {
    ImageId image_id;
    Common::ScratchBuffer<u8> decoded_data;
    boost::container::small_vector<BufferImageCopy, 16> copies;
    std::mutex mutex;
    std::atomic_bool complete;
    /// The image was deleted: drop the result instead of uploading it into the slot's next image.
    bool cancelled{};
};

using TextureCacheGPUMap = ::Common::unordered_map<u64, std::vector<ImageId>, Common::IdentityHash<u64>>;

class TextureCacheChannelInfo : public ChannelInfo {
public:
    TextureCacheChannelInfo() = delete;
    TextureCacheChannelInfo(Tegra::Control::ChannelState& state) noexcept;
    TextureCacheChannelInfo(const TextureCacheChannelInfo& state) = delete;
    TextureCacheChannelInfo& operator=(const TextureCacheChannelInfo&) = delete;

    DescriptorTable<TICEntry> graphics_image_table;
    DescriptorTable<TSCEntry> graphics_sampler_table;
    DescriptorTable<TICEntry> compute_image_table;
    DescriptorTable<TSCEntry> compute_sampler_table;

    DependencyCache<TICEntry, ImageViewId, ImageViewId, 1> image_views;
    std::unordered_map<TSCEntry, SamplerId> samplers;

    ::Common::unordered_map<u32, SamplerId> sampler_ids;
    ::Common::unordered_map<u32, ImageViewId> image_view_ids;

    TextureCacheGPUMap* gpu_page_table = nullptr;
    TextureCacheGPUMap* sparse_page_table = nullptr;
};

template <class P>
class TextureCache : public VideoCommon::ChannelSetupCaches<TextureCacheChannelInfo> {
    /// Address shift for caching images into a hash table
    static constexpr u64 YUZU_PAGEBITS = 20;

    /// Enables debugging features to the texture cache
    static constexpr bool ENABLE_VALIDATION = P::ENABLE_VALIDATION;
    /// Implement blits as copies between framebuffers
    static constexpr bool FRAMEBUFFER_BLITS = P::FRAMEBUFFER_BLITS;
    /// True when some copies have to be emulated
    static constexpr bool HAS_EMULATED_COPIES = P::HAS_EMULATED_COPIES;
    /// True when the API can provide info about the memory of the device.
    static constexpr bool HAS_DEVICE_MEMORY_INFO = P::HAS_DEVICE_MEMORY_INFO;
    /// True when the API can do asynchronous texture downloads.
    static constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS = P::IMPLEMENTS_ASYNC_DOWNLOADS;

    static constexpr size_t UNSET_CHANNEL{(std::numeric_limits<size_t>::max)()};

#ifdef YUZU_LEGACY
    static constexpr s64 TARGET_THRESHOLD = 3_GiB;
#else
    static constexpr s64 TARGET_THRESHOLD = 4_GiB;
#endif

    static constexpr s64 DEFAULT_EXPECTED_MEMORY = 1_GiB + 125_MiB;
    static constexpr s64 DEFAULT_CRITICAL_MEMORY = 1_GiB + 625_MiB;
    static constexpr size_t GC_EMERGENCY_COUNTS = 2;

    using Runtime = typename P::Runtime;
    using Image = typename P::Image;
    using ImageAlloc = typename P::ImageAlloc;
    using ImageView = typename P::ImageView;
    using Sampler = typename P::Sampler;
    using Framebuffer = typename P::Framebuffer;
    using AsyncBuffer = typename P::AsyncBuffer;
    using BufferType = typename P::BufferType;

    struct PendingUnswizzle {
        ImageId image_id;
        VideoCommon::ImageInfo info;
        size_t current_offset = 0;
        size_t total_size = 0;
        AsyncBuffer staging_buffer;
        size_t last_submitted_offset = 0;
        size_t bytes_per_slice;
        bool initialized = false;
    };

    struct BlitImages {
        ImageId dst_id;
        ImageId src_id;
        PixelFormat dst_format;
        PixelFormat src_format;
    };

public:
    explicit TextureCache(Runtime&, Tegra::MaxwellDeviceMemoryManager&);

    /// Notify the cache that a new frame has been queued
    void TickFrame();

    /// Return a constant reference to the given image view id
    [[nodiscard]] const ImageView& GetImageView(ImageViewId id) const noexcept;

    /// Return a reference to the given image view id
    [[nodiscard]] ImageView& GetImageView(ImageViewId id) noexcept;

    /// Get the imageview from the graphics descriptor table in the specified index
    [[nodiscard]] ImageView& GetImageView(u32 index) noexcept;

    /// Mark an image as modified from the GPU
    void MarkModification(ImageId id) noexcept;

    /// Fill image_view_ids with the graphics/compute images in indices
    void FillImageViews(std::span<ImageViewInOut> views, bool compute, bool blacklist = true);

    /// Handle feedback loops during draws.
    void CheckFeedbackLoop(std::span<const ImageViewInOut> views);

    /// As above, for the views plus more_ids (views bound without an element-by-element copy, see
    /// compact_texture_table.h). Only which ids are bound matters, not how often.
    /// bound_views_key: names the bound views in place of hashing every id. banks: when given, the
    /// blocks' ids (more_ids) as resolved banks, checked through a per-bank image list.
    void CheckFeedbackLoop(std::span<const ImageViewInOut> views, std::span<const ImageViewId> more_ids,
                           const u64* bound_views_key = nullptr,
                           std::span<const FeedbackBank> banks = {});

    /// Get the sampler from the graphics/compute descriptor table in the specified index
    Sampler* GetSampler(u32 index, bool compute);

    /// Get the sampler id from the graphics/compute descriptor table in the specified index
    SamplerId GetSamplerId(u32 index, bool compute);

    /// Return a constant reference to the given sampler id
    [[nodiscard]] const Sampler& GetSampler(SamplerId id) const noexcept;

    /// Return a reference to the given sampler id
    [[nodiscard]] Sampler& GetSampler(SamplerId id) noexcept;

    /// Refresh the state for graphics/compute image view and sampler descriptors
    void SynchronizeDescriptors(bool compute);

    /// Updates the Render Targets if they can be rescaled
    /// @retval True if the Render Targets have been rescaled.
    bool RescaleRenderTargets();

    /// Update bound render targets and upload memory if necessary
    /// @param is_clear True when the render targets are being used for clears
    void UpdateRenderTargets(bool is_clear);

    /// Find a framebuffer with the currently bound render targets
    /// UpdateRenderTargets should be called before this
    Framebuffer* GetFramebuffer();

    /// Mark images in a range as modified from the CPU
    void WriteMemory(DAddr cpu_addr, size_t size);

    /// Download contents of host images to guest memory in a region
    void DownloadMemory(DAddr cpu_addr, size_t size);

    std::optional<VideoCore::RasterizerDownloadArea> GetFlushArea(DAddr cpu_addr, u64 size);

    /// Remove images in a region
    void UnmapMemory(DAddr cpu_addr, size_t size);

    /// Remove images in a region
    void UnmapGPUMemory(size_t as_id, GPUVAddr gpu_addr, size_t size);

    /// Blit an image with the given parameters
    bool BlitImage(const Tegra::Engines::Fermi2D::Surface& dst,
                   const Tegra::Engines::Fermi2D::Surface& src,
                   const Tegra::Engines::Fermi2D::Config& copy);

    /// NVIDIA DLSS in place of a game's DLSS evaluation: temporal upscale of src into dst along
    /// motion_vectors, with the depth (see the runtime's TemporalUpscale). Only images the cache
    /// already has are used (as the game's own passes find them). False when they cannot take it.
    bool TemporalUpscale(const Tegra::Texture::TICEntry& dst, const Tegra::Texture::TICEntry& src,
                         const Tegra::Texture::TICEntry& motion_vectors,
                         const Tegra::Texture::TICEntry& depth, std::array<f32, 2> jitter,
                         std::array<f32, 2> motion_scale);

    /// Try to find a cached image view in the given CPU address
    [[nodiscard]] std::pair<ImageView*, bool> TryFindFramebufferImageView(
        const Tegra::FramebufferConfig& config, DAddr cpu_addr);

    /// Return true when there are uncommitted images to be downloaded
    [[nodiscard]] bool HasUncommittedFlushes() const noexcept;

    /// Return true when the caller should wait for async downloads
    [[nodiscard]] bool ShouldWaitAsyncFlushes() const noexcept;

    /// Commit asynchronous downloads
    void CommitAsyncFlushes();

    /// Pop asynchronous downloads
    void PopAsyncFlushes();

    [[nodiscard]] ImageId DmaImageId(const Tegra::DMA::ImageOperand& operand, bool is_upload);

    [[nodiscard]] std::pair<Image*, BufferImageCopy> DmaBufferImageCopy(
        const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::BufferOperand& buffer_operand,
        const Tegra::DMA::ImageOperand& image_operand, ImageId image_id, bool modifies_image);

    void DownloadImageIntoBuffer(Image* image, BufferType buffer, size_t buffer_offset,
                                 std::span<const VideoCommon::BufferImageCopy> copies,
                                 GPUVAddr address = 0, size_t size = 0);

    /// Return true when a CPU region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(DAddr addr, size_t size);

    [[nodiscard]] bool IsRescaling() const noexcept;

    [[nodiscard]] bool IsRescaling(const ImageViewBase& image_view) const noexcept;

    /// Create channel state.
    void CreateChannel(Tegra::Control::ChannelState& channel) final override;

    /// Prepare an image to be used
    void PrepareImage(ImageId image_id, bool is_modification, bool invalidate);

    /// Texture banks reused between graphics draws (texture_bank_reuse.h). Bumped wherever a
    /// resolved bank may have become stale.
    [[nodiscard]] u64 BankGeneration() const noexcept {
        return bank_generation;
    }

    /// Bumped only where image views are erased (their ids may then name other views) or channels
    /// created. Within one view generation an image view id keeps its handles and its image's
    /// scaling, so descriptors made from the same ids are the same (see PushImageDescriptors).
    [[nodiscard]] u64 ViewGeneration() const noexcept {
        return view_generation;
    }

    /// Writes the view ids last resolved for these TIC indices into ids, if still valid, after
    /// redoing what reuse cannot skip (alias synchronization, LRU ticks). Returns the bank's stamp,
    /// or 0 if it has to be resolved (FillImageViews) and stored again.
    [[nodiscard]] u64 TryReuseViewBank(u64 hash, std::span<const u32> tics, std::span<ImageViewId> ids);

    /// Keeps view ids just resolved for these TIC indices. Returns their stamp, or 0 if they cannot
    /// be kept.
    u64 StoreViewBank(u64 hash, std::span<const u32> tics, std::span<const ImageViewId> ids);

    /// As TryReuseViewBank, for sampler ids of TSC indices.
    [[nodiscard]] u64 TryReuseSamplerBank(u64 hash, std::span<const u32> tscs, std::span<SamplerId> ids);

    /// As StoreViewBank, for sampler ids of TSC indices.
    u64 StoreSamplerBank(u64 hash, std::span<const u32> tscs, std::span<const SamplerId> ids);

    std::recursive_mutex mutex;

private:
    /// Pops the front batch of committed_downloads. Images kept alive only for its downloads go
    /// to retired_images rather than being destroyed here.
    void PopCommittedDownloads();

    /// Whether a finished download of a deleted image must not be written back to guest memory.
    [[nodiscard]] bool SkipRetiredSwizzle(const PendingImageDownload<Image>& download_info) const;

    /// Iterate over all page indices in a range
    template <typename Func>
    static void ForEachCPUPage(DAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> YUZU_PAGEBITS;
        for (u64 page = addr >> YUZU_PAGEBITS; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    template <typename Func>
    static void ForEachGPUPage(GPUVAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> YUZU_PAGEBITS;
        for (u64 page = addr >> YUZU_PAGEBITS; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    void OnGPUASRegister(size_t map_id) final override;

    /// Runs the Garbage Collector.
    u32 RunGarbageCollector(); ///< Returns how many images it freed

    /// Find or create an image view in the guest descriptor table
    ImageViewId VisitImageView(u32 index, bool compute);

    /// Find or create a framebuffer with the given render target parameters
    FramebufferId GetFramebufferId(const RenderTargets& key);

    /// Refresh the contents (pixel data) of an image
    [[nodiscard]] bool IsDownloadable(const ImageBase& image) const noexcept;

    void RefreshContents(Image& image, ImageId image_id);

    /// Upload data from guest to an image
    template <typename StagingBuffer>
    void UploadImageContents(Image& image, StagingBuffer& staging_buffer);

    /// Create a new image view from a guest descriptor
    [[nodiscard]] ImageViewId CreateImageView(const TICEntry& config);

    /// Find or create an image from the given parameters
    [[nodiscard]] ImageId FindOrInsertImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                            RelaxedOptions options = RelaxedOptions{});

    /// Find an image from the given parameters
    [[nodiscard]] ImageId FindImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                    RelaxedOptions options);

    /// Create an image from the given parameters
    [[nodiscard]] ImageId InsertImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                      RelaxedOptions options);

    /// Create a new image and join perfectly matching existing images
    /// Remove joined images from the cache
    [[nodiscard]] ImageId JoinImages(const ImageInfo& info, GPUVAddr gpu_addr, DAddr cpu_addr);

    [[nodiscard]] ImageId FindDMAImage(const ImageInfo& info, GPUVAddr gpu_addr);

    /// The CPU address of an image at gpu_addr, or a stand-in fixed by the GPU address when the
    /// range has no CPU backing (so later lookups find the same image).
    [[nodiscard]] std::optional<DAddr> ImageCpuAddress(GPUVAddr gpu_addr, size_t size) const;

    /// Return a blit image pair from the given guest blit parameters
    [[nodiscard]] std::optional<BlitImages> GetBlitImages(
        const Tegra::Engines::Fermi2D::Surface& dst, const Tegra::Engines::Fermi2D::Surface& src,
        const Tegra::Engines::Fermi2D::Config& copy);

    /// Find or create a sampler from a guest descriptor sampler
    [[nodiscard]] SamplerId FindSampler(const TSCEntry& config, bool compute);

    /// Find or create an image view for the given color buffer index
    [[nodiscard]] ImageViewId FindColorBuffer(size_t index);

    /// Find or create an image view for the depth buffer
    [[nodiscard]] ImageViewId FindDepthBuffer();

    /// Find or create a view for a render target with the given image parameters
    [[nodiscard]] ImageViewId FindRenderTargetView(const ImageInfo& info, GPUVAddr gpu_addr);

    /// Iterates over all the images in a region calling func
    template <typename Func>
    void ForEachImageInRegion(DAddr cpu_addr, size_t size, Func&& func);

    template <typename Func>
    void ForEachImageInRegionGPU(size_t as_id, GPUVAddr gpu_addr, size_t size, Func&& func);

    template <typename Func>
    void ForEachSparseImageInRegion(size_t as_id, GPUVAddr gpu_addr, size_t size, Func&& func);

    /// Iterates over all the images in a region calling func
    template <typename Func>
    void ForEachSparseSegment(ImageBase& image, Func&& func);

    /// Find or create an image view in the given image with the passed parameters
    [[nodiscard]] ImageViewId FindOrEmplaceImageView(ImageId image_id, const ImageViewInfo& info);

    /// Register image in the page table
    void RegisterImage(ImageId image);

    /// Unregister image from the page table
    void UnregisterImage(ImageId image);

    /// Track CPU reads and writes for image
    void TrackImage(ImageBase& image, ImageId image_id);

    /// Stop tracking CPU reads and writes for image
    void UntrackImage(ImageBase& image, ImageId image_id);

    /// Delete image from the cache
    void DeleteImage(ImageId image, bool immediate_delete = false);

    /// The video memory an image is counted as using (what DeleteImage gives back).
    [[nodiscard]] u64 ImageMemoryEstimate(const ImageBase& image);

    /// Remove image views references from the cache
    void RemoveImageViewReferences(std::span<const ImageViewId> removed_views);

    /// Remove framebuffers using the given image views from the cache
    void RemoveFramebuffers(std::span<const ImageViewId> removed_views);

    /// Mark an image as modified from the GPU
    void MarkModification(ImageBase& image) noexcept;

    /// Synchronize image aliases, copying data if needed
    void SynchronizeAliases(ImageId image_id);

    /// Prepare an image view to be used
    void PrepareImageView(ImageViewId image_view_id, bool is_modification, bool invalidate);

    /// Execute copies from one image to the other, even if they are incompatible
    void CopyImage(ImageId dst_id, ImageId src_id, std::vector<ImageCopy> copies);

    /// Bind an image view as render target, downloading resources preemtively if needed
    void BindRenderTarget(ImageViewId* old_id, ImageViewId new_id);

    /// Create a render target from a given image and image view parameters
    [[nodiscard]] std::pair<FramebufferId, ImageViewId> RenderTargetFromImage(
        ImageId, const ImageViewInfo& view_info);

    /// Returns true if the current clear parameters clear the whole image of a given image view
    [[nodiscard]] bool IsFullClear(ImageViewId id);

    [[nodiscard]] std::pair<u32, u32> PrepareDmaImage(ImageId dst_id, GPUVAddr base_addr,
                                                      bool mark_as_modified);

    bool ImageCanRescale(ImageBase& image);
    void InvalidateScale(Image& image);
    bool ScaleUp(Image& image);
    bool ScaleDown(Image& image);
    u64 GetScaledImageSizeBytes(const ImageBase& image);

    void QueueAsyncDecode(Image& image, ImageId image_id);
    void TickAsyncDecode();

    void QueueAsyncUnswizzle(Image& image, ImageId image_id);
    void TickAsyncUnswizzle();

    Runtime& runtime;

    Tegra::MaxwellDeviceMemoryManager& device_memory;
    std::deque<TextureCacheGPUMap> gpu_page_table_storage;

    RenderTargets render_targets;
    u64 render_targets_serial = 0;

    // Testing (EDEN_FG_TRACE=1): one frame's draws after the DLSS evaluation, by render target,
    // to find where the game starts drawing its UI.
    void TraceRenderTargets();

public:
    /// Frame generation: the game's UI layer this frame (Fortune's Weave draws its UI into its own
    /// full-size RGBA8 image right after the DLSS evaluation, and combines it with the 3D picture
    /// at the end), or null.
    [[nodiscard]] std::array<ImageView*, 4> FrameGenUiLayers();

    /// Frame generation: which of the game's frames is being presented (the frame's buffer is at
    /// the given device address). The game may already be drawing its next frame when the
    /// previous one is presented; that frame's UI layer is then gone (the stash has it).
    struct FrameGenFrameInfo {
        u64 seq;      ///< The frame's DLSS evaluation (1, 2, ...); 0 for none.
        bool known;   ///< Its buffer was seen being drawn after an evaluation.
        bool previous; ///< The game already evaluated DLSS for its next frame.
        std::array<ImageView*, 4> layers; ///< The UI layers (null when previous).
    };
    [[nodiscard]] FrameGenFrameInfo FrameGenFrame(DAddr presented, u32 width, u32 height);

private:
    void WatchUiLayer();
    [[nodiscard]] std::array<ImageId, 4> FrameGenUiLayerIds();
    /// DLSS evaluations so far (each starts a frame).
    u64 fg_seq = 0;
    /// Presented buffers and the frame last drawn into each. Only draws to an image of the
    /// buffer's own size count: Fortune's Weave also uses the buffers' memory as a smaller
    /// scratch target (2560x1440 in a 3840x2160 buffer) right after each DLSS evaluation, while
    /// the buffer still holds a frame two evaluations old waiting to be presented.
    struct PresentMark {
        DAddr address;
        u32 width;
        u32 height;
        u64 seq;
    };
    std::array<PresentMark, 4> fg_present_marks{};
    size_t fg_present_next = 0;
    /// The last frame whose UI layer was copied.
    u64 fg_ui_copied = 0;
    /// Testing (EDEN_FG_TRACE=1): frame order events around 30 s in.
    [[nodiscard]] bool FrameGenOrderTrace() const;
    void FrameGenCopyUi();
    bool fg_ui_watch = false;     ///< Counting draws (the game evaluated DLSS last frame or this).
    bool fg_ui_dlss_seen = false; ///< DLSS evaluated this frame.
    ImageId fg_ui_exclude{};
    Extent3D fg_ui_size{};
    /// Full-size 8-bit colour targets drawn this frame and how many draws each got.
    std::array<std::pair<ImageId, u32>, 8> fg_ui_counts{};
    size_t fg_ui_count_size = 0;
    /// The last frame's most drawn UI layer: frames that don't redraw the UI keep showing it.
    ImageId fg_ui_last{};
    GPUVAddr fg_ui_last_addr = 0;
    void FlushRenderTargetTrace();
    bool fg_trace_active = false;
    bool fg_trace_done = false;
    std::string fg_trace_last;
    u32 fg_trace_count = 0;
    u32 fg_trace_lines = 0;
    u32 rt_active_mask = 0;
    std::array<ImageId, 8> rt_image_id{};
    ImageId rt_depth_image_id{};
    u64 texture_bindings_serial = 0;
    u64 last_feedback_loop_serial = 0;
    u64 last_feedback_texture_serial = 0;
    u64 last_feedback_views_hash = 0;
    bool last_feedback_loop_result = false;
    /// Per bank stamp: the bank's (image, view) pairs sorted by image, for the feedback-loop check.
    /// Valid within one view generation (view ids keep their images until one is erased).
    ::Common::unordered_map<u64, std::vector<std::pair<ImageId, ImageViewId>>> feedback_bank_images;
    u64 feedback_bank_generation = 0;
    FramebufferId last_framebuffer_id{};
    u64 last_framebuffer_serial = 0;

    DependencyCache<RenderTargets, FramebufferId, ImageViewId, NUM_RT + 1> framebuffers;
    ::Common::unordered_map<u64, std::vector<ImageMapId>, Common::IdentityHash<u64>> page_table;
    ::Common::unordered_map<ImageId, boost::container::small_vector<ImageViewId, 16>> sparse_views;

    DAddr virtual_invalid_space{};

    bool has_deleted_images = false;
    bool is_rescaling = false;

    /// Validates the entry's table and snapshot against the current graphics descriptor table.
    template <typename Table>
    [[nodiscard]] bool BankTableMatches(const TextureBankKey& entry, const Table& table) const;

    /// Fills the common part of a bank entry; false if the snapshot cannot be taken or does not
    /// hold what the table resolved (the guest rewrote an entry since).
    template <typename Table>
    bool CaptureBank(TextureBankKey& entry, u64 hash, std::span<const u32> indices, const Table& table);


    u64 bank_generation = 1;
    u64 view_generation = 1;
    u64 bank_stamp = 0;
    TextureBankCache<ViewBankEntry> view_banks;
    TextureBankCache<SamplerBankEntry> sampler_banks;
    u64 total_used_memory = 0;
    u64 minimum_memory;
    u64 expected_memory;
    u64 critical_memory;
    size_t gpu_unswizzle_maxsize = 0;
    size_t swizzle_chunk_size = 0;
    u32 swizzle_slices_per_batch = 0;

    struct BufferDownload {
        GPUVAddr address;
        size_t size;
    };

    using PendingDownload = PendingImageDownload<Image>;

    Common::SlotVector<Image> slot_images;
    Common::SlotVector<ImageMapView> slot_map_views;
    Common::SlotVector<ImageView> slot_image_views;
    Common::SlotVector<ImageAlloc> slot_image_allocs;
    Common::SlotVector<Sampler> slot_samplers;
    Common::SlotVector<Framebuffer> slot_framebuffers;
    Common::SlotVector<BufferDownload> slot_buffer_downloads;

    // TODO: This data structure is not optimal and it should be reworked

    std::vector<PendingDownload> uncommitted_downloads;
    std::deque<std::vector<PendingDownload>> committed_downloads;
    std::vector<AsyncBuffer> uncommitted_async_buffers;
    std::deque<std::vector<AsyncBuffer>> async_buffers;
    std::deque<AsyncBuffer> async_buffers_death_ring;
    /// Images whose queued downloads have finished. Commands recorded after a download can still
    /// use them, so TickFrame moves them into sentenced_images instead of destroying them at once.
    std::vector<std::shared_ptr<Image>> retired_images;

    struct LRUItemParams {
        using ObjectType = ImageId;
        using TickType = u64;
    };
    Common::LeastRecentlyUsedCache<LRUItemParams> lru_cache;

 #ifdef YUZU_LEGACY
    static constexpr size_t TICKS_TO_DESTROY = 6;
 #else
    static constexpr size_t TICKS_TO_DESTROY = 8;
#endif
    DelayedDestructionRing<Image, TICKS_TO_DESTROY> sentenced_images;
    u64 memory_limit = ~0ULL;        ///< Video memory the emulator may use
    /// Bytes freed by recent cleanup passes (frame, bytes): images are destroyed, and stop
    /// counting in the memory in use, TICKS_TO_DESTROY frames after a pass frees them.
    std::array<std::pair<u64, u64>, 16> recent_frees{};
    /// Loading screens (bursts of the game's map data reads): from the first read's frame to the
    /// last, and how much was read. Once 2 s pass without reads, what the previous scene left
    /// behind is freed (scene_sweep_below: images unused since the load began).
    struct SceneLoad {
        bool active = false;
        u64 start_frame = 0;
        u64 last_frame = 0;
        u64 bytes = 0;
    } scene_load;
    u64 scene_load_bytes_seen = 0;
    u64 scene_sweep_below = 0;
    u64 collect_quiet_until = 0;     ///< No cleanup pass before this frame unless over the limit
    u64 texture_memory_estimate = 0; ///< Bytes of the cached images (diagnostics)
    u32 minute_passes = 0;           ///< Cleanup since the last "Video memory in use" report
    u64 minute_freed = 0;
    double minute_collect_ms = 0.0;
    DelayedDestructionRing<ImageView, TICKS_TO_DESTROY> sentenced_image_view;
    DelayedDestructionRing<Framebuffer, TICKS_TO_DESTROY> sentenced_framebuffers;

    ::Common::unordered_map<GPUVAddr, ImageAllocId> image_allocs_table;

    Common::ScratchBuffer<u8> swizzle_data_buffer;
    Common::ScratchBuffer<u8> unswizzle_data_buffer;

    u64 modification_tick = 0;
    u64 frame_tick = 0;

    // Declared before the worker so they outlive it: members are destroyed in reverse order, and
    // the worker's thread is joined (finishing the decode it is on) only when the worker is
    // destroyed. The other way round, stopping the emulation while a texture was decoding
    // (accelerate_astc=CpuAsynchronous) let that decode write into a freed context and crashed.
    std::vector<std::unique_ptr<AsyncDecodeContext>> async_decodes;
    Common::ThreadWorker texture_decode_worker{1, "TextureDecoder", {},
                                               Common::ThreadPlacement::Efficiency};

    std::deque<PendingUnswizzle> unswizzle_queue;
    u8 current_unswizzle_frame;

    // Join caching
    boost::container::small_vector<ImageId, 4> join_overlap_ids;
    ::Common::unordered_set<ImageId> join_overlaps_found;
    boost::container::small_vector<ImageId, 4> join_left_aliased_ids;
    boost::container::small_vector<ImageId, 4> join_right_aliased_ids;
    ::Common::unordered_set<ImageId> join_ignore_textures;
    boost::container::small_vector<ImageId, 4> join_bad_overlap_ids;
    struct JoinCopy {
        bool is_alias;
        ImageId id;
    };
    boost::container::small_vector<JoinCopy, 4> join_copies_to_do;
    ::Common::unordered_map<ImageId, size_t> join_alias_indices;
};

} // namespace VideoCommon
