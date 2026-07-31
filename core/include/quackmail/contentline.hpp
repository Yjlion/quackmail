#pragma once

#include <string>
#include <utility>
#include <vector>

namespace quackmail {
namespace contentline {

// The line grammar shared by vCard (RFC 6350), iCalendar (RFC 5545) and vNote.
//
// All three are the same format wearing different property names: CRLF lines
// folded at 75 octets, `NAME;PARAM=value:content`, and the same four escapes in
// a value. Each of vcard.cpp, ical.cpp and vnote.cpp used to carry its own copy
// of the unfolder, the folder and the escaper; this is that code, once.
//
// Kept deliberately dumb: it knows about lines, parameters and escapes, and
// nothing about which properties exist or what any of them mean. Deciding that
// `ADR` splits on ';' but `NOTE` does not is the caller's business, because the
// answer differs between the three formats.

// Split into logical lines, joining continuations. A line beginning with SP or
// HTAB continues the previous one, and unfolding has to happen before anything
// else: a fold can land inside a property name, inside a parameter, or in the
// middle of a UTF-8 sequence.
//
// Tolerant of LF-only input and of a stray CR, because real files have both.
std::vector<std::string> Unfold(const std::string &text);

// Append one logical line, folded at 75 octets. Backs off to a UTF-8 character
// boundary rather than splitting a multi-byte character, and prefixes each
// continuation with the single space Unfold strips back off.
void AppendFolded(std::string &out, const std::string &line);

// Resolve `\n`, `\,`, `\;` and `\\`.
//
// An unrecognised escape keeps *both* characters. Guessing loses data, and these
// formats in the wild are full of stray backslashes that were never meant as
// escapes — a Windows path in a NOTE, most often.
std::string Unescape(const std::string &in);

// The inverse, escaping all four. Separators are escaped too: one that appears
// *inside* a component is content and has to survive, and the caller inserts the
// real separators between components itself.
std::string Escape(const std::string &in);

// Split on unescaped `sep`, resolving escapes in each piece.
std::vector<std::string> SplitEscaped(const std::string &in, char sep);

// Split on unescaped `sep` *without* resolving escapes, for a value whose
// pieces are themselves structured (an RRULE, a parameter list).
std::vector<std::string> SplitRaw(const std::string &in, char sep);

// One parsed line. `value` is left exactly as it appeared — the caller decides
// whether to unescape it or to treat its separators as structure.
struct Line {
	std::string group;                                       // "item1", usually empty
	std::string name;                                        // upper-cased
	std::vector<std::pair<std::string, std::string>> params; // names upper-cased
	std::string value;

	Line();
};

// Parse `NAME;PARAM=v:value`. False when there is no name, or no colon outside a
// quoted parameter value — which is why the colon cannot simply be found with
// find(':'): a quoted parameter may contain one.
//
// `allow_group` enables the `item1.TEL` prefix Apple emits. iCalendar has no
// such construct, so passing false there keeps a property containing a dot from
// being mistaken for one.
bool Parse(const std::string &line, Line &out, bool allow_group);

// Serialize a name, its parameters and an already-escaped value into a line
// ready for AppendFolded. A parameter value is quoted only when it contains ':'
// or ';' — never for ',', which is the list separator for every parameter that
// takes a list, so quoting would collapse the list into one member.
std::string Format(const Line &line);

} // namespace contentline
} // namespace quackmail
