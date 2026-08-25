#define DUCKDB_EXTENSION_MAIN

#include "quackmail_spool_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include "quackmail/acme.hpp"
#include "quackmail/fetch.hpp"
#include "quackmail/listserv.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/worker.hpp"

#include <ctime>
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
PeriodicWorker g_fetch;
PeriodicWorker g_acme;

void ListservTick(Connection &con) {
	listserv::SpoolResult res;
	std::string err;
	listserv::SpoolOnce(con, res, err);
}

void AcmeTick(Connection &con) {
	// Certificate renewal. The pass records each certificate's outcome and
	// defers a failure with backoff, so one broken name does not stop the
	// others and does not hammer a rate-limited CA.
	std::vector<acme::Result> results;
	acme::RunOnce(con, "", false, results);
}

void FetchTick(Connection &con) {
	// RunDue records each feed's outcome on its own row, so a source that is
	// down shows up in `qm_feeds()` rather than stopping the others.
	fetch::RunDue(con, false);
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

// ---------------------------------------------------------------------------
// qm_fetch_run(feed := NULL, force := true) — poll now, one row per feed.
//
// The same reasoning as qm_listserv_run: the worker exists to do this on a
// timer, and everything that asserts on a pull goes through here instead.
// `force` defaults to true because calling this by hand means "now", not "if
// the interval happens to have elapsed".
// ---------------------------------------------------------------------------

struct FetchRunBindData : public FunctionData {
	std::string feed;
	bool force = true;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<FetchRunBindData>(*this);
	}
	bool Equals(const FunctionData &) const override {
		return false;
	}
};

struct FetchRunGlobalState : public GlobalTableFunctionState {
	std::vector<fetch::RunResult> results;
	idx_t idx = 0;
};

unique_ptr<FunctionData> FetchRunBind(ClientContext &, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<FetchRunBindData>();
	for (auto &kv : input.named_parameters) {
		auto key = StringUtil::Lower(kv.first);
		if (key == "feed") {
			result->feed = kv.second.IsNull() ? std::string() : kv.second.ToString();
		} else if (key == "force") {
			result->force = kv.second.GetValue<bool>();
		}
	}
	names = {"feed", "fetched", "stored", "skipped", "status", "error"};
	return_types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT,  LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> FetchRunInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<FetchRunBindData>();
	auto gstate = make_uniq<FetchRunGlobalState>();
	Connection con(*context.db);
	store::EnsureSchema(con);
	if (bind.feed.empty()) {
		gstate->results = fetch::RunDue(con, bind.force);
	} else {
		fetch::Feed f;
		fetch::RunResult res;
		res.feed = bind.feed;
		if (!fetch::GetFeed(con, bind.feed, f)) {
			res.status = "error";
			res.error = "no feed called '" + bind.feed + "'";
		} else {
			fetch::RunFeed(con, f, res);
		}
		gstate->results.push_back(res);
	}
	return std::move(gstate);
}

void FetchRunFunc(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &g = data.global_state->Cast<FetchRunGlobalState>();
	idx_t count = 0;
	while (g.idx < g.results.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &r = g.results[g.idx];
		output.SetValue(0, count, Value(r.feed));
		output.SetValue(1, count, Value::BIGINT(r.fetched));
		output.SetValue(2, count, Value::BIGINT(r.stored));
		output.SetValue(3, count, Value::BIGINT(r.skipped));
		output.SetValue(4, count, Value(r.status));
		output.SetValue(5, count, Value(r.error));
		g.idx++;
		count++;
	}
	output.SetCardinality(count);
}


// ---------------------------------------------------------------------------
// ACME: the SQL surface.
//
// qm_acme_run() is the one-shot beside the worker's controls, for the same
// reason qm_listserv_run() and qm_fetch_run() are: an integration test calls it
// and asserts, instead of sleeping past a poll interval. It is also what an
// operator wants after changing something.
//
// **No function here selects a private key.** qm_acme_account() reports the
// thumbprint — which is public by construction and is the thing you actually
// want to see — and qm_acme_certs() reports paths and dates. The account key and
// the certificate key exist only in the table and on disk.
// ---------------------------------------------------------------------------

