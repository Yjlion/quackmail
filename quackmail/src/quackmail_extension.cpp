#define DUCKDB_EXTENSION_MAIN

#include "quackmail_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/citadel_store.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mime.hpp"

#include <ctime>

namespace duckdb {

namespace {

enum class UmbrellaKind {
	VERSION,
	USER_ADD,
	USER_REMOVE,
	STATUS,
	MIME_HEADERS,
	MIME_DECODE_HEADER,
	MIME_DECODE,
	MIME_ADDRESSES,
	PARSE_DATE,
	MIME_PARTS,
	CIT_ROOM_ADD,
	CIT_FLOOR_ADD,
};

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
		gstate->rows.push_back({Value("QuackCit 0.3.0")});
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
		add_metric("users", "SELECT count(*) FROM quackmail_users");
		add_metric("floors", "SELECT count(*) FROM citadel_floors");
		add_metric("rooms", "SELECT count(*) FROM citadel_rooms");
		add_metric("messages", "SELECT count(*) FROM citadel_messages");
		add_metric("outbound_queued", "SELECT count(*) FROM quackmail_outbound WHERE status = 'queued'");
		break;
	}
	case UmbrellaKind::CIT_ROOM_ADD: {
		std::string err;
		int64_t num = quackmail::citadel::CreateRoom(con, bind.args[0], 0, 0, "", 0, err);
		gstate->rows.push_back(
		    {Value::BOOLEAN(num >= 0), Value(num >= 0 ? ("room " + std::to_string(num) + " created") : err)});
		break;
	}
	case UmbrellaKind::CIT_FLOOR_ADD: {
		std::string err;
		int64_t num = quackmail::citadel::CreateFloor(con, bind.args[0], err);
		gstate->rows.push_back(
		    {Value::BOOLEAN(num >= 0), Value(num >= 0 ? ("floor " + std::to_string(num) + " created") : err)});
		break;
	}
	case UmbrellaKind::MIME_HEADERS: {
		auto parsed = quackmail::mime::Parse(bind.args[0]);
		for (auto &h : parsed.headers) {
			gstate->rows.push_back({Value(h.first), Value(h.second)});
		}
		break;
	}
	case UmbrellaKind::MIME_DECODE_HEADER: {
		gstate->rows.push_back({Value(quackmail::mime::DecodeEncodedWords(bind.args[0]))});
		break;
	}
	case UmbrellaKind::MIME_DECODE: {
		gstate->rows.push_back(
		    {Value(quackmail::mime::DecodeContentTransferEncoding(bind.args[0], bind.args[1]))});
		break;
	}
	case UmbrellaKind::MIME_ADDRESSES: {
		for (auto &a : quackmail::mime::ParseAddressList(bind.args[0])) {
			gstate->rows.push_back({Value(a.name), Value(a.addr)});
		}
		break;
	}
	case UmbrellaKind::PARSE_DATE: {
		int64_t epoch = 0;
		if (quackmail::mime::ParseDate(bind.args[0], epoch)) {
			std::time_t t = static_cast<std::time_t>(epoch);
			std::tm tm_utc{};
#if defined(_WIN32)
			gmtime_s(&tm_utc, &t);
#else
			gmtime_r(&t, &tm_utc);
#endif
			char buf[32];
			std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
			gstate->rows.push_back({Value::BIGINT(epoch), Value(std::string(buf))});
		} else {
			gstate->rows.push_back({Value(LogicalType::BIGINT), Value(LogicalType::VARCHAR)});
		}
		break;
	}
	case UmbrellaKind::MIME_PARTS: {
		auto root = quackmail::mime::ParseEntity(bind.args[0]);
		for (auto &p : quackmail::mime::FlattenParts(root)) {
			gstate->rows.push_back({Value(p.section), Value(p.content_type), Value(p.charset),
			                        Value(p.encoding), Value::BIGINT(p.size_bytes), Value(p.filename),
			                        Value(p.content)});
		}
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

unique_ptr<FunctionData> MimeHeadersBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::MIME_HEADERS;
	b->args = {input.inputs[0].ToString()};
	names = {"name", "value"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> MimeDecodeHeaderBind(ClientContext &, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::MIME_DECODE_HEADER;
	b->args = {input.inputs[0].ToString()};
	names = {"decoded"};
	return_types = {LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> MimeDecodeBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::MIME_DECODE;
	b->args = {input.inputs[0].ToString(), input.inputs[1].ToString()};
	names = {"decoded"};
	return_types = {LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> MimeAddressesBind(ClientContext &, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::MIME_ADDRESSES;
	b->args = {input.inputs[0].ToString()};
	names = {"name", "address"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> ParseDateBind(ClientContext &, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::PARSE_DATE;
	b->args = {input.inputs[0].ToString()};
	names = {"epoch", "iso"};
	return_types = {LogicalType::BIGINT, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> MimePartsBind(ClientContext &, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::MIME_PARTS;
	b->args = {input.inputs[0].ToString()};
	names = {"section", "content_type", "charset", "encoding", "size_bytes", "filename", "content"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> CitRoomAddBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::CIT_ROOM_ADD;
	b->args = {input.inputs[0].ToString()};
	names = {"ok", "note"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> CitFloorAddBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::CIT_FLOOR_ADD;
	b->args = {input.inputs[0].ToString()};
	names = {"ok", "note"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR};
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

	// Citadel admin: create a public room / a floor.
	loader.RegisterFunction(
	    TableFunction("cit_room_add", {LogicalType::VARCHAR}, RowsFunc, CitRoomAddBind, RowsInit));
	loader.RegisterFunction(
	    TableFunction("cit_floor_add", {LogicalType::VARCHAR}, RowsFunc, CitFloorAddBind, RowsInit));

	// MIME / message-format helpers (RFC 2045-2049, 822/2822/5322).
	loader.RegisterFunction(
	    TableFunction("qm_mime_headers", {LogicalType::VARCHAR}, RowsFunc, MimeHeadersBind, RowsInit));
	loader.RegisterFunction(TableFunction("qm_mime_decode_header", {LogicalType::VARCHAR}, RowsFunc,
	                                      MimeDecodeHeaderBind, RowsInit));
	loader.RegisterFunction(TableFunction("qm_mime_decode", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                      RowsFunc, MimeDecodeBind, RowsInit));
	loader.RegisterFunction(
	    TableFunction("qm_mime_addresses", {LogicalType::VARCHAR}, RowsFunc, MimeAddressesBind, RowsInit));
	loader.RegisterFunction(
	    TableFunction("qm_parse_date", {LogicalType::VARCHAR}, RowsFunc, ParseDateBind, RowsInit));
	loader.RegisterFunction(
	    TableFunction("qm_mime_parts", {LogicalType::VARCHAR}, RowsFunc, MimePartsBind, RowsInit));
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
