#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace quackmail {
namespace ical {

// iCalendar (RFC 5545) — the format Citadel stores a calendar event, task or
// journal entry in, as a text/calendar part of an ordinary message keyed by its
// UID.
//
// Two representations, on purpose:
//
//   Component  the full tree, exactly as it arrived. Alarms, RRULE exceptions,
//              attendee parameters, inline VTIMEZONE definitions and every X-
//              property a phone wrote are all in here.
//   Item       the flat shape a form edits: summary, start, end, location.
//
// Editing goes Component -> Item -> form -> Item -> Component, and the fields
// Item does not model stay untouched in the tree. That is what stops saving an
// event in the browser from deleting the alarm someone set on their phone.
//
// Times resolve through the bundled IANA database (tz.hpp). An inline VTIMEZONE
// still wins when the calendar carries one, because a sender's own definition of
// their zone is what they meant; the database is the fallback for the common
// case where TZID names a zone and nothing defines it.

struct Property {
	std::string name; // upper-cased: "DTSTART", "SUMMARY"
	std::vector<std::pair<std::string, std::string>> params;
	std::string value; // escapes resolved

	std::string Param(const std::string &name) const;

	Property();
};

// A resolved instant. `epoch` is always UTC; the other fields record how it was
// written, because emitting it back has to say the same thing.
struct DateTime {
	int64_t epoch = 0;
	bool all_day = false; // VALUE=DATE — a day, not a moment
	std::string tzid;     // as received; "" for UTC or floating
	bool utc = false;     // the trailing-Z form
	bool valid = false;

	DateTime();
};

struct Component {
	std::string name; // VCALENDAR, VEVENT, VTODO, VJOURNAL, VALARM, VTIMEZONE
	std::vector<Property> props;
	std::vector<Component> children;

	const Property *Find(const std::string &name) const;
	std::string Get(const std::string &name) const;
	void Set(const std::string &name, const std::string &value);
	void Remove(const std::string &name);
	// First child of this type, or nullptr.
	const Component *Child(const std::string &name) const;
	std::vector<const Component *> Children(const std::string &name) const;

	Component();
};

// The flat view. Everything the UI does not model stays in the Component tree.
struct Item {
	enum Kind { Event, Todo, Journal };

	Kind kind = Event;
	std::string uid;
	std::string summary, description, location, organizer, status;
	std::string rrule;                  // verbatim, for Expand and for emitting
	std::vector<std::string> attendees; // the CAL-ADDRESS values
	std::vector<int64_t> exdates;       // EXDATE instants, for Expand
	DateTime start, end, due, completed;
	int64_t sequence = 0;
	int priority = 0;
	int percent_complete = 0;

	Item();
};

// Parse into the tree. False when there is no VCALENDAR at all. As with vCard,
// a component with an unparseable line still yields what it could read.
bool Parse(const std::string &text, Component &out);

// Parse and flatten every VEVENT / VTODO / VJOURNAL. Nested VALARMs stay in the
// tree and are not items.
bool ParseItems(const std::string &text, std::vector<Item> &out);

// Serialize a tree, folding at 75 octets on a UTF-8 boundary.
std::string Emit(const Component &root);

// Serialize one item as a complete VCALENDAR, from nothing.
//
// **Only for an item being created.** It builds a fresh component from the
// Item's modelled fields, so anything an Item does not model — alarms, X-
// properties, attendee parameters — is simply absent from the result. Calling
// this to save an *edit* is how you delete the reminder someone set on their
// phone.
//
// To save an edit: Parse the original, ApplyItem, then Emit. That path keeps
// what it does not understand, which is the whole point of having two
// representations.
//
// A TZID named by any of the item's times gets a matching VTIMEZONE beside it —
// without one, Outlook and Thunderbird are free to guess what the zone means.
std::string EmitItem(const Item &item, const std::string &prodid);

// Fold an Item's fields back into an existing component tree, leaving every
// property the Item does not model alone. This is the save path; see EmitItem.
// False when the tree holds no component with this item's UID and kind.
bool ApplyItem(Component &root, const Item &item);

// A VTIMEZONE for `tzid` covering [from_year, to_year], built from the bundled
// database. Empty when the zone is unknown.
std::string EmitVtimezone(const std::string &tzid, int from_year, int to_year);

struct Occurrence {
	int64_t start = 0;
	int64_t end = 0;
	bool all_day = false;
	std::string uid;

	Occurrence();
};

// Expand `item` into the occurrences that fall in [from, to).
//
// Supports FREQ=DAILY/WEEKLY/MONTHLY/YEARLY with INTERVAL, COUNT, UNTIL and
// BYDAY, minus EXDATE. A rule using anything else yields the single master
// instance rather than nothing: an event that cannot be expanded should still
// appear on the day it was set, not vanish from the calendar.
//
// Recurrence is resolved in the event's own zone, so a weekly 09:00 meeting
// stays at 09:00 across a DST boundary instead of drifting an hour.
//
// Hard-capped at kMaxOccurrences: an unbounded RRULE must not be able to hang a
// request thread.
extern const size_t kMaxOccurrences;
std::vector<Occurrence> Expand(const Item &item, int64_t from, int64_t to);

// The euid an item is stored under: its UID, or a stable derived one.
std::string EuidFor(const Item &item);
std::string NewUid(const std::string &fqdn);

// Format an instant as iCalendar writes it: "20260315T090000Z", or
// "20260315T090000" with a TZID, or "20260315" for an all-day value.
std::string FormatDateTime(const DateTime &dt);
// Parse a DTSTART/DTEND/DUE value, given its parameters.
DateTime ParseDateTime(const std::string &value,
                       const std::vector<std::pair<std::string, std::string>> &params,
                       const Component *calendar);

} // namespace ical
} // namespace quackmail