struct AcmeRunBindData : public FunctionData {
	std::string name;
	// **false**, unlike qm_fetch_run's. Forcing here does not mean "now", it
	// means "re-issue regardless of how much life the certificate has left",
	// and every needless issuance is charged against a rate limit that is
	// counted per account per hostname per week. Calling this by hand should
	// run the pass, not spend the quota.
	bool force = false;
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<AcmeRunBindData>(*this);
	}
	bool Equals(const FunctionData &) const override {
		return false;
	}
};

struct AcmeRunGlobalState : public GlobalTableFunctionState {
	std::vector<acme::Result> results;
	idx_t idx = 0;
};

unique_ptr<FunctionData> AcmeRunBind(ClientContext &, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<AcmeRunBindData>();
	for (auto &kv : input.named_parameters) {
		auto key = StringUtil::Lower(kv.first);
		if (key == "name") {
			result->name = kv.second.IsNull() ? std::string() : kv.second.ToString();
		} else if (key == "force") {
			result->force = kv.second.GetValue<bool>();
		}
	}
	names = {"name", "status", "note", "not_after"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BIGINT};
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> AcmeRunInit(ClientContext &context,
                                                 TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<AcmeRunBindData>();
	auto gstate = make_uniq<AcmeRunGlobalState>();
	Connection con(*context.db);
	acme::RunOnce(con, bind.name, bind.force, gstate->results);
	return std::move(gstate);
}

void AcmeRunFunc(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &g = data.global_state->Cast<AcmeRunGlobalState>();
	idx_t count = 0;
	while (g.idx < g.results.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &r = g.results[g.idx];
		output.SetValue(0, count, Value(r.name));
		output.SetValue(1, count, Value(r.status));
		output.SetValue(2, count, Value(r.note));
		output.SetValue(3, count, Value::BIGINT(r.not_after));
		g.idx++;
		count++;
	}
	output.SetCardinality(count);
}

// ---- a one-row (ok, note) shape, shared by order/revoke/forget -------------

struct AcmeActionBindData : public FunctionData {
	std::string name;
	std::string domains;
	int64_t reason = 0;
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<AcmeActionBindData>(*this);
	}
	bool Equals(const FunctionData &) const override {
		return false;
	}
};

struct AcmeActionGlobalState : public GlobalTableFunctionState {
	bool emitted = false;
	bool ok = false;
	std::string note;
};

unique_ptr<FunctionData> AcmeActionBind(TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<AcmeActionBindData>();
	if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
		result->name = input.inputs[0].ToString();
	}
	if (input.inputs.size() >= 2 && !input.inputs[1].IsNull()) {
		result->domains = input.inputs[1].ToString();
	}
	for (auto &kv : input.named_parameters) {
		if (StringUtil::Lower(kv.first) == "reason") {
			result->reason = kv.second.GetValue<int64_t>();
		}
	}
	names = {"ok", "note"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR};
	return std::move(result);
}

void AcmeActionFunc(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &g = data.global_state->Cast<AcmeActionGlobalState>();
	if (g.emitted) {
		output.SetCardinality(0);
		return;
	}
	output.SetCardinality(1);
	output.SetValue(0, 0, Value::BOOLEAN(g.ok));
	output.SetValue(1, 0, Value(g.note));
	g.emitted = true;
}

unique_ptr<FunctionData> AcmeOrderBind(ClientContext &, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	return AcmeActionBind(input, return_types, names);
}

unique_ptr<GlobalTableFunctionState> AcmeOrderInit(ClientContext &context,
                                                   TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<AcmeActionBindData>();
	auto gstate = make_uniq<AcmeActionGlobalState>();
	Connection con(*context.db);
	std::string err;
	gstate->ok = acme::Order(con, bind.name, bind.domains, err);
	gstate->note = gstate->ok ? "queued" : err;
	return std::move(gstate);
}
void AcmeOrderFunc(ClientContext &c, TableFunctionInput &d, DataChunk &o) {
	AcmeActionFunc(c, d, o);
}

unique_ptr<FunctionData> AcmeRevokeBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	return AcmeActionBind(input, return_types, names);
}
unique_ptr<GlobalTableFunctionState> AcmeRevokeInit(ClientContext &context,
                                                    TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<AcmeActionBindData>();
	auto gstate = make_uniq<AcmeActionGlobalState>();
	Connection con(*context.db);
	std::string err;
	gstate->ok = acme::Revoke(con, bind.name, (int)bind.reason, err);
	gstate->note = gstate->ok ? "revoked" : err;
	return std::move(gstate);
}
void AcmeRevokeFunc(ClientContext &c, TableFunctionInput &d, DataChunk &o) {
	AcmeActionFunc(c, d, o);
}

