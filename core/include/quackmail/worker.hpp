#pragma once

#include "duckdb.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace quackmail {

// A background thread that runs one function on a fixed interval.
//
// The counterpart to ServerController: that owns a socket and a thread per
// connection, this owns a thread and a clock. Everything periodic in QuackCit
// is the same shape — the outbound relay drainer, mailing-list distribution,
// remote message pulls — so the loop lives here once.
//
// The tick gets a fresh duckdb::Connection each pass and may throw; a throw is
// swallowed and the next tick runs on schedule, because a worker that dies on
// one bad row stops draining every other row too. As everywhere else in the
// tree, workers coordinate through tables, never through shared C++ state.
class PeriodicWorker {
public:
	using Tick = std::function<void(duckdb::Connection &)>;

	PeriodicWorker() = default;
	~PeriodicWorker();
	PeriodicWorker(const PeriodicWorker &) = delete;
	PeriodicWorker &operator=(const PeriodicWorker &) = delete;

	// `poll_secs` <= 0 falls back to 60. Fails if already running.
	bool Start(duckdb::DatabaseInstance &db, int poll_secs, Tick tick, std::string &err);
	bool Stop(std::string &err);

	bool IsRunning() const {
		return running_;
	}
	int PollSecs() const {
		return poll_secs_;
	}
	// Ticks completed since Start, for the _status table functions.
	uint64_t Passes() const {
		return passes_;
	}

private:
	void Loop();

	std::mutex mutex_;
	std::thread thread_;
	Tick tick_;
	std::atomic<bool> running_ {false};
	std::atomic<bool> stop_ {false};
	std::atomic<uint64_t> passes_ {0};
	std::atomic<int> poll_secs_ {60};
	duckdb::DatabaseInstance *db_ = nullptr;
};

} // namespace quackmail
