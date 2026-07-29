#include "quackmail/worker.hpp"

#include "quackmail/mail_store.hpp"

#include <chrono>

namespace quackmail {

using duckdb::Connection;
using duckdb::DatabaseInstance;

PeriodicWorker::~PeriodicWorker() {
	std::string err;
	Stop(err);
}

bool PeriodicWorker::Start(DatabaseInstance &db, int poll_secs, Tick tick, std::string &err) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (running_) {
		err = "already running";
		return false;
	}
	if (!tick) {
		err = "no tick function";
		return false;
	}
	db_ = &db;
	poll_secs_ = poll_secs > 0 ? poll_secs : 60;
	tick_ = std::move(tick);
	passes_ = 0;
	stop_ = false;
	running_ = true;
	thread_ = std::thread(&PeriodicWorker::Loop, this);
	return true;
}

bool PeriodicWorker::Stop(std::string &) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!running_) {
			return true;
		}
		stop_ = true;
	}
	if (thread_.joinable()) {
		thread_.join();
	}
	running_ = false;
	return true;
}

void PeriodicWorker::Loop() {
	while (!stop_) {
		try {
			Connection con(*db_);
			// The tick's first act on a fresh database would otherwise be a
			// catalog error: EnsureSchema runs from a table function's init, not
			// from LOAD (see CLAUDE.md).
			store::EnsureSchema(con);
			tick_(con);
			passes_++;
		} catch (...) {
			// Swallow and retry on the next tick. A worker that exits on one bad
			// row stops processing every other row too.
		}
		// Sleep in slices so Stop() returns promptly rather than after a full
		// poll interval — shutdown joins this thread.
		for (int i = 0; i < poll_secs_ * 10 && !stop_; i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}
}

} // namespace quackmail