unique_ptr<FunctionData> AcmeForgetBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	return AcmeActionBind(input, return_types, names);
}
unique_ptr<GlobalTableFunctionState> AcmeForgetInit(ClientContext &context,
                                                    TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<AcmeActionBindData>();
	auto gstate = make_uniq<AcmeActionGlobalState>();
	Connection con(*context.db);
	std::string err;
	gstate->ok = acme::Forget(con, bind.name, err);
	gstate->note = gstate->ok ? "forgotten" : err;
	return std::move(gstate);
}
void AcmeForgetFunc(ClientContext &c, TableFunctionInput &d, DataChunk &o) {
	AcmeActionFunc(c, d, o);
}

// ---- listings --------------------------------------------------------------

struct AcmeRowsBindData : public FunctionData {
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<AcmeRowsBindData>(*this);
	}
	bool Equals(const FunctionData &) const override {
		return false;
	}
};

struct AcmeRowsGlobalState : public GlobalTableFunctionState {
	std::vector<std::vector<Value>> rows;
	idx_t idx = 0;
};

void EmitRows(TableFunctionInput &data, DataChunk &output) {
	auto &g = data.global_state->Cast<AcmeRowsGlobalState>();
	idx_t count = 0;
	while (g.idx < g.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = g.rows[g.idx];
		for (idx_t c = 0; c < row.size(); c++) {
			output.SetValue(c, count, row[c]);
		}
		g.idx++;
		count++;
	}
	output.SetCardinality(count);
}

unique_ptr<FunctionData> AcmeCertsBind(ClientContext &, TableFunctionBindInput &,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	names = {"name", "domains", "status", "not_after", "days_left", "issued", "cert_path",
	         "error"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::BIGINT,
	                LogicalType::VARCHAR, LogicalType::VARCHAR};
	return make_uniq<AcmeRowsBindData>();
}

unique_ptr<GlobalTableFunctionState> AcmeCertsInit(ClientContext &context, TableFunctionInitInput &) {
	auto gstate = make_uniq<AcmeRowsGlobalState>();
	Connection con(*context.db);
	acme::EnsureSchema(con);
	const int64_t now = (int64_t)std::time(nullptr);
	// Deliberately no key_pem, and no cert_pem either: the chain is public but
	// it is not what an operator is asking for, and the less this returns the
	// less there is to leak into a page by accident.
	auto r = con.Query("SELECT o.name, o.domains, o.status, c.not_after, c.issued, c.cert_path, "
	                   "       o.error "
	                   "  FROM quackmail_acme_orders o "
	                   "  LEFT JOIN quackmail_acme_certs c ON c.name = o.name "
	                   " ORDER BY o.name");
	if (!r || r->HasError()) {
		return std::move(gstate);
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		const bool have = !mat.GetValue(3, i).IsNull();
		const int64_t not_after = have ? mat.GetValue(3, i).GetValue<int64_t>() : 0;
		gstate->rows.push_back({
		    mat.GetValue(0, i),
		    mat.GetValue(1, i),
		    mat.GetValue(2, i),
		    Value::BIGINT(not_after),
		    Value::BIGINT(have ? (not_after - now) / 86400 : 0),
		    Value::BIGINT(mat.GetValue(4, i).IsNull() ? 0 : mat.GetValue(4, i).GetValue<int64_t>()),
		    mat.GetValue(5, i).IsNull() ? Value("") : mat.GetValue(5, i),
		    mat.GetValue(6, i).IsNull() ? Value("") : mat.GetValue(6, i),
		});
	}
	return std::move(gstate);
}
void AcmeCertsFunc(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	EmitRows(data, output);
}

unique_ptr<FunctionData> AcmeAccountBind(ClientContext &, TableFunctionBindInput &,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	names = {"directory_url", "account_url", "contact", "thumbprint", "tos_agreed", "created"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::BIGINT};
	return make_uniq<AcmeRowsBindData>();
}

