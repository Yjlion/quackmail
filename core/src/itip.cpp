#include "quackmail/itip.hpp"

#include "quackmail/citadel_msg.hpp"
#include "quackmail/citadel_store.hpp"
#include "quackmail/ical.hpp"
#include "quackmail/mailpolicy.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/submission.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>

namespace quackmail {
namespace itip {

namespace {

// The components iTIP schedules. VJOURNAL is schedulable in the abstract and
// nothing implements it, so leaving it out is the honest choice.
bool IsScheduled(const std::string &name) {
	return name == "VEVENT" || name == "VTODO";
}

std::string Lower(const std::string &s) {
	return util::Lower(s);
}

// Set one parameter on a property, replacing it if it is already there. The
// tree keeps parameters that the flat Item view drops, and PARTSTAT is exactly
// such a parameter — which is why a reply has to be applied here rather than
// through ical::Item.
void SetParam(ical::Property &p, const std::string &name, const std::string &value) {
	for (auto &kv : p.params) {
		if (util::Upper(kv.first) == name) {
			kv.second = value;
			return;
		}
	}
	p.params.emplace_back(name, value);
}

int64_t SequenceOf(const ical::Component &c) {
	return (int64_t)std::atoll(c.Get("SEQUENCE").c_str());
}

// A DTSTAMP for right now, which every iTIP message needs and which is how a
// receiver orders two messages with the same SEQUENCE.
std::string NowStamp() {
	ical::DateTime dt;
	dt.epoch = (int64_t)std::time(nullptr);
	dt.utc = true;
	dt.valid = true;
	return ical::FormatDateTime(dt);
}

// The first scheduled component in a parsed calendar, or nullptr.
const ical::Component *FirstScheduled(const ical::Component &root) {
	for (const auto &c : root.children) {
		if (IsScheduled(c.name)) {
			return &c;
		}
	}
	return nullptr;
}

} // namespace

std::string SelfAddress(duckdb::Connection &con, const std::string &username) {
	if (username.empty()) {
		return std::string();
	}
	std::string fqdn = citadel::GetConfig(con, "c_fqdn", "localhost");
	return "mailto:" + username + "@" + fqdn;
}

std::string AddressOf(const std::string &cal_address) {
	std::string v = cal_address;
	// A CAL-ADDRESS may arrive with surrounding whitespace from a folded line.
	while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) {
		v.erase(v.begin());
	}
	while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) {
		v.pop_back();
	}
	if (Lower(v).rfind("mailto:", 0) == 0) {
		v = v.substr(7);
	} else if (v.find(':') != std::string::npos) {
		// urn:uuid:..., http://..., or anything else RFC 5545 allows. We can
		// only send mail, so a scheme we cannot post to is not a recipient.
		return std::string();
	}
	if (v.find('@') == std::string::npos || v.find(' ') != std::string::npos) {
		return std::string();
	}
	return Lower(v);
}

bool IsOrganizer(duckdb::Connection &con, const std::string &username, const std::string &ics) {
	if (username.empty()) {
		return false;
	}
	ical::Component root;
	if (!ical::Parse(ics, root)) {
		return false;
	}
	const ical::Component *c = FirstScheduled(root);
	if (!c) {
		return false;
	}
	std::string org = AddressOf(c->Get("ORGANIZER"));
	if (org.empty()) {
		return false;
	}
	size_t at = org.find('@');
	std::string local = org.substr(0, at);
	std::string domain = org.substr(at + 1);
	// The local part must be this user. Checking only the domain would let
	// anyone with an account organize as anyone else on the site.
	if (local != Lower(username)) {
		return false;
	}
	return policy::IsLocalDomain(con, domain);
}

std::vector<std::string> Recipients(const std::string &ics, const std::string &organizer_addr) {
	std::vector<std::string> out;
	ical::Component root;
	if (!ical::Parse(ics, root)) {
		return out;
	}
	// Normalized the same way the attendees are, so a caller may hand this
	// either the bare address or the CAL-ADDRESS it came from.
	std::string skip = AddressOf(organizer_addr);
	if (skip.empty()) {
		skip = Lower(organizer_addr);
	}
	for (const auto &c : root.children) {
		if (!IsScheduled(c.name)) {
			continue;
		}
		for (const auto &p : c.props) {
			if (util::Upper(p.name) != "ATTENDEE") {
				continue;
			}
			std::string addr = AddressOf(p.value);
			if (addr.empty() || addr == skip) {
				continue;
			}
			if (std::find(out.begin(), out.end(), addr) == out.end()) {
				out.push_back(addr);
			}
		}
	}
	return out;
}

