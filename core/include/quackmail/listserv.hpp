#pragma once

#include "duckdb.hpp"
#include "quackmail/citadel_store.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace quackmail {
namespace listserv {

// The mailing list manager: a Citadel room, redistributed.
//
// Citadel models a list as netconfig entries on a room — `listrecp` for people
// who get every post, `digestrecp` for people who get a periodic batch. This is
// the same model in tables: one `citadel_lists` row per room, subscribers in
// `citadel_list_subs`.
//
// The important design point is *where distribution is driven from*. It would
// be easy to fan out inside the SMTP handler, but then a post made from telnet,
// the web BBS, NNTP or a native ENT0 would reach nobody. So the room is the
// interface: a spooler walks each list room's messages past a watermark and
// enqueues them, exactly the way Citadel's network spooler works. Every
// front-end that can post to a room gets list distribution for free, and
// there is only one path to reason about.
//
// Sending itself is not done here — every copy goes onto quackmail_outbound and
// the existing relay drainer delivers it, so retries and backoff stay in one
// place.

// Create citadel_lists / citadel_list_subs / citadel_list_held. Called from
// store::EnsureSchema, so load order does not matter. Idempotent.
void EnsureSchema(duckdb::Connection &con);

// ---- lists --------------------------------------------------------------

// What a list does with a message posted to it.
enum class Mode {
	Post,   // fan out immediately to every subscriber
	Digest, // batch into a periodic digest only
	Both,   // both, to the subscribers who asked for each
};

// Who may post.
enum class PostPolicy {
	Anyone,      // any sender (the room's own ACL still applies)
	Subscribers, // active subscribers post; everyone else is held
	Moderated,   // everything is held for an aide
};

struct List {
	int64_t room_num = 0;
	std::string address;      // local part mail reaches it at; "" = room_<name>
	std::string display_name; // the room's display name, filled in on read
	bool enabled = true;
	Mode mode = Mode::Post;
	PostPolicy post_policy = PostPolicy::Subscribers;
	bool reply_to_list = false; // set Reply-To: to the list rather than the author
	std::string subject_tag;    // "[quackcit]" — prepended when not already there
	std::string footer;         // appended to the text part of every copy
	int64_t digest_interval_secs = 86400;
	int64_t digest_max = 50;
	int64_t last_sent = 0;   // highest msgnum fanned out
	int64_t last_digest = 0; // highest msgnum digested
	int64_t last_digest_at = 0;

	List() = default;
};

bool GetList(duckdb::Connection &con, int64_t room_num, List &out);
std::vector<List> ListLists(duckdb::Connection &con, bool enabled_only = false);
// Create or update. Fails if room_num names no room, or if `address` collides
// with another list or with a local user.
bool SetList(duckdb::Connection &con, const List &list, std::string &err);
bool RemoveList(duckdb::Connection &con, int64_t room_num, std::string &err);
// Set one field by name, for the admin CLI and the web form. Recognised keys
// mirror the struct: address, enabled, mode, post_policy, reply_to, subject_tag,
// footer, digest_interval, digest_max.
bool SetField(duckdb::Connection &con, int64_t room_num, const std::string &key,
              const std::string &value, std::string &err);

// The address a list is reachable at, with domain: "<address>@<c_fqdn>".
std::string ListAddress(duckdb::Connection &con, const List &list);

// ---- addressing ---------------------------------------------------------

// What an inbound envelope recipient turned out to be.
struct Command {
	enum Kind {
		None,        // not a list address at all
		Post,        // <list>@ — an ordinary post
		Subscribe,   // <list>-subscribe@
		Unsubscribe, // <list>-unsubscribe@
		Confirm,     // <list>-confirm-<token>@
		Help,        // <list>-request@ / <list>-help@
		Bounce,      // <list>-bounces@ — swallowed, never delivered
	};
	Kind kind = None;
	int64_t room_num = -1;
	std::string token;

