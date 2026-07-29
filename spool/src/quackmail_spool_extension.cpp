#define DUCKDB_EXTENSION_MAIN

#include "quackmail_spool_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/listserv.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/worker.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace {

using namespace quackmail;

// ---------------------------------------------------------------------------
// The workers.
//
// Each is a PeriodicWorker plus the function it ticks. They share the control
// plumbing below, which is the ServerController/RegisterServerControls pattern
// applied to a clock instead of a socket: one `<prefix>_start/_stop/_status`
// trio per worker.
// ---------------------------------------------------------------------------

PeriodicWorker g_listserv;

void ListservTick(Connection &con) {
	listserv::SpoolResult res;
	std::string err;
	listserv::SpoolOnce(con, res, err);
}

// ---- worker control table functions ----------------------------------------

enum class WorkerAction { START, STOP, STATUS };

struct WorkerInfo : public TableFunctionInfo {
	WorkerAction action = WorkerAction::STATUS;
	std::string name;      // the registered function name, echoed back
	std::string worker;    // "listserv"
	PeriodicWorker *worker_ptr = nullptr;
	PeriodicWorker::Tick tick;
};

struct WorkerBindData : public FunctionData {
	WorkerInfo *info = nullptr;
	int poll_secs = 60;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<WorkerBindData>(*this);
	}
	bool Equals(const FunctionData &) const override {
		return false;
	}
};

struct WorkerGlobalState : public GlobalTableFunctionState {
	bool emitted = false;
	std::string worker;
	bool running = false;
	int64_t poll = 0;
	int64_t passes = 0;
	std::string note;
};

unique_ptr<FunctionData> WorkerBind(ClientContext &, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto info = reinterpret_cast<WorkerInfo *>(input.info.get());
	auto result = make_uniq<WorkerBindData>();
	result->info = info;
	if (info->action == WorkerAction::START) {
		for (auto &kv : input.named_parameters) {
			if (StringUtil::Lower(kv.first) == "poll_secs") {
				result->poll_secs = kv.second.GetValue<int32_t>();
			}
		}
	}
	names = {"worker", "running", "poll_secs", "passes", "note"};
	return_types = {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::VARCHAR};
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> WorkerInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<WorkerBindData>();
	auto info = bind.info;
	auto gstate = make_uniq<WorkerGlobalState>();
	auto &worker = *info->worker_ptr;

	std::string note;
	switch (info->action) {
	case WorkerAction::START: {
		std::string err;
		note = worker.Start(*context.db, bind.poll_secs, info->tick, err) ? "started" : ("error: " + err);
		break;
	}
	case WorkerAction::STOP: {
		std::string err;
		worker.Stop(err);
		note = "stopped";
		break;
	}
	case WorkerAction::STATUS:
		note = worker.IsRunning() ? "running" : "stopped";
		break;
	}

	gstate->worker = info->worker;
	gstate->running = worker.IsRunning();
	gstate->poll = worker.PollSecs();
	gstate->passes = (int64_t)worker.Passes();
	gstate->note = note;
	return std::move(gstate);
}

void WorkerFunc(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &g = data.global_state->Cast<WorkerGlobalState>();
	if (g.emitted) {
		output.SetCardinality(0);
		return;
	}
	output.SetCardinality(1);
	output.SetValue(0, 0, Value(g.worker));
	output.SetValue(1, 0, Value::BOOLEAN(g.running));
	output.SetValue(2, 0, Value::BIGINT(g.poll));
	output.SetValue(3, 0, Value::BIGINT(g.passes));
	output.SetValue(4, 0, Value(g.note));
	g.emitted = true;
}

// Register `<prefix>_start`, `<prefix>_stop` and `<prefix>_status` for one
// worker, mirroring RegisterServerControls.
void RegisterWorkerControls(ExtensionLoader &loader, const std::string &prefix, const std::string &worker_name,
                            PeriodicWorker &worker, PeriodicWorker::Tick tick) {
	struct Entry {
		const char *suffix;
		WorkerAction action;
		bool params;
	};
	static const Entry kEntries[] = {
	    {"_start", WorkerAction::START, true},
	    {"_stop", WorkerAction::STOP, false},
	    {"_status", WorkerAction::STATUS, false},
	};
	for (const auto &e : kEntries) {
		std::string name = prefix + e.suffix;
		TableFunction f(name, {}, WorkerFunc, WorkerBind, WorkerInit);
		if (e.params) {
			f.named_parameters["poll_secs"] = LogicalType::INTEGER;
		}
		auto info = make_shared_ptr<WorkerInfo>();
		info->action = e.action;
		info->name = name;
		info->worker = worker_name;
		info->worker_ptr = &worker;
		info->tick = tick;
		f.function_info = std::move(info);
		loader.RegisterFunction(f);
	}
}

// ---------------------------------------------------------------------------
// qm_listserv_run() — one pass, synchronously.
//
// The worker exists to run this on a timer, but a test (and an admin who just
// changed something) needs it to happen *now* and to report what it did. Every
// integration assertion about list distribution goes through this rather than
// sleeping past a poll interval.
// ---------------------------------------------------------------------------

struct RunBindData : public FunctionData {
	int64_t room_num = -1; // -1 = every enabled list

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<RunBindData>(*this);
	}
	bool Equals(const FunctionData &) const override {
		return false;
	}
};

struct RunGlobalState : public GlobalTableFunctionState {
	bool emitted = false;
	listserv::SpoolResult res;
	std::string note;
};

unique_ptr<FunctionData> RunBind(ClientContext &, TableFunctionBindInput &input,
                                 vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<RunBindData>();
	for (auto &kv : input.named_parameters) {
		if (StringUtil::Lower(kv.first) == "room_num") {
			result->room_num = kv.second.GetValue<int64_t>();
		}
	}
	names = {"distributed", "recipients", "digests", "held", "note"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::VARCHAR};
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> RunInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<RunBindData>();
	auto gstate = make_uniq<RunGlobalState>();
	Connection con(*context.db);
	store::EnsureSchema(con);
	std::string err;
	bool ok = bind.room_num >= 0 ? listserv::SpoolRoom(con, bind.room_num, gstate->res, err)
	                             : listserv::SpoolOnce(con, gstate->res, err);
	gstate->note = ok ? (err.empty() ? "ok" : err) : ("error: " + err);
	return std::move(gstate);
}

void RunFunc(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &g = data.global_state->Cast<RunGlobalState>();
	if (g.emitted) {
		output.SetCardinality(0);
		return;
	}
	output.SetCardinality(1);
	output.SetValue(0, 0, Value::BIGINT(g.res.distributed));
	output.SetValue(1, 0, Value::BIGINT(g.res.recipients));
	output.SetValue(2, 0, Value::BIGINT(g.res.digests));
	output.SetValue(3, 0, Value::BIGINT(g.res.held));
	output.SetValue(4, 0, Value(g.note));
	g.emitted = true;
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);

	RegisterWorkerControls(loader, "qm_listserv", "listserv", g_listserv, ListservTick);

	TableFunction run("qm_listserv_run", {}, RunFunc, RunBind, RunInit);
	run.named_parameters["room_num"] = LogicalType::BIGINT;
	loader.RegisterFunction(run);
}

} // namespace

void QuackmailSpoolExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailSpoolExtension::Name() {
	return "quackmail_spool";
}
std::string QuackmailSpoolExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_SPOOL
	return EXT_VERSION_QUACKMAIL_SPOOL;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_spool, loader) {
	duckdb::LoadInternal(loader);
}
}
