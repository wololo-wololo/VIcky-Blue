#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHGUARD_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHGUARD_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <cstdint>
#include <memory>
#include <utility>

namespace Libs::Graphics {

// Workaround: checks the group counts of an indirect dispatch on the GPU before the dispatch
// reads them. Counts the GPU wrote cannot be checked on the CPU without waiting for the GPU, and
// counts above the device limit, or a huge number of groups, make the host driver reset the
// device. Such a dispatch is zeroed and reported instead.
//
//   KYTY_DISPATCH_GUARD=0             disables the guard
//   KYTY_DISPATCH_GUARD_MAX_GROUPS=n  the largest x * y * z let through (default 4194304)
class DispatchArgsGuard {
public:
	DispatchArgsGuard(GraphicContext& graphics, CommandScheduler& scheduler);
	~DispatchArgsGuard();
	KYTY_CLASS_NO_COPY(DispatchArgsGuard);

	[[nodiscard]] static bool Enabled();

	// Records the check of the three counts at `offset` of `args`. Returns the buffer and offset
	// to dispatch from. Binds its own compute pipeline and push descriptors: record it before
	// the dispatch's own bindings are committed.
	[[nodiscard]] std::pair<vk::Buffer, uint64_t> Record(CommandBuffer& buffer, const Buffer& args,
	                                                     uint64_t offset, uint64_t guest_address);

private:
	static constexpr uint64_t SlotBytes = 16;
	static constexpr uint64_t SlotCount = 65536;

	void Report();

	GraphicContext&         m_graphics;
	std::unique_ptr<Buffer> m_ring;
	// Host-visible: the number of zeroed dispatches and what the last 16 held.
	std::unique_ptr<Buffer> m_trips;
	uint32_t                m_reported_trips = 0;
	uint64_t                m_next           = 0;
	uint32_t                m_max_groups     = 1u << 22u;
	vk::DescriptorSetLayout m_descriptor_layout = nullptr;
	vk::PipelineLayout      m_layout            = nullptr;
	vk::Pipeline            m_pipeline          = nullptr;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHGUARD_H_ */
