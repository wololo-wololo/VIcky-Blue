#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <map>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

// Debug aid (issue 22, KYTY_ALIAS_LOG): counts GPU-cache uses of physically aliased guest ranges.
// StaleImage: an aliased image, not GPU-modified and not known to be CPU-dirty, whose sampled
// guest bytes changed since its last refresh (a write the caches did not see).
enum class AliasUse { BufferSync, ImageRefresh, WriteFault, ReadFault, StaleImage, Count };
// Returns whether the range overlaps an aliased range.
bool AliasLogNote(AliasUse use, uint64_t vaddr, uint64_t size);
// Returns whether the snapshot of aliased ranges changed.
bool AliasLogTick();
// Experiment (issue 22), KYTY_ALIAS_LOG=3: the caches keep aliased guest ranges in step through
// guest memory. AliasPartners lists, for the aliased pieces of a range, the piece on this side and
// the address of the same bytes on the other side.
struct AliasPartner {
	uint64_t vaddr;
	uint64_t partner;
	uint64_t size;
};
int  AliasLogMode();
// Experiment (issue 22), KYTY_ALIAS_FLUSH=1 (with KYTY_ALIAS_LOG set): before each end-of-pipe
// signal and each flip, GPU-written bytes in aliased ranges are downloaded into guest memory, so a
// CPU read through the other address (which does not fault) sees them too.
bool AliasFlushEnabled();
void AliasPartners(uint64_t vaddr, uint64_t size, std::vector<AliasPartner>& out);

struct GraphicContext;
class CommandScheduler;
class TextureCache;

using BufferId = Common::SlotId;
inline constexpr BufferId NULL_BUFFER_ID {0};

class BufferCache {
public:
	static constexpr uint32_t CACHING_PAGEBITS  = 14;
	static constexpr uint64_t CACHING_PAGESIZE  = uint64_t {1} << CACHING_PAGEBITS;
	static constexpr uint64_t CACHING_NUMPAGES  = (LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemorySize) >> CACHING_PAGEBITS;
	static constexpr uint64_t BDA_PAGETABLE_SIZE =
	    CACHING_NUMPAGES * sizeof(vk::DeviceAddress);

	static constexpr uint64_t PageIndex(uint64_t address) {
		return (address < LOWER_ADDRESS_SIZE
		            ? address
		            : address - LibKernel::Memory::kExtendedMemoryBase + LOWER_ADDRESS_SIZE) >>
		       CACHING_PAGEBITS;
	}
	static constexpr uint64_t GuestAddress(uint64_t offset) {
		return offset < LOWER_ADDRESS_SIZE
		           ? offset
		           : offset - LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemoryBase;
	}

	BufferCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	            TextureCache& texture_cache);
	~BufferCache();
	KYTY_CLASS_NO_COPY(BufferCache);

	void                   InvalidateMemory(uint64_t vaddr, uint64_t size);
	void                   ReadMemory(uint64_t vaddr, uint64_t size, bool is_write = false);
	// The span a readback widens to (a power of two); returns the previous one. For tests.
	static uint64_t        SetDownloadWindow(uint64_t bytes) noexcept;
	// Ends the draw window, whose draws share the stream copies of CPU-written ranges.
	void                   EndDrawWindow() noexcept { m_draw_window++; }
	[[nodiscard]] uint64_t DrawWindow() const noexcept { return m_draw_window; }
	[[nodiscard]] Buffer&  GetBuffer(BufferId id) { return m_slot_buffers[id]; }
	[[nodiscard]] BufferId FindBuffer(uint64_t vaddr, uint64_t size);
	// precise_writes: a storage-buffer binding of a shader that records the pages it stores to
	// (KYTY_PRECISE_WRITES), so pages it leaves untouched can drop the GPU-modified marking.
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBuffer(uint64_t vaddr, uint64_t size,
	                                                        bool     is_written,
	                                                        bool     is_texel_buffer = false,
	                                                        BufferId id              = {},
	                                                        bool     precise_writes  = false);
	[[nodiscard]] StreamBuffer&                GetUtilityBuffer(MemoryUsage usage) noexcept {
		switch (usage) {
			case MemoryUsage::Upload: return m_staging_buffer;
			case MemoryUsage::Stream: return m_stream_buffer;
			case MemoryUsage::Download: return m_download_buffer;
			case MemoryUsage::DeviceLocal: return m_device_buffer;
		}
		EXIT("BufferCache: invalid utility-buffer usage\n");
	}
	[[nodiscard]] const Buffer* GetGdsBuffer() const noexcept { return &m_gds_buffer; }
	[[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept { return &m_bda_pagetable_buffer; }
	[[nodiscard]] Buffer* GetFaultBuffer() noexcept { return m_fault_manager.GetFaultBuffer(); }
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBufferForImage(uint64_t vaddr, uint64_t size);
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasOnlyGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	// Color metadata the GPU found free of the clear keys in `keys` (bits of code >> 5) stays so
	// until a GPU write or download.
	void               MarkColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys);
	[[nodiscard]] bool IsColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) const;
	void               ProcessFaultBuffer();
	// Pages a shader wrote through device addresses, reported after the dispatch ran.
	void               MarkGpuWritten(uint64_t vaddr, uint64_t size);
	// Pages a fault-buffer scan reported as stored to, through a binding or a device address.
	void               NoteScannedWrite(uint64_t vaddr, uint64_t size);
	// Uploads the CPU-dirty bytes of the queued ranges `mapped` covers behind one barrier pair.
	void SynchronizeBda(const RangeSet& mapped);
	// Queue after the range's state change becomes visible, or a drain could consume it first.
	void               QueueBdaSync(uint64_t vaddr, uint64_t size);
	void               RunGarbageCollector();
	// Experiment (issue 22), KYTY_ALIAS_LOG=4: device-address accesses through the upper side of
	// an alias reach the lower side's host buffer. Rewrites the entries of every registered buffer.
	void               RefreshAliasPageTable();
	// KYTY_ALIAS_FLUSH: downloads the GPU-modified bytes of aliased ranges. GPU thread only.
	void               DownloadAliasedGpuWrites();

