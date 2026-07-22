#define DUCKDB_EXTENSION_MAIN

#include "quackmail_pop3_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/citadel_store.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/util.hpp"

#include <cstdlib>
#include <string>
#include <vector>

namespace duckdb {
namespace {

using namespace quackmail;

ServerController g_pop3;

// One maildrop entry (POP3 message index -> Citadel message number).
struct Entry {
	int64_t msgnum = 0;
	int64_t size = 0;
	bool deleted = false;
};

// Load the (msgnum, size) list for a user's Mail room, oldest first.
std::vector<Entry> LoadMaildrop(Connection &con, const std::string &username) {
	std::vector<Entry> out;
	int64_t room = citadel::GetOrCreateMailRoom(con, username);
	if (room < 0) {
		return out;
	}
	auto stmt = con.Prepare("SELECT m.msgnum, octet_length(m.raw) FROM citadel_messages m "
	                        "JOIN citadel_room_msgs rm ON rm.msgnum = m.msgnum "
	                        "WHERE rm.room_num = $1 ORDER BY m.msgnum");
	if (stmt->HasError()) {
		return out;
	}
	duckdb::vector<Value> params = {Value::BIGINT(room)};
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		Entry e;
		e.msgnum = mat.GetValue(0, i).GetValue<int64_t>();
		Value sz = mat.GetValue(1, i);
		e.size = sz.IsNull() ? 0 : sz.GetValue<int64_t>();
		out.push_back(e);
	}
	return out;
}

// Send a message body dot-stuffed, terminated by a "." line (RFC 1939).
void SendDotStuffed(net::ClientStream &stream, const std::string &raw) {
	std::string line;
	auto flush = [&]() {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (!line.empty() && line[0] == '.') {
			line = "." + line;
		}
		stream.WriteLine(line);
		line.clear();
	};
	for (char c : raw) {
		if (c == '\n') {
			flush();
		} else {
			line.push_back(c);
		}
	}
	if (!line.empty()) {
		flush();
	}
	stream.WriteLine(".");
}

void HandlePop3(DatabaseInstance &db, net::ClientStream &stream) {
	Connection con(db);
	store::EnsureSchema(con);

	std::string pending_user;
	std::string user;
	bool authed = false;
	std::vector<Entry> drop;

	stream.WriteLine("+OK quackcit POP3 ready");

	std::string line;
	while (stream.ReadLine(line, 8192)) {
		std::string verb, rest;
		size_t sp = line.find(' ');
		if (sp == std::string::npos) {
			verb = util::Upper(line);
		} else {
			verb = util::Upper(line.substr(0, sp));
			rest = line.substr(sp + 1);
		}

		if (verb == "QUIT") {
			// Apply deletions: drop the pointers from the user's Mail room.
			if (authed) {
				int64_t room = citadel::GetOrCreateMailRoom(con, user);
				auto stmt = con.Prepare("DELETE FROM citadel_room_msgs WHERE room_num = $1 AND msgnum = $2");
				if (!stmt->HasError()) {
					for (auto &e : drop) {
						if (e.deleted) {
							duckdb::vector<Value> params = {Value::BIGINT(room), Value::BIGINT(e.msgnum)};
							stmt->Execute(params, false);
						}
					}
				}
			}
			stream.WriteLine("+OK quackcit signing off");
			return;
		} else if (verb == "CAPA") {
			stream.WriteLine("+OK capability list follows");
			stream.WriteLine("USER");
			stream.WriteLine("UIDL");
			stream.WriteLine("TOP");
			stream.WriteLine(".");
		} else if (verb == "USER") {
			pending_user = rest;
			stream.WriteLine("+OK");
		} else if (verb == "PASS") {
			if (pending_user.empty()) {
				stream.WriteLine("-ERR send USER first");
			} else if (auth::Verify(con, pending_user, rest)) {
				user = pending_user;
				authed = true;
				drop = LoadMaildrop(con, user);
				stream.WriteLine("+OK mailbox ready");
			} else {
				stream.WriteLine("-ERR authentication failed");
			}
		} else if (!authed) {
			stream.WriteLine("-ERR log in first");
		} else if (verb == "STAT") {
			int64_t count = 0, total = 0;
			for (auto &e : drop) {
				if (!e.deleted) {
					count++;
					total += e.size;
				}
			}
			stream.WriteLine("+OK " + std::to_string(count) + " " + std::to_string(total));
		} else if (verb == "LIST" || verb == "UIDL") {
			bool uidl = verb == "UIDL";
			if (!rest.empty()) {
				size_t idx = (size_t)std::atoi(rest.c_str());
				if (idx < 1 || idx > drop.size() || drop[idx - 1].deleted) {
					stream.WriteLine("-ERR no such message");
				} else {
					auto &e = drop[idx - 1];
					std::string val = uidl ? std::to_string(e.msgnum) : std::to_string(e.size);
					stream.WriteLine("+OK " + std::to_string(idx) + " " + val);
				}
			} else {
				stream.WriteLine("+OK");
				for (size_t i = 0; i < drop.size(); i++) {
					if (drop[i].deleted) {
						continue;
					}
					std::string val = uidl ? std::to_string(drop[i].msgnum) : std::to_string(drop[i].size);
					stream.WriteLine(std::to_string(i + 1) + " " + val);
				}
				stream.WriteLine(".");
			}
		} else if (verb == "RETR" || verb == "TOP") {
			size_t idx = (size_t)std::atoi(rest.c_str());
			if (idx < 1 || idx > drop.size() || drop[idx - 1].deleted) {
				stream.WriteLine("-ERR no such message");
			} else {
				citadel::Message msg;
				if (!citadel::LoadMessage(con, drop[idx - 1].msgnum, msg)) {
					stream.WriteLine("-ERR message not found");
				} else {
					stream.WriteLine("+OK message follows");
					SendDotStuffed(stream, msg.raw);
				}
			}
		} else if (verb == "DELE") {
			size_t idx = (size_t)std::atoi(rest.c_str());
			if (idx < 1 || idx > drop.size()) {
				stream.WriteLine("-ERR no such message");
			} else {
				drop[idx - 1].deleted = true;
				stream.WriteLine("+OK marked for deletion");
			}
		} else if (verb == "RSET") {
			for (auto &e : drop) {
				e.deleted = false;
			}
			stream.WriteLine("+OK");
		} else if (verb == "NOOP") {
			stream.WriteLine("+OK");
		} else {
			stream.WriteLine("-ERR unsupported command");
		}
	}
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "qm_pop3", 1110, g_pop3, HandlePop3);
}

} // namespace

void QuackmailPop3Extension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailPop3Extension::Name() {
	return "quackmail_pop3";
}
std::string QuackmailPop3Extension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_POP3
	return EXT_VERSION_QUACKMAIL_POP3;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_pop3, loader) {
	duckdb::LoadInternal(loader);
}
}
