#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/presentation/videoOut.h"
#include "kernel/memory.h"
#include "libs/errno.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <atomic>
#include <chrono>

namespace Libs::Graphics {

namespace {

struct AliasLog {
	struct Hit {
		uint64_t size        = 0;
		uint64_t other_vaddr = 0;
		uint64_t counts[static_cast<int>(AliasUse::Count)] {};
	};
	std::mutex                                     mutex;
	std::vector<LibKernel::Memory::AliasedRange> ranges;
	uint64_t                                       max_size = 0;
	uint64_t                                       tick     = 0;
	uint64_t uses[static_cast<int>(AliasUse::Count)] {}, bytes[static_cast<int>(AliasUse::Count)] {};
	uint64_t all[static_cast<int>(AliasUse::Count)] {};
	std::map<uint64_t, Hit> hits;
};

AliasLog& GetAliasLog() {
	static AliasLog log;
	return log;
}

// Debug aid: KYTY_UNMAP_TRACE=1 prints, once a second, how many guest unmaps went through the
// GPU thread and how long the callers waited (queue wait, run time, full GPU waits).
struct UnmapTrace {
	std::atomic<uint64_t> count {0};
	std::atomic<uint64_t> bytes {0};
	std::atomic<uint64_t> wall_ns {0};
	std::atomic<uint64_t> queue_ns {0};
	std::atomic<uint64_t> run_ns {0};
	std::atomic<uint64_t> finish_count {0};
	std::atomic<uint64_t> finish_ns {0};
	std::atomic<uint64_t> max_wall_ns {0};
	std::atomic<int64_t>  last_print_ns {0};
};

UnmapTrace& GetUnmapTrace() {
	static UnmapTrace trace;
	return trace;
}

bool UnmapTraceEnabled() {
	static const bool enabled = std::getenv("KYTY_UNMAP_TRACE") != nullptr;
	return enabled;
}

int64_t NowNs() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

void RecordUnmap(uint64_t size, int64_t start, int64_t run_start, int64_t run_end, int64_t end,
                 bool finished, int64_t finish_ns) {
	auto&      trace = GetUnmapTrace();
	const auto wall  = static_cast<uint64_t>(end - start);
	trace.count.fetch_add(1, std::memory_order_relaxed);
	trace.bytes.fetch_add(size, std::memory_order_relaxed);
	trace.wall_ns.fetch_add(wall, std::memory_order_relaxed);
	trace.queue_ns.fetch_add(static_cast<uint64_t>(run_start - start), std::memory_order_relaxed);
	trace.run_ns.fetch_add(static_cast<uint64_t>(run_end - run_start), std::memory_order_relaxed);
	if (finished) {
		trace.finish_count.fetch_add(1, std::memory_order_relaxed);
		trace.finish_ns.fetch_add(static_cast<uint64_t>(finish_ns), std::memory_order_relaxed);
	}
	auto max = trace.max_wall_ns.load(std::memory_order_relaxed);
	while (wall > max && !trace.max_wall_ns.compare_exchange_weak(max, wall)) {
	}
	auto last = trace.last_print_ns.load(std::memory_order_relaxed);
	if (end - last < 1'000'000'000 || !trace.last_print_ns.compare_exchange_strong(last, end)) {
		return;
	}
	const auto count = trace.count.exchange(0);
	std::fprintf(stderr,
	             "unmap trace: n=%llu bytes=%llu wall_ms=%.1f queue_ms=%.1f run_ms=%.1f "
	             "finish_n=%llu finish_ms=%.1f max_ms=%.2f\n",
	             static_cast<unsigned long long>(count),
	             static_cast<unsigned long long>(trace.bytes.exchange(0)),
	             static_cast<double>(trace.wall_ns.exchange(0)) / 1e6,
	             static_cast<double>(trace.queue_ns.exchange(0)) / 1e6,
	             static_cast<double>(trace.run_ns.exchange(0)) / 1e6,
	             static_cast<unsigned long long>(trace.finish_count.exchange(0)),
	             static_cast<double>(trace.finish_ns.exchange(0)) / 1e6,
	             static_cast<double>(trace.max_wall_ns.exchange(0)) / 1e6);
	std::fflush(stderr);
}

} // namespace

bool AliasLogNote(AliasUse use, uint64_t vaddr, uint64_t size) {
	if (!LibKernel::Memory::AliasLogEnabled() || size == 0) {
		return false;
	}
	auto&           log = GetAliasLog();
	std::lock_guard lock(log.mutex);
	const auto      kind = static_cast<int>(use);
	++log.all[kind];
	auto it = std::ranges::upper_bound(log.ranges, vaddr + size - 1, {},
	                                   &LibKernel::Memory::AliasedRange::vaddr);
	bool counted = false;
	while (it != log.ranges.begin()) {
		--it;
		if (it->vaddr + log.max_size <= vaddr) {
			break;
		}
		if (it->vaddr + it->size <= vaddr) {
			continue;
		}
		const auto start = std::max(it->vaddr, vaddr);
		const auto end   = std::min(it->vaddr + it->size, vaddr + size);
		if (!counted) {
			++log.uses[kind];
			counted = true;
		}
		log.bytes[kind] += end - start;
		auto& hit       = log.hits[it->vaddr];
		hit.size        = it->size;
		hit.other_vaddr = it->other_vaddr;
		++hit.counts[kind];
	}
	return counted;
}

int AliasLogMode() {
	static const int mode = [] {
		const char* value = std::getenv("KYTY_ALIAS_LOG");
		return value != nullptr ? std::max(1, std::atoi(value)) : 0;
	}();
	return mode;
}

bool AliasFlushEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_ALIAS_FLUSH");
		return value != nullptr && std::atoi(value) != 0 && AliasLogMode() >= 1;
	}();
	return enabled;
}

