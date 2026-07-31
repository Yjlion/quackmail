#include "quackmail/vnote.hpp"

#include "quackmail/contentline.hpp"
#include "quackmail/util.hpp"

#include <cctype>

namespace quackmail {
namespace vnote {

Note::Note() {
}

namespace {

namespace cl = contentline;

bool IEq(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); i++) {
		if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) {
			return false;
		}
	}
	return true;
}

std::string Trim(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return std::string();
	}
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

// Fill the projected fields from the property list, so a caller can read
// note.summary without walking it.
void Project(Note &n) {
	n.uid.clear();
	n.summary.clear();
	n.body.clear();
	n.color.clear();
	for (auto &kv : n.props) {
		if (kv.first == "UID") {
			n.uid = Trim(kv.second);
		} else if (kv.first == "SUMMARY") {
			n.summary = kv.second;
		} else if (kv.first == "BODY") {
			n.body = kv.second;
		} else if (kv.first == "X-OUTLOOK-COLOR") {
			n.color = Trim(kv.second);
		}
	}
}

// Write a projected field back into the property list, keeping its position.
void Put(Note &n, const std::string &name, const std::string &value) {
	for (auto &kv : n.props) {
		if (kv.first == name) {
			kv.second = value;
			return;
		}
	}
	n.props.emplace_back(name, value);
}

} // namespace

bool Parse(const std::string &text, std::vector<Note> &out) {
	out.clear();
	bool in_note = false;
	Note cur;

	for (auto &raw : cl::Unfold(text)) {
		std::string line = Trim(raw);
		if (line.empty()) {
			continue;
		}
		if (IEq(line, "BEGIN:VNOTE")) {
			in_note = true;
			cur = Note();
			continue;
		}
		if (IEq(line, "END:VNOTE")) {
			if (in_note) {
				Project(cur);
				out.push_back(cur);
			}
			in_note = false;
			continue;
		}
		if (!in_note) {
			continue;
		}
		// No groups: vNote has no `item1.` construct, and a property containing
		// a dot should not be mistaken for one.
		cl::Line parsed;
		if (!cl::Parse(line, parsed, false)) {
			continue;
		}
		if (parsed.name == "VERSION") {
			// Re-emitted as 1.1; keeping it would let the two disagree.
			continue;
		}
		// Every vNote value is plain text — there are no structured properties —
		// so the whole value unescapes and its semicolons are content.
		cur.props.emplace_back(parsed.name, cl::Unescape(parsed.value));
	}

	return !out.empty();
}

bool ParseOne(const std::string &text, Note &out) {
	std::vector<Note> notes;
	if (!Parse(text, notes) || notes.empty()) {
		return false;
	}
	out = notes[0];
	return true;
}

std::string Emit(const Note &note) {
	// Work on a copy so the projected fields win over whatever the property list
	// happens to hold — a caller that set note.summary directly expects that to
	// be what is written.
	Note n = note;
	if (!n.uid.empty()) {
		Put(n, "UID", n.uid);
	}
	Put(n, "SUMMARY", n.summary);
	Put(n, "BODY", n.body);
	if (!n.color.empty()) {
		Put(n, "X-OUTLOOK-COLOR", n.color);
	}

	std::string out;
	cl::AppendFolded(out, "BEGIN:VNOTE");
	cl::AppendFolded(out, "VERSION:1.1");
	for (auto &kv : n.props) {
		if (kv.first == "VERSION" || kv.first == "BEGIN" || kv.first == "END") {
			continue;
		}
		cl::Line line;
		line.name = kv.first;
		line.value = cl::Escape(kv.second);
		cl::AppendFolded(out, cl::Format(line));
	}
	cl::AppendFolded(out, "END:VNOTE");
	return out;
}

std::string TitleOf(const Note &note) {
	std::string s = Trim(note.summary);
	if (!s.empty()) {
		return s;
	}
	// Fall back to the body's first line, which is what a note without an
	// explicit title actually reads as.
	std::string body = Trim(note.body);
	size_t nl = body.find('\n');
	std::string first = Trim(nl == std::string::npos ? body : body.substr(0, nl));
	if (!first.empty()) {
		return first.size() > 60 ? first.substr(0, 57) + "..." : first;
	}
	return "(untitled)";
}

std::string EuidFor(const Note &note) {
	if (!note.uid.empty()) {
		return note.uid;
	}
	return "qc-" + util::Sha256Hex(note.summary + "\x1f" + note.body).substr(0, 32);
}

std::string NewUid(const std::string &fqdn) {
	return util::RandomHex(16) + "@" + (fqdn.empty() ? "localhost" : fqdn);
}

} // namespace vnote
} // namespace quackmail
