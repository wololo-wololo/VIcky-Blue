#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_

#include "common/abi.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <cstdint>

namespace Libs::Graphics {

class BufferCache;

// The guest address of a scan record of a page stored to, or 0 for a fault record.
uint64_t WrittenPageAddress(uint64_t record);

class FaultManager {
	static constexpr size_t MaxPendingFaults = 8;

public:
	// Page records one scan reports, the count included; matches MAX_PAGE_FAULTS of
	// fault_buffer_process.comp. Bits past them stay set for the next scan.
	static constexpr size_t MaxPageFaults = 16384;

	FaultManager(GraphicContext& graphics, CommandScheduler& scheduler, BufferCache& buffer_cache);
	~FaultManager();
	KYTY_CLASS_NO_COPY(FaultManager);

	[[nodiscard]] Buffer* GetFaultBuffer() noexcept { return &m_fault_buffer; }
	void                  ProcessFaultBuffer();
	// ProcessFaultBuffer would not wait for and run deferred operations to free its area.
	[[nodiscard]] bool    CanScanNow() const noexcept { return m_fault_areas[m_current_area] == 0; }
	// After the scans' ticks completed: passes each written page of the scans whose deferred
	// operation has not run yet to `page`. False if the latest scan overflowed, leaving pages
	// for a later scan.
	template <typename Fn>
	bool CollectWrittenPages(Fn&& page);

private:
	GraphicContext&                            m_graphics;
	CommandScheduler&                          m_scheduler;
	BufferCache&                               m_buffer_cache;
	Buffer                                     m_fault_buffer;
	Buffer                                     m_download_buffer;
	std::array<uint64_t, MaxPendingFaults>      m_fault_areas {};
	uint32_t                                   m_current_area = 0;
	vk::DescriptorSetLayout                    m_fault_process_desc_layout = nullptr;
	vk::Pipeline                               m_fault_process_pipeline = nullptr;
	vk::PipelineLayout                         m_fault_process_pipeline_layout = nullptr;

	// Records of a completed scan area: count (capped) and whether the scan overflowed.
	[[nodiscard]] const uint64_t* AreaRecords(uint32_t area, uint64_t& count, bool& overflow);
};

template <typename Fn>
bool FaultManager::CollectWrittenPages(Fn&& page) {
	bool latest_overflow = false;
	const auto latest = (m_current_area + MaxPendingFaults - 1) % MaxPendingFaults;
	for (uint32_t area = 0; area < MaxPendingFaults; ++area) {
		if (m_fault_areas[area] == 0) continue;
		uint64_t count    = 0;
		bool     overflow = false;
		const auto* records = AreaRecords(area, count, overflow);
		latest_overflow |= area == latest && overflow;
		for (uint64_t index = 1; index <= count; ++index) {
			const auto address = WrittenPageAddress(records[index]);
			if (address != 0) page(address);
		}
	}
	return !latest_overflow;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_