void AliasPartners(uint64_t vaddr, uint64_t size, std::vector<AliasPartner>& out) {
	if ((AliasLogMode() < 3 && !AliasFlushEnabled()) || size == 0) {
		return;
	}
	auto&           log = GetAliasLog();
	std::lock_guard lock(log.mutex);
	auto it = std::ranges::upper_bound(log.ranges, vaddr + size - 1, {},
	                                   &LibKernel::Memory::AliasedRange::vaddr);
	while (it != log.ranges.begin()) {
		--it;
		if (it->vaddr + log.max_size <= vaddr) {
			break;
		}
		if (it->vaddr + it->size <= vaddr) {
			continue;
		}
		const auto start = std::max(it->vaddr, vaddr);
		const auto end   = std::min(it->vaddr + it->size, vaddr + size);
		out.push_back({start, it->other_vaddr + (start - it->vaddr), end - start});
	}
}

bool AliasLogTick() {
	if (!LibKernel::Memory::AliasLogEnabled()) {
		return false;
	}
	auto ranges = LibKernel::Memory::AliasedRanges();
	auto& log   = GetAliasLog();
	std::lock_guard lock(log.mutex);
	const auto tick    = log.tick++;
	bool       changed = false;
	if (tick % 64u == 0u) {
		uint64_t total = 0;
		log.max_size   = 0;
		for (const auto& range: ranges) {
			total += range.size;
			log.max_size = std::max(log.max_size, range.size);
		}
		changed    = ranges.size() != log.ranges.size();
		log.ranges = std::move(ranges);
		std::fprintf(stderr, "alias ranges: %llu entries, %llu KiB (both sides)\n",
		             static_cast<unsigned long long>(log.ranges.size()),
		             static_cast<unsigned long long>(total >> 10));
	}
	if (tick % 256u != 255u) {
		return changed;
	}
	std::fprintf(stderr,
	             "alias use in 256 frames: buffer syncs %llu of %llu (%llu KiB), image refreshes "
	             "%llu of %llu (%llu KiB), write faults %llu of %llu, read faults %llu of %llu, "
	             "stale aliased images %llu (%llu KiB)\n",
	             static_cast<unsigned long long>(log.uses[0]),
	             static_cast<unsigned long long>(log.all[0]),
	             static_cast<unsigned long long>(log.bytes[0] >> 10),
	             static_cast<unsigned long long>(log.uses[1]),
	             static_cast<unsigned long long>(log.all[1]),
	             static_cast<unsigned long long>(log.bytes[1] >> 10),
	             static_cast<unsigned long long>(log.uses[2]),
	             static_cast<unsigned long long>(log.all[2]),
	             static_cast<unsigned long long>(log.uses[3]),
	             static_cast<unsigned long long>(log.all[3]),
	             static_cast<unsigned long long>(log.uses[4]),
	             static_cast<unsigned long long>(log.bytes[4] >> 10));
	std::vector<std::pair<uint64_t, AliasLog::Hit>> hits(log.hits.begin(), log.hits.end());
	std::ranges::sort(hits, [](const auto& a, const auto& b) {
		const auto sum = [](const AliasLog::Hit& h) {
			return h.counts[0] + h.counts[1] + h.counts[2] + h.counts[3] + h.counts[4] * 1000u;
		};
		return sum(a.second) > sum(b.second);
	});
	for (size_t i = 0; i < hits.size() && i < 12; i++) {
		const auto& [vaddr, hit] = hits[i];
		const auto other         = log.hits.find(hit.other_vaddr);
		std::fprintf(stderr,
		             "  alias va 0x%llx+0x%llx (other side 0x%llx): buffer %llu image %llu "
		             "write-fault %llu read-fault %llu stale %llu; other side used %s\n",
		             static_cast<unsigned long long>(vaddr),
		             static_cast<unsigned long long>(hit.size),
		             static_cast<unsigned long long>(hit.other_vaddr),
		             static_cast<unsigned long long>(hit.counts[0]),
		             static_cast<unsigned long long>(hit.counts[1]),
		             static_cast<unsigned long long>(hit.counts[2]),
		             static_cast<unsigned long long>(hit.counts[3]),
		             static_cast<unsigned long long>(hit.counts[4]),
		             other != log.hits.end() ? "yes" : "no");
	}
	std::fill(std::begin(log.uses), std::end(log.uses), 0);
	std::fill(std::begin(log.bytes), std::end(log.bytes), 0);
	std::fill(std::begin(log.all), std::end(log.all), 0);
	log.hits.clear();
	return changed;
}

