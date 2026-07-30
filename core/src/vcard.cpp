#include "quackmail/vcard.hpp"

#include "quackmail/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace quackmail {
namespace vcard {

Property::Property() {
}
Card::Card() {
}

namespace {

std::string Upper(const std::string &s) {
	return util::Upper(s);
}

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

// Unfold: a line beginning with space or tab continues the previous one. Done
// before anything else, because a folded line can split a property name, a
// parameter, or the middle of a UTF-8 sequence.
std::vector<std::string> Unfold(const std::string &text) {
	std::vector<std::string> lines;
	std::string cur;
	size_t i = 0;
	bool have = false;
	while (i <= text.size()) {
		if (i == text.size() || text[i] == '\n') {
			std::string line = cur;
			// Tolerate CRLF, LF, and a stray CR at the end.
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && have) {
				lines.back() += line.substr(1);
			} else {
				lines.push_back(line);
				have = true;
			}
			cur.clear();
			if (i == text.size()) {
				break;
			}
		} else {
			cur += text[i];
		}
		i++;
	}
	return lines;
}

// Split a value on unescaped `sep`, resolving escapes as it goes.
std::vector<std::string> SplitEscaped(const std::string &in, char sep) {
	std::vector<std::string> out;
	std::string cur;
	for (size_t i = 0; i < in.size(); i++) {
		char c = in[i];
		if (c == '\\' && i + 1 < in.size()) {
			char n = in[i + 1];
			switch (n) {
			case 'n':
			case 'N':
				cur += '\n';
				break;
			case ',':
				cur += ',';
				break;
			case ';':
				cur += ';';
				break;
			case '\\':
				cur += '\\';
				break;
			default:
				// An unknown escape keeps both characters: guessing would lose
				// data, and vCard in the wild contains plenty of stray
				// backslashes that were never meant as escapes.
				cur += '\\';
				cur += n;
				break;
			}
			i++;
			continue;
		}
		if (c == sep) {
			out.push_back(cur);
			cur.clear();
			continue;
		}
		cur += c;
	}
	out.push_back(cur);
	return out;
}

// Resolve escapes without splitting on anything. A non-structured property's
// ';' is ordinary text, so it must survive.
std::string Unescape(const std::string &in) {
	// SplitEscaped on a separator that cannot occur returns exactly one element,
	// already unescaped.
	return SplitEscaped(in, '\0')[0];
}

// Escape one component. Every special is escaped, including the separators —
// a separator that is *content* inside a component has to survive, and the
// caller inserts the real separators between components itself.
std::string EscapeValue(const std::string &in) {
	std::string out;
	for (char c : in) {
		switch (c) {
		case '\\':
			out += "\\\\";
			break;
		case ';':
			out += "\\;";
			break;
		case ',':
			out += "\\,";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			break; // folded away; a bare CR in a value is never meaningful
		default:
			out += c;
		}
	}
	return out;
}

// Split the part before ':' into name, group and parameters. Parameter values
// may be quoted, and a quoted value may contain ':' and ';' — which is why the
// caller cannot simply find(':').
bool ParseNameAndParams(const std::string &head, Property &p) {
	// Group prefix: "item1.TEL".
	std::string rest = head;
	size_t dot = rest.find('.');
	size_t semi = rest.find(';');
	if (dot != std::string::npos && (semi == std::string::npos || dot < semi)) {
		p.group = rest.substr(0, dot);
		rest = rest.substr(dot + 1);
	}

	// Parameters, split on unquoted ';'.
	std::vector<std::string> parts;
	std::string cur;
	bool quoted = false;
	for (char c : rest) {
		if (c == '"') {
			quoted = !quoted;
			continue;
		}
		if (c == ';' && !quoted) {
			parts.push_back(cur);
			cur.clear();
			continue;
		}
		cur += c;
	}
	parts.push_back(cur);

	if (parts.empty() || Trim(parts[0]).empty()) {
		return false;
	}
	p.name = Upper(Trim(parts[0]));
	for (size_t i = 1; i < parts.size(); i++) {
		std::string kv = Trim(parts[i]);
		if (kv.empty()) {
			continue;
		}
		size_t eq = kv.find('=');
		if (eq == std::string::npos) {
			// vCard 2.1 shorthand: "TEL;WORK;VOICE:..." with no "TYPE=".
			p.params.emplace_back("TYPE", Upper(kv));
		} else {
			p.params.emplace_back(Upper(Trim(kv.substr(0, eq))), Trim(kv.substr(eq + 1)));
		}
	}
	return true;
}

// The separator that is *structure* in this property's value, or '\0' when the
// whole value is one piece of text.
//
// This has to be known both ways round or the round-trip loses information:
// parsing resolves "\," to a comma, so a value that arrives as "a\,b" and one
// that arrives as "a,b" would become indistinguishable unless the second is
// split into components here. Everything that is not a separator gets escaped
// on the way out — including the comma, which RFC 6350's TEXT grammar excludes
// (%x21-2B skips 0x2C) and RFC 2426 requires escaped as well.
char SeparatorFor(const std::string &name) {
	// ';' separates components: family;given;..., street;locality;...
	static const char *kSemi[] = {"N", "ADR", "ORG", "GENDER", "CLIENTPIDMAP"};
	for (const char *n : kSemi) {
		if (name == n) {
			return ';';
		}
	}
	// ',' separates list members.
	static const char *kComma[] = {"CATEGORIES", "NICKNAME"};
	for (const char *n : kComma) {
		if (name == n) {
			return ',';
		}
	}
	return '\0';
}

// Fold a line at 75 octets, never inside a UTF-8 sequence. A continuation
// starts with one space, which Unfold strips back off.
void AppendFolded(std::string &out, const std::string &line) {
	const size_t kLimit = 75;
	size_t i = 0;
	bool first = true;
	while (i < line.size()) {
		size_t budget = first ? kLimit : kLimit - 1;
		size_t take = std::min(budget, line.size() - i);
		// Back off to a character boundary: continuation bytes are 10xxxxxx.
		if (i + take < line.size()) {
			while (take > 1 && ((unsigned char)line[i + take] & 0xC0) == 0x80) {
				take--;
			}
		}
		if (!first) {
			out += " ";
		}
		out.append(line, i, take);
		out += "\r\n";
		i += take;
		first = false;
	}
	if (line.empty()) {
		out += "\r\n";
	}
}

} // namespace

