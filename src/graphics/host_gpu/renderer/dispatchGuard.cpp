#include "graphics/host_gpu/renderer/dispatchGuard.h"

#include "common/logging/log.h"
#include "gpu_blit_shaders/gpu_dispatch_guard_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cinttypes>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics {

namespace {

// The push constants of gpu_dispatch_guard.comp.
struct Params {
	uint32_t source_word;
	uint32_t destination_word;
	uint32_t max_x;
	uint32_t max_y;
	uint32_t max_z;
	uint32_t max_groups;
	uint32_t address_low;
	uint32_t address_high;
};

// The trip buffer of gpu_dispatch_guard.comp.
constexpr uint64_t TripValuesOffset    = 16;
constexpr uint64_t TripAddressesOffset = 16 + 16 * 16;
constexpr uint64_t TripBytes           = 16 + 2 * 16 * 16;

} // namespace

bool DispatchArgsGuard::Enabled() {
	static const bool enabled = [] {
		const char* text = std::getenv("KYTY_DISPATCH_GUARD");
		return text == nullptr || text[0] != '0';
	}();
	return enabled;
}

DispatchArgsGuard::DispatchArgsGuard(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics) {
	m_ring = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
	                                  SlotBytes * SlotCount);
	m_trips =
	    std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, TripBytes);
	std::memset(m_trips->Mapped().data(), 0, m_trips->Mapped().size());
	m_trips->Flush(0, m_trips->Size());

	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = std::size(bindings);
	layout_info.pBindings    = bindings;
	RequireVulkanSuccess(
	    graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_descriptor_layout),
	    "create dispatch guard descriptor layout");
	const vk::PushConstantRange  range {vk::ShaderStageFlagBits::eCompute, 0, sizeof(Params)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_descriptor_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &range;
	RequireVulkanSuccess(
	    graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr, &m_layout),
	    "create dispatch guard pipeline layout");
	m_pipeline = CreateComputePipeline(graphics.device, GPU_DISPATCH_GUARD_SPV, m_layout,
	                                   "create dispatch guard pipeline");

	// A full-screen 4K pass of 8x8 groups is about 130,000 groups. More than 4M is taken as
	// garbage: it keeps the host GPU busy for seconds, which the driver answers with a reset.
	if (const char* text = std::getenv("KYTY_DISPATCH_GUARD_MAX_GROUPS");
	    text != nullptr && text[0] != '\0') {
		char*      end    = nullptr;
		const auto parsed = std::strtoull(text, &end, 0);
		if (end != text && *end == '\0' && parsed >= 1024 && parsed <= UINT32_MAX) {
			m_max_groups = static_cast<uint32_t>(parsed);
		}
	}
	LOGF("DispatchArgsGuard: temporary workaround: indirect dispatches above %u groups are zeroed "
	     "(KYTY_DISPATCH_GUARD=0 disables, KYTY_DISPATCH_GUARD_MAX_GROUPS sets the limit)\n",
	     m_max_groups);
}

DispatchArgsGuard::~DispatchArgsGuard() {
	const auto device = m_graphics.device;
	device.destroyPipeline(m_pipeline, nullptr);
	device.destroyPipelineLayout(m_layout, nullptr);
	device.destroyDescriptorSetLayout(m_descriptor_layout, nullptr);
}

void DispatchArgsGuard::Report() {
	if (!m_trips->IsCoherent()) {
		m_trips->Invalidate(0, m_trips->Size());
	}
	const auto* base  = m_trips->Mapped().data();
	uint32_t    count = 0;
	std::memcpy(&count, base, sizeof(count));
	if (count == m_reported_trips) {
		return;
	}
	const uint32_t          at = (count - 1u) % 16u;
	std::array<uint32_t, 4> values {};
	std::array<uint32_t, 4> address {};
	std::memcpy(values.data(), base + TripValuesOffset + at * 16u, sizeof(values));
	std::memcpy(address.data(), base + TripAddressesOffset + at * 16u, sizeof(address));
	LOGF("DispatchArgsGuard: zeroed %u indirect dispatch(es) with out-of-range group counts; the "
	     "last had groups=%u,%u,%u from args at 0x%016" PRIx64 "\n",
	     count - m_reported_trips, values[0], values[1], values[2],
	     (static_cast<uint64_t>(address[1]) << 32u) | address[0]);
	m_reported_trips = count;
}

std::pair<vk::Buffer, uint64_t> DispatchArgsGuard::Record(CommandBuffer& buffer, const Buffer& args,
                                                          uint64_t offset, uint64_t guest_address) {
	// The trip counter is only reported, and reading it is not free: look every 64 dispatches.
	if ((m_next & 63u) == 0) {
		Report();
	}
	const auto     command = buffer.Handle();
	const auto&    limits  = m_graphics.GetPhysicalDeviceProperties().limits;
	const uint64_t slot    = (m_next++ % SlotCount) * SlotBytes;
	// A storage buffer binding starts on the device's alignment; the shader indexes from there.
	const uint64_t alignment   = limits.minStorageBufferOffsetAlignment;
	const uint64_t source_base = offset - offset % alignment;
	EXIT_IF((offset - source_base) % 4u != 0);

	// Every shader pass of the renderer ends with ShaderAccessBarrier, which already orders the
	// writers of the counts before this read; the guard ends with the same barrier.
	const std::array buffers {
	    vk::DescriptorBufferInfo {args.Handle(), source_base,
		                          offset - source_base + sizeof(vk::DispatchIndirectCommand)},
	    vk::DescriptorBufferInfo {m_ring->Handle(), 0, VK_WHOLE_SIZE},
	    vk::DescriptorBufferInfo {m_trips->Handle(), 0, VK_WHOLE_SIZE}};
	std::array<vk::WriteDescriptorSet, 3> writes {};
	for (uint32_t i = 0; i < writes.size(); ++i) {
		writes[i].dstBinding      = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[i].pBufferInfo     = &buffers[i];
	}
	const Params params {.source_word      = static_cast<uint32_t>((offset - source_base) / 4u),
	                     .destination_word = static_cast<uint32_t>(slot / 4u),
	                     .max_x            = limits.maxComputeWorkGroupCount[0],
	                     .max_y            = limits.maxComputeWorkGroupCount[1],
	                     .max_z            = limits.maxComputeWorkGroupCount[2],
	                     .max_groups       = m_max_groups,
	                     .address_low      = static_cast<uint32_t>(guest_address),
	                     .address_high     = static_cast<uint32_t>(guest_address >> 32u)};
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_layout, 0, writes.size(),
	                             writes.data());
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	command.pushConstants(m_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params);
	command.dispatch(1, 1, 1);

	ShaderAccessBarrier(command, vk::PipelineStageFlagBits::eComputeShader);
	return {m_ring->Handle(), slot};
}

} // namespace Libs::Graphics
