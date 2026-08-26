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

// A resolved date or instant.
//
// **What `epoch` means depends on `all_day`, and getting this wrong is an
// off-by-one-day bug.**
//
//   all_day == false   a UTC instant. Render it by shifting into a zone.
//   all_day == true    a *wall-clock* value: seconds since the epoch as if the
//                      date were UTC. VALUE=DATE names a day, not a moment, so
//                      there is no instant to shift and shifting it anyway moves
//                      the date — a 1 April all-day event becomes 31 March for
//                      anyone west of Greenwich.
//
// The other fields record how the value was written, because emitting it back
// has to say the same thing.
struct DateTime {
	int64_t epoch = 0;
	bool all_day = false; // VALUE=DATE — a day, not a moment; see above
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
	// "text/html" or "text/x-markdown" — a QuackCit extension (the
	// X-QM-DESC-FORMAT property), never emitted for an item this field is
	// empty on. DESCRIPTION stays plain text on the wire either way (RFC 5545
	// has no notion of an HTML description), so this only changes how
	// QuackCit's own web view renders it — every other client, and every item
	// with no format set (which is every item written before this field
	// existed), keeps reading and writing an ordinary plain-text DESCRIPTION.
	std::string desc_format;
	// TRANSP, **read only**. Not something a form edits, but free/busy has to
	// know: an event marked TRANSPARENT occupies the calendar without occupying
	// the person. ParseItems fills it and FoldItemInto deliberately does not
	// write it back, so it stays where it already lives — in the Component tree,
	// which is what keeps an edit from dropping it.
	std::string transp;
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

// ---- free/busy -----------------------------------------------------------
//
// The question a scheduling client asks before proposing a time: when is this
// person unavailable? It is deliberately *not* "what is on their calendar" —
// a free/busy reply carries intervals and nothing else, no summary, no
// location, no attendees. That is what makes it safe to answer for a calendar
// the asker cannot read.

// A half-open interval of UTC seconds, [start, end).
struct Period {
	int64_t start = 0;
	int64_t end = 0;

	Period();
};

// Sort, then merge everything that overlaps or merely touches. The result is
// the minimal set covering the same time — which is the whole point: a busy
// reply describes availability, and two adjacent meetings are one unavailable
// stretch, not two.
std::vector<Period> MergePeriods(std::vector<Period> in);

// What one calendar object contributes to a free/busy answer, split because
// RFC 5545 reports the two under different FBTYPEs and a client draws them
// differently.
struct Busy {
	std::vector<Period> busy;
	std::vector<Period> tentative;

	Busy();
};

// Accumulate the busy time in `text` that falls within [from, to).
//
// The rules are RFC 4791 §7.10's: only VEVENTs count; `TRANSP:TRANSPARENT`
// contributes nothing, which is how an all-day "on holiday" marker avoids
// blocking the whole day; `STATUS:CANCELLED` contributes nothing;
// `STATUS:TENTATIVE` lands in `tentative`. Recurrences are expanded, so a
// weekly meeting is busy every week rather than once.
//
// Zero-length events are skipped: an instant is not an interval, and emitting
// one would claim a period no client can render.
//
// Appends, so a caller can sweep a whole collection into one Busy and merge at
// the end.
void CollectBusy(const std::string &text, int64_t from, int64_t to, Busy &out);

// A complete VCALENDAR holding one VFREEBUSY over [from, to).
//
// `organizer` and `attendee` are CAL-ADDRESS values ("mailto:ada@example.com")
// and may be empty, which is the plain CalDAV free-busy-query case — those
// properties belong to an iTIP reply, not to a REPORT.
std::string EmitFreeBusy(int64_t from, int64_t to, const Busy &busy,
                         const std::string &organizer, const std::string &attendee,
                         const std::string &prodid);

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