std::string BuildRequest(const std::string &ics) {
	ical::Component root;
	if (!ical::Parse(ics, root) || !FirstScheduled(root)) {
		return std::string();
	}
	root.Set("METHOD", "REQUEST");
	for (auto &c : root.children) {
		if (IsScheduled(c.name)) {
			c.Set("DTSTAMP", NowStamp());
		}
	}
	return ical::Emit(root);
}

std::string BuildCancel(const std::string &ics) {
	ical::Component root;
	if (!ical::Parse(ics, root) || !FirstScheduled(root)) {
		return std::string();
	}
	root.Set("METHOD", "CANCEL");
	for (auto &c : root.children) {
		if (!IsScheduled(c.name)) {
			continue;
		}
		c.Set("STATUS", "CANCELLED");
		// Without the bump a cancellation looks like a stale copy of the
		// invitation, and a client that has already seen this SEQUENCE drops it.
		c.Set("SEQUENCE", std::to_string(SequenceOf(c) + 1));
		c.Set("DTSTAMP", NowStamp());
	}
	return ical::Emit(root);
}

std::string BuildReply(const std::string &ics, const std::string &attendee,
                       const std::string &partstat) {
	std::string want = AddressOf(attendee);
	if (want.empty()) {
		return std::string();
	}
	ical::Component root;
	if (!ical::Parse(ics, root)) {
		return std::string();
	}

	ical::Component out;
	out.name = "VCALENDAR";
	out.Set("VERSION", "2.0");
	out.Set("PRODID", "-//QuackCit//EN");
	out.Set("METHOD", "REPLY");

	for (const auto &c : root.children) {
		if (!IsScheduled(c.name)) {
			continue;
		}
		const ical::Property *mine = nullptr;
		for (const auto &p : c.props) {
			if (util::Upper(p.name) == "ATTENDEE" && AddressOf(p.value) == want) {
				mine = &p;
				break;
			}
		}
		if (!mine) {
			continue;
		}

		ical::Component reply;
		reply.name = c.name;
		// Exactly what RFC 5546 §3.2.3 asks for and nothing else. In
		// particular the reply does *not* carry the other attendees: what they
		// said is not this attendee's to report, and a reply that listed them
		// would read as an attempt to answer on their behalf.
		reply.Set("UID", c.Get("UID"));
		if (!c.Get("ORGANIZER").empty()) {
			reply.Set("ORGANIZER", c.Get("ORGANIZER"));
		}
		if (!c.Get("SEQUENCE").empty()) {
			reply.Set("SEQUENCE", c.Get("SEQUENCE"));
		}
		if (const ical::Property *rid = c.Find("RECURRENCE-ID")) {
			// A reply to one occurrence has to say which one, or it answers for
			// the whole series.
			reply.props.push_back(*rid);
		}
		reply.Set("DTSTAMP", NowStamp());

		ical::Property att = *mine;
		SetParam(att, "PARTSTAT", partstat.empty() ? "ACCEPTED" : util::Upper(partstat));
		// RSVP has been honoured by the act of replying; leaving it set asks
		// the client to keep nagging.
		for (size_t i = 0; i < att.params.size(); i++) {
			if (util::Upper(att.params[i].first) == "RSVP") {
				att.params.erase(att.params.begin() + (long)i);
				break;
			}
		}
		reply.props.push_back(att);
		out.children.push_back(reply);
	}

	if (out.children.empty()) {
		return std::string(); // this attendee is not on the event
	}
	return ical::Emit(out);
}

bool ApplyReply(const std::string &stored, const std::string &reply, std::string &out) {
	ical::Component have;
	ical::Component said;
	if (!ical::Parse(stored, have) || !ical::Parse(reply, said)) {
		return false;
	}

	bool changed = false;
	for (const auto &rc : said.children) {
		if (!IsScheduled(rc.name)) {
			continue;
		}
		std::string uid = rc.Get("UID");
		if (uid.empty()) {
			continue;
		}
		for (auto &hc : have.children) {
			if (!IsScheduled(hc.name) || hc.Get("UID") != uid) {
				continue;
			}
			// A reply to an older version of the event is stale. Applying it
			// would let a delayed "accepted" for last week's time overwrite the
			// answer somebody gave to this week's.
			if (!rc.Get("SEQUENCE").empty() && SequenceOf(rc) < SequenceOf(hc)) {
				continue;
			}
			for (const auto &rp : rc.props) {
				if (util::Upper(rp.name) != "ATTENDEE") {
					continue;
				}
				std::string who = AddressOf(rp.value);
				if (who.empty()) {
					continue;
				}
				std::string partstat = rp.Param("PARTSTAT");
				if (partstat.empty()) {
					continue;
				}
				for (auto &hp : hc.props) {
					if (util::Upper(hp.name) != "ATTENDEE" || AddressOf(hp.value) != who) {
						continue;
					}
					SetParam(hp, "PARTSTAT", util::Upper(partstat));
					// The organizer's copy no longer needs an answer from
					// somebody who has just given one.
					SetParam(hp, "RSVP", "FALSE");
					changed = true;
				}
			}
		}
	}

	if (!changed) {
		return false;
	}
	out = ical::Emit(have);
	return true;
}

