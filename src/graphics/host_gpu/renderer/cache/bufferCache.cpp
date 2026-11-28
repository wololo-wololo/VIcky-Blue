#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;
// GPU-modified ranges come as many small scattered islands, so a readback widens to a window.
// A readback of GPU-modified bytes also takes the GPU-modified bytes around it, so later reads
// nearby do not each wait for the GPU. 4 MiB measured 9 % faster in the cell than 512 KiB (most
// reads after a wait then find their bytes already downloaded). KYTY_DOWNLOAD_WINDOW_KIB (a power
// of two) overrides it for experiments.
uint64_t DownloadWindowSizeFromEnv() {
	const char* value = std::getenv("KYTY_DOWNLOAD_WINDOW_KIB");
	const auto  kib   = value != nullptr ? std::strtoull(value, nullptr, 10) : 0ull;
	return kib != 0 && (kib & (kib - 1)) == 0 ? kib * 1024 : 4 * 1024 * 1024;
}
uint64_t DownloadWindowSize = DownloadWindowSizeFromEnv();

// KYTY_PRECISE_WRITES: 1 drops the GPU-modified marking of written-binding bytes no shader store
// reached; 2 only predicts them and counts downloaded bytes there that changed (should be none).
// Must match Spirv::PreciseWrites(), which makes shaders record their written pages.
uint32_t PreciseWritesModeFromEnv() {
	const char* value = std::getenv("KYTY_PRECISE_WRITES");
	return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
}
const uint32_t PreciseWritesMode = PreciseWritesModeFromEnv();

struct PreciseStats {
	uint64_t resolutions = 0, skipped_busy = 0, overflows = 0, clean_bytes = 0, unmarked_pages = 0;
	uint64_t checked_bytes = 0, missed_bytes = 0;

	void Report() {
		if (++resolutions % 500u != 0u) return;
		std::fprintf(stderr,
		             "precise writes: 500 resolutions, %llu skipped (scan areas busy), %llu "
		             "overflowed; %llu KiB found untouched, %llu tracker pages released; check: "
		             "%llu KiB downloaded, %llu B changed\n",
		             static_cast<unsigned long long>(skipped_busy),
		             static_cast<unsigned long long>(overflows),
		             static_cast<unsigned long long>(clean_bytes >> 10),
		             static_cast<unsigned long long>(unmarked_pages),
		             static_cast<unsigned long long>(checked_bytes >> 10),
		             static_cast<unsigned long long>(missed_bytes));
		skipped_busy = overflows = clean_bytes = unmarked_pages = checked_bytes = missed_bytes = 0;
	}
} g_precise_stats;

// Debug aid (KYTY_READBACK_STATS): synchronous buffer downloads, which wait for the GPU. Counts
// which marking made the bytes GPU-modified (a written binding or a device-address store) and how
// many downloaded bytes differ from guest memory, so needless waits show up.
struct ReadbackStats {
	bool     enabled = std::getenv("KYTY_READBACK_STATS") != nullptr;
	RangeSet binding_writes; // Marked by written bindings since the last download of the bytes.
	RangeSet bda_writes;     // Marked by device-address stores.
	std::map<std::pair<uint64_t, uint64_t>, uint64_t> bindings; // Written binding -> count.
	struct Window {
		uint64_t count = 0, bytes = 0, changed = 0, binding = 0, bda = 0, buffer = 0, size = 0;
	};
	std::map<uint64_t, Window> windows;
	uint64_t downloads = 0, bytes = 0, changed = 0, binding = 0, bda = 0, unchanged_downloads = 0;
	uint64_t window = 0; // Window of the download in progress.

	void Report() {
		if (++downloads % 500u != 0u) return;
		std::fprintf(stderr,
		             "readback stats: 500 sync downloads, %llu KiB, %llu KiB changed (%.1f%%), "
		             "%llu downloads changed nothing; marked by bindings %llu KiB, by device "
		             "address %llu KiB\n",
		             static_cast<unsigned long long>(bytes >> 10),
		             static_cast<unsigned long long>(changed >> 10),
		             bytes != 0 ? 100.0 * static_cast<double>(changed) / static_cast<double>(bytes)
		                        : 0.0,
		             static_cast<unsigned long long>(unchanged_downloads),
		             static_cast<unsigned long long>(binding >> 10),
		             static_cast<unsigned long long>(bda >> 10));
		std::vector<std::pair<uint64_t, uint64_t>> top; // (count, window)
		for (const auto& [start, entry]: windows) top.emplace_back(entry.count, start);
		std::sort(top.begin(), top.end(), std::greater<>());
		for (size_t i = 0; i < top.size() && i < 8u; i++) {
			const auto& w = windows[top[i].second];
			std::fprintf(stderr,
			             "readback stats:   x%llu window 0x%llx buffer 0x%llx+0x%llx: %llu KiB, "
			             "%llu B changed, bindings %llu KiB, device address %llu KiB;",
			             static_cast<unsigned long long>(w.count),
			             static_cast<unsigned long long>(top[i].second),
			             static_cast<unsigned long long>(w.buffer),
			             static_cast<unsigned long long>(w.size),
			             static_cast<unsigned long long>(w.bytes >> 10),
			             static_cast<unsigned long long>(w.changed),
			             static_cast<unsigned long long>(w.binding >> 10),
			             static_cast<unsigned long long>(w.bda >> 10));
			// The written bindings over this window, most frequent first.
			std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> over;
			for (const auto& [range, count]: bindings) {
				if (range.first < top[i].second + DownloadWindowSize &&
				    top[i].second < range.first + range.second) {
					over.emplace_back(count, range);
				}
			}
			std::sort(over.begin(), over.end(), std::greater<>());
			for (size_t j = 0; j < over.size() && j < 3u; j++) {
				std::fprintf(stderr, " written 0x%llx+0x%llx x%llu",
				             static_cast<unsigned long long>(over[j].second.first),
				             static_cast<unsigned long long>(over[j].second.second),
				             static_cast<unsigned long long>(over[j].first));
			}
			std::fprintf(stderr, "\n");
		}
		windows.clear();
		bindings.clear();
		bytes = changed = binding = bda = unchanged_downloads = 0;
	}
} g_readback_stats;

