#pragma once

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace quackmail {
namespace itip {

// iTIP (RFC 5546) — the scheduling half of iCalendar, and iMIP (RFC 6047) — the
// same messages carried as mail.
//
// Everything here is a **pure function of the iCalendar text**, apart from
// SelfAddress and IsOrganizer, which need to know what this site calls its
// users. That is deliberate: the parts that are easy to get subtly wrong — who
// gets told, what a CANCEL has to say, which ATTENDEE a REPLY speaks for — are
// then assertable from `test/sql/itip.test` with no socket and no mail server.
// The wiring that sends and receives is thin by comparison.
//
// The one rule worth stating up front: **only the organizer sends.** An
// attendee editing their own copy of an event must not mail everyone else,
// and a server that let them would turn every calendar into a mailing list.

// The CAL-ADDRESS this server gives a local user: "mailto:<user>@<c_fqdn>".
// The same string `dav_propfind` reports as `calendar-user-address-set`, which
// is what makes that property the inverse of this one.
std::string SelfAddress(duckdb::Connection &con, const std::string &username);

// "MAILTO:Ada@Example.COM" -> "ada@example.com". Returns "" for anything that
// is not a mail address, which includes the URN and http forms RFC 5545 also
// allows — we can only send mail, so anything else is not a recipient.
std::string AddressOf(const std::string &cal_address);

// Is `username` the ORGANIZER of the first VEVENT in `ics`?
//
// The local part must equal the username and the domain must be one this site
// hosts, so a user cannot claim to organize on behalf of an address they do
// not own. False when there is no ORGANIZER at all, which is the ordinary case
// for a private appointment nobody is invited to.
bool IsOrganizer(duckdb::Connection &con, const std::string &username, const std::string &ics);

// Every attendee who has to be told, as bare mail addresses, deduplicated and
// with `organizer_addr` removed — an organizer who is also on the ATTENDEE list
// (most clients add themselves) does not need to be mailed their own
// invitation.
std::vector<std::string> Recipients(const std::string &ics, const std::string &organizer_addr);

// METHOD:REQUEST over the components as they stand. This is an invitation and
// an update both: RFC 5546 uses the same method for each, and SEQUENCE is what
// tells them apart.
std::string BuildRequest(const std::string &ics);

// METHOD:CANCEL. Also sets STATUS:CANCELLED and bumps SEQUENCE, because a
// cancellation that did not would be indistinguishable from a stale copy of the
// invitation and clients drop it.
std::string BuildCancel(const std::string &ics);

// METHOD:REPLY on behalf of one attendee.
//
// The reply carries **only that attendee**, which is the point of it: an
// attendee has no business telling the organizer what everyone else said, and
// several clients treat a reply listing other attendees as an attempt to.
// `partstat` is ACCEPTED / DECLINED / TENTATIVE. Empty when the attendee is not
// on the event at all.
std::string BuildReply(const std::string &ics, const std::string &attendee,
                       const std::string &partstat);

// Fold a METHOD:REPLY into the organizer's stored copy: every ATTENDEE the
// reply speaks for has its PARTSTAT updated in place, and nothing else about
// the stored event changes.
//
// False — leaving `out` untouched — when the reply names nobody the stored copy
// knows, when the UIDs disagree, or when the reply's SEQUENCE is older than the
// stored one. That last is what stops a delayed reply to last week's version
// from overwriting an answer to this week's.
bool ApplyReply(const std::string &stored, const std::string &reply, std::string &out);

// ---- iMIP: the same messages, carried as mail (RFC 6047) -----------------

enum class Method { Request, Cancel };

struct Sent {
	bool sent = false;
	std::vector<std::string> rcpts; // who was told
	std::string err;                // set only when something actually failed

	Sent();
};

// Mail an iTIP message about `ics` to its attendees.
//
// Returns false and leaves `err` **empty** for the ordinary "nothing to do"
// cases — no organizer, not this user's event, nobody to tell — because those
// are not failures and a caller must not report them as one. A non-empty `err`
// means the send was refused or broke.
//
// Charges `policy::CheckRate` and `policy::RecordSend` itself. `submission::Send`
// is quota-agnostic by contract, so a scheduling path that skipped the quota
// would be a third door onto the mail path with the limit turned off.
//
// **Only the organizer sends.** An attendee saving their own copy of an event
// must not mail everyone else, so `IsOrganizer` gates this before anything is
// built.
bool Notify(duckdb::Connection &con, const std::string &username, const std::string &ics,
            Method method, const std::string &via, bool tls, Sent &out);

// Fold an inbound iTIP message into `username`'s calendar.
//
// REPLY updates the PARTSTAT on the organizer's stored copy; REQUEST files the
// event into their Calendar room so an invitation shows up where they look for
// it; CANCEL marks the stored copy STATUS:CANCELLED rather than deleting it,
// because a meeting that vanishes without trace is indistinguishable from one
// that was never there.
//
// False when there was nothing to apply, which includes every message that is
// not a scheduling message at all.
bool Receive(duckdb::Connection &con, const std::string &username, const std::string &ics,
             const std::string &method, std::string &err);

// The text/calendar part of an RFC822 message, and the METHOD it carries.
//
// Returns "" when the message has no scheduling content, which is almost every
// message — so callers check the cheap way first. `method` is upper-cased and
// comes from the *Content-Type parameter or the calendar body*, whichever is
// present; a client that sets one and not the other is common enough that
// insisting on both would drop real invitations.
std::string CalendarPart(const std::string &raw, std::string &method);

} // namespace itip
} // namespace quackmail