// ---- Property -------------------------------------------------------------

std::string Property::Param(const std::string &pname) const {
	std::string want = Upper(pname);
	std::string out;
	for (auto &kv : params) {
		if (kv.first == want) {
			if (!out.empty()) {
				out += ",";
			}
			out += kv.second;
		}
	}
	return out;
}

bool Property::HasType(const std::string &t) const {
	std::string types = Param("TYPE");
	for (auto &one : SplitEscaped(types, ',')) {
		if (IEq(Trim(one), t)) {
			return true;
		}
	}
	return false;
}

std::string Property::Value() const {
	std::string out;
	for (size_t i = 0; i < values.size(); i++) {
		if (i) {
			out += ";";
		}
		out += values[i];
	}
	return out;
}

// ---- Card -----------------------------------------------------------------

const Property *Card::Find(const std::string &name) const {
	std::string want = Upper(name);
	for (auto &p : props) {
		if (p.name == want) {
			return &p;
		}
	}
	return nullptr;
}

std::vector<const Property *> Card::FindAll(const std::string &name) const {
	std::string want = Upper(name);
	std::vector<const Property *> out;
	for (auto &p : props) {
		if (p.name == want) {
			out.push_back(&p);
		}
	}
	return out;
}

void Card::Set(const std::string &name, const std::string &value) {
	SetComponents(name, {value});
}

void Card::SetComponents(const std::string &name, const std::vector<std::string> &values) {
	std::string want = Upper(name);
	for (auto &p : props) {
		if (p.name == want) {
			p.values = values;
			return;
		}
	}
	Property p;
	p.name = want;
	p.values = values;
	props.push_back(p);
}

void Card::Remove(const std::string &name) {
	std::string want = Upper(name);
	props.erase(std::remove_if(props.begin(), props.end(),
	                           [&](const Property &p) { return p.name == want; }),
	            props.end());
}

std::string Card::Uid() const {
	const Property *p = Find("UID");
	return p ? Trim(p->Value()) : std::string();
}

std::string Card::Fn() const {
	const Property *fn = Find("FN");
	if (fn && !Trim(fn->Value()).empty()) {
		return Trim(fn->Value());
	}
	// N is family;given;additional;prefix;suffix — assemble something readable
	// rather than showing the semicolons.
	const Property *n = Find("N");
	if (n) {
		std::string family = n->values.size() > 0 ? Trim(n->values[0]) : "";
		std::string given = n->values.size() > 1 ? Trim(n->values[1]) : "";
		std::string joined = given;
		if (!family.empty()) {
			if (!joined.empty()) {
				joined += " ";
			}
			joined += family;
		}
		if (!joined.empty()) {
			return joined;
		}
	}
	auto mails = Emails();
	if (!mails.empty()) {
		return mails[0];
	}
	// A card with no label at all still has to be listable.
	return "(no name)";
}