std::pair<Buffer*, uint64_t> UploadMemory(GraphicContext& graphics, CommandScheduler& scheduler,
                                        StreamBuffer& staging, uint64_t size, uint64_t alignment,
                                        vk::BufferUsageFlags usage, const auto& write) {
	const auto [mapped, offset] = staging.Map(size, alignment);
	if (mapped != nullptr) {
		write(mapped, offset);
		staging.Commit();
		return {&staging, offset};
	}

	// Large transfers use the same one-time host allocation as ordinary buffer uploads.
	auto temporary = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Upload, 0,
	                                         usage, size);
	write(temporary->Mapped().data(), 0);
	temporary->Flush(0, size);
	auto* buffer = temporary.get();
	scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return {buffer, 0};
}

} // namespace

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		// Copy waits for ring reuse and flushes the fresh host data before submission.
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		const auto destination_offset = buffer.Offset(address);
		EXIT_IF(destination_offset > buffer.Size() || chunk > buffer.Size() - destination_offset);
		m_scheduler.EndRendering();
		const auto command = m_scheduler.Current().Handle();
		vk::BufferMemoryBarrier2 before {};
		before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		before.srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		before.dstStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		before.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer        = buffer.Handle();
		before.offset        = destination_offset;
		before.size          = chunk;
		vk::DependencyInfo dependency {};
		dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers    = &before;
		command.pipelineBarrier2(dependency);
		const vk::BufferCopy copy {offset, destination_offset, chunk};
		command.copyBuffer(m_staging_buffer.Handle(), buffer.Handle(), 1, &copy);
		auto after          = before;
		after.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		after.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		after.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		after.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		dependency.pBufferMemoryBarriers = &after;
		command.pipelineBarrier2(dependency);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

void BufferCache::Register(BufferId id) {
	ChangeRegister<true>(id);
}

