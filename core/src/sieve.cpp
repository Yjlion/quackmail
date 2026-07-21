#include "quackmail/sieve.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace quackmail {
namespace sieve {

using duckdb::Connection;
using duckdb::Value;

static std::string ToLower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
	return s;
}

static std::string HeaderValue(const mime::ParsedMessage &msg, const std::string &name) {
	std::string want = ToLower(name);
	for (auto &h : msg.headers) {
		if (ToLower(h.first) == want) {
			return h.second;
		}
	}
	return "";
}

// Parse the first decisive action out of a fragment of Sieve.
static bool ParseAction(const std::string &text, Action &out) {
	std::smatch m;
	std::regex fileinto_re("fileinto\\s+\"([^\"]*)\"");
	if (std::regex_search(text, m, fileinto_re)) {
		out.type = Action::FILEINTO;
		out.folder = m[1].str();
		return true;
	}
	if (std::regex_search(text, std::regex("\\bdiscard\\b"))) {
		out.type = Action::DISCARD;
		return true;
	}
	if (std::regex_search(text, std::regex("\\bkeep\\b"))) {
		out.type = Action::KEEP;
		return true;
	}
	return false;
}

Action Evaluate(const std::string &script_in, const mime::ParsedMessage &msg) {
	Action result; // default KEEP

	// Strip '#' line comments.
	std::string script = std::regex_replace(script_in, std::regex("#[^\n]*"), "");

	// Evaluate `if header :contains "Name" "Substr" { body }` guards in order.
	std::regex guard_re("if\\s+header\\s+:contains\\s+\"([^\"]*)\"\\s+\"([^\"]*)\"\\s*\\{([^}]*)\\}");
	auto begin = std::sregex_iterator(script.begin(), script.end(), guard_re);
	auto end = std::sregex_iterator();
	for (auto it = begin; it != end; ++it) {
		std::string name = (*it)[1].str();
		std::string needle = ToLower((*it)[2].str());
		std::string body = (*it)[3].str();
		std::string hv = ToLower(HeaderValue(msg, name));
		if (!needle.empty() && hv.find(needle) != std::string::npos) {
			Action a;
			if (ParseAction(body, a)) {
				return a;
			}
		}
	}

	// Fall through to any top-level action outside guard blocks.
	std::string remainder = std::regex_replace(script, guard_re, "");
	Action a;
	if (ParseAction(remainder, a)) {
		return a;
	}
	return result;
}

std::string LoadActiveScript(Connection &con, const std::string &username) {
	auto stmt = con.Prepare("SELECT script FROM quackmail_sieve_scripts "
	                        "WHERE username = $1 AND active = true LIMIT 1");
	if (stmt->HasError()) {
		return "";
	}
	duckdb::vector<Value> params = {Value(username)};
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		return "";
	}
	auto &mat = r->Cast<duckdb::MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return "";
	}
	Value v = mat.GetValue(0, 0);
	if (v.IsNull()) {
		return "";
	}
	return v.ToString();
}

} // namespace sieve
} // namespace quackmail
