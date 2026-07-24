#define DUCKDB_EXTENSION_MAIN

#include "quackmail_managesieve_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include "quackmail/mail_store.hpp"
#include "quackmail/sasl.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/sieve.hpp"
#include "quackmail/util.hpp"

#include <cstdlib>
#include <string>
#include <vector>

namespace duckdb {
namespace {

using namespace quackmail;

ServerController g_managesieve;

constexpr size_t kMaxScriptBytes = 1 * 1024 * 1024;

// ---------------------------------------------------------------------------
// Wire syntax (RFC 5804 §1.2 — the same quoting/literal rules as IMAP)
// ---------------------------------------------------------------------------

std::string Quote(const std::string &in) {
	std::string out = "\"";
	for (char c : in) {
		if (c == '"' || c == '\\') {
			out += '\\';
		}
		out += c;
	}
	out += '"';
	return out;
}

// A literal is written as {N+} followed by CRLF and N raw bytes. Scripts are
// sent this way because they contain newlines and quotes.
std::string Literal(const std::string &in) {
	return "{" + std::to_string(in.size()) + "+}\r\n" + in;
}

// Read one argument from `rest`, consuming it. Handles quoted strings and
// literals; a literal pulls its payload straight off the stream, which is why
// this needs the stream rather than working on the line alone.
bool ReadArg(net::ClientStream &stream, std::string &rest, std::string &out, std::string &err) {
	out.clear();
	size_t i = 0;
	while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) {
		i++;
	}
	if (i >= rest.size()) {
		err = "missing argument";
		return false;
	}

	if (rest[i] == '"') {
		i++;
		while (i < rest.size() && rest[i] != '"') {
			if (rest[i] == '\\' && i + 1 < rest.size()) {
				i++;
			}
			out += rest[i++];
		}
		if (i >= rest.size()) {
			err = "unterminated quoted string";
			return false;
		}
		i++; // closing quote
		rest = rest.substr(i);
		return true;
	}

	if (rest[i] == '{') {
		auto close = rest.find('}', i);
		if (close == std::string::npos) {
			err = "malformed literal";
			return false;
		}
		std::string digits = rest.substr(i + 1, close - i - 1);
		if (!digits.empty() && digits.back() == '+') {
			digits.pop_back(); // non-synchronizing literal
		}
		size_t want = (size_t)std::atoll(digits.c_str());
		if (want > kMaxScriptBytes) {
			err = "literal too large";
			return false;
		}
		// The literal payload starts on the next line and is exactly `want`
		// bytes, newlines included, so it cannot be read with ReadLine.
		std::string payload;
		while (payload.size() < want) {
			std::string chunk;
			if (!stream.ReadAvailable(chunk, want - payload.size())) {
				err = "connection closed inside a literal";
				return false;
			}
			payload += chunk;
		}
		out = payload;
		// The payload is followed by the rest of the command line (usually just
		// its CRLF). Consume it, and hand any further arguments back through
		// `rest` — leaving the CRLF unread would desync the next command.
		std::string tail;
		if (!stream.ReadLine(tail, 8192)) {
			tail.clear();
		}
		rest = tail;
		return true;
	}

	// A bare atom.
	size_t start = i;
	while (i < rest.size() && rest[i] != ' ' && rest[i] != '\t') {
		i++;
	}
	out = rest.substr(start, i - start);
	rest = rest.substr(i);
	return true;
}

// ---------------------------------------------------------------------------
// Script storage (quackmail_sieve_scripts)
// ---------------------------------------------------------------------------

duckdb::unique_ptr<QueryResult> ExecP(Connection &con, const std::string &sql, vector<Value> params) {
	auto stmt = con.Prepare(sql);
	if (stmt->HasError()) {
		return nullptr;
	}
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		return nullptr;
	}
	return r;
}

bool ScriptExists(Connection &con, const std::string &user, const std::string &name) {
	auto r = ExecP(con, "SELECT 1 FROM quackmail_sieve_scripts WHERE username = $1 AND name = $2",
	               {Value(user), Value(name)});
	return r && r->Cast<MaterializedQueryResult>().RowCount() > 0;
}