	bool IsCommand() const {
		return kind != None && kind != Post;
	}
	Command() = default;
};

// Classify the local part of an envelope recipient. Returns false when it names
// no list. The suffix forms are checked before the bare name, so a list called
// "x-subscribe" cannot shadow another list's subscribe address.
bool ResolveAddress(duckdb::Connection &con, const std::string &local_part, List &list, Command &cmd);

// The envelope sender every outgoing copy carries: "<address>-bounces@<fqdn>".
// Delivery failures must land on the list, not on whoever happened to post.
std::string BounceAddress(duckdb::Connection &con, const List &list);

// ---- subscribers --------------------------------------------------------

enum class SubKind {
	Post,   // every message, as it is posted
	Digest, // periodic batch
};

enum class SubState {
	Pending,      // asked to join; awaiting confirmation
	Active,       // receiving
	UnsubPending, // asked to leave; awaiting confirmation
};

struct Sub {
	int64_t room_num = 0;
	std::string address;
	SubKind kind = SubKind::Post;
	SubState state = SubState::Pending;
	int64_t created_at = 0;
	int64_t confirmed_at = 0;

	Sub() = default;
};

// `state` empty returns every subscriber regardless of state.
std::vector<Sub> Subscribers(duckdb::Connection &con, int64_t room_num, const std::string &state = "active");
bool IsSubscriber(duckdb::Connection &con, int64_t room_num, const std::string &address);

// Add a subscriber. With `confirmed` the row goes straight to active — that is
// the admin path. Without it the row is Pending and `token` receives a fresh
// confirmation secret the caller must mail to `address` and nowhere else: the
// whole point is that only someone who reads that mailbox can complete it.
bool Subscribe(duckdb::Connection &con, const List &list, const std::string &address, SubKind kind,
               bool confirmed, std::string &token, std::string &err);
// The mirror image. `confirmed` removes the row outright.
bool Unsubscribe(duckdb::Connection &con, const List &list, const std::string &address, bool confirmed,
                 std::string &token, std::string &err);
// Complete a pending subscribe or unsubscribe. `what` receives a human-readable
// description of what happened, for the reply mail. Tokens expire.
bool Confirm(duckdb::Connection &con, const std::string &token, std::string &what, std::string &err);

// ---- moderation ---------------------------------------------------------

struct Held {
	int64_t id = 0;
	int64_t room_num = 0;
	std::string mail_from;
	std::string subject;
	std::string raw;
	int64_t received_at = 0;
	std::string state; // "held" | "approved" | "rejected"
};

// Park a message for an aide instead of delivering it. Posts a notice into the
// Aide room.
bool Hold(duckdb::Connection &con, const List &list, const std::string &mail_from,
          const std::string &raw, std::string &err);
std::vector<Held> HeldMessages(duckdb::Connection &con, int64_t room_num, const std::string &state = "held");
// Approve: post it into the room. The spooler distributes it on its next pass —
// approval deliberately does not send anything itself, so there is exactly one
// distribution path.
bool Approve(duckdb::Connection &con, int64_t id, std::string &err);
bool Reject(duckdb::Connection &con, int64_t id, std::string &err);

// ---- distribution -------------------------------------------------------

// Rewrite a stored message into the copy subscribers receive: RFC 2369/2919
// List-* headers, the subject tag, the footer, and Reply-To. Pure apart from
// reading the list's own row, so it is assertable from SQL with no socket.
std::string RenderForList(duckdb::Connection &con, const List &list, const citadel::Message &msg);

struct SpoolResult {
	int64_t distributed = 0; // messages fanned out
	int64_t recipients = 0;  // copies enqueued
	int64_t digests = 0;     // digest batches built
	int64_t held = 0;        // messages parked for moderation
};

// Fan out one list's new messages, and build its digest if one is due.
bool SpoolRoom(duckdb::Connection &con, int64_t room_num, SpoolResult &out, std::string &err);
// Every enabled list. This is the worker's tick.
bool SpoolOnce(duckdb::Connection &con, SpoolResult &out, std::string &err);

// ---- self-service -------------------------------------------------------

// Act on a command address and queue the reply. `body` is the inbound message
// (unused for now beyond logging, but subscribe-by-body is the obvious
// extension). Returns false only on a storage error — an unrecognised request
// still produces a helpful reply.
bool HandleCommand(duckdb::Connection &con, const List &list, const Command &cmd,
                   const std::string &mail_from, const std::string &body, std::string &err);

// Send the confirmation mail for a pending subscribe/unsubscribe. Exposed so
// the web front-end's self-service form can use the same wording as the mail
// interface.
bool SendConfirmation(duckdb::Connection &con, const List &list, const std::string &address,
                      const std::string &token, bool subscribing);

} // namespace listserv
} // namespace quackmail