void BufferCache::Unregister(BufferId id) {
	ChangeRegister<false>(id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId id) {
	auto& buffer = m_slot_buffers[id];
	PageTable::PageRange pages {};
	EXIT_IF(!(GuestRange {buffer.CpuAddress(), buffer.Size()}.Valid()) ||
	        !PageTable::TryGetPageRange(buffer.CpuAddress(), buffer.Size(), pages));
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		if constexpr (insert) {
			m_page_table[page] = id;
		} else {
			m_page_table[page] = {};
		}
	}
	const auto size_pages = pages.last_exclusive - pages.first;
	const auto table_offset = PageIndex(buffer.CpuAddress()) * sizeof(vk::DeviceAddress);
	if constexpr (insert) {
		m_buffers.emplace(buffer.CpuAddress(), id);
		m_total_used_memory += buffer.Size();
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		WriteDataBuffer(m_bda_pagetable_buffer, table_offset,
		                addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
		QueueBdaSync(buffer.CpuAddress(), buffer.Size());
	} else {
		m_buffers.erase(m_buffers.find(buffer.CpuAddress()));
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		m_bda_pagetable_buffer.Fill(table_offset,
		                            size_pages * sizeof(vk::DeviceAddress), 0);
		buffer.is_deleted = true;
	}
	if (AliasLogMode() >= 4) {
		UpdateAliasPageEntries(buffer.CpuAddress(), buffer.Size());
	}
}

vk::DeviceAddress BufferCache::OwnPageAddress(uint64_t page_vaddr) {
	const auto* owner = m_page_table.Find(page_vaddr >> PageTable::kPageBits);
	if (owner == nullptr || !*owner) {
		return 0;
	}
	const auto& buffer = m_slot_buffers[*owner];
	if (buffer.is_deleted) {
		return 0;
	}
	return buffer.BufferDeviceAddress() + (page_vaddr - buffer.CpuAddress());
}

uint64_t BufferCache::StorePage(uint64_t page_vaddr) {
	std::vector<AliasPartner> partners;
	AliasPartners(page_vaddr, CACHING_PAGESIZE, partners);
	for (const auto& partner: partners) {
		if (partner.partner >= partner.vaddr || partner.vaddr != page_vaddr ||
		    partner.size != CACHING_PAGESIZE ||
		    (partner.vaddr - partner.partner) % CACHING_PAGESIZE != 0) {
			continue;
		}
		if (OwnPageAddress(partner.partner) != 0) {
			return partner.partner;
		}
	}
	return page_vaddr;
}

void BufferCache::UpdateAliasPageEntries(uint64_t vaddr, uint64_t size) {
	// The lower side of each alias owns the bytes: entries of the upper side's pages point at the
	// lower side's buffer while it has one, else at their own buffer (or nothing).
	std::vector<AliasPartner> partners;
	AliasPartners(vaddr, size, partners);
	if (partners.empty()) {
		return;
	}
	std::vector<std::pair<uint64_t, vk::DeviceAddress>> entries;
	for (const auto& partner: partners) {
		const auto lower = std::min(partner.vaddr, partner.partner);
		const auto upper = std::max(partner.vaddr, partner.partner);
		const auto delta = upper - lower;
		if (delta == 0 || delta % CACHING_PAGESIZE != 0) {
			continue;
		}
		const auto first = Common::AlignUp(upper, CACHING_PAGESIZE);
		const auto last  = Common::AlignDown(upper + partner.size, CACHING_PAGESIZE);
		for (auto page = first; page < last; page += CACHING_PAGESIZE) {
			auto entry = OwnPageAddress(page - delta);
			if (entry == 0) {
				entry = OwnPageAddress(page);
			}
			entries.emplace_back(page, entry);
		}
	}
	std::ranges::sort(entries, {}, &std::pair<uint64_t, vk::DeviceAddress>::first);
	std::vector<vk::DeviceAddress> run;
	uint64_t                       run_page = 0;
	const auto                     flush    = [&] {
        if (!run.empty()) {
            WriteDataBuffer(m_bda_pagetable_buffer, PageIndex(run_page) * sizeof(vk::DeviceAddress),
                            run.data(), run.size() * sizeof(vk::DeviceAddress));
            m_alias_entries += run.size();
            run.clear();
        }
	};
	for (const auto& [page, entry]: entries) {
		if (!run.empty() && page == run_page + run.size() * CACHING_PAGESIZE) {
			run.push_back(entry);
			continue;
		}
		if (!run.empty() && page < run_page + run.size() * CACHING_PAGESIZE) {
			continue; // The same page listed twice.
		}
		flush();
		run_page = page;
		run.push_back(entry);
	}
	flush();
}

void BufferCache::DownloadAliasedGpuWrites() {
	++m_alias_flush_calls;
	// Collect first: each read changes the GPU-modified set.
	RangeSet pieces;
	m_gpu_modified_ranges.ForEach([&](uint64_t start, uint64_t end) {
		std::vector<AliasPartner> partners;
		AliasPartners(start, end - start, partners);
		for (const auto& partner: partners) {
			pieces.Add(partner.vaddr, partner.size);
		}
	});
	std::vector<std::pair<uint64_t, uint64_t>> ranges;
	pieces.ForEach([&](uint64_t start, uint64_t end) { ranges.emplace_back(start, end - start); });
	for (const auto& [vaddr, size]: ranges) {
		if (!IsRegionGpuModified(vaddr, size)) {
			continue;
		}
		ReadMemory(vaddr, size);
		++m_alias_flush_reads;
		m_alias_flush_bytes += size;
	}
}

void BufferCache::RefreshAliasPageTable() {
	for (const auto& [address, id]: m_buffers) {
		UpdateAliasPageEntries(address, m_slot_buffers[id].Size());
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
}

void BufferCache::DeleteBuffer(BufferId id) {
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

template <bool async>
bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	std::vector<std::pair<uint64_t, uint64_t>> predicted; // KYTY_PRECISE_WRITES=2 checks.
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer download");
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    copies.emplace_back(start - buffer_address, total_size, end - start);
			    // Start each packed range on its own 64-byte cache line.
			    total_size += Common::AlignUp(end - start, 64);
		    });
		    m_gpu_modified_ranges.Subtract(address, bytes);
		    if (PreciseWritesMode != 0) {
			    m_gpu_confirmed.Subtract(address, bytes);
			    if (PreciseWritesMode == 2) {
				    m_predicted_clean.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
					    predicted.emplace_back(start, end);
				    });
				    m_predicted_clean.Subtract(address, bytes);
			    }
		    }
		    ForgetColorMetadataChecks(address, bytes);
	    });
	if (copies.empty()) {
		return false;
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferDownloadBytes, total_size);

	auto download = m_download_buffer.MapDownload(total_size, 64);
	for (auto& copy: copies) {
		copy.dstOffset += download.offset;
	}

	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto              native = command.Handle();
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = buffer.Handle();
	before.offset              = 0;
	before.size                = buffer.Size();
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
	                       nullptr);
	native.copyBuffer(buffer.Handle(), download.buffer->Handle(),
	                  static_cast<uint32_t>(copies.size()), copies.data());

	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	after.buffer        = download.buffer->Handle();
	after.offset        = download.offset;
	after.size          = total_size;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands |
	                           vk::PipelineStageFlagBits::eHost,
	                       {}, 0, nullptr, 1, &after, 0, nullptr);
	auto& stats   = g_readback_stats;
	const bool measure = !async && stats.enabled;
	if (measure) {
		auto& window = stats.windows[stats.window];
		++window.count;
		window.buffer = buffer_address;
		window.size   = buffer.Size();
		for (const auto& copy: copies) {
			const auto start = buffer_address + copy.srcOffset;
			uint64_t   from_binding = 0, from_bda = 0;
			stats.binding_writes.ForEachInRange(start, copy.size, [&](uint64_t a, uint64_t b) {
				from_binding += b - a;
			});
			stats.bda_writes.ForEachInRange(start, copy.size, [&](uint64_t a, uint64_t b) {
				from_bda += b - a;
			});
			stats.binding_writes.Subtract(start, copy.size);
			stats.bda_writes.Subtract(start, copy.size);
			window.bytes += copy.size;
			window.binding += from_binding;
			window.bda += from_bda;
			stats.bytes += copy.size;
			stats.binding += from_binding;
			stats.bda += from_bda;
		}
	}
	auto publish = [total_size, buffer_address, copies = std::move(copies),
	                download = std::move(download), measure, predicted = std::move(predicted)] {
		download.buffer->Invalidate(download.offset, total_size);
		uint64_t             changed = 0;
		std::vector<uint8_t> old;
		for (const auto& copy: copies) {
			const auto* bytes = download.mapped + (copy.dstOffset - download.offset);
			const auto  copy_start = buffer_address + copy.srcOffset;
			for (const auto& [start, end]: predicted) {
				// Bytes resolution found untouched must download unchanged.
				const auto first = std::max(start, copy_start);
				const auto last  = std::min(end, copy_start + copy.size);
				if (first >= last) continue;
				old.resize(last - first);
				if (Libs::LibKernel::Memory::TryReadBacking(first, old.data(), last - first)) {
					const auto* fresh = bytes + (first - copy_start);
					uint64_t    missed = 0;
					for (uint64_t i = 0; i < last - first; i++) {
						missed += old[i] != fresh[i] ? 1u : 0u;
					}
					g_precise_stats.checked_bytes += last - first;
					g_precise_stats.missed_bytes += missed;
					static uint32_t reported = 0;
					if (missed != 0 && reported++ < 32u) {
						std::fprintf(stderr,
						             "precise writes: missed write, %llu B changed in 0x%llx+0x%llx\n",
						             static_cast<unsigned long long>(missed),
						             static_cast<unsigned long long>(first),
						             static_cast<unsigned long long>(last - first));
					}
				}
			}
			if (measure) {
				old.resize(copy.size);
				if (Libs::LibKernel::Memory::TryReadBacking(buffer_address + copy.srcOffset,
				                                            old.data(), copy.size)) {
					for (uint64_t i = 0; i < copy.size; i++) {
						changed += old[i] != bytes[i] ? 1u : 0u;
					}
				}
			}
			Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset, bytes,
			                                      copy.size);
		}
		if (measure) {
			auto& stats = g_readback_stats;
			stats.windows[stats.window].changed += changed;
			stats.changed += changed;
			stats.unchanged_downloads += changed == 0 ? 1u : 0u;
			stats.Report();
		}
	};
	if constexpr (async) {
		m_scheduler.DeferPriorityOperation(std::move(publish));
	} else {
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
		publish();
	}
	return true;
}

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 64 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache) {
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
	SetVulkanObjectNameF(m_graphics.device, m_bda_pagetable_buffer.Handle(),
	                     "BDA Page Table Buffer");
	m_scheduler.BeginCommand();
	for (auto* buffer: {&m_bda_pagetable_buffer, m_fault_manager.GetFaultBuffer()}) {
		buffer->Fill(0, buffer->Size(), 0);
	}
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	if (!m_graphics.CanReportMemoryUsage()) {
		return;
	}
	constexpr int64_t GiB              = 1024ll * 1024 * 1024;
	constexpr int64_t target_threshold = 8 * GiB;
	const auto        budget =
	    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
	const auto threshold = std::min(budget, target_threshold);
	const auto expected  = std::min(budget - 6 * threshold / 10, budget - GiB);
	const auto critical  = std::min(budget - 2 * threshold / 10, budget - GiB / 2);
	m_trigger_gc_memory  = static_cast<uint64_t>(std::max<int64_t>(expected, GiB));
	m_critical_gc_memory = static_cast<uint64_t>(std::max<int64_t>(critical, 2 * GiB));
}

