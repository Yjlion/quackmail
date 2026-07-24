#define DUCKDB_EXTENSION_MAIN

#include "quackmail_pop3_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/citadel_msg.hpp"
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

// Two listeners over one implementation, mirroring Citadel's pop3/pop3s pair:
// STARTTLS-capable plaintext (110; dev 1110) and implicit TLS (995; dev 1995).
ServerController g_pop3;
ServerController g_pop3s;

// One maildrop entry (POP3 message index -> Citadel message number). `size` is
// the RFC822 length, which is what a POP3 client expects in LIST/STAT — for
// native (format 0) messages that is the rendered form, not the stored body.
struct Entry {
	int64_t msgnum = 0;
	int64_t size = 0;
	bool deleted = false;
};

// The Mail room maildrop, oldest first, plus the index of the last message the
// user has already seen (Citadel's POP3 "LAST" pointer).
struct Maildrop {
	std::vector<Entry> msgs;
	int64_t last = -1; // 0-based index; LAST reports last + 1
};

Maildrop LoadMaildrop(Connection &con, const std::string &username, const std::string &node) {
	Maildrop drop;
	int64_t room = citadel::GetOrCreateMailRoom(con, username);
	if (room < 0) {
		return drop;
	}
	auto stats = citadel::GetRoomStats(con, username, room);
	for (int64_t msgnum : citadel::RoomMessages(con, room, "all", 0, stats.last_read)) {
		citadel::Message msg;
		if (!citadel::LoadMessage(con, msgnum, msg)) {
			continue;
		}
		Entry e;
		e.msgnum = msgnum;
		e.size = (int64_t)citadel::RenderRfc822(msg, node).size();
		drop.msgs.push_back(e);
		if (msgnum <= stats.last_read) {
			drop.last = (int64_t)drop.msgs.size() - 1;
		}
	}
	return drop;
}

// Send text dot-stuffed, terminated by a "." line (RFC 1939). `max_body_lines`
// < 0 sends everything; otherwise the header block is sent in full and the body
// is truncated to that many lines (the TOP command).
void SendDotStuffed(net::ClientStream &stream, const std::string &raw, int64_t max_body_lines) {
	bool in_body = false;
	int64_t body_lines = 0;
	std::string line;
	auto flush = [&]() -> bool {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (in_body && max_body_lines >= 0 && body_lines >= max_body_lines) {
			return false;
		}
		std::string out = line;
		if (!out.empty() && out[0] == '.') {
			out = "." + out;
		}
		stream.WriteLine(out);
		if (in_body) {
			body_lines++;
		} else if (line.empty()) {
			in_body = true; // the blank line ends the header block
		}
		line.clear();
		return true;
	};
	for (char c : raw) {
		if (c == '\n') {
			if (!flush()) {
				break;
			}
		} else {
			line.push_back(c);
		}
	}
	if (!line.empty()) {
		flush();
	}
	stream.WriteLine(".");
}

// Apply the POP3 UPDATE state on QUIT: expunge messages flagged for deletion and
// advance the last-read pointer over the whole maildrop.
void ApplyUpdate(Connection &con, const std::string &user, const Maildrop &drop) {
	int64_t room = citadel::GetOrCreateMailRoom(con, user);
	if (room < 0) {
		return;
	}
	auto del_ptr = con.Prepare("DELETE FROM citadel_room_msgs WHERE room_num = $1 AND msgnum = $2");
	auto del_flags = con.Prepare("DELETE FROM citadel_msg_flags WHERE msgnum = $1 AND lower(username) = lower($2)");
	for (auto &e : drop.msgs) {
		if (!e.deleted) {
			continue;
		}
		if (!del_ptr->HasError()) {
			duckdb::vector<Value> params = {Value::BIGINT(room), Value::BIGINT(e.msgnum)};
			del_ptr->Execute(params, false);
		}
		if (!del_flags->HasError()) {
			duckdb::vector<Value> params = {Value::BIGINT(e.msgnum), Value(user)};
			del_flags->Execute(params, false);
		}
	}
	if (!drop.msgs.empty()) {
		citadel::SetLastRead(con, user, room, drop.msgs.back().msgnum);
	}
}