RenderContext::RenderContext(GraphicContext& graphics, const std::filesystem::path& shader_store)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics, &m_image_heap, shader_store), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_resolve_cache(*this), m_texture_cache(graphics, m_command_scheduler, m_page_manager,
	                                          m_buffer_cache, m_resolve_cache),
      m_image_heap(*this, m_page_manager), m_occlusion_queries(graphics, m_command_scheduler) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	if (graphics.ray_query_enabled) {
		m_ray_tracing = std::make_unique<RayTracing>(*this);
	}
}

RenderContext::~RenderContext() {
	ShutdownGpu();
	m_command_scheduler.Shutdown();
}

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	m_video_out = video_out;
	m_gpu       = std::make_unique<GuestGpu>(*this);
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.Finish();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

GuestGpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	// The access retries once the image write-backs over its page land.
	if (WaitWriteBacks(fault_vaddr, fault_size)) {
		return true;
	}
	AliasLogNote(access == PageFaultAccess::Write ? AliasUse::WriteFault : AliasUse::ReadFault,
	             fault_vaddr, fault_size);
	if (access == PageFaultAccess::Write && AliasLogMode() >= 3) {
		std::vector<AliasPartner> partners;
		AliasPartners(fault_vaddr, fault_size, partners);
		for (const auto& partner: partners) {
			if (IsMapped(partner.partner, partner.size)) {
				m_buffer_cache.InvalidateMemory(partner.partner, partner.size);
				m_texture_cache.InvalidateMemory(partner.partner, partner.size);
				m_image_heap.InvalidateMemory(partner.partner, partner.size);
			}
		}
	}
	if (access == PageFaultAccess::Write) {
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_image_heap.InvalidateMemory(fault_vaddr, fault_size);
	} else {
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
	}
	return true;
}