BufferCache::~BufferCache() {
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, id]: m_buffers) {
		(void)vaddr;
		const auto& buffer = m_slot_buffers[id];
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] { ReadMemory(vaddr, size, true); });
	QueueBdaSync(vaddr, size);
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write) {
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	m_scheduler.Context().GetGpu().SendCommandSync([this, vaddr, size, is_write] {
		if (is_write && !IsRegionRegistered(vaddr, size)) {
			return;
		}
		auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];
		if (IsRegionGpuModified(vaddr, size)) {
			const auto start =
			    std::max(Common::AlignDown(vaddr, DownloadWindowSize), buffer.CpuAddress());
			const auto end = std::min(std::max(start + DownloadWindowSize, vaddr + size),
			                          buffer.CpuAddress() + buffer.Size());
			if (PreciseWritesMode != 0) {
				ResolvePreciseWrites(start, end - start);
			}
			g_readback_stats.window = start;
			if (DownloadBufferMemory<false>(buffer, start, end - start)) {
				m_memory_tracker.UnmarkRegionAsGpuModified(start, end - start);
			}
		}
		if (is_write) {
			m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
			QueueBdaSync(vaddr, size);
		}
	});
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			return *owner;
		}
	}
	return CreateBuffer(vaddr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(uint64_t vaddr, uint64_t size) {
	static constexpr int      StreamLeapThreshold = 16;
	static constexpr uint64_t StreamLeapSize      = CACHING_PAGESIZE * 128;

	auto       begin      = vaddr;
	auto       end        = vaddr + size;
	const auto find_first = [&](uint64_t address) {
		auto first = m_buffers.lower_bound(address);
		if (first != m_buffers.begin()) {
			const auto  previous = std::prev(first);
			const auto& buffer   = m_slot_buffers[previous->second];
			if (buffer.CpuAddress() + buffer.Size() > address) {
				first = previous;
			}
		}
		return first;
	};
	auto first           = find_first(begin);
	auto last            = first;
	int  stream_score    = 0;
	bool has_stream_leap = false;
	for (; last != m_buffers.end() && last->first < end; ++last) {
		const auto& buffer        = m_slot_buffers[last->second];
		const auto  buffer_begin  = buffer.CpuAddress();
		const auto  buffer_end    = buffer_begin + buffer.Size();
		const bool  expands_left  = buffer_begin < begin;
		const bool  expands_right = buffer_end > end;
		begin                     = std::min(begin, buffer_begin);
		end                       = std::max(end, buffer_end);
		if (!has_stream_leap && (stream_score += buffer.StreamScore()) > StreamLeapThreshold) {
			has_stream_leap = true;
			// Reserve space in the incoming stream's direction of growth.
			// The old buffer extending left of the request predicts growth to the right, and vice versa.
			if (expands_left) {
				end += std::min(StreamLeapSize, (vaddr < LOWER_ADDRESS_SIZE ? LOWER_ADDRESS_SIZE
				                                       : LibKernel::Memory::kExtendedMemoryBase +
				                                             LibKernel::Memory::kExtendedMemorySize) - end);
			}
			if (expands_right) {
				const auto minimum = vaddr < LOWER_ADDRESS_SIZE
				                         ? CACHING_PAGESIZE * 2
				                         : LibKernel::Memory::kExtendedMemoryBase;
				if (begin > minimum) {
					begin -= std::min(StreamLeapSize, begin - minimum);
				}
				first = find_first(begin);
				begin = std::min(begin, first->first);
			}
		}
	}
	return {first, last, begin, end, has_stream_leap};
}

void BufferCache::JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score) {
	auto& new_buffer = m_slot_buffers[new_id];
	auto& overlap    = m_slot_buffers[overlap_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
	}
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	const auto end = Common::AlignUp(vaddr + size, CACHING_PAGESIZE);
	vaddr = Common::AlignDown(vaddr, CACHING_PAGESIZE);
	size               = end - vaddr;
	const auto overlap = ResolveOverlaps(vaddr, size);

	const auto id = m_slot_buffers.insert(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, overlap.begin,
	    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, overlap.end - overlap.begin);
	const auto& buffer = m_slot_buffers[id];
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	for (auto it = overlap.first; it != overlap.last;) {
		const auto old_id = (it++)->second;
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
	if (AliasLogNote(AliasUse::BufferSync, vaddr, size) && AliasLogMode() >= 3) {
		// Bytes the GPU wrote through the other side reach this side through guest memory.
		std::vector<AliasPartner> partners;
		AliasPartners(vaddr, size, partners);
		for (const auto& partner: partners) {
			if (!IsRegionRegistered(partner.partner, partner.size) ||
			    !IsRegionGpuModified(partner.partner, partner.size)) {
				continue;
			}
			if (IsRegionGpuModified(partner.vaddr, partner.size)) {
				++m_alias_conflicts;
				continue;
			}
			ReadMemory(partner.partner, partner.size);
			m_memory_tracker.MarkRegionAsCpuModified(partner.vaddr, partner.size);
			QueueBdaSync(partner.vaddr, partner.size);
			++m_alias_syncs;
			m_alias_sync_bytes += partner.size;
		}
	}
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size = 0;
	m_last_upload_bytes                    = 0;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    copies.emplace_back(total_size, buffer.Offset(address), bytes);
		    total_size += bytes;
	    },
	    [&]() noexcept {
		    m_last_upload_bytes = total_size;
		    const BufferUpload upload {&buffer, copies.size()};
		    UploadCopies({&upload, 1}, copies, total_size);
	    });
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