// ---- iMIP ----------------------------------------------------------------

Sent::Sent() {
}

namespace {

// A one-line human summary, for the subject and the text/plain part. A mail
// client that cannot read the calendar attachment still has to show the
// recipient something meaningful.
std::string SummaryOf(const ical::Component &root) {
	const ical::Component *c = FirstScheduled(root);
	if (!c) {
		return std::string();
	}
	std::string s = c->Get("SUMMARY");
	return s.empty() ? "(no subject)" : s;
}

std::string PlainBody(const ical::Component &root, Method method, const std::string &organizer) {
	const ical::Component *c = FirstScheduled(root);
	std::string out;
	out += method == Method::Request ? "You have been invited to an event.\r\n"
	                                 : "An event has been cancelled.\r\n";
	out += "\r\n";
	if (c) {
		out += "Subject:  " + SummaryOf(root) + "\r\n";
		if (!c->Get("DTSTART").empty()) {
			out += "Starts:   " + c->Get("DTSTART") + "\r\n";
		}
		if (!c->Get("LOCATION").empty()) {
			out += "Location: " + c->Get("LOCATION") + "\r\n";
		}
	}
	if (!organizer.empty()) {
		out += "Organizer: " + organizer + "\r\n";
	}
	out += "\r\n";
	// Said plainly, because it is true and because a recipient whose client
	// ignores the attachment needs to know why nothing happened when they
	// clicked nothing.
	out += "Your calendar program should offer to add this to your calendar.\r\n";
	return out;
}

} // namespace

bool Notify(duckdb::Connection &con, const std::string &username, const std::string &ics,
            Method method, const std::string &via, bool tls, Sent &out) {
	out = Sent();
	if (!IsOrganizer(con, username, ics)) {
		return false; // not ours to announce
	}

	ical::Component root;
	if (!ical::Parse(ics, root)) {
		return false;
	}
	const ical::Component *first = FirstScheduled(root);
	if (!first) {
		return false;
	}
	std::string organizer = AddressOf(first->Get("ORGANIZER"));
	std::vector<std::string> rcpts = Recipients(ics, organizer);
	if (rcpts.empty()) {
		return false; // an appointment with no attendees invites nobody
	}

	std::string payload = method == Method::Request ? BuildRequest(ics) : BuildCancel(ics);
	if (payload.empty()) {
		return false;
	}

	auto quota = policy::CheckRate(con, username, (int64_t)rcpts.size());
	if (!quota.allowed) {
		out.err = quota.reason.empty() ? "send quota exceeded" : quota.reason;
		return false;
	}

	std::string fqdn = citadel::GetConfig(con, "c_fqdn", "localhost");
	std::string summary = SummaryOf(root);
	std::string subject = (method == Method::Request ? "Invitation: " : "Cancelled: ") + summary;
	std::string received = submission::ReceivedHeader(con, via.empty() ? "caldav" : via, username, tls);

	bool any = false;
	for (const auto &rcpt : rcpts) {
		mime::HeaderList headers;
		headers.push_back({"Message-ID", "<" + util::RandomHex(12) + "." +
		                                     std::to_string((int64_t)std::time(nullptr)) + "@" + fqdn + ">"});
		headers.push_back({"Date", util::RfcDate(0)});
		headers.push_back({"From", organizer});
		headers.push_back({"To", rcpt});
		headers.push_back({"Subject", subject});

		std::vector<mime::BuildPart> parts;
		mime::BuildPart text;
		text.content_type = "text/plain";
		text.content = PlainBody(root, method, organizer);
		parts.push_back(text);

		mime::BuildPart cal;
		// The method belongs on the Content-Type as well as in the body. Several
		// clients dispatch on the parameter alone and treat a part without one as
		// an ordinary attachment, which is how an invitation arrives as a file
		// called "invite.ics" that nothing offers to open.
		cal.content_type = std::string("text/calendar; method=") +
		                   (method == Method::Request ? "REQUEST" : "CANCEL");
		cal.content = payload;
		parts.push_back(cal);

		std::string body = mime::BuildMessage(headers, parts);
		submission::Result res;
		if (submission::Send(con, organizer, {rcpt}, received, body, res)) {
			policy::RecordSend(con, username, rcpt, 1);
			out.rcpts.push_back(rcpt);
			any = true;
		} else if (out.err.empty()) {
			out.err = res.err.empty() ? "the invitation could not be sent" : res.err;
		}
	}

	out.sent = any;
	return any;
}