bool LoadScript(Connection &con, const std::string &user, const std::string &name, std::string &out) {
	auto r = ExecP(con, "SELECT script FROM quackmail_sieve_scripts WHERE username = $1 AND name = $2",
	               {Value(user), Value(name)});
	if (!r) {
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return false;
	}
	Value v = mat.GetValue(0, 0);
	out = v.IsNull() ? "" : v.ToString();
	return true;
}

void StoreScript(Connection &con, const std::string &user, const std::string &name,
                 const std::string &script) {
	// No unique constraint on the table, so replace explicitly.
	ExecP(con, "DELETE FROM quackmail_sieve_scripts WHERE username = $1 AND name = $2",
	      {Value(user), Value(name)});
	ExecP(con,
	      "INSERT INTO quackmail_sieve_scripts (username, name, active, script) "
	      "VALUES ($1, $2, false, $3)",
	      {Value(user), Value(name), Value(script)});
}

// Exactly one script may be active per user, so activation is a single update.
void SetActive(Connection &con, const std::string &user, const std::string &name) {
	ExecP(con, "UPDATE quackmail_sieve_scripts SET active = false WHERE username = $1",
	      {Value(user)});
	if (!name.empty()) {
		ExecP(con, "UPDATE quackmail_sieve_scripts SET active = true WHERE username = $1 AND name = $2",
		      {Value(user), Value(name)});
	}
}

// ---------------------------------------------------------------------------
// Protocol
// ---------------------------------------------------------------------------

void WriteCapabilities(net::ClientStream &stream, bool tls_active, bool authed) {
	stream.WriteLine("\"IMPLEMENTATION\" \"QuackCit ManageSieve\"");
	stream.WriteLine("\"VERSION\" \"1.0\"");
	stream.WriteLine("\"SIEVE\" " + Quote(sieve::Capabilities()));
	if (!tls_active && g_managesieve.StartTlsEnabled()) {
		stream.WriteLine("\"STARTTLS\"");
	}
	// RFC 5804 §2.1: credentials must not cross the wire in the clear, so the
	// mechanisms are only advertised once the channel is encrypted.
	if (!authed) {
		stream.WriteLine(tls_active ? "\"SASL\" \"PLAIN LOGIN\"" : "\"SASL\" \"\"");
	}
	stream.WriteLine("\"OWNER\" \"\"");
}