void HandlePop3(DatabaseInstance &db, net::ClientStream &stream, ServerController &ctrl) {
	Connection con(db);
	store::EnsureSchema(con);

	std::string node = citadel::GetConfig(con, "c_nodename", "quackcit");
	std::string version = citadel::GetConfig(con, "c_version", "QuackCit");

	std::string pending_user;
	std::string user;
	bool authed = false;
	Maildrop drop;

	// Same shape as Citadel's greeting ("+OK Citadel POP3 server ready.").
	stream.WriteLine("+OK QuackCit POP3 server ready.");

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

		// Resolve a 1-based message argument, replying with Citadel's exact
		// error strings. Returns nullptr when the argument is unusable.
		auto pick = [&](const std::string &arg) -> Entry * {
			int64_t idx = std::atoll(arg.c_str());
			if (idx < 1 || idx > (int64_t)drop.msgs.size()) {
				stream.WriteLine("-ERR No such message.");
				return nullptr;
			}
			Entry &e = drop.msgs[idx - 1];
			if (e.deleted) {
				stream.WriteLine("-ERR Sorry, you deleted that message.");
				return nullptr;
			}
			return &e;
		};

		if (verb == "NOOP") {
			stream.WriteLine("+OK No operation.");
		} else if (verb == "CAPA") {
			// Citadel advertises exactly these, even on the implicit-TLS port.
			stream.WriteLine("+OK Capability list follows");
			stream.WriteLine("TOP");
			stream.WriteLine("USER");
			stream.WriteLine("UIDL");
			stream.WriteLine("IMPLEMENTATION " + version);
			stream.WriteLine(".");
		} else if (verb == "QUIT") {
			stream.WriteLine("+OK Goodbye...");
			if (authed) {
				ApplyUpdate(con, user, drop);
			}
			return;
		} else if (verb == "USER") {
			if (authed) {
				stream.WriteLine("-ERR You are already logged in.");
			} else if (citadel::GetOrAssignUserNum(con, rest) <= 0) {
				stream.WriteLine("-ERR No such user.");
			} else {
				pending_user = rest;
				stream.WriteLine("+OK Password required for " + rest);
			}
		} else if (verb == "PASS") {
			if (pending_user.empty()) {
				stream.WriteLine("-ERR That is NOT the password.");
			} else if (!auth::Verify(con, pending_user, rest)) {
				stream.WriteLine("-ERR That is NOT the password.");
			} else {
				user = pending_user;
				authed = true;
				citadel::EnsureUserRooms(con, user);
				drop = LoadMaildrop(con, user, node);
				stream.WriteLine("+OK " + user + " is logged in (" +
				                 std::to_string(drop.msgs.size()) + " messages)");
			}
		} else if (verb == "STLS") {
			if (stream.IsTls()) {
				stream.WriteLine("-ERR TLS not supported here");
			} else if (!ctrl.StartTlsEnabled()) {
				stream.WriteLine("-ERR TLS not supported here");
			} else {
				stream.WriteLine("+OK Begin TLS negotiation now");
				std::string terr;
				if (!stream.StartTls(ctrl.TlsCtx(), terr)) {
					return;
				}
			}
		} else if (!authed) {
			stream.WriteLine("-ERR Not logged in.");
		} else if (verb == "STAT") {
			int64_t count = 0, total = 0;
			for (auto &e : drop.msgs) {
				if (!e.deleted) {
					count++;
					total += e.size;
				}
			}
			stream.WriteLine("+OK " + std::to_string(count) + " " + std::to_string(total));
		} else if (verb == "LIST" || verb == "UIDL") {
			bool uidl = verb == "UIDL";
			int64_t which = std::atoll(rest.c_str());
			if (which > 0) {
				// "list one" mode: Citadel reports the maildrop size here.
				if (which > (int64_t)drop.msgs.size()) {
					stream.WriteLine("-ERR no such message, only " +
					                 std::to_string(drop.msgs.size()) + " are here");
				} else if (drop.msgs[which - 1].deleted) {
					stream.WriteLine("-ERR Sorry, you deleted that message.");
				} else {
					auto &e = drop.msgs[which - 1];
					stream.WriteLine("+OK " + std::to_string(which) + " " +
					                 std::to_string(uidl ? e.msgnum : e.size));
				}
			} else {
				stream.WriteLine("+OK Here's your mail:");
				for (size_t i = 0; i < drop.msgs.size(); i++) {
					if (drop.msgs[i].deleted) {
						continue;
					}
					stream.WriteLine(std::to_string(i + 1) + " " +
					                 std::to_string(uidl ? drop.msgs[i].msgnum : drop.msgs[i].size));
				}
				stream.WriteLine(".");
			}
		} else if (verb == "RETR" || verb == "TOP") {
			// TOP takes "<which> <lines>"; RETR sends the whole message.
			int64_t lines = -1;
			std::string which_arg = rest;
			if (verb == "TOP") {
				size_t gap = rest.find(' ');
				which_arg = rest.substr(0, gap);
				lines = gap == std::string::npos ? 0 : std::atoll(rest.c_str() + gap + 1);
			}
			if (Entry *e = pick(which_arg)) {
				citadel::Message msg;
				if (!citadel::LoadMessage(con, e->msgnum, msg)) {
					stream.WriteLine("-ERR No such message.");
				} else {
					int64_t which = std::atoll(which_arg.c_str());
					stream.WriteLine("+OK Message " + std::to_string(which) + ":");
					SendDotStuffed(stream, citadel::RenderRfc822(msg, node), lines);
				}
			}
		} else if (verb == "DELE") {
			int64_t idx = std::atoll(rest.c_str());
			if (idx < 1 || idx > (int64_t)drop.msgs.size()) {
				stream.WriteLine("-ERR No such message.");
			} else if (drop.msgs[idx - 1].deleted) {
				stream.WriteLine("-ERR You already deleted that message.");
			} else {
				drop.msgs[idx - 1].deleted = true;
				stream.WriteLine("+OK Message " + std::to_string(idx) + " deleted.");
			}
		} else if (verb == "RSET") {
			for (auto &e : drop.msgs) {
				e.deleted = false;
			}
			stream.WriteLine("+OK Reset completed.");
		} else if (verb == "LAST") {
			stream.WriteLine("+OK " + std::to_string(drop.last + 1));
		} else {
			stream.WriteLine("-ERR I'm afraid I can't do that.");
		}
	}
}

// Thin per-listener wrappers: the handler needs its controller for STARTTLS.
void HandlePop3Conn(DatabaseInstance &db, net::ClientStream &stream) {
	HandlePop3(db, stream, g_pop3);
}
void HandlePop3sConn(DatabaseInstance &db, net::ClientStream &stream) {
	HandlePop3(db, stream, g_pop3s);
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "qm_pop3", 1110, g_pop3, HandlePop3Conn);
	RegisterServerControls(loader, "qm_pop3s", 1995, g_pop3s, HandlePop3sConn);
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