void BufferCache::UploadCopies(std::span<const BufferUpload> uploads,
                               std::span<vk::BufferCopy> copies, uint64_t total_size) {
	if (copies.empty()) {
		return;
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferUploadBytes, total_size);

	const auto source = UploadMemory(
	    m_graphics, m_scheduler, m_staging_buffer, total_size, 4,
	    vk::BufferUsageFlagBits::eTransferSrc, [&](uint8_t* mapped, uint64_t base_offset) {
		    size_t first = 0;
		    for (const auto& upload: uploads) {
			    for (auto& copy: copies.subspan(first, upload.end - first)) {
				    const auto address = upload.buffer->CpuAddress() + copy.dstOffset;
				    if (!LibKernel::Memory::CopySparseMemory(address, mapped + copy.srcOffset,
					                                         copy.size)) {
					    EXIT("BufferCache: failed to read sparse guest buffer memory\n");
				    }
				    copy.srcOffset += base_offset;
			    }
			    first = upload.end;
		    }
	    });
	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto              native = command.Handle();
	const vk::MemoryBarrier before {
	    .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                 vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
	    .dstAccessMask = vk::AccessFlagBits::eTransferWrite};
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
	                       1, &before, 0, nullptr, 0, nullptr);
	size_t first = 0;
	for (const auto& upload: uploads) {
		native.copyBuffer(source.first->Handle(), upload.buffer->Handle(),
		                  static_cast<uint32_t>(upload.end - first), copies.data() + first);
		first = upload.end;
	}
	const vk::MemoryBarrier after {.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
	                               .dstAccessMask = vk::AccessFlagBits::eMemoryRead |
	                                                vk::AccessFlagBits::eMemoryWrite};
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands,
	                       vk::DependencyFlagBits::eByRegion, 1, &after, 0, nullptr, 0, nullptr);
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id, bool precise_writes) {
	EXIT_IF(!(GuestRange {vaddr, size}.Valid()));

	if (!is_written && size <= CACHING_PAGESIZE &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		// Within one draw window the guest has no signal to rewrite the range. A copy made in this
		// tick at most an eighth of a lap ago is nearly a lap ahead of the ring's writes, like a
		// fresh copy, even if the draw's preparation submits the tick.
		auto& copy = m_stream_copies[(vaddr * 0x9e3779b97f4a7c15ull) >> (64u - STREAM_COPY_BITS)];
		const auto tick = m_scheduler.CurrentTick();
		if (copy.vaddr == vaddr && copy.size == size && copy.window == m_draw_window &&
		    copy.tick == tick &&
		    m_stream_buffer.Position() - copy.position <= m_stream_buffer.Size() / 8) {
			return {&m_stream_buffer, copy.offset};
		}
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		if (mapped != nullptr) {
			if (!LibKernel::Memory::CopySparseMemory(vaddr, mapped, size)) {
				EXIT("BufferCache: failed to read sparse guest buffer memory\n");
			}
			m_stream_buffer.Commit();
			copy = {vaddr, size, m_draw_window, tick, offset, m_stream_buffer.Position()};
			return {&m_stream_buffer, offset};
		}
	}

	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	(void)SynchronizeBuffer(buffer, vaddr, size, is_written, is_texel_buffer);
	if (is_written) {
		if (g_readback_stats.enabled) {
			g_readback_stats.binding_writes.Add(vaddr, size);
			++g_readback_stats.bindings[{vaddr, size}];
		}
		m_gpu_modified_ranges.Add(vaddr, size);
		ForgetColorMetadataChecks(vaddr, size);
		if (PreciseWritesMode != 0) {
			// A prediction holds only up to its resolution: the bytes may be written again now.
			m_predicted_clean.Subtract(vaddr, size);
			if (precise_writes) {
				if (m_precise_open_window != m_draw_window) {
					m_precise_open.ForEach([this](uint64_t start, uint64_t end) {
						m_precise_recorded.Add(start, end - start);
					});
					m_precise_open        = {};
					m_precise_open_window = m_draw_window;
				}
				m_precise_open.Add(vaddr, size);
			} else {
				m_gpu_confirmed.Add(vaddr, size);
			}
		}
	}
	return {&buffer, buffer.Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			TouchBuffer(buffer);
			(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
			return {&buffer, buffer.Offset(vaddr)};
		}
	}
	if (IsRegionGpuModified(vaddr, size)) {
		return ObtainBuffer(vaddr, size, false, false);
	}

	return UploadMemory(m_graphics, m_scheduler, m_staging_buffer, size, 16,
	    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eStorageBuffer,
	    [&](uint8_t* mapped, uint64_t) {
		if (!Libs::LibKernel::Memory::TryReadSparseBacking(vaddr, mapped, size)) {
			EXIT("BufferCache: failed to read mapped guest image backing\n");
		}
	});
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: fill range must be dword aligned\n");
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			EXIT("BufferCache: GDS fill range is out of bounds\n");
		}
		m_gds_buffer.Fill(vaddr, size, value);
		return;
	}
	if (vaddr == 0) {
		EXIT("BufferCache: invalid fill memory address\n");
	}
	(void)m_texture_cache.ClearMeta(vaddr);
	if (!IsRegionGpuModified(vaddr, size)) {
		(void)m_scheduler.Context().InvalidateMemory(vaddr, size);
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	dst->Fill(dst_offset, size, value);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		EXIT("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n",
		     src_vaddr, dst_vaddr, size, static_cast<int>(src_gds), static_cast<int>(dst_gds));
	}
	// A copy into whole levels of a GPU-written image lands on its GPU contents, not stale guest
	// bytes, while no buffer holds the range.
	if (src_memory && dst_memory && !IsRegionRegistered(dst_vaddr, size) &&
	    m_texture_cache.CopyToImage(dst_vaddr, src_vaddr, size)) {
		return;
	}
	if (src_memory && dst_memory && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) && !m_texture_cache.FindImageFromRange(src_vaddr, size)) {
		std::memcpy(reinterpret_cast<void*>(dst_vaddr), reinterpret_cast<const void*>(src_vaddr),
		            size);
		return;
	}

	auto& command = m_scheduler.Current();
	if (dst_memory) {
		m_texture_cache.InvalidateMemoryFromGPU(dst_vaddr, size);
	}
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, id] = *std::prev(candidate);
	return address + m_slot_buffers[id].Size() > vaddr;
}