private:
	friend struct BufferCacheTestAccess;

	// KYTY_PRECISE_WRITES: before a readback of [vaddr, vaddr + size), scans the written pages
	// and drops the GPU-modified marking of recorded precise bindings' bytes no store reached.
	void ResolvePreciseWrites(uint64_t vaddr, uint64_t size);

	bool IsBufferInvalid(BufferId id) const { return !id || m_slot_buffers[id].is_deleted; }

	using BufferMap = std::map<uint64_t, BufferId>;
	struct OverlapResult {
		BufferMap::iterator first;
		BufferMap::iterator last;
		uint64_t            begin;
		uint64_t            end;
		bool                has_stream_leap;
	};

	// Copies [previous upload's end, end) go to `buffer`.
	struct BufferUpload {
		Buffer* buffer;
		size_t  end;
	};

	// The stream copy of a CPU-written range, in a slot picked by its address.
	struct StreamCopy {
		uint64_t vaddr    = 0;
		uint64_t size     = 0;
		uint64_t window   = 0;
		uint64_t tick     = 0;
		uint64_t offset   = 0;
		uint64_t position = 0;
	};
	static constexpr uint32_t STREAM_COPY_BITS = 10;

	using PageTable = MultiLevelPageTable<BufferId, CACHING_PAGEBITS, 44, 20>;
	static_assert(CACHING_PAGESIZE == (uint64_t {1} << PageTable::kPageBits));
	void WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source, uint64_t size);
	void TouchBuffer(const Buffer& buffer);
	[[nodiscard]] OverlapResult ResolveOverlaps(uint64_t vaddr, uint64_t size);
	void JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score);
	[[nodiscard]] BufferId CreateBuffer(uint64_t vaddr, uint64_t size);
	void                   Register(BufferId id);
	void Unregister(BufferId id);
	template <bool insert>
	void ChangeRegister(BufferId id);
	void DeleteBuffer(BufferId id);
	// KYTY_ALIAS_LOG=4: the host address the page at `page_vaddr` has in its own buffer, or 0.
	[[nodiscard]] vk::DeviceAddress OwnPageAddress(uint64_t page_vaddr);
	// KYTY_ALIAS_LOG=4: the guest page holding the bytes a store through `page_vaddr` reached.
	[[nodiscard]] uint64_t          StorePage(uint64_t page_vaddr);
	// KYTY_ALIAS_LOG=4: rewrites the upper-side entries of the aliased pieces in the range.
	void                            UpdateAliasPageEntries(uint64_t vaddr, uint64_t size);
	void                            MarkGpuWrittenRange(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                     bool is_written, bool is_texel_buffer);
	void UploadCopies(std::span<const BufferUpload> uploads, std::span<vk::BufferCopy> copies,
	                  uint64_t total_size);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size);
	void               ForgetColorMetadataChecks(uint64_t vaddr, uint64_t size);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	FaultManager                                      m_fault_manager;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	Common::SlotVector<Buffer>                        m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
	// KYTY_PRECISE_WRITES bookkeeping. Precise bindings of the current draw window (their
	// commands may not be recorded yet), of earlier windows (recorded, unresolved), and the
	// GPU-modified bytes some writer is known to have written, kept until they are downloaded.
	RangeSet                                          m_precise_open;
	uint64_t                                          m_precise_open_window = 0;
	RangeSet                                          m_precise_recorded;
	RangeSet                                          m_gpu_confirmed;
	// KYTY_PRECISE_WRITES=2: bytes resolution found untouched but left marked, to check that
	// their downloads change nothing.
	RangeSet                                          m_predicted_clean;
	// One per TileManager::ColorClearSlots key slot.
	std::array<RangeSet, 8>                           m_checked_color_metadata;
	MemoryTracker                                     m_memory_tracker;
	std::mutex                                        m_bda_sync_mutex;
	RangeSet                                          m_bda_sync;
	StreamBuffer                                      m_staging_buffer;
	StreamBuffer                                      m_stream_buffer;
	StreamBuffer                                      m_download_buffer;
	StreamBuffer                                      m_device_buffer;
	std::array<StreamCopy, 1u << STREAM_COPY_BITS>    m_stream_copies {};
	uint64_t                                          m_draw_window = 0;
	// Issue 22 counter: bytes SynchronizeBuffer last uploaded, and those MarkGpuWritten uploaded
	// from CPU-dirtied guest memory over pages a shader wrote through device addresses.
	uint64_t                                          m_last_upload_bytes = 0;
	uint64_t m_bda_overwrite_bytes = 0, m_bda_overwrite_uploads = 0, m_bda_marked_bytes = 0;
	uint64_t m_alias_syncs = 0, m_alias_sync_bytes = 0, m_alias_conflicts = 0;
	uint64_t m_alias_entries = 0, m_alias_redirected_stores = 0;
	uint64_t m_alias_flush_calls = 0, m_alias_flush_reads = 0, m_alias_flush_bytes = 0;
	TextureCache&                                     m_texture_cache;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t m_gc_tick            = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