bool Receive(duckdb::Connection &con, const std::string &username, const std::string &ics,
             const std::string &method, std::string &err) {
	std::string m = util::Upper(method);
	if (m != "REQUEST" && m != "REPLY" && m != "CANCEL") {
		return false; // PUBLISH, COUNTER, REFRESH: nothing here acts on them
	}
	ical::Component incoming;
	if (!ical::Parse(ics, incoming)) {
		return false;
	}
	const ical::Component *first = FirstScheduled(incoming);
	if (!first) {
		return false;
	}
	std::string uid = first->Get("UID");
	if (uid.empty()) {
		return false; // nothing to key it by, so nothing to update or file
	}

	// The user's own calendar. Created if it is missing, the same way delivery
	// creates a mail folder — an invitation arriving before anybody has opened
	// the calendar is normal.
	int64_t room = citadel::GetOrCreateUserRoom(con, username, "Calendar");
	if (room < 0) {
		err = "no calendar to file this in";
		return false;
	}

	int64_t existing = citadel::FindByEuid(con, room, uid);
	std::string body;

	if (m == "REPLY") {
		// A reply only ever *updates*. Filing a reply as a new event would put
		// a stub on the organizer's calendar for a meeting they already have.
		if (existing < 0) {
			return false;
		}
		citadel::Message have;
		if (!citadel::LoadMessage(con, existing, have)) {
			return false;
		}
		std::string stored = citadel::ObjectBody(have, "text/calendar");
		if (stored.empty() || !ApplyReply(stored, ics, body)) {
			return false;
		}
	} else if (m == "CANCEL") {
		if (existing < 0) {
			return false;
		}
		citadel::Message have;
		if (!citadel::LoadMessage(con, existing, have)) {
			return false;
		}
		std::string stored = citadel::ObjectBody(have, "text/calendar");
		ical::Component root;
		if (stored.empty() || !ical::Parse(stored, root)) {
			return false;
		}
		// Marked rather than deleted: a meeting that vanishes without trace is
		// indistinguishable from one that was never there, and the attendee may
		// want to know it was called off.
		bool touched = false;
		for (auto &c : root.children) {
			if (IsScheduled(c.name)) {
				c.Set("STATUS", "CANCELLED");
				touched = true;
			}
		}
		if (!touched) {
			return false;
		}
		body = ical::Emit(root);
	} else {
		// REQUEST: file it, replacing any earlier version of the same event.
		// UpsertByEuid keys on the UID, so an update lands on the existing row
		// rather than beside it.
		body = ics;
	}

	if (body.empty()) {
		return false;
	}

	citadel::Message msg;
	msg.euid = uid;
	msg.author = username;
	msg.author_usernum = citadel::GetOrAssignUserNum(con, username);
	msg.msgtime = (int64_t)std::time(nullptr);
	msg.subject = SummaryOf(incoming);
	msg.format_type = 4;
	msg.node = citadel::GetConfig(con, "c_nodename", "");
	msg.raw = citadel::WrapObject("text/calendar", body, msg.subject, uid, username,
	                              citadel::GetConfig(con, "c_fqdn", "localhost"));

	int64_t msgnum = citadel::UpsertByEuid(con, msg, room, err);
	return msgnum >= 0;
}

std::string CalendarPart(const std::string &raw, std::string &method) {
	method.clear();
	// The cheap check first. Almost no message is a scheduling message, and
	// building the whole part tree for every inbound mail to find that out
	// would be a tax on the delivery path.
	if (raw.find("text/calendar") == std::string::npos &&
	    raw.find("TEXT/CALENDAR") == std::string::npos) {
		return std::string();
	}

	std::string found;
	std::string found_method;
	// Depth-first, first match wins. A message carrying two calendar parts is
	// malformed rather than interesting.
	std::vector<const mime::MimeEntity *> stack;
	mime::MimeEntity root = mime::ParseEntity(raw);
	stack.push_back(&root);
	while (!stack.empty()) {
		const mime::MimeEntity *e = stack.back();
		stack.pop_back();
		if (e->content_type.Mime() == "text/calendar") {
			found = e->body_decoded;
			found_method = util::Upper(e->content_type.Param("method"));
			break;
		}
		for (size_t i = e->children.size(); i > 0; i--) {
			stack.push_back(&e->children[i - 1]);
		}
	}
	if (found.empty()) {
		return std::string();
	}

	if (found_method.empty()) {
		// A client that set the calendar's METHOD but not the Content-Type
		// parameter is common enough that insisting on the parameter would drop
		// real invitations.
		ical::Component root_cal;
		if (ical::Parse(found, root_cal)) {
			found_method = util::Upper(root_cal.Get("METHOD"));
		}
	}
	method = found_method;
	return found;
}

} // namespace itip
} // namespace quackmail
