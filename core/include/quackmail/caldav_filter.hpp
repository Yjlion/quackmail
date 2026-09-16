#pragma once

// RFC 4791 §9.7 <C:filter> evaluation, and its RFC 6352 §10.5 CardDAV twin.
//
// This lives in core/ rather than beside the DAV handler for one reason: it is
// pure. A filter and a body go in, a boolean comes out — no Connection, no Ctx,
// no request. That is what lets `qm_caldav_filter` and `qm_carddav_filter`
// expose it to SQL, and filter combinatorics are exactly what sqllogictest is
// good at and what a socket-driven Python test is bad at.
//
// The shape the RFC describes is a tree: a comp-filter may hold comp-filters,
// prop-filters and a time-range; a prop-filter may hold param-filters, a
// text-match, or is-not-defined; a param-filter may hold either of the last
// two. Every level carries `test="allof"|"anyof"` (RFC 6352 §10.5.1; CalDAV
// itself only ever means allof, but clients send the attribute on both).
//
// Recursion is bounded by the XML parser rather than by a counter here:
// dav::ParseDoc refuses a document deeper than kMaxDepth (32) or larger than
// kMaxNodes, so a filter tree cannot be deeper than the document that carried
// it.

#include "quackmail/davxml.hpp"
#include "quackmail/ical.hpp"
#include "quackmail/vcard.hpp"

#include <string>

namespace quackmail {
namespace caldav {

// Evaluate a <C:filter> against one iCalendar object.
//
// `filter` is the <C:filter> element itself, not its parent. An empty filter —
// no comp-filter children at all — matches everything, which is what a client
// asking for the whole collection sends.
//
// Returns false for a body that does not parse: a filter cannot be said to
// match something we could not read, and returning it would put an object into
// a result set that the client then cannot interpret either.
bool MatchCalendar(const dav::Node &filter, const std::string &ical_body);

// The CardDAV equivalent, over <CARD:filter>. Same rules, with prop-filter
// applying directly to vCard properties since a vCard has no component nesting.
bool MatchAddressBook(const dav::Node &filter, const std::string &vcard_body);

// A single <*:text-match> test, exposed because both evaluators use it and
// because it is the part with collation and negation rules worth testing on its
// own. `value` is the property or parameter value being tested.
bool TextMatches(const dav::Node &text_match, const std::string &value);

} // namespace caldav
} // namespace quackmail
