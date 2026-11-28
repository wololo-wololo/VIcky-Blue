#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/presentation/videoOut.h"
#include "kernel/memory.h"
#include "libs/errno.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <string>
#include <unordered_map>
#include <xxhash.h>

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

// Debug aid (issue 22), KYTY_BUFFER_READ_LOG: guest CPU read faults on pages whose buffer data
// the GPU wrote (the first CPU read after each GPU write). One line per draw dump frame with the
// count and the top reading instructions with their page addresses.
struct BufferReadLog {
	struct Site {
		uint64_t count = 0;
		uint64_t page  = 0;
		uint64_t pages = 0;
	};
	bool                               enabled = std::getenv("KYTY_BUFFER_READ_LOG") != nullptr;
	std::mutex                         mutex;
	uint64_t                           frame = 0, gpu_reads = 0, other_reads = 0;
	std::unordered_map<uint64_t, Site> sites;

	void FlushLocked() {
		if (gpu_reads + other_reads == 0) {
			return;
		}
		std::vector<std::pair<uint64_t, Site>> top(sites.begin(), sites.end());
		std::sort(top.begin(), top.end(),
		          [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
		char   text[1200] = {};
		size_t used       = 0;
		for (size_t i = 0; i < top.size() && i < 12 && used < sizeof(text) - 80; i++) {
			const int n = std::snprintf(text + used, sizeof(text) - used, " 0x%llx:%llu@0x%llx",
			                            static_cast<unsigned long long>(top[i].first),
			                            static_cast<unsigned long long>(top[i].second.count),
			                            static_cast<unsigned long long>(top[i].second.page));
			if (n < 0) {
				break;
			}
			used += static_cast<size_t>(n);
		}
		std::fprintf(stderr, "buffer read log: frame %llu gpu-data reads %llu other %llu sites %zu%s\n",
		             static_cast<unsigned long long>(frame),
		             static_cast<unsigned long long>(gpu_reads),
		             static_cast<unsigned long long>(other_reads), sites.size(), text);
		gpu_reads = other_reads = 0;
		sites.clear();
	}

	// KYTY_BUFFER_READ_LOG=data: also one line per read of GPU data from the main program or libc
	// (instruction 0x900000000-0x920000000) with a summary of the 4 KiB page after its download:
	// hash, set bits and non-zero dwords.
	bool data = [] {
		const char* value = std::getenv("KYTY_BUFFER_READ_LOG");
		return value != nullptr && std::strcmp(value, "data") == 0;
	}();

	void Contents(uint64_t vaddr, uint64_t rip) const {
		if (!data || rip < 0x900000000ull || rip >= 0x920000000ull) {
			return;
		}
		const auto page = vaddr & ~0xfffull;
		uint64_t   words[512];
		if (!LibKernel::Memory::TryReadBacking(page, words, sizeof(words))) {
			return;
		}
		uint64_t bits = 0, nonzero = 0;
		for (const auto word: words) {
			bits += static_cast<uint64_t>(std::popcount(word));
			nonzero += static_cast<uint64_t>((word & 0xffffffffull) != 0) +
			           static_cast<uint64_t>((word >> 32u) != 0);
		}
		std::fprintf(stderr,
		             "buffer read data: frame %llu at 0x%llx page 0x%llx +%llx hash %016llx bits %llu "
		             "nonzero-dw %llu\n",
		             static_cast<unsigned long long>(DrawDumpFrameIndex()),
		             static_cast<unsigned long long>(rip), static_cast<unsigned long long>(page),
		             static_cast<unsigned long long>(vaddr - page),
		             static_cast<unsigned long long>(XXH3_64bits(words, sizeof(words))),
		             static_cast<unsigned long long>(bits), static_cast<unsigned long long>(nonzero));
	}

	void Note(uint64_t vaddr, uint64_t rip, bool gpu_data) {
		const auto current = DrawDumpFrameIndex();
		std::scoped_lock lock {mutex};
		if (current != frame) {
			FlushLocked();
			frame = current;
		}
		if (!gpu_data) {
			other_reads++;
			return;
		}
		gpu_reads++;
		auto& site = sites[rip];
		if (site.count++ == 0) {
			site.page = vaddr & ~0xfffull;
		}
	}
} g_buffer_read_log;

// Debug aid (issue 22), KYTY_WRITE_WATCH=<begin>-<end>[,...]: at every guest flip, each page of
// the listed ranges is write-protected once. The first guest write to it faults, is recorded
// with the writing instruction and the first code addresses found on the writer's stack (a
// stand-in for its callers), and unprotects the page. KYTY_WRITE_WATCH_CODE=<begin>-<end> sets
// the code range those stack values must fall in (default: the main program's usual range).
// KYTY_WRITE_WATCH_DEPTH=<n> walks the stack up to its readable end and keeps up to n values that
// are return addresses (the bytes before them encode a call), instead of the first 4 code values
// in 64 qwords. KYTY_WRITE_WATCH_FP=1 instead follows the frame-pointer chain from the writer's
// rbp ([rbp] is the caller's rbp, [rbp + 8] the return address), which lists only live frames; the
// value at [rsp] is added first when it is a return address, for a writer without its own frame.
// KYTY_WRITE_WATCH_PTR=<slot>[,<slot>...] watches data reached through pointers: at every flip the
// qword at each slot is read as an address, the pages of the bit array there are armed too, and a
// line gives the frame, the address, the set bits and a hash of the array. The array holds
// KYTY_WRITE_WATCH_PTR_BITS=<n> bits, or as many as the dword at KYTY_WRITE_WATCH_PTR_COUNT=<addr>
// says. A first write inside an array is reported as range 100 + the slot's index; one elsewhere on
// its pages as range 90 + the index. KYTY_WRITE_WATCH_PTR_READ=1 protects those pages against
// reads too: each access then faults once, is counted per instruction with the array's set bits at
// that moment, and the page is unprotected until the next write fault on a watched fixed range
// (the draw jobs write command buffers between their reads) or the next flip. One line per frame,
// slot and instruction gives the count and the bit range seen.
// A slot can be written <slot>[@<offset>...][+<add>][:<bits>]: each @offset reads the next pointer
// at that offset from the previous one, +add is added to the final address, and :bits gives that
// slot's own array size. Each first write inside a slot's array also prints a line with the frame,
// the slot, the written address and the writer's chain.
struct WriteWatch {
	struct Range {
		uint64_t begin = 0;
		uint64_t end   = 0;
	};
	struct Chain {
		uint64_t count = 0;
		uint64_t page  = 0;
		int      range = 0;
	};
	std::vector<Range>                           ranges = Parse(std::getenv("KYTY_WRITE_WATCH"));
	Range                                        code   = [] {
        const auto list = Parse(std::getenv("KYTY_WRITE_WATCH_CODE"));
        return list.empty() ? Range {0x900000000ull, 0x910000000ull} : list.front();
	}();
	std::mutex                                   mutex;
	std::vector<uint64_t>                        armed_pages;
	std::unordered_map<uint64_t, bool>           armed;
	std::map<std::vector<uint64_t>, Chain>       chains;
	uint64_t                                     faults = 0, flips = 0;
	int64_t                                      last_report_ns = 0;
	size_t                                       depth = [] {
        const char* value = std::getenv("KYTY_WRITE_WATCH_DEPTH");
        return value == nullptr ? size_t {0} : static_cast<size_t>(std::strtoull(value, nullptr, 0));
	}();

	struct Access {
		uint64_t count = 0;
		uint64_t min   = ~0ull;
		uint64_t max   = 0;
	};
	struct Pointer {
		uint64_t slot       = 0;
		std::vector<uint64_t> derefs;
		uint64_t add        = 0;
		uint64_t own_bits   = 0;
		uint64_t target     = 0;
		uint64_t bytes      = 0;
		uint64_t bits       = 0;
		uint64_t writes     = 0;
		uint64_t read_page  = 0;
		bool     read_armed = false;
		// Key: instruction, with bit 63 set for a write access.
		std::map<uint64_t, Access> accesses;
	};
	std::vector<Pointer> pointers = [] {
		std::vector<Pointer> list;
		const char*          text = std::getenv("KYTY_WRITE_WATCH_PTR");
		while (text != nullptr && *text != '\0') {
			char*      end  = nullptr;
			const auto slot = std::strtoull(text, &end, 0);
			if (end == text) {
				break;
			}
			Pointer pointer;
			pointer.slot = slot;
			while (*end == '@') {
				text = end + 1;
				pointer.derefs.push_back(std::strtoull(text, &end, 0));
			}
			if (*end == '+') {
				text        = end + 1;
				pointer.add = std::strtoull(text, &end, 0);
			}
			if (*end == ':') {
				text             = end + 1;
				pointer.own_bits = std::strtoull(text, &end, 0);
			}
			list.push_back(std::move(pointer));
			text = (*end == ',') ? end + 1 : end;
		}
		return list;
	}();
	uint64_t pointer_bits = [] {
		const char* value = std::getenv("KYTY_WRITE_WATCH_PTR_BITS");
		return value == nullptr ? uint64_t {0} : std::strtoull(value, nullptr, 0);
	}();
	uint64_t pointer_count = [] {
		const char* value = std::getenv("KYTY_WRITE_WATCH_PTR_COUNT");
		return value == nullptr ? uint64_t {0} : std::strtoull(value, nullptr, 0);
	}();

	bool pointer_read = [] {
		const char* value = std::getenv("KYTY_WRITE_WATCH_PTR_READ");
		return value != nullptr && value[0] == '1';
	}();

	[[nodiscard]] uint64_t SetBits(const Pointer& pointer) const {
		if (pointer.bytes == 0 ||
		    LibKernel::Memory::ReadableHostBytes(pointer.target, pointer.bytes) != pointer.bytes) {
			return ~0ull;
		}
		uint64_t set = 0;
		for (uint64_t i = 0; i < pointer.bytes; i++) {
			auto byte = reinterpret_cast<const uint8_t*>(pointer.target)[i];
			if (i + 1 == pointer.bytes && (pointer.bits & 7u) != 0) {
				byte &= static_cast<uint8_t>((1u << (pointer.bits & 7u)) - 1u);
			}
			set += static_cast<uint64_t>(std::popcount(byte));
		}
		return set;
	}

	void RearmReads(PageManager& pages, uint64_t except_page) {
		for (auto& pointer: pointers) {
			if (pointer.read_page != 0 && !pointer.read_armed && pointer.read_page != except_page) {
				pointer.read_armed = true;
				pages.UpdatePageWatchers<true, true>(pointer.read_page, 0x1000);
			}
		}
	}

	// Called for every fault before Note(). Returns true when the fault was on a read-armed array
	// page (it is then unprotected).
	bool NoteAccess(PageManager& pages, bool write, uint64_t vaddr, uint64_t rip) {
		if (!pointer_read) {
			return false;
		}
		const auto       page = vaddr & ~0xfffull;
		std::scoped_lock lock {mutex};
		bool             hit = false;
		for (auto& pointer: pointers) {
			if (!pointer.read_armed || pointer.read_page != page) {
				continue;
			}
			pointer.read_armed = false;
			pages.UpdatePageWatchers<false, true>(page, 0x1000);
			hit = true;
		}
		if (!hit) {
			return false;
		}
		for (auto& pointer: pointers) {
			if (pointer.read_page != page || vaddr < pointer.target ||
			    vaddr >= pointer.target + pointer.bytes) {
				continue;
			}
			const auto set    = SetBits(pointer);
			auto&      access = pointer.accesses[rip | (write ? (1ull << 63u) : 0)];
			access.count++;
			access.min = std::min(access.min, set);
			access.max = std::max(access.max, set);
		}
		return true;
	}

	// KYTY_WRITE_WATCH_PAGES=1: one line per first write with the frame, the page, the written
	// address and its chain.
	bool log_pages = [] {
		const char* value = std::getenv("KYTY_WRITE_WATCH_PAGES");
		return value != nullptr && value[0] == '1';
	}();

	bool frame_pointers = [] {
		const char* value = std::getenv("KYTY_WRITE_WATCH_FP");
		return value != nullptr && value[0] == '1';
	}();

	// True when the code bytes before ret encode a call: E8 rel32, or FF /2 (with an optional REX
	// prefix) through a register or memory operand of 0, 1, 4 or 5 more bytes.
	static bool IsReturnAddress(uint64_t ret) {
		uint8_t bytes[8];
		if (LibKernel::Memory::ReadableHostBytes(ret - 8, 8) < 8) {
			return false;
		}
		std::memcpy(bytes, reinterpret_cast<const void*>(ret - 8), 8);
		if (bytes[3] == 0xe8) {
			return true;
		}
		for (const int n: {2, 3, 6, 7}) {
			const auto* insn = bytes + 8 - n;
			if (insn[0] == 0xff && ((insn[1] >> 3u) & 7u) == 2) {
				return true;
			}
			if (n > 2 && (insn[0] & 0xf0u) == 0x40 && insn[1] == 0xff && ((insn[2] >> 3u) & 7u) == 2) {
				return true;
			}
		}
		return false;
	}

	static std::vector<Range> Parse(const char* text) {
		std::vector<Range> list;
		while (text != nullptr && *text != '\0') {
			char*      end   = nullptr;
			const auto begin = std::strtoull(text, &end, 0);
			if (end == text || *end != '-') {
				break;
			}
			text           = end + 1;
			const auto top = std::strtoull(text, &end, 0);
			if (end == text) {
				break;
			}
			list.push_back({begin & ~0xfffull, (top + 0xfffull) & ~0xfffull});
			text = (*end == ',') ? end + 1 : end;
		}
		return list;
	}

	[[nodiscard]] bool Enabled() const { return !ranges.empty() || !pointers.empty(); }

	template <typename IsMapped>
	void Flip(PageManager& pages, IsMapped&& is_mapped) {
		std::scoped_lock lock {mutex};
		flips++;
		const auto arm = [&](uint64_t page) {
			auto [it, inserted] = armed.try_emplace(page, true);
			if (!inserted && it->second) {
				return;
			}
			if (!is_mapped(page)) {
				armed.erase(it);
				return;
			}
			it->second = true;
			pages.UpdatePageWatchers<true, false>(page, 0x1000);
		};
		for (const auto& range: ranges) {
			for (auto page = range.begin; page < range.end; page += 0x1000) {
				arm(page);
			}
		}
		unarm_pages = &pages;
		FlipPointers(arm, is_mapped);
		if (pointer_read) {
			RearmReads(pages, 0);
		}
		Report();
	}

	PageManager* unarm_pages = nullptr;
	void         Unarm(uint64_t page) const {
        if (unarm_pages != nullptr && page != 0) {
            unarm_pages->UpdatePageWatchers<false, true>(page, 0x1000);
        }
	}

	template <typename Arm, typename IsMapped>
	void FlipPointers(Arm&& arm, IsMapped&& is_mapped) {
		if (pointers.empty()) {
			return;
		}
		uint64_t bits = pointer_bits;
		if (pointer_count != 0 && LibKernel::Memory::ReadableHostBytes(pointer_count, 4) == 4) {
			bits = *reinterpret_cast<const uint32_t*>(pointer_count);
		}
		bits = std::min<uint64_t>(bits, 0x10000);
		for (size_t i = 0; i < pointers.size(); i++) {
			auto& pointer = pointers[i];
			if (LibKernel::Memory::ReadableHostBytes(pointer.slot, 8) != 8) {
				continue;
			}
			for (const auto& [key, access]: pointer.accesses) {
				std::fprintf(stderr,
				             "write watch access: frame %llu slot %zu %s 0x%llx x%llu bits %llu-%llu\n",
				             static_cast<unsigned long long>(DrawDumpFrameIndex()), i,
				             (key >> 63u) != 0 ? "write" : "read",
				             static_cast<unsigned long long>(key & ~(1ull << 63u)),
				             static_cast<unsigned long long>(access.count),
				             static_cast<unsigned long long>(access.min),
				             static_cast<unsigned long long>(access.max));
			}
			pointer.accesses.clear();
			auto target = *reinterpret_cast<const uint64_t*>(pointer.slot);
			for (const auto offset: pointer.derefs) {
				target = (target != 0 && LibKernel::Memory::ReadableHostBytes(target + offset, 8) == 8)
				             ? *reinterpret_cast<const uint64_t*>(target + offset)
				             : 0;
			}
			if (target != 0) {
				target += pointer.add;
			}
			const auto slot_bits = std::min<uint64_t>(pointer.own_bits != 0 ? pointer.own_bits : bits, 0x10000);
			if (pointer_read) {
				// Unprotected here so the array can be read below; RearmReads() protects it again.
				if (pointer.read_armed) {
					pointer.read_armed = false;
					Unarm(pointer.read_page);
				}
				const auto page   = target & ~0xfffull;
				pointer.read_page = (target != 0 && is_mapped(page)) ? page : 0;
			}
			pointer.target = target;
			pointer.bytes  = (slot_bits + 7) / 8;
			pointer.bits   = slot_bits;
			if (pointer.target == 0 || pointer.bytes == 0) {
				continue;
			}
			if (LibKernel::Memory::ReadableHostBytes(pointer.target, pointer.bytes) != pointer.bytes) {
				// Read-protected, for example while the buffer cache holds GPU-written data there.
				std::fprintf(stderr, "write watch ptr: frame %llu slot %zu at 0x%llx unreadable\n",
				             static_cast<unsigned long long>(DrawDumpFrameIndex()), i,
				             static_cast<unsigned long long>(pointer.target));
				continue;
			}
			std::vector<uint8_t> data(pointer.bytes);
			std::memcpy(data.data(), reinterpret_cast<const void*>(pointer.target), pointer.bytes);
			if ((slot_bits & 7u) != 0) {
				data.back() &= static_cast<uint8_t>((1u << (slot_bits & 7u)) - 1u);
			}
			uint64_t set = 0;
			for (const auto byte: data) {
				set += static_cast<uint64_t>(std::popcount(byte));
			}
			std::fprintf(stderr,
			             "write watch ptr: frame %llu slot %zu at 0x%llx bits %llu/%llu hash %016llx cpu-writes "
			             "%llu\n",
			             static_cast<unsigned long long>(DrawDumpFrameIndex()), i,
			             static_cast<unsigned long long>(pointer.target), static_cast<unsigned long long>(set),
			             static_cast<unsigned long long>(slot_bits),
			             static_cast<unsigned long long>(XXH3_64bits(data.data(), data.size())),
			             static_cast<unsigned long long>(pointer.writes));
			pointer.writes = 0;
			if (pointer_read) {
				continue;
			}
			for (auto page = pointer.target & ~0xfffull; page < pointer.target + pointer.bytes; page += 0x1000) {
				arm(page);
			}
		}
	}

	bool Note(PageManager& pages, uint64_t vaddr, uint64_t rip, uint64_t rsp, uint64_t rbp) {
		const auto page = vaddr & ~0xfffull;
		std::scoped_lock lock {mutex};
		const auto it = armed.find(page);
		if (it == armed.end() || !it->second) {
			return false;
		}
		it->second = false;
		pages.UpdatePageWatchers<false, false>(page, 0x1000);
		faults++;
		if (pointer_read) {
			RearmReads(pages, page);
		}
		std::vector<uint64_t> key {rip};
		if (frame_pointers && rsp != 0) {
			const auto limit = depth > 0 ? depth : size_t {40};
			if (LibKernel::Memory::ReadableHostBytes(rsp, 8) == 8) {
				const auto top = *reinterpret_cast<const uint64_t*>(rsp);
				if (top >= code.begin && top < code.end && IsReturnAddress(top)) {
					key.push_back(top);
				}
			}
			auto frame = rbp;
			if (rip < code.begin || rip >= code.end) {
				// Emulator or system code (an HLE library call) keeps no frame-pointer chain. Take the
				// first return address into guest code on the stack, then the guest caller's saved
				// rbp between it and rsp: a value just above that slot whose frame returns into guest
				// code too. The exact walk continues from there.
				frame          = 0;
				const auto  n  = LibKernel::Memory::ReadableHostBytes(rsp, 0x10000) / 8;
				const auto* st = reinterpret_cast<const uint64_t*>(rsp);
				for (uint64_t i = 0; i < n; i++) {
					if (st[i] < code.begin || st[i] >= code.end || !IsReturnAddress(st[i])) {
						continue;
					}
					key.push_back(st[i]);
					const auto slot = rsp + i * 8;
					for (uint64_t j = 0; j < i; j++) {
						const auto v = st[j];
						if (v > slot && v - slot < 0x10000 && (v & 7u) == 0 &&
						    LibKernel::Memory::ReadableHostBytes(v, 16) == 16) {
							const auto ret = reinterpret_cast<const uint64_t*>(v)[1];
							if (ret >= code.begin && ret < code.end && IsReturnAddress(ret)) {
								frame = v;
								break;
							}
						}
					}
					break;
				}
			}
			while (frame != 0 && key.size() <= limit && frame >= rsp && (frame & 7u) == 0 &&
			       LibKernel::Memory::ReadableHostBytes(frame, 16) == 16) {
				const auto* slots = reinterpret_cast<const uint64_t*>(frame);
				const auto  ret   = slots[1];
				if (ret >= code.begin && ret < code.end) {
					key.push_back(ret);
				}
				if (slots[0] <= frame || slots[0] - frame > 0x100000) {
					break;
				}
				frame = slots[0];
			}
		} else if (rsp != 0 && depth > 0) {
			const auto  bytes = LibKernel::Memory::ReadableHostBytes(rsp, 0x40000);
			const auto* stack = reinterpret_cast<const uint64_t*>(rsp);
			for (uint64_t i = 0; i < bytes / 8 && key.size() <= depth; i++) {
				const auto value = stack[i];
				if (value >= code.begin && value < code.end && IsReturnAddress(value)) {
					key.push_back(value);
				}
			}
		} else if (rsp != 0) {
			const auto* stack = reinterpret_cast<const uint64_t*>(rsp);
			for (int i = 0; i < 64 && key.size() < 5; i++) {
				const auto value = stack[i];
				if (value >= code.begin && value < code.end) {
					key.push_back(value);
				}
			}
		}
		std::string chain_text;
		for (const auto value: key) {
			char number[24];
			std::snprintf(number, sizeof(number), " 0x%llx", static_cast<unsigned long long>(value));
			chain_text += number;
		}
		int range_index = 0;
		for (size_t i = 0; i < ranges.size(); i++) {
			if (page >= ranges[i].begin && page < ranges[i].end) {
				range_index = static_cast<int>(i);
			}
		}
		for (size_t i = 0; i < pointers.size(); i++) {
			auto& pointer = pointers[i];
			if (pointer.bytes == 0 || page < (pointer.target & ~0xfffull) ||
			    page >= pointer.target + pointer.bytes) {
				continue;
			}
			const bool inside = vaddr >= pointer.target && vaddr < pointer.target + pointer.bytes;
			range_index       = static_cast<int>((inside ? 100 : 90) + i);
			pointer.writes += inside ? 1 : 0;
			if (inside) {
				std::fprintf(stderr, "write watch ptr write: frame %llu slot %zu addr 0x%llx (+0x%llx) at%s\n",
				             static_cast<unsigned long long>(DrawDumpFrameIndex()), i,
				             static_cast<unsigned long long>(vaddr),
				             static_cast<unsigned long long>(vaddr - pointer.target), chain_text.c_str());
			}
		}
		auto& chain = chains[key];
		if (chain.count++ == 0) {
			chain.page  = page;
			chain.range = range_index;
		}
		if (log_pages) {
			char   text[1024] = {};
			size_t used       = 0;
			for (const auto value: key) {
				const int n = std::snprintf(text + used, sizeof(text) - used, " 0x%llx",
				                            static_cast<unsigned long long>(value));
				if (n < 0 || static_cast<size_t>(n) >= sizeof(text) - used) {
					break;
				}
				used += static_cast<size_t>(n);
			}
			std::fprintf(stderr, "write watch page: frame %llu page 0x%llx addr 0x%llx at%s\n",
			             static_cast<unsigned long long>(DrawDumpFrameIndex()),
			             static_cast<unsigned long long>(page), static_cast<unsigned long long>(vaddr), text);
		}
		return true;
	}

	void Report() {
		const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
		                     std::chrono::steady_clock::now().time_since_epoch())
		                     .count();
		if (last_report_ns == 0) {
			last_report_ns = now;
			return;
		}
		if (now - last_report_ns < 2000000000LL) {
			return;
		}
		last_report_ns = now;
		std::fprintf(stderr, "write watch: %llu flips, %llu first writes, %zu writer chains\n",
		             static_cast<unsigned long long>(flips),
		             static_cast<unsigned long long>(faults), chains.size());
		std::vector<std::pair<std::vector<uint64_t>, Chain>> top(chains.begin(), chains.end());
		std::sort(top.begin(), top.end(),
		          [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
		const size_t limit = depth > 0 || frame_pointers ? 200 : 16;
		for (size_t i = 0; i < top.size() && i < limit; i++) {
			char   text[1024] = {};
			size_t used      = 0;
			for (const auto value: top[i].first) {
				const int n = std::snprintf(text + used, sizeof(text) - used, " 0x%llx",
				                            static_cast<unsigned long long>(value));
				if (n < 0 || static_cast<size_t>(n) >= sizeof(text) - used) {
					break;
				}
				used += static_cast<size_t>(n);
			}
			std::fprintf(stderr, "write watch:   range %d x%llu first page 0x%llx: at%s\n",
			             top[i].second.range, static_cast<unsigned long long>(top[i].second.count),
			             static_cast<unsigned long long>(top[i].second.page), text);
		}
		chains.clear();
		faults = flips = 0;
	}
} g_write_watch;

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

namespace {

// KYTY_FLIP_PEEK=<addr>[@<offset>][,...] prints one line per flip with the frame and one guest
// dword per entry: the dword at addr, or with @offset the dword at offset from the address stored
// in the qword at addr. -1 means unreadable. Used to log the length of guest lists per frame.
struct FlipPeek {
	struct Entry {
		uint64_t address  = 0;
		uint64_t offset   = 0;
		bool     indirect = false;
	};
	std::vector<Entry> entries = [] {
		std::vector<Entry> list;
		const char*        text = std::getenv("KYTY_FLIP_PEEK");
		while (text != nullptr && *text != '\0') {
			char* end   = nullptr;
			Entry entry = {std::strtoull(text, &end, 0)};
			if (end == text) {
				break;
			}
			if (*end == '@') {
				text           = end + 1;
				entry.offset   = std::strtoull(text, &end, 0);
				entry.indirect = true;
			}
			list.push_back(entry);
			text = (*end == ',') ? end + 1 : end;
		}
		return list;
	}();

	void Print() const {
		if (entries.empty()) {
			return;
		}
		char   line[512];
		size_t used = static_cast<size_t>(std::snprintf(line, sizeof(line), "flip peek: frame %llu",
		                                                static_cast<unsigned long long>(DrawDumpFrameIndex())));
		for (const auto& entry: entries) {
			int64_t  value   = -1;
			uint64_t address = entry.address;
			if (entry.indirect) {
				address = (LibKernel::Memory::ReadableHostBytes(address, 8) == 8)
				              ? *reinterpret_cast<const uint64_t*>(address) + entry.offset
				              : 0;
			}
			if (address != 0 && LibKernel::Memory::ReadableHostBytes(address, 4) == 4) {
				value = *reinterpret_cast<const uint32_t*>(address);
			}
			if (used < sizeof(line)) {
				used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, " %lld",
				                                          static_cast<long long>(value)));
			}
		}
		std::fprintf(stderr, "%s\n", line);
	}
};

const FlipPeek g_flip_peek;

} // namespace

void RenderContext::WriteWatchFlip() {
	g_flip_peek.Print();
	if (g_write_watch.Enabled()) {
		g_write_watch.Flip(m_page_manager,
		                   [this](uint64_t page) { return IsMapped(page, 0x1000); });
	}
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr, uint64_t fault_rip,
                                uint64_t fault_rsp, uint64_t fault_rbp) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	if (g_write_watch.Enabled() &&
	    g_write_watch.NoteAccess(m_page_manager, access == PageFaultAccess::Write, fault_vaddr, fault_rip)) {
		return true;
	}
	if (access == PageFaultAccess::Write && g_write_watch.Enabled()) {
		(void)g_write_watch.Note(m_page_manager, fault_vaddr, fault_rip, fault_rsp, fault_rbp);
	}
	// The access retries once the image write-backs over its page land.
	if (WaitWriteBacks(fault_vaddr, fault_size)) {
		return true;
	}
	m_texture_cache.NoteReadWatchFault(access == PageFaultAccess::Write, fault_vaddr, fault_rip);
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
		const bool gpu_data = g_buffer_read_log.enabled &&
		                      m_buffer_cache.IsRegionGpuModified(fault_vaddr, fault_size);
		if (g_buffer_read_log.enabled) {
			g_buffer_read_log.Note(fault_vaddr, fault_rip, gpu_data);
		}
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
		if (gpu_data) {
			g_buffer_read_log.Contents(fault_vaddr, fault_rip);
		}
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