std::vector<std::string> Card::Emails() const {
	std::vector<std::string> out;
	for (auto *p : FindAll("EMAIL")) {
		std::string v = Trim(p->Value());
		if (!v.empty()) {
			out.push_back(v);
		}
	}
	return out;
}

std::vector<std::string> Card::Phones() const {
	std::vector<std::string> out;
	for (auto *p : FindAll("TEL")) {
		std::string v = Trim(p->Value());
		if (!v.empty()) {
			out.push_back(v);
		}
	}
	return out;
}

// ---- parsing --------------------------------------------------------------

bool Parse(const std::string &text, std::vector<Card> &out) {
	out.clear();
	auto lines = Unfold(text);

	bool in_card = false;
	Card cur;
	for (auto &raw : lines) {
		std::string line = Trim(raw);
		if (line.empty()) {
			continue;
		}
		if (IEq(line, "BEGIN:VCARD")) {
			in_card = true;
			cur = Card();
			continue;
		}
		if (IEq(line, "END:VCARD")) {
			if (in_card) {
				out.push_back(cur);
			}
			in_card = false;
			continue;
		}
		if (!in_card) {
			continue;
		}

		// Find the ':' that ends the name-and-parameters part, skipping any
		// inside a quoted parameter value.
		size_t colon = std::string::npos;
		bool quoted = false;
		for (size_t i = 0; i < line.size(); i++) {
			if (line[i] == '"') {
				quoted = !quoted;
			} else if (line[i] == ':' && !quoted) {
				colon = i;
				break;
			}
		}
		if (colon == std::string::npos) {
			continue; // not a property; skip it rather than fail the card
		}

		Property p;
		if (!ParseNameAndParams(line.substr(0, colon), p)) {
			continue;
		}
		std::string value = line.substr(colon + 1);

		if (p.name == "VERSION") {
			std::string v = Trim(value);
			cur.version = (v.rfind("4", 0) == 0) ? 4 : 3;
			// VERSION is re-emitted from Card::version, so it is not kept as a
			// property; keeping it would let the two disagree.
			continue;
		}

		char sep = SeparatorFor(p.name);
		p.values = sep ? SplitEscaped(value, sep) : std::vector<std::string>{Unescape(value)};
		cur.props.push_back(p);
	}

	return !out.empty();
}

bool ParseOne(const std::string &text, Card &out) {
	std::vector<Card> cards;
	if (!Parse(text, cards) || cards.empty()) {
		return false;
	}
	out = cards[0];
	return true;
}

// ---- emitting -------------------------------------------------------------

std::string Emit(const Card &card, int version) {
	int v = (version == 4) ? 4 : 3;
	std::string out;
	AppendFolded(out, "BEGIN:VCARD");
	AppendFolded(out, v == 4 ? "VERSION:4.0" : "VERSION:3.0");

	for (auto &p : card.props) {
		if (p.name == "VERSION" || p.name == "BEGIN" || p.name == "END") {
			continue;
		}
		std::string line;
		if (!p.group.empty()) {
			line += p.group + ".";
		}
		line += p.name;
		for (auto &kv : p.params) {
			line += ";" + kv.first + "=";
			// Quote only for ':' and ';'. Not for ',' — a comma in a parameter
			// value is the list separator ("TYPE=INTERNET,PREF" is two types),
			// so quoting would collapse the list into one member with a comma
			// in its name.
			if (kv.second.find_first_of(":;") != std::string::npos) {
				line += "\"" + kv.second + "\"";
			} else {
				line += kv.second;
			}
		}
		line += ":";
		char sep = SeparatorFor(p.name);
		for (size_t i = 0; i < p.values.size(); i++) {
			if (i && sep) {
				line += sep;
			}
			line += EscapeValue(p.values[i]);
		}
		AppendFolded(out, line);
	}

	AppendFolded(out, "END:VCARD");
	return out;
}

std::string EuidFor(const Card &card) {
	std::string uid = card.Uid();
	if (!uid.empty()) {
		return uid;
	}
	// No UID: derive one that is stable for this content, so re-importing the
	// same card replaces it rather than duplicating it.
	std::string basis = card.Fn();
	for (auto &e : card.Emails()) {
		basis += "\x1f" + e;
	}
	return "qc-" + util::Sha256Hex(basis).substr(0, 32);
}

std::string NewUid(const std::string &fqdn) {
	return util::RandomHex(16) + "@" + (fqdn.empty() ? "localhost" : fqdn);
}

} // namespace vcard
} // namespace quackmail
