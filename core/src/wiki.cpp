#include "quackmail/wiki.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/diff.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/util.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace quackmail {
namespace wiki {

namespace {

const char *kHistorySuffix = "_HISTORY_";
const size_t kHistorySuffixLen = 9;

// The preamble Citadel writes, reproduced so a history message we create for
// the first time is indistinguishable from one it created.
const char *kPreamble =
    "This is a Citadel wiki history encoded as multipart MIME.\n"
    "Each part is comprised of a diff script representing one change set.\n";

// How many revisions deep a reconstruction will go. A history is a chain of
// patches over text a user wrote, walked on every "show me this revision"
// request; without a bound, a page edited fifty thousand times turns a page view
// into a fifty-thousand-patch replay.
constexpr size_t kMaxRevisions = 2000;

std::string LowerAscii(const std::string &s) {
	std::string out = s;
	for (char &c : out) {
		c = (char)tolower((unsigned char)c);
	}
	return out;
}

bool EqualsIgnoreCase(const std::string &a, const std::string &b) {
	return a.size() == b.size() && LowerAscii(a) == LowerAscii(b);
}

// Split "old_msgnum|timestamp|author|" (with the trailing NUL Citadel encodes).
bool ParseMemo(const std::string &decoded, Revision &out) {
	std::string memo = decoded;
	while (!memo.empty() && memo.back() == '\0') {
		memo.pop_back();
	}
	size_t p1 = memo.find('|');
	if (p1 == std::string::npos) {
		return false;
	}
	size_t p2 = memo.find('|', p1 + 1);
	std::string rev = memo.substr(0, p1);
	if (rev.empty() || rev.find_first_not_of("0123456789") != std::string::npos) {
		return false;
	}
	out.rev = std::strtoll(rev.c_str(), nullptr, 10);
	if (p2 == std::string::npos) {
		out.timestamp = 0;
		out.author.clear();
		return true;
	}
	std::string ts = memo.substr(p1 + 1, p2 - p1 - 1);
	out.timestamp = ts.find_first_not_of("0123456789") == std::string::npos && !ts.empty()
	                    ? std::strtoll(ts.c_str(), nullptr, 10)
	                    : 0;
	size_t p3 = memo.find('|', p2 + 1);
	out.author = memo.substr(p2 + 1, p3 == std::string::npos ? std::string::npos : p3 - p2 - 1);
	return true;
}

std::string EncodeMemo(int64_t rev, int64_t timestamp, const std::string &author) {
	std::string memo = std::to_string(rev) + "|" + std::to_string(timestamp) + "|" + author + "|";
	memo.push_back('\0'); // Citadel encodes strlen()+1 bytes; so do we
	return util::Base64Encode(memo);
}

// Find the boundary Citadel used, so appending to an existing history keeps it
// rather than restructuring the message.
std::string BoundaryOf(const std::string &raw) {
	size_t body = raw.find("\n\n");
	size_t limit = body == std::string::npos ? raw.size() : body;
	std::string head = LowerAscii(raw.substr(0, limit));
	size_t ct = head.find("content-type:");
	if (ct == std::string::npos) {
		return "";
	}
	size_t b = head.find("boundary", ct);
	if (b == std::string::npos) {
		return "";
	}
	size_t eq = head.find('=', b);
	if (eq == std::string::npos) {
		return "";
	}
	size_t start = eq + 1;
	while (start < limit && (raw[start] == ' ' || raw[start] == '"')) {
		start++;
	}
	size_t end = start;
	while (end < limit && raw[end] != '"' && raw[end] != ';' && raw[end] != '\n' &&
	       raw[end] != '\r') {
		end++;
	}
	return raw.substr(start, end - start);
}

std::string NewBoundary() {
	return "Citadel--Multipart--" + util::RandomHex(2) + "--" + util::RandomHex(4);
}

int64_t NowSeconds() {
	return (int64_t)std::time(nullptr);
}

} // namespace

std::string NormalizeName(const std::string &name) {
	std::string out;
	out.reserve(name.size());
	for (unsigned char c : name) {
		out.push_back((c < 0x20 || c > 0x7F) ? '_' : (char)tolower(c));
	}
	// Leading and trailing whitespace is never what someone meant to type, and
	// keeping it makes two pages that look identical be different pages.
	size_t a = out.find_first_not_of(' ');
	size_t b = out.find_last_not_of(' ');
	out = (a == std::string::npos) ? std::string() : out.substr(a, b - a + 1);
	return out.empty() ? std::string("home") : out;
}

std::string HistoryEuid(const std::string &page_euid) {
	return page_euid + kHistorySuffix;
}

bool IsHistoryEuid(const std::string &euid) {
	return euid.size() >= kHistorySuffixLen &&
	       EqualsIgnoreCase(euid.substr(euid.size() - kHistorySuffixLen), kHistorySuffix);
}

bool IsWikiView(int64_t default_view) {
	return default_view == citadel::VIEW_WIKI || default_view == citadel::VIEW_WIKIMD;
}

std::vector<Revision> History(duckdb::Connection &con, int64_t room_num,
                              const std::string &page_euid) {
	std::vector<Revision> out;
	int64_t msgnum = citadel::FindByEuid(con, room_num, HistoryEuid(page_euid));
	if (msgnum <= 0) {
		return out;
	}
	citadel::Message msg;
	if (!citadel::LoadMessage(con, msgnum, msg) || msg.raw.empty()) {
		return out;
	}
	mime::MimeEntity root = mime::ParseEntity(msg.raw);
	for (const mime::MimeEntity &child : root.children) {
		if (out.size() >= kMaxRevisions) {
			break;
		}
		if (child.filename.empty()) {
			continue;
		}
		std::string decoded;
		if (!util::Base64Decode(child.filename, decoded)) {
			continue;
		}
		Revision rev;
		if (!ParseMemo(decoded, rev)) {
			continue;
		}
		rev.diff = child.body_decoded.empty() ? child.body_raw : child.body_decoded;
		out.push_back(rev);
	}
	return out;
}

bool RevisionRaw(duckdb::Connection &con, int64_t room_num, const std::string &page_euid,
                 int64_t rev, std::string &raw_out, std::string &err) {
	err.clear();
	int64_t current = citadel::FindByEuid(con, room_num, page_euid);
	if (current <= 0) {
		err = "There is no page by that name.";
		return false;
	}
	citadel::Message msg;
	if (!citadel::LoadMessage(con, current, msg)) {
		err = "The page could not be read.";
		return false;
	}
	raw_out = msg.raw;
	if (rev == 0 || rev == current) {
		return true;
	}

	// Each stored diff walks one step backwards. Applying them in order until
	// the memo names the wanted revision is exactly what Citadel's wiki_rev does.
	std::vector<Revision> revs = History(con, room_num, page_euid);
	for (const Revision &r : revs) {
		std::string patched;
		std::string patch_err;
		if (!diff::Apply(raw_out, r.diff, patched, patch_err)) {
			err = "Revision " + std::to_string(rev) + " could not be reconstructed: " + patch_err;
			return false;
		}
		raw_out = patched;
		if (r.rev == rev) {
			return true;
		}
	}
	err = "There is no revision " + std::to_string(rev) + " of that page.";
	return false;
}

RecordResult RecordRevision(duckdb::Connection &con, int64_t room_num,
                            const std::string &page_euid, const std::string &new_raw,
                            std::string &err) {
	err.clear();
	if (page_euid.empty() || IsHistoryEuid(page_euid)) {
		return RecordResult::NoPrevious;
	}
	int64_t old_msgnum = citadel::FindByEuid(con, room_num, page_euid);
	if (old_msgnum <= 0) {
		return RecordResult::NoPrevious;
	}
	citadel::Message old_msg;
	if (!citadel::LoadMessage(con, old_msgnum, old_msg)) {
		return RecordResult::NoPrevious;
	}
	if (old_msg.raw == new_raw) {
		return RecordResult::Unchanged;
	}

	// New to old: applying this diff walks the current text back one revision.
	// The direction is the whole design, and reversing it silently produces a
	// history that reconstructs forwards from nothing.
	std::string patch = diff::Unified(new_raw, old_msg.raw);
	if (patch.empty()) {
		return RecordResult::Unchanged;
	}

	const std::string hist_euid = HistoryEuid(page_euid);
	int64_t hist_msgnum = citadel::FindByEuid(con, room_num, hist_euid);
	std::string old_history;
	if (hist_msgnum > 0) {
		citadel::Message hist;
		if (citadel::LoadMessage(con, hist_msgnum, hist)) {
			old_history = hist.raw;
		}
	}

	std::string boundary = BoundaryOf(old_history);
	std::string tail;
	if (!boundary.empty()) {
		const std::string opener = "--" + boundary;
		const std::string closer = "--" + boundary + "--";
		size_t first = old_history.find(opener);
		size_t last = old_history.find(closer);
		if (first != std::string::npos && last != std::string::npos) {
			tail = old_history.substr(first);
		}
	}
	if (tail.empty()) {
		// No usable history: start one, whose tail is just the closing boundary.
		boundary = NewBoundary();
		tail = "--" + boundary + "--\n";
	}

	std::string body;
	body += "Content-type: multipart/mixed; boundary=\"" + boundary + "\"\n\n";
	body += kPreamble;
	body += "\n";
	body += "--" + boundary + "\n";
	body += "Content-type: text/x-diff\n";
	body += "Content-disposition: attachment; filename=\"" +
	        EncodeMemo(old_msgnum, old_msg.msgtime, old_msg.author) + "\"\n";
	body += "\n";
	body += patch;
	if (!patch.empty() && patch.back() != '\n') {
		body += "\n";
	}
	body += tail;

	citadel::Message hist;
	hist.euid = hist_euid;
	hist.subject = hist_euid;
	hist.author = "Citadel";
	hist.msgtime = NowSeconds();
	hist.format_type = 4;
	hist.raw = body;

	std::string upsert_err;
	if (citadel::UpsertByEuid(con, hist, room_num, upsert_err) < 0) {
		err = upsert_err.empty() ? "The page history could not be written." : upsert_err;
		return RecordResult::Error;
	}
	return RecordResult::Recorded;
}

std::vector<Page> ListPages(duckdb::Connection &con, int64_t room_num) {
	std::vector<Page> out;
	auto stmt = con.Prepare(
	    "SELECT m.euid, m.subject, m.msgnum, m.msgtime, m.author "
	    "  FROM citadel_room_msgs rm JOIN citadel_messages m ON m.msgnum = rm.msgnum "
	    " WHERE rm.room_num = $1 AND m.euid IS NOT NULL AND m.euid <> '' "
	    " ORDER BY lower(m.euid)");
	if (!stmt || stmt->HasError()) {
		return out;
	}
	// allow_stream_result = false: Cast<MaterializedQueryResult> on a streamed
	// result is an internal error rather than a miss.
	duckdb::vector<duckdb::Value> params {duckdb::Value::BIGINT(room_num)};
	auto res = stmt->Execute(params, false);
	if (!res || res->HasError()) {
		return out;
	}
	auto &mat = res->Cast<duckdb::MaterializedQueryResult>();
	for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
		Page p;
		p.euid = mat.GetValue(0, i).ToString();
		if (IsHistoryEuid(p.euid)) {
			continue;
		}
		p.title = mat.GetValue(1, i).IsNull() ? p.euid : mat.GetValue(1, i).ToString();
		if (p.title.empty()) {
			p.title = p.euid;
		}
		p.msgnum = mat.GetValue(2, i).IsNull() ? 0 : mat.GetValue(2, i).GetValue<int64_t>();
		p.msgtime = mat.GetValue(3, i).IsNull() ? 0 : mat.GetValue(3, i).GetValue<int64_t>();
		p.author = mat.GetValue(4, i).IsNull() ? "" : mat.GetValue(4, i).ToString();
		out.push_back(p);
	}
	return out;
}

bool DeletePage(duckdb::Connection &con, int64_t room_num, const std::string &page_euid,
                std::string &err) {
	err.clear();
	int64_t msgnum = citadel::FindByEuid(con, room_num, page_euid);
	if (msgnum <= 0) {
		err = "There is no page by that name.";
		return false;
	}
	if (!citadel::DeleteMessage(con, room_num, msgnum, err)) {
		return false;
	}
	// The companion goes with it: leaving it behind would resurrect the old
	// revisions under a page somebody creates with the same name later.
	int64_t hist = citadel::FindByEuid(con, room_num, HistoryEuid(page_euid));
	if (hist > 0) {
		std::string ignored;
		citadel::DeleteMessage(con, room_num, hist, ignored);
	}
	return true;
}

} // namespace wiki
} // namespace quackmail
