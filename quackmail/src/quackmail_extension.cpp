#define DUCKDB_EXTENSION_MAIN

#include "quackmail_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/mail_store.hpp"

namespace duckdb {

namespace {

enum class UmbrellaKind { VERSION, USER_ADD, USER_REMOVE, STATUS };

struct RowsBindData : public FunctionData {
	UmbrellaKind kind = UmbrellaKind::VERSION;
	vector<string> args;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<RowsBindData>(*this);
	}
	bool Equals(const FunctionData &) const override {
		return false;
	}
};

struct RowsGlobalState : public GlobalTableFunctionState {
	vector<vector<Value>> rows;
	idx_t idx = 0;
};

unique_ptr<GlobalTableFunctionState> RowsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<RowsBindData>();
	auto gstate = make_uniq<RowsGlobalState>();
	Connection con(*context.db);
	quackmail::store::EnsureSchema(con);

	switch (bind.kind) {
	case UmbrellaKind::VERSION:
		gstate->rows.push_back({Value("QuackMail 0.1.0")});
		break;
	case UmbrellaKind::USER_ADD: {
		std::string err;
		bool ok = quackmail::auth::AddUser(con, bind.args[0], bind.args[1], err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "user added" : err)});
		break;
	}
	case UmbrellaKind::USER_REMOVE: {
		std::string err;
		bool ok = quackmail::auth::RemoveUser(con, bind.args[0], err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "user removed" : err)});
		break;
	}
	case UmbrellaKind::STATUS: {
		auto add_metric = [&](const std::string &name, const std::string &sql) {
			auto r = con.Query(sql);
			int64_t v = (!r->HasError() && r->RowCount() == 1) ? r->GetValue(0, 0).GetValue<int64_t>() : -1;
			gstate->rows.push_back({Value(name), Value::BIGINT(v)});
		};
		add_metric("messages", "SELECT count(*) FROM quackmail_messages");
		add_metric("recipients", "SELECT count(*) FROM quackmail_recipients");
		add_metric("users", "SELECT count(*) FROM quackmail_users");
		add_metric("outbound_queued", "SELECT count(*) FROM quackmail_outbound WHERE status = 'queued'");
		break;
	}
	}
	return std::move(gstate);
}

void RowsFunc(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &g = data.global_state->Cast<RowsGlobalState>();
	idx_t count = 0;
	while (g.idx < g.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = g.rows[g.idx];
		for (idx_t c = 0; c < row.size(); c++) {
			output.SetValue(c, count, row[c]);
		}
		g.idx++;
		count++;
	}
	output.SetCardinality(count);
}

unique_ptr<FunctionData> VersionBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                     vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::VERSION;
	names = {"version"};
	return_types = {LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> UserAddBind(ClientContext &, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::USER_ADD;
	b->args = {input.inputs[0].ToString(), input.inputs[1].ToString()};
	names = {"ok", "note"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> UserRemoveBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::USER_REMOVE;
	b->args = {input.inputs[0].ToString()};
	names = {"ok", "note"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> StatusBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                    vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::STATUS;
	names = {"metric", "value"};
	return_types = {LogicalType::VARCHAR, LogicalType::BIGINT};
	return std::move(b);
}

void LoadInternal(ExtensionLoader &loader) {
	// Ensure the shared schema exists as soon as the umbrella loads.
	Connection con(loader.GetDatabaseInstance());
	quackmail::store::EnsureSchema(con);

	loader.RegisterFunction(TableFunction("qm_version", {}, RowsFunc, VersionBind, RowsInit));
	loader.RegisterFunction(
	    TableFunction("qm_user_add", {LogicalType::VARCHAR, LogicalType::VARCHAR}, RowsFunc, UserAddBind, RowsInit));
	loader.RegisterFunction(
	    TableFunction("qm_user_remove", {LogicalType::VARCHAR}, RowsFunc, UserRemoveBind, RowsInit));
	loader.RegisterFunction(TableFunction("qm_status", {}, RowsFunc, StatusBind, RowsInit));
}

} // namespace

void QuackmailExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailExtension::Name() {
	return "quackmail";
}
std::string QuackmailExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL
	return EXT_VERSION_QUACKMAIL;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail, loader) {
	duckdb::LoadInternal(loader);
}
}