unique_ptr<GlobalTableFunctionState> AcmeAccountInit(ClientContext &context,
                                                     TableFunctionInitInput &) {
	auto gstate = make_uniq<AcmeRowsGlobalState>();
	Connection con(*context.db);
	acme::EnsureSchema(con);
	auto r = con.Query("SELECT directory_url, account_url, contact, key_pem, tos_agreed, created "
	                   "  FROM quackmail_acme_accounts ORDER BY directory_url");
	if (!r || r->HasError()) {
		return std::move(gstate);
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		// The **thumbprint**, computed here and thrown away — never the key. It
		// is the public identity of the account and the thing that has to match
		// what a challenge serves, so it is what is worth showing.
		std::string thumb;
		std::string err;
		if (!mat.GetValue(3, i).IsNull()) {
			acme::JwkThumbprint(mat.GetValue(3, i).ToString(), thumb, err);
		}
		gstate->rows.push_back({
		    mat.GetValue(0, i),
		    mat.GetValue(1, i).IsNull() ? Value("") : mat.GetValue(1, i),
		    mat.GetValue(2, i).IsNull() ? Value("") : mat.GetValue(2, i),
		    Value(thumb),
		    Value::BOOLEAN(!mat.GetValue(4, i).IsNull() && mat.GetValue(4, i).GetValue<bool>()),
		    Value::BIGINT(mat.GetValue(5, i).IsNull() ? 0 : mat.GetValue(5, i).GetValue<int64_t>()),
		});
	}
	return std::move(gstate);
}
void AcmeAccountFunc(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	EmitRows(data, output);
}

unique_ptr<FunctionData> AcmeReloadBind(ClientContext &, TableFunctionBindInput &,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	names = {"function_name", "note"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	return make_uniq<AcmeRowsBindData>();
}

unique_ptr<GlobalTableFunctionState> AcmeReloadInit(ClientContext &context,
                                                    TableFunctionInitInput &) {
	auto gstate = make_uniq<AcmeRowsGlobalState>();
	Connection con(*context.db);
	for (const auto &r : acme::ReloadListeners(con)) {
		gstate->rows.push_back({Value(r.first), Value(r.second)});
	}
	return std::move(gstate);
}
void AcmeReloadFunc(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	EmitRows(data, output);
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	acme::EnsureSchema(con);

	RegisterWorkerControls(loader, "qm_listserv", "listserv", g_listserv, ListservTick);
	RegisterWorkerControls(loader, "qm_fetch", "fetch", g_fetch, FetchTick);
	RegisterWorkerControls(loader, "qm_acme", "acme", g_acme, AcmeTick);

	TableFunction run("qm_listserv_run", {}, RunFunc, RunBind, RunInit);
	run.named_parameters["room_num"] = LogicalType::BIGINT;
	loader.RegisterFunction(run);

	TableFunction fetch_run("qm_fetch_run", {}, FetchRunFunc, FetchRunBind, FetchRunInit);
	fetch_run.named_parameters["feed"] = LogicalType::VARCHAR;
	fetch_run.named_parameters["force"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(fetch_run);

	TableFunction acme_run("qm_acme_run", {}, AcmeRunFunc, AcmeRunBind, AcmeRunInit);
	acme_run.named_parameters["name"] = LogicalType::VARCHAR;
	acme_run.named_parameters["force"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(acme_run);

	TableFunction acme_order("qm_acme_order", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                         AcmeOrderFunc, AcmeOrderBind, AcmeOrderInit);
	loader.RegisterFunction(acme_order);

	TableFunction acme_certs("qm_acme_certs", {}, AcmeCertsFunc, AcmeCertsBind, AcmeCertsInit);
	loader.RegisterFunction(acme_certs);

	TableFunction acme_account("qm_acme_account", {}, AcmeAccountFunc, AcmeAccountBind,
	                           AcmeAccountInit);
	loader.RegisterFunction(acme_account);

	TableFunction acme_revoke("qm_acme_revoke", {LogicalType::VARCHAR}, AcmeRevokeFunc,
	                          AcmeRevokeBind, AcmeRevokeInit);
	acme_revoke.named_parameters["reason"] = LogicalType::BIGINT;
	loader.RegisterFunction(acme_revoke);

	TableFunction acme_forget("qm_acme_forget", {LogicalType::VARCHAR}, AcmeForgetFunc,
	                          AcmeForgetBind, AcmeForgetInit);
	loader.RegisterFunction(acme_forget);

	// Deliberately not "qm_acme_tls_reload": the sweep it performs looks for
	// functions named `%_tls_reload`, and a sweep that finds itself recurses.
	TableFunction acme_reload("qm_acme_reload", {}, AcmeReloadFunc, AcmeReloadBind,
	                          AcmeReloadInit);
	loader.RegisterFunction(acme_reload);
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
