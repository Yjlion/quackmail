#include "quackmail/vcard.hpp"

#include "quackmail/contentline.hpp"
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

// The line grammar — unfolding, escapes, folding — lives in
// core/src/contentline.cpp, shared with iCalendar and vNote. What stays here is
// the part that is specific to vCard: which properties are structured, and what
// each one means.
namespace cl = contentline;

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
	for (auto &one : cl::SplitEscaped(types, ',')) {
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
	auto lines = cl::Unfold(text);

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

		// vCard allows the `item1.TEL` grouping Apple emits, so groups are on.
		cl::Line parsed;
		if (!cl::Parse(line, parsed, true)) {
			continue; // not a property; skip it rather than fail the card
		}
		Property p;
		p.group = parsed.group;
		p.name = parsed.name;
		p.params = parsed.params;
		const std::string &value = parsed.value;

		if (p.name == "VERSION") {
			std::string v = Trim(value);
			cur.version = (v.rfind("4", 0) == 0) ? 4 : 3;
			// VERSION is re-emitted from Card::version, so it is not kept as a
			// property; keeping it would let the two disagree.
			continue;
		}

		char sep = SeparatorFor(p.name);
		p.values = sep ? cl::SplitEscaped(value, sep) : std::vector<std::string>{cl::Unescape(value)};
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
	cl::AppendFolded(out, "BEGIN:VCARD");
	cl::AppendFolded(out, v == 4 ? "VERSION:4.0" : "VERSION:3.0");

	for (auto &p : card.props) {
		if (p.name == "VERSION" || p.name == "BEGIN" || p.name == "END") {
			continue;
		}
		cl::Line line;
		line.group = p.group;
		line.name = p.name;
		line.params = p.params;
		char sep = SeparatorFor(p.name);
		for (size_t i = 0; i < p.values.size(); i++) {
			if (i && sep) {
				line.value += sep;
			}
			line.value += cl::Escape(p.values[i]);
		}
		cl::AppendFolded(out, cl::Format(line));
	}

	cl::AppendFolded(out, "END:VCARD");
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