void HandleManageSieve(DatabaseInstance &db, net::ClientStream &stream) {
	Connection con(db);
	store::EnsureSchema(con);

	bool tls_active = stream.IsTls();
	bool authed = false;
	std::string user;

	WriteCapabilities(stream, tls_active, authed);
	stream.WriteLine("OK \"ManageSieve ready\"");

	std::string line;
	while (stream.ReadLine(line, 8192)) {
		std::string rest;
		std::string verb;
		{
			size_t sp = line.find(' ');
			verb = util::Upper(sp == std::string::npos ? line : line.substr(0, sp));
			rest = sp == std::string::npos ? "" : line.substr(sp + 1);
		}

		if (verb == "LOGOUT") {
			stream.WriteLine("OK \"Bye\"");
			return;
		}
		if (verb == "NOOP") {
			stream.WriteLine("OK \"NOOP completed\"");
			continue;
		}
		if (verb == "CAPABILITY") {
			WriteCapabilities(stream, tls_active, authed);
			stream.WriteLine("OK \"Capability completed\"");
			continue;
		}
		if (verb == "STARTTLS") {
			if (tls_active) {
				stream.WriteLine("NO \"TLS already active\"");
				continue;
			}
			if (!g_managesieve.StartTlsEnabled()) {
				stream.WriteLine("NO \"STARTTLS not available\"");
				continue;
			}
			stream.WriteLine("OK \"Begin TLS negotiation now\"");
			std::string terr;
			if (!stream.StartTls(g_managesieve.TlsCtx(), terr)) {
				return;
			}
			tls_active = true;
			// §2.2: the session resets, so the capabilities are re-sent and any
			// prior authentication is discarded.
			authed = false;
			user.clear();
			WriteCapabilities(stream, tls_active, authed);
			stream.WriteLine("OK \"TLS negotiation successful\"");
			continue;
		}
		if (verb == "AUTHENTICATE") {
			if (authed) {
				stream.WriteLine("NO \"Already authenticated\"");
				continue;
			}
			if (!tls_active) {
				stream.WriteLine("NO \"Encryption required before authentication\"");
				continue;
			}
			std::string mech, initial, err;
			if (!ReadArg(stream, rest, mech, err)) {
				stream.WriteLine("NO \"" + err + "\"");
				continue;
			}
			// The optional initial response is a second argument.
			if (!rest.empty()) {
				ReadArg(stream, rest, initial, err);
			}

			// ManageSieve frames challenges as a literal, and the client's
			// response comes back as a literal or quoted string.
			auto challenge = [&](const std::string &c, std::string &resp) -> bool {
				if (!stream.WriteLine(Literal(c))) {
					return false;
				}
				std::string reply;
				if (!stream.ReadLine(reply, 8192)) {
					return false;
				}
				std::string arg, e;
				if (!ReadArg(stream, reply, arg, e)) {
					return false;
				}
				resp = arg;
				return true;
			};

			auto r = sasl::ServerAuth(con, util::Upper(mech), initial, challenge, user);
			if (r == sasl::Result::Ok) {
				authed = true;
				stream.WriteLine("OK \"Logged in\"");
			} else if (r == sasl::Result::Unsupported) {
				stream.WriteLine("NO \"Unsupported authentication mechanism\"");
			} else {
				user.clear();
				stream.WriteLine("NO \"Authentication failed\"");
			}
			continue;
		}
		if (verb == "UNAUTHENTICATE") {
			if (!authed) {
				stream.WriteLine("NO \"Not authenticated\"");
				continue;
			}
			authed = false;
			user.clear();
			stream.WriteLine("OK \"Unauthenticated\"");
			continue;
		}

		// Everything past this point needs a logged-in user.
		if (!authed) {
			stream.WriteLine("NO \"Authenticate first\"");
			continue;
		}

		if (verb == "LISTSCRIPTS") {
			auto r = ExecP(con,
			               "SELECT name, active FROM quackmail_sieve_scripts "
			               "WHERE username = $1 ORDER BY name",
			               {Value(user)});
			if (r) {
				auto &mat = r->Cast<MaterializedQueryResult>();
				for (idx_t i = 0; i < mat.RowCount(); i++) {
					Value nv = mat.GetValue(0, i);
					Value av = mat.GetValue(1, i);
					std::string name = nv.IsNull() ? "" : nv.ToString();
					bool active = !av.IsNull() && av.GetValue<bool>();
					stream.WriteLine(Quote(name) + (active ? " ACTIVE" : ""));
				}
			}
			stream.WriteLine("OK \"Listscripts completed\"");
			continue;
		}

		if (verb == "HAVESPACE") {
			std::string name, size_text, err;
			if (!ReadArg(stream, rest, name, err) || !ReadArg(stream, rest, size_text, err)) {
				stream.WriteLine("NO \"" + err + "\"");
				continue;
			}
			if ((size_t)std::atoll(size_text.c_str()) > kMaxScriptBytes) {
				stream.WriteLine("NO (QUOTA/MAXSIZE) \"Script too large\"");
			} else {
				stream.WriteLine("OK \"Have space\"");
			}
			continue;
		}

		if (verb == "PUTSCRIPT" || verb == "CHECKSCRIPT") {
			std::string name, script, err;
			// CHECKSCRIPT takes only the script; PUTSCRIPT takes name + script.
			if (verb == "PUTSCRIPT") {
				if (!ReadArg(stream, rest, name, err)) {
					stream.WriteLine("NO \"" + err + "\"");
					continue;
				}
			}
			if (!ReadArg(stream, rest, script, err)) {
				stream.WriteLine("NO \"" + err + "\"");
				continue;
			}
			if (verb == "PUTSCRIPT" && name.empty()) {
				stream.WriteLine("NO \"Script name must not be empty\"");
				continue;
			}
			std::string parse_err;
			if (!sieve::Check(script, parse_err)) {
				// The structured error form lets a client point at the problem.
				stream.WriteLine("NO " + Literal(parse_err));
				continue;
			}
			if (verb == "CHECKSCRIPT") {
				stream.WriteLine("OK \"Script is valid\"");
				continue;
			}
			if (script.size() > kMaxScriptBytes) {
				stream.WriteLine("NO (QUOTA/MAXSIZE) \"Script too large\"");
				continue;
			}
			StoreScript(con, user, name, script);
			stream.WriteLine("OK \"Putscript completed\"");
			continue;
		}

		if (verb == "GETSCRIPT") {
			std::string name, err;
			if (!ReadArg(stream, rest, name, err)) {
				stream.WriteLine("NO \"" + err + "\"");
				continue;
			}
			std::string script;
			if (!LoadScript(con, user, name, script)) {
				stream.WriteLine("NO (NONEXISTENT) \"No such script\"");
				continue;
			}
			stream.WriteLine(Literal(script));
			stream.WriteLine("OK \"Getscript completed\"");
			continue;
		}

		if (verb == "SETACTIVE") {
			std::string name, err;
			if (!ReadArg(stream, rest, name, err)) {
				stream.WriteLine("NO \"" + err + "\"");
				continue;
			}
			// An empty name deactivates whatever is active (§2.8).
			if (!name.empty() && !ScriptExists(con, user, name)) {
				stream.WriteLine("NO (NONEXISTENT) \"No such script\"");
				continue;
			}
			SetActive(con, user, name);
			stream.WriteLine("OK \"Setactive completed\"");
			continue;
		}

		if (verb == "DELETESCRIPT") {
			std::string name, err;
			if (!ReadArg(stream, rest, name, err)) {
				stream.WriteLine("NO \"" + err + "\"");
				continue;
			}
			if (!ScriptExists(con, user, name)) {
				stream.WriteLine("NO (NONEXISTENT) \"No such script\"");
				continue;
			}
			// §2.10: the active script cannot be deleted out from under delivery.
			auto r = ExecP(con,
			               "SELECT 1 FROM quackmail_sieve_scripts "
			               "WHERE username = $1 AND name = $2 AND active",
			               {Value(user), Value(name)});
			if (r && r->Cast<MaterializedQueryResult>().RowCount() > 0) {
				stream.WriteLine("NO (ACTIVE) \"Cannot delete the active script\"");
				continue;
			}
			ExecP(con, "DELETE FROM quackmail_sieve_scripts WHERE username = $1 AND name = $2",
			      {Value(user), Value(name)});
			stream.WriteLine("OK \"Deletescript completed\"");
			continue;
		}

		if (verb == "RENAMESCRIPT") {
			std::string from, to, err;
			if (!ReadArg(stream, rest, from, err) || !ReadArg(stream, rest, to, err)) {
				stream.WriteLine("NO \"" + err + "\"");
				continue;
			}
			if (!ScriptExists(con, user, from)) {
				stream.WriteLine("NO (NONEXISTENT) \"No such script\"");
				continue;
			}
			if (ScriptExists(con, user, to)) {
				stream.WriteLine("NO (ALREADYEXISTS) \"A script by that name already exists\"");
				continue;
			}
			ExecP(con, "UPDATE quackmail_sieve_scripts SET name = $3 WHERE username = $1 AND name = $2",
			      {Value(user), Value(from), Value(to)});
			stream.WriteLine("OK \"Renamescript completed\"");
			continue;
		}

		stream.WriteLine("NO \"Unknown command\"");
	}
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "qm_managesieve", 4190, g_managesieve, HandleManageSieve);
}

} // namespace

void QuackmailManagesieveExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailManagesieveExtension::Name() {
	return "quackmail_managesieve";
}
std::string QuackmailManagesieveExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_MANAGESIEVE
	return EXT_VERSION_QUACKMAIL_MANAGESIEVE;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_managesieve, loader) {
	duckdb::LoadInternal(loader);
}
}
