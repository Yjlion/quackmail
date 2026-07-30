#pragma once

#include <string>
#include <utility>
#include <vector>

namespace quackmail {
namespace vcard {

// vCard 3.0 (RFC 2426) and 4.0 (RFC 6350) — the format Citadel stores a contact
// in, as a text/vcard part of an ordinary message keyed by its UID.
//
// The design rule that matters: **every property is preserved, including ones
// this code does not understand.** A contact edited in the web UI must not lose
// the fields a phone or Thunderbird wrote into it, so parsing keeps the whole
// property list and emitting writes it back. Card::Set touches one property and
// leaves the rest alone.
//
// Parsing is deliberately tolerant. Real vCards arrive with LF instead of CRLF,
// with lowercase property names, with the `item1.TEL` grouping Apple emits, and
// occasionally with a stray blank line. None of that is worth rejecting a
// contact over.

struct Property {
	std::string group;                                      // "item1", usually empty
	std::string name;                                       // upper-cased: "FN", "TEL"
	std::vector<std::pair<std::string, std::string>> params; // names upper-cased
	// Components, split on unescaped ';'. A single-valued property has one.
	// "N" has five, "ADR" seven. Escapes are already resolved.
	std::vector<std::string> values;

	// First value of a parameter, "" when absent. Repeated parameters (TYPE
	// appears twice as often as it appears once) are joined with ','.
	std::string Param(const std::string &name) const;
	// Does TYPE contain `t`, case-insensitively? TYPE=WORK,VOICE and
	// TYPE=work;TYPE=voice mean the same thing and both answer yes.
	bool HasType(const std::string &t) const;
	// The whole value, components rejoined with ';'.
	std::string Value() const;

	Property();
};

struct Card {
	int version = 3; // 3 or 4; what Emit writes unless told otherwise
	std::vector<Property> props;

	std::string Uid() const;
	// The display name: FN, or a name assembled from N, or the first e-mail
	// address. Never empty for a card that parsed, because a contact with no
	// label at all cannot be listed.
	std::string Fn() const;
	std::vector<std::string> Emails() const;
	std::vector<std::string> Phones() const;

	const Property *Find(const std::string &name) const;
	std::vector<const Property *> FindAll(const std::string &name) const;
	// Replace the first property of this name, or append when there is none.
	void Set(const std::string &name, const std::string &value);
	// Replace with a multi-component value ("N", "ADR").
	void SetComponents(const std::string &name, const std::vector<std::string> &values);
	void Remove(const std::string &name);

	Card();
};

// Parse a file that may hold several cards. Returns false only when there is no
// BEGIN:VCARD at all — a card with unparseable lines still yields what it could
// read, because dropping a contact is worse than dropping one of its fields.
bool Parse(const std::string &text, std::vector<Card> &out);
bool ParseOne(const std::string &text, Card &out);

// Serialize. Folds at 75 octets on a UTF-8 boundary (never mid-character) and
// escapes per the target version: vCard 3.0 and 4.0 differ on whether ',' is
// special outside structured values, and on how TYPE parameters are written.
std::string Emit(const Card &card, int version);

// The euid a card is stored under: its UID, or a stable one derived from the
// card's content when it has none. Never empty — an object with no euid cannot
// be replaced in place, so it would duplicate on every edit.
std::string EuidFor(const Card &card);

// A fresh UID for a card being created here, in the form Citadel uses.
std::string NewUid(const std::string &fqdn);

} // namespace vcard
} // namespace quackmail
