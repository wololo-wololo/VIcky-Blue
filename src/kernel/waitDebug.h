#ifndef EMULATOR_SRC_KERNEL_WAITDEBUG_H_
#define EMULATOR_SRC_KERNEL_WAITDEBUG_H_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

// Debug aid and experiment (issue 22). KYTY_WAIT_LOG: counts guest waits with a nonzero timeout
// and those that end by timeout, per primitive and by requested timeout, reported every two
// seconds; the timeouts per guest frame also go to the KYTY_DRAW_DUMP frame headers.
// KYTY_TIMEOUT_SCALE=<n>: multiplies every nonzero guest wait timeout by n.
namespace Libs::LibKernel::WaitDebug {

enum Kind { EventFlag, Equeue, CondRel, CondAbs, Mutex, Rwlock, Sema, PosixSema, Address, KindCount };

inline const char* KindName(int kind) {
	static const char* const names[KindCount] = {"eventflag", "equeue", "cond",  "cond-abs", "mutex",
	                                             "rwlock",    "sema",   "psema", "address"};
	return names[kind];
}

// KYTY_GUEST_TIME_SCALE=<n>: guest time runs n times slower than host time. The guest TSC
// (patched RDTSC with --guest-tsc, sceKernelReadTsc), process time and the monotonic clocks are
// divided by n; sleeps and, unless KYTY_TIMEOUT_SCALE says otherwise, wait timeouts are
// multiplied by n.
inline uint64_t GuestTimeScale() {
	static const uint64_t scale = [] {
		const char* value = std::getenv("KYTY_GUEST_TIME_SCALE");
		const long long n = value != nullptr ? std::atoll(value) : 1;
		return static_cast<uint64_t>(std::max(1LL, n));
	}();
	return scale;
}

inline uint64_t Scale() {
	static const uint64_t scale = [] {
		const char* value = std::getenv("KYTY_TIMEOUT_SCALE");
		const long long n = value != nullptr ? std::atoll(value) : static_cast<long long>(GuestTimeScale());
		return static_cast<uint64_t>(std::max(1LL, n));
	}();
	return scale;
}

inline bool LogEnabled() {
	static const bool enabled = std::getenv("KYTY_WAIT_LOG") != nullptr;
	return enabled;
}

// A requested timeout in microseconds, scaled; 0 (a poll) stays 0.
template <typename T>
inline T ScaleUs(T micros) {
	const uint64_t scaled = static_cast<uint64_t>(micros) * Scale();
	return static_cast<T>(std::min<uint64_t>(scaled, std::numeric_limits<T>::max()));
}

inline std::chrono::steady_clock::time_point ScaleDeadline(std::chrono::steady_clock::time_point deadline) {
	if (Scale() == 1) {
		return deadline;
	}
	const auto now = std::chrono::steady_clock::now();
	if (deadline <= now || deadline == std::chrono::steady_clock::time_point::max()) {
		return deadline;
	}
	const auto remaining = deadline - now;
	if (remaining > std::chrono::hours(24)) {
		return deadline;
	}
	return now + remaining * static_cast<int64_t>(Scale());
}

constexpr int BucketCount = 6; // <100 us, <1 ms, <10 ms, <100 ms, <1 s, longer

struct Stats {
	std::atomic<uint64_t> waits[KindCount] {};
	std::atomic<uint64_t> timeouts[KindCount] {};
	std::atomic<uint64_t> buckets[KindCount][BucketCount] {};
	std::atomic<int64_t>  last_report_ns {0};
};

inline Stats& GetStats() {
	static Stats stats;
	return stats;
}

// Timed-out waits since the last guest flip (read and reset by the draw dump).
inline std::atomic<uint64_t>& FrameTimeouts() {
	static std::atomic<uint64_t> count {0};
	return count;
}

inline void Report(bool force) {
	auto&      stats = GetStats();
	const auto now   = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
	auto last = stats.last_report_ns.load();
	if (last == 0) {
		stats.last_report_ns.compare_exchange_strong(last, now);
		return;
	}
	if (!force && now - last < 2000000000LL) {
		return;
	}
	if (!stats.last_report_ns.compare_exchange_strong(last, now)) {
		return;
	}
	char     line[1024];
	int      at    = std::snprintf(line, sizeof(line), "wait timeouts in %.1f s:",
	                               static_cast<double>(now - last) / 1e9);
	for (int kind = 0; kind < KindCount && at < static_cast<int>(sizeof(line)) - 96; kind++) {
		const auto waits    = stats.waits[kind].exchange(0);
		const auto timeouts = stats.timeouts[kind].exchange(0);
		uint64_t   b[BucketCount];
		for (int i = 0; i < BucketCount; i++) {
			b[i] = stats.buckets[kind][i].exchange(0);
		}
		if (waits == 0) {
			continue;
		}
		at += std::snprintf(line + at, sizeof(line) - at,
		                    " %s %llu/%llu [%llu %llu %llu %llu %llu %llu]", KindName(kind),
		                    static_cast<unsigned long long>(timeouts),
		                    static_cast<unsigned long long>(waits),
		                    static_cast<unsigned long long>(b[0]), static_cast<unsigned long long>(b[1]),
		                    static_cast<unsigned long long>(b[2]), static_cast<unsigned long long>(b[3]),
		                    static_cast<unsigned long long>(b[4]), static_cast<unsigned long long>(b[5]));
	}
	std::fprintf(stderr, "%s (timed out/timed waits [by requested timeout: <100us <1ms <10ms <100ms <1s more])\n",
	             line);
}

// A guest wait with a nonzero requested timeout (unscaled, in microseconds) has ended.
inline void NoteWait(Kind kind, uint64_t requested_us, bool timed_out) {
	if (!LogEnabled() || requested_us == 0) {
		return;
	}
	auto& stats = GetStats();
	stats.waits[kind].fetch_add(1, std::memory_order_relaxed);
	if (timed_out) {
		stats.timeouts[kind].fetch_add(1, std::memory_order_relaxed);
		FrameTimeouts().fetch_add(1, std::memory_order_relaxed);
		int bucket = 0;
		for (uint64_t limit = 100; bucket < BucketCount - 1 && requested_us >= limit; limit *= 10) {
			bucket++;
		}
		stats.buckets[kind][bucket].fetch_add(1, std::memory_order_relaxed);
	}
	Report(false);
}

} // namespace Libs::LibKernel::WaitDebug

#endif // EMULATOR_SRC_KERNEL_WAITDEBUG_H_
