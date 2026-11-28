#ifndef KYTY_COMMON_ASYNCJOBS_H_
#define KYTY_COMMON_ASYNCJOBS_H_

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Common {

// Runs jobs on worker threads; the owner polls for results. Destruction waits for running jobs
// and drops queued ones.
template <class Result>
class AsyncJobs {
public:
	explicit AsyncJobs(uint32_t threads) {
		for (uint32_t i = 0; i < std::max(threads, 1u); ++i) {
			m_workers.emplace_back([this](std::stop_token stop) { Work(stop); });
		}
	}

	~AsyncJobs() {
		{
			std::lock_guard lock(m_mutex);
			m_queue.clear();
		}
		for (auto& worker: m_workers) worker.request_stop();
		m_ready.notify_all();
		m_workers.clear();
	}

	AsyncJobs(const AsyncJobs&)            = delete;
	AsyncJobs& operator=(const AsyncJobs&) = delete;

	uint64_t Submit(std::function<Result()> job) {
		std::lock_guard lock(m_mutex);
		const auto id = m_next++;
		m_queue.emplace_back(id, std::move(job));
		m_pending.insert(id);
		m_ready.notify_one();
		return id;
	}

	// Results of the jobs finished since the last call, in completion order.
	std::vector<std::pair<uint64_t, Result>> TakeFinished() {
		std::lock_guard lock(m_mutex);
		for (const auto& result: m_finished) m_pending.erase(result.first);
		return std::exchange(m_finished, {});
	}

	// True until the job's result has been taken.
	[[nodiscard]] bool Pending(uint64_t id) const {
		std::lock_guard lock(m_mutex);
		return m_pending.contains(id);
	}

private:
	void Work(std::stop_token stop) {
		std::unique_lock lock(m_mutex);
		while (m_ready.wait(lock, stop, [this] { return !m_queue.empty(); })) {
			auto job = std::move(m_queue.front());
			m_queue.pop_front();
			lock.unlock();
			auto result = job.second();
			lock.lock();
			m_finished.emplace_back(job.first, std::move(result));
		}
	}

	mutable std::mutex                                        m_mutex;
	std::condition_variable_any                               m_ready;
	std::deque<std::pair<uint64_t, std::function<Result()>>>  m_queue;
	std::vector<std::pair<uint64_t, Result>>                  m_finished;
	std::unordered_set<uint64_t>                              m_pending;
	uint64_t                                                  m_next = 1;
	std::vector<std::jthread>                                 m_workers; // Last: joined first.
};

} // namespace Common

#endif // KYTY_COMMON_ASYNCJOBS_H_