void BufferCache::MarkColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) {
	for (uint32_t slot = 0; slot < m_checked_color_metadata.size(); slot++) {
		if ((keys & (1u << slot)) != 0) {
			m_checked_color_metadata[slot].Add(vaddr, size);
		}
	}
}

bool BufferCache::IsColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) const {
	for (uint32_t slot = 0; slot < m_checked_color_metadata.size(); slot++) {
		if ((keys & (1u << slot)) != 0 && !m_checked_color_metadata[slot].Contains(vaddr, size)) {
			return false;
		}
	}
	return true;
}

void BufferCache::ForgetColorMetadataChecks(uint64_t vaddr, uint64_t size) {
	// Resident image heaps re-resolve their DCC elements when their metadata changes.
	m_scheduler.Context().GetImageHeap().NoteMetadataWrite(vaddr, size);
	for (auto& checked: m_checked_color_metadata) {
		checked.Subtract(vaddr, size);
	}
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Intersects(vaddr, size);
}

bool BufferCache::HasOnlyGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Contains(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::RunGarbageCollector() {
	const auto tick = m_gc_tick++;
	if (tick % 256u == 255u) {
		std::fprintf(stderr,
		             "bda overwrite: %llu uploads, %llu KiB of CPU-dirty pages uploaded over "
		             "device-address writes in 256 frames (%llu KiB marked)\n",
		             static_cast<unsigned long long>(m_bda_overwrite_uploads),
		             static_cast<unsigned long long>(m_bda_overwrite_bytes >> 10),
		             static_cast<unsigned long long>(m_bda_marked_bytes >> 10));
		m_bda_overwrite_uploads = m_bda_overwrite_bytes = m_bda_marked_bytes = 0;
		if (AliasLogMode() >= 3) {
			std::fprintf(stderr,
			             "alias sync: %llu syncs (%llu KiB) from the other side's GPU writes, %llu "
			             "conflicts (both sides GPU-written) in 256 frames\n",
			             static_cast<unsigned long long>(m_alias_syncs),
			             static_cast<unsigned long long>(m_alias_sync_bytes >> 10),
			             static_cast<unsigned long long>(m_alias_conflicts));
			m_alias_syncs = m_alias_sync_bytes = m_alias_conflicts = 0;
		}
		if (AliasLogMode() >= 4) {
			std::fprintf(stderr,
			             "alias bda: %llu page-table entries rewritten, %llu stores through the upper "
			             "side marked on the lower side in 256 frames\n",
			             static_cast<unsigned long long>(m_alias_entries),
			             static_cast<unsigned long long>(m_alias_redirected_stores));
			m_alias_entries = m_alias_redirected_stores = 0;
		}
		if (AliasFlushEnabled()) {
			std::fprintf(stderr,
			             "alias flush: %llu signals, %llu reads (%llu KiB of aliased ranges with GPU "
			             "writes) in 256 frames\n",
			             static_cast<unsigned long long>(m_alias_flush_calls),
			             static_cast<unsigned long long>(m_alias_flush_reads),
			             static_cast<unsigned long long>(m_alias_flush_bytes >> 10));
			m_alias_flush_calls = m_alias_flush_reads = m_alias_flush_bytes = 0;
		}
	}
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}

	const bool     aggressive = m_total_used_memory >= m_critical_gc_memory;
	const uint64_t age        = std::min<uint64_t>(aggressive ? 80 : 160, tick);
	const size_t   limit      = aggressive ? 64 : 32;

	std::vector<BufferId> dirty_buffers;
	size_t                retire_count = 0;
	m_lru_cache.ForEachItemBelow(tick - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty && !aggressive) {
			return false;
		}
		if (dirty) {
			EXIT_NOT_IMPLEMENTED(!DownloadBufferMemory<true>(buffer, buffer.CpuAddress(), buffer.Size()));
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.MarkRegionAsCpuModified(buffer.CpuAddress(), buffer.Size());
			DeleteBuffer(id);
			Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferEvictions);
		}
		return ++retire_count == limit;
	});
	if (dirty_buffers.empty()) {
		return;
	}

	// Publish all queued downloads before releasing their tracked pages and owners.
	const auto completion_tick = m_scheduler.CurrentTick();
	m_scheduler.Wait(completion_tick);
	m_scheduler.WaitPriorityOperations(completion_tick);
	for (const auto id: dirty_buffers) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		m_memory_tracker.MarkRegionAsCpuModified(buffer.CpuAddress(), buffer.Size());
		Unregister(id);
		m_slot_buffers.erase(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::MarkGpuWritten(uint64_t vaddr, uint64_t size) {
	if (AliasLogMode() < 4) {
		MarkGpuWrittenRange(vaddr, size);
		return;
	}
	// KYTY_ALIAS_LOG=4: a store through the upper side of an alias reached the lower side's buffer.
	const auto end = vaddr + size;
	for (auto page = Common::AlignDown(vaddr, CACHING_PAGESIZE); page < end;
	     page += CACHING_PAGESIZE) {
		const auto first  = std::max(page, vaddr);
		const auto last   = std::min(page + CACHING_PAGESIZE, end);
		const auto target = StorePage(page);
		if (target != page) {
			++m_alias_redirected_stores;
		}
		MarkGpuWrittenRange(target + (first - page), last - first);
	}
}

void BufferCache::MarkGpuWrittenRange(uint64_t vaddr, uint64_t size) {
	// The store went through the page table, so cache buffers back the range. Each takes the
	// same path as a written binding: pending guest writes are uploaded first, then the range
	// becomes GPU-modified and is downloaded when the guest next reads it.
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		auto&      buffer = m_slot_buffers[it->second];
		const auto first  = std::max(buffer.CpuAddress(), vaddr);
		const auto last   = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (first >= last) {
			continue;
		}
		(void)SynchronizeBuffer(buffer, first, last - first, true, false);
		// The upload replaces whole CPU-dirty tracker pages, so GPU bytes in them are lost.
		m_bda_marked_bytes += last - first;
		if (m_last_upload_bytes != 0) {
			m_bda_overwrite_bytes += m_last_upload_bytes;
			++m_bda_overwrite_uploads;
		}
		if (g_readback_stats.enabled) {
			g_readback_stats.bda_writes.Add(first, last - first);
		}
		m_gpu_modified_ranges.Add(first, last - first);
		ForgetColorMetadataChecks(first, last - first);
		if (PreciseWritesMode != 0) {
			m_gpu_confirmed.Add(first, last - first);
			m_predicted_clean.Subtract(first, last - first);
		}
	}
}