bool RenderContext::WaitWriteBacks(uint64_t vaddr, uint64_t size) {
	const auto tick = m_texture_cache.WriteBackTick(vaddr, size);
	if (tick == 0) {
		return false;
	}
	// The GPU thread can fault inside a draw, so it submits without running deferred operations.
	if (GuestGpu::IsGpuThread()) {
		m_command_scheduler.Wait(tick);
	} else if (tick == m_command_scheduler.CurrentTick()) {
		GetGpu().SendCommandSync([this, tick] { m_command_scheduler.Wait(tick); });
	}
	m_command_scheduler.WaitPriorityOperations(tick);
	return true;
}

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	// Host I/O into the range, even one running past its mapping, would fail on its inaccessible
	// pages, and a later write-back would overwrite it.
	(void)WaitWriteBacks(vaddr, size);
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	m_image_heap.InvalidateMemory(vaddr, size);
	return true;
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mapped_ranges_mutex);
	m_mapped_ranges.Add(vaddr, size);
	m_buffer_cache.QueueBdaSync(vaddr, size);
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const bool trace     = UnmapTraceEnabled();
	const auto start     = trace ? NowNs() : 0;
	int64_t    run_start = 0;
	int64_t    run_end   = 0;
	int64_t    finish_ns = 0;
	bool       finished  = false;
	const auto unmap     = [&, this, vaddr, size] {
		if (trace) {
			run_start = NowNs();
		}
		// Check cache ownership on the GPU thread. Guest-memory callbacks can still
		// access a range with no cached data, so they must finish before it is unmapped.
		if (m_command_scheduler.Active() &&
		    (m_buffer_cache.IsRegionRegistered(vaddr, size) ||
		     m_texture_cache.IsRegionRegistered(vaddr, size) ||
		     m_command_scheduler.HasPendingPriorityOperations())) {
			const auto finish_start = trace ? NowNs() : 0;
			const auto tick         = m_command_scheduler.CurrentTick();
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
			if (trace) {
				finished  = true;
				finish_ns = NowNs() - finish_start;
			}
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		m_image_heap.UnmapMemory(vaddr, size);
		{
			std::lock_guard lock(m_mapped_ranges_mutex);
			m_mapped_ranges.Subtract(vaddr, size);
		}
		if (trace) {
			run_end = NowNs();
		}
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
	if (trace) {
		RecordUnmap(size, start, run_start, run_end, NowNs(), finished, finish_ns);
	}
}

void RenderContext::PrepareBda() {
	[[maybe_unused]] static const bool logged = [] {
		Log::WriteToConsoleAndLog("GPU: using buffer device address (BDA) shader memory access.\n");
		return true;
	}();
	std::shared_lock lock(m_mapped_ranges_mutex);
	m_buffer_cache.SynchronizeBda(m_mapped_ranges);
	m_fault_process_pending = true;
}

void RenderContext::PublishGpuWrites() {
	static const bool enabled = std::getenv("KYTY_SYNC_GPU_WRITES") != nullptr;
	if (!enabled || !m_fault_process_pending) {
		return;
	}
	m_fault_process_pending = false;
	m_buffer_cache.ProcessFaultBuffer();
	// The written pages are marked by a deferred operation that runs once the GPU has scanned
	// the fault bitmap.
	m_command_scheduler.Finish();
}

void RenderContext::PublishGpuWritesBeforeRead() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_SYNC_GPU_WRITES");
		return value != nullptr && std::atoi(value) >= 2;
	}();
	if (!enabled || !m_fault_process_pending) {
		return;
	}
	static uint64_t count = 0;
	if (++count % 10000u == 1u) {
		std::fprintf(stderr, "sync gpu writes: %llu publishes before reads\n",
		             static_cast<unsigned long long>(count));
	}
	m_fault_process_pending = false;
	m_buffer_cache.ProcessFaultBuffer();
	m_command_scheduler.Finish();
}

void RenderContext::RunGarbageCollector() {
	if (AliasLogTick() && AliasLogMode() >= 4) {
		m_buffer_cache.RefreshAliasPageTable();
	}
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_image_heap.RunGarbageCollector();
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
	if (m_ray_tracing) {
		m_ray_tracing->RunGarbageCollector();
	}
}

void RenderContext::AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it != m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.push_back({eq, event_id});
}

void RenderContext::DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it == m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.erase(it);
}

void RenderContext::TriggerInterrupt(int event_id, uint32_t context_id) {
	std::vector<InterruptEqRegistration> registrations;
	{
		Common::LockGuard lock(m_interrupt_mutex);
		for (const auto& registration: m_interrupt_eqs) {
			if (registration.event_id == event_id) {
				registrations.push_back(registration);
			}
		}
	}

	const auto ident = static_cast<uintptr_t>(event_id);
	auto*      data  = reinterpret_cast<void*>((static_cast<uintptr_t>(context_id) << 16u) | ident);
	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, ident, LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS, data);
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteInterruptEq(registration.eq, registration.event_id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
}

} // namespace Libs::Graphics
