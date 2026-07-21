#include "quackmail/server_controls.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"

namespace quackmail {

using namespace duckdb;

enum class ServerAction { START, STOP, STATUS };

// Carried to the generic bind via TableFunction.function_info so one set of
// callbacks serves every extension's start/stop/status functions.
struct ServerControlInfo : public TableFunctionInfo {
	ServerController *ctrl = nullptr;
	ConnHandler handler = nullptr;
	int default_port = 0;
	ServerAction action = ServerAction::STATUS;
	std::string action_name;
};

struct ServerControlBindData : public FunctionData {
	ServerControlInfo *info = nullptr;
	std::string host = "0.0.0.0";
	int64_t port = 0;
	tls::TlsConfig tls;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ServerControlBindData>(*this);
	}
	bool Equals(const FunctionData &) const override {
		return false;
	}
};

struct ServerControlGlobalState : public GlobalTableFunctionState {
	bool emitted = false;
	std::string action_name;
	bool running = false;
	std::string host;
	int64_t port = 0;
	int64_t connections = 0;
	std::string note;
};

static unique_ptr<FunctionData> ControlBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	auto info = reinterpret_cast<ServerControlInfo *>(input.info.get());
	auto result = make_uniq<ServerControlBindData>();
	result->info = info;
	result->port = info->default_port;

	if (info->action == ServerAction::START) {
		if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
			result->host = input.inputs[0].ToString();
		}
		if (input.inputs.size() >= 2 && !input.inputs[1].IsNull()) {
			result->port = input.inputs[1].GetValue<int64_t>();
		}
		for (auto &kv : input.named_parameters) {
			auto key = StringUtil::Lower(kv.first);
			if (key == "tls_cert") {
				result->tls.cert_path = kv.second.ToString();
			} else if (key == "tls_key") {
				result->tls.key_path = kv.second.ToString();
			} else if (key == "implicit_tls") {
				result->tls.implicit = BooleanValue::Get(kv.second);
			} else if (key == "starttls") {
				result->tls.starttls = BooleanValue::Get(kv.second);
			}
		}
	}

	names = {"action", "running", "host", "port", "connections", "note"};
	return_types = {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR,
	                LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::VARCHAR};
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> ControlInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ServerControlBindData>();
	auto info = bind.info;
	auto *ctrl = info->ctrl;
	auto gstate = make_uniq<ServerControlGlobalState>();
	auto &db = *context.db;

	std::string note;
	switch (info->action) {
	case ServerAction::START: {
		std::string err;
		bool ok = ctrl->Start(db, bind.host, (int)bind.port, bind.tls, info->handler, err);
		note = ok ? "started" : ("error: " + err);
		break;
	}
	case ServerAction::STOP: {
		std::string err;
		bool ok = ctrl->Stop(err);
		note = ok ? "stopped" : ("error: " + err);
		break;
	}
	case ServerAction::STATUS:
		note = ctrl->IsRunning() ? "running" : "stopped";
		break;
	}

	gstate->action_name = info->action_name;
	gstate->running = ctrl->IsRunning();
	gstate->host = ctrl->Host();
	gstate->port = ctrl->Port();
	gstate->connections = (int64_t)ctrl->Connections();
	gstate->note = note;
	return std::move(gstate);
}

static void ControlFunc(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &g = data.global_state->Cast<ServerControlGlobalState>();
	if (g.emitted) {
		output.SetCardinality(0);
		return;
	}
	output.SetCardinality(1);
	output.SetValue(0, 0, Value(g.action_name));
	output.SetValue(1, 0, Value::BOOLEAN(g.running));
	output.SetValue(2, 0, Value(g.host));
	output.SetValue(3, 0, Value::BIGINT(g.port));
	output.SetValue(4, 0, Value::BIGINT(g.connections));
	output.SetValue(5, 0, Value(g.note));
	g.emitted = true;
}

static void RegisterOne(ExtensionLoader &loader, const std::string &name, ServerAction action,
                        const vector<LogicalType> &args, bool with_tls_params, int default_port,
                        ServerController &controller, ConnHandler handler) {
	TableFunction f(name, args, ControlFunc, ControlBind, ControlInit);
	if (with_tls_params) {
		f.named_parameters["tls_cert"] = LogicalType::VARCHAR;
		f.named_parameters["tls_key"] = LogicalType::VARCHAR;
		f.named_parameters["implicit_tls"] = LogicalType::BOOLEAN;
		f.named_parameters["starttls"] = LogicalType::BOOLEAN;
	}
	auto info = make_shared_ptr<ServerControlInfo>();
	info->ctrl = &controller;
	info->handler = handler;
	info->default_port = default_port;
	info->action = action;
	info->action_name = name;
	f.function_info = std::move(info);
	loader.RegisterFunction(f);
}

void RegisterServerControls(ExtensionLoader &loader, const std::string &prefix, int default_port,
                            ServerController &controller, ConnHandler handler) {
	RegisterOne(loader, prefix + "_start", ServerAction::START,
	            {LogicalType::VARCHAR, LogicalType::INTEGER}, true, default_port, controller, handler);
	RegisterOne(loader, prefix + "_stop", ServerAction::STOP, {}, false, default_port, controller, handler);
	RegisterOne(loader, prefix + "_status", ServerAction::STATUS, {}, false, default_port, controller, handler);
}

} // namespace quackmail