uint64_t BufferCache::SetDownloadWindow(uint64_t bytes) noexcept {
	const auto previous = DownloadWindowSize;
	DownloadWindowSize  = bytes;
	return previous;
}

void BufferCache::NoteScannedWrite(uint64_t vaddr, uint64_t size) {
	if (PreciseWritesMode != 0) {
		m_gpu_confirmed.Add(vaddr, size);
		m_predicted_clean.Subtract(vaddr, size);
	}
}

void BufferCache::ResolvePreciseWrites(uint64_t vaddr, uint64_t size) {
	if (m_precise_open_window != m_draw_window) {
		m_precise_open.ForEach([this](uint64_t start, uint64_t end) {
			m_precise_recorded.Add(start, end - start);
		});
		m_precise_open        = {};
		m_precise_open_window = m_draw_window;
	}
	if (!m_precise_recorded.Intersects(vaddr, size)) {
		return;
	}
	auto& stats = g_precise_stats;
	// The scan's area must be free without running deferred operations: those can merge
	// buffers while a draw is being prepared. Draws and dispatches pop them at their start.
	if (!m_fault_manager.CanScanNow()) {
		++stats.skipped_busy;
		stats.Report();
		return;
	}
	// Every recorded precise binding's stores precede this scan, which reports each page a
	// shader stored to since the scan before it; earlier scans' pages are collected with it.
	m_fault_manager.ProcessFaultBuffer();
	m_scheduler.Wait(m_scheduler.CurrentTick());
	if (!m_fault_manager.CollectWrittenPages(
	        [this](uint64_t address) { NoteScannedWrite(address, CACHING_PAGESIZE); })) {
		// Pages the scan could not report stay in the bitmap: resolve after a later scan.
		++stats.overflows;
		stats.Report();
		return;
	}
	RangeSet clean;
	m_precise_recorded.ForEach([&](uint64_t start, uint64_t end) {
		clean.Add(start, end - start);
		m_precise_open.ForEachInRange(start, end - start,
		                              [&](uint64_t a, uint64_t b) { clean.Subtract(a, b - a); });
		m_gpu_confirmed.ForEachInRange(start, end - start,
		                               [&](uint64_t a, uint64_t b) { clean.Subtract(a, b - a); });
	});
	m_precise_recorded = {};
	clean.ForEach([&](uint64_t start, uint64_t end) {
		uint64_t marked = 0;
		m_gpu_modified_ranges.ForEachInRange(start, end - start,
		                                     [&](uint64_t a, uint64_t b) { marked += b - a; });
		stats.clean_bytes += marked;
		if (marked == 0) {
			return;
		}
		if (PreciseWritesMode == 2) {
			m_predicted_clean.Add(start, end - start);
			return;
		}
		m_gpu_modified_ranges.Subtract(start, end - start);
		// Release the tracker pages left with no GPU-modified byte, in runs.
		const auto first = Common::AlignDown(start, TRACKER_PAGE_SIZE);
		const auto last  = Common::AlignUp(end, TRACKER_PAGE_SIZE);
		uint64_t   run   = 0;
		for (uint64_t page = first; page < last; page += TRACKER_PAGE_SIZE) {
			const bool release = !m_gpu_modified_ranges.Intersects(page, TRACKER_PAGE_SIZE);
			if (release) {
				run = run == 0 ? page : run;
				++stats.unmarked_pages;
			}
			if (run != 0 && (!release || page + TRACKER_PAGE_SIZE == last)) {
				const auto run_end = release ? page + TRACKER_PAGE_SIZE : page;
				m_memory_tracker.UnmarkRegionAsGpuModified(run, run_end - run);
				run = 0;
			}
		}
	});
	stats.Report();
}

void BufferCache::QueueBdaSync(uint64_t vaddr, uint64_t size) {
	const auto begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	std::lock_guard lock(m_bda_sync_mutex);
	m_bda_sync.Add(begin, end - begin);
}

void BufferCache::SynchronizeBda(const RangeSet& mapped) {
	const auto queued = [this] {
		std::lock_guard lock(m_bda_sync_mutex);
		return std::exchange(m_bda_sync, {});
	}();
	std::vector<vk::BufferCopy> copies;
	std::vector<BufferUpload>   uploads;
	uint64_t                    total_size = 0;
	queued.ForEach([&](uint64_t begin, uint64_t end) {
		mapped.ForEachInRange(begin, end - begin, [&](uint64_t start, uint64_t finish) {
			auto it = m_buffers.upper_bound(start);
			if (it != m_buffers.begin()) {
				--it;
			}
			for (; it != m_buffers.end() && it->first < finish; ++it) {
				auto&      buffer = m_slot_buffers[it->second];
				const auto first  = std::max(buffer.CpuAddress(), start);
				const auto last   = std::min(buffer.CpuAddress() + buffer.Size(), finish);
				if (first < last) {
					// Reads follow every range's protection; a write in between re-queues its page.
					m_memory_tracker.ForEachUploadRange(
					    first, last - first, false,
					    [&](uint64_t address, uint64_t bytes) noexcept {
						    if (uploads.empty() || uploads.back().buffer != &buffer) {
							    uploads.push_back({&buffer, 0});
						    }
						    copies.emplace_back(total_size, buffer.Offset(address), bytes);
						    uploads.back().end = copies.size();
						    total_size += bytes;
					    },
					    []() noexcept {});
				}
			}
		});
	});
	UploadCopies(uploads, copies, total_size);
}

} // namespace Libs::Graphics
