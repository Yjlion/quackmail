#pragma once

#include "duckdb.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace quackmail {
namespace citadel {

// Room attribute flags. Values match Citadel's canonical QR_* bitmask so the
// numbers we put on the wire (LKRA/GOTO/GETR) are byte-compatible with a real
// Citadel server and its clients.
enum QRFlags {
	QR_PERMANENT = 2,      // do not auto-purge when empty
	QR_PRIVATE = 4,        // invitation-only / hidden
	QR_PASSWORDED = 8,
	QR_GUESSNAME = 16,
	QR_DIRECTORY = 32,
	QR_UPLOAD = 64,
	QR_DOWNLOAD = 128,
	QR_VISDIR = 256,
	QR_NETWORK = 2048,
	QR_PREFONLY = 4096,
	QR_READONLY = 8192,
	QR_MAILBOX = 16384, // personal mailbox room (owned by one user)
};

// Per-user, per-room state bits (citadel_room_state.flags).
enum RoomStateFlags {
	RS_ZAPPED = 1,   // user has forgotten this room; it drops out of listings
	RS_UNLOCKED = 2, // user has given the password of a QR_PASSWORDED room
};

// User attribute flags (citadel_users.flags). Values match Citadel's canonical
// US_* bitmask (libcitadel.h), for the same reason QR_* does.
enum USFlags {
	US_NEEDVALID = 1,    // account awaits aide validation
	US_EXTEDIT = 2,      // always use an external editor
	US_PERM = 4,         // permanent user
	US_LASTOLD = 16,     // print the last old message along with the new ones
	US_EXPERT = 32,      // experienced user: suppress the menu
	US_UNLISTED = 64,    // hide from the user listing
	US_NOPROMPT = 128,   // do not prompt after each message
	US_PROMPTCTL = 256,  // <N>ext and <S>top work at the message prompt
	US_DISAPPEAR = 512,  // disappearing message prompts
	US_REGIS = 1024,     // user has filled in their registration
	US_PAGINATOR = 2048, // pause after each screenful
	US_INTERNET = 4096,  // internet mail privileges
	US_FLOORS = 8192,    // user wants to see floors
	US_COLOR = 16384,    // user wants ANSI colour
};

// The subset a user may set for themselves (Citadel's US_USER_SET).
constexpr int64_t kUserSettableFlags = US_LASTOLD | US_EXPERT | US_UNLISTED | US_NOPROMPT | US_PROMPTCTL |
                                       US_DISAPPEAR | US_PAGINATOR | US_COLOR;

// Reserved room numbers (fixed, seeded ids) — match Citadel's low room numbers.
constexpr int64_t kLobbyRoom = 0;
constexpr int64_t kAideRoom = 1;

// Access level at which a user is an aide (system administrator).
constexpr int64_t kAideAxLevel = 6;

// Citadel default_view codes (VIEW_*): what kind of content a room holds.
//
// These numbers go on the wire in GETR/SETR, where a real Citadel client reads
// them, so they are not ours to choose. Transcribed from the ROOM_VIEWS enum in
// libcitadel/lib/libcitadel.h on the parity oracle — verify there, not against
// documentation, before adding to this list.
//
// Not every code has a renderer here. An unimplemented view falls back to the
// ordinary message list, which is always a truthful way to show a room: the
// objects really are messages.
enum RoomView {
	VIEW_BBS = 0,     // ordinary message board
	VIEW_MAILBOX = 1, // mail folder
	VIEW_ADDRESSBOOK = 2,
	VIEW_CALENDAR = 3,
	VIEW_TASKS = 4,
	VIEW_NOTES = 5,
	VIEW_WIKI = 6,
	VIEW_CALBRIEF = 7, // the calendar, listed rather than gridded
	VIEW_JOURNAL = 8,
	VIEW_DRAFTS = 9,
	VIEW_BLOG = 10,
	VIEW_QUEUE = 11, // Citadel's own SMTP spool view, not a user view
};

struct Room {
	int64_t room_num = 0;
	std::string name;         // unique internal key ("Lobby", "<usernum>.Mail")
	std::string display_name; // shown to clients ("Lobby", "Mail")
	int64_t floor_num = 0;
	int64_t qr_flags = 0;
	std::string password;
	int64_t listorder = 0;
	int64_t default_view = 0;
	std::string info;
	int64_t mailbox_owner = 0; // usernum for personal rooms; 0 = public/shared
	int64_t highest_msg = 0;
};

struct Floor {
	int64_t floor_num = 0;
	std::string name;
	int64_t room_count = 0;
};

// A stored Citadel message. `raw` holds the canonical bytes (RFC822 for
// format_type 4, plain body text for the native format 0); the other columns
// are denormalized for cheap listing.
struct Message {
	int64_t msgnum = 0;
	std::string euid;
	std::string author;
	int64_t author_usernum = 0;
	std::string recipient; // set for personal mail
	std::string node;
	int64_t msgtime = 0; // unix seconds
	std::string subject;
	int format_type = 0; // 0 = Citadel, 1 = fixed, 4 = RFC822/MIME
	std::string references;
	std::string origin_room; // display name of the room it was posted to
	std::string raw;
};

struct RoomStats {
	int64_t total = 0;
	int64_t new_count = 0;
	int64_t highest = 0;
	int64_t last_read = 0;
};

// Create the citadel_* tables and seed floor 0, the Lobby/Aide rooms, and the
// default config. Idempotent; safe to call from every extension on load.
void EnsureCitadelSchema(duckdb::Connection &con);

// ---- config -------------------------------------------------------------
std::string GetConfig(duckdb::Connection &con, const std::string &name, const std::string &dflt = "");

// ---- users --------------------------------------------------------------
// Return the user's numeric id, assigning (and persisting) one on first use.
// Returns 0 if the username is not a known local user.
int64_t GetOrAssignUserNum(duckdb::Connection &con, const std::string &username);
int64_t GetAxLevel(duckdb::Connection &con, const std::string &username);

// The Citadel user record joined with its credential row. `enabled` comes from
// quackmail_users; everything else from citadel_users.
struct UserInfo {
	std::string username;
	int64_t usernum = 0;
	int64_t axlevel = 4;
	int64_t flags = 0; // US_* bitmask
	int64_t times_called = 0;
	int64_t num_posts = 0;
	int64_t last_call = 0; // unix seconds, 0 if never
	bool enabled = false;
	int64_t screenwidth = 80;
	int64_t screenheight = 24;
};

std::vector<UserInfo> ListUsers(duckdb::Connection &con);
bool GetUser(duckdb::Connection &con, const std::string &username, UserInfo &out);
bool SetAxLevel(duckdb::Connection &con, const std::string &username, int64_t axlevel, std::string &err);
bool SetUserFlags(duckdb::Connection &con, const std::string &username, int64_t flags);
bool SetScreenSize(duckdb::Connection &con, const std::string &username, int64_t width, int64_t height);
// Bump times_called and stamp last_call. Call once per login.
void RecordCall(duckdb::Connection &con, const std::string &username);

// Citadel's REGI registration record, in REGI's field order, plus the free-text
// biography EBIO/RBIO carry.
struct Registration {
	std::string real_name;
	std::string street;
	std::string city;
	std::string state;
	std::string zipcode;
	std::string phone;
	std::string email;
	std::string country;
	std::string bio;
};

bool GetRegistration(duckdb::Connection &con, const std::string &username, Registration &out);
bool SetRegistration(duckdb::Connection &con, const std::string &username, const Registration &reg);
// Replace only the biography, leaving the registration fields alone.
bool SetBio(duckdb::Connection &con, const std::string &username, const std::string &bio);
// True when `addr` is deliverable locally: its domain (if present) matches the
// configured c_fqdn and its local-part is a known local user. Used by the SMTP
// front-ends to accept local mail vs. reject unknown users / deny relay.
bool IsLocalUser(duckdb::Connection &con, const std::string &addr);

// ---- floors -------------------------------------------------------------
std::vector<Floor> ListFloors(duckdb::Connection &con);
bool GetFloor(duckdb::Connection &con, int64_t floor_num, Floor &out);
int64_t CreateFloor(duckdb::Connection &con, const std::string &name, std::string &err);
bool RenameFloor(duckdb::Connection &con, int64_t floor_num, const std::string &name, std::string &err);
// Refuses floor 0 and any floor that still holds rooms.
bool KillFloor(duckdb::Connection &con, int64_t floor_num, std::string &err);

// ---- rooms --------------------------------------------------------------
// Resolve a client-supplied room name (matches a public room's display name, or
// the logged-in user's own mailbox room). Returns false if not found/visible.
bool ResolveRoom(duckdb::Connection &con, const std::string &username, const std::string &wanted, Room &out);
bool GetRoomByNum(duckdb::Connection &con, int64_t room_num, Room &out);
// which: "all" | "new" | "old" | "zapped". Zapped rooms are excluded from the
// first three and are the only thing "zapped" returns. floor < 0 = every floor.
// Rooms come back in internal-key order, which puts every personal room before
// the public ones — the order a real Citadel server's LKRA produces.
std::vector<Room> ListRooms(duckdb::Connection &con, const std::string &username, int64_t floor,
                            const std::string &which);
int64_t CreateRoom(duckdb::Connection &con, const std::string &display_name, int64_t floor, int64_t qr_flags,
                   const std::string &password, int64_t mailbox_owner, std::string &err);
// Update the mutable attributes of an existing room (everything but room_num,
// mailbox_owner and highest_msg). Renames the internal key to match.
bool UpdateRoom(duckdb::Connection &con, const Room &room, std::string &err);
bool KillRoom(duckdb::Connection &con, int64_t room_num, std::string &err);
// Get (creating if needed) a personal room owned by a user, identified by its
// display name (e.g. "Mail", or a Sieve fileinto folder). Returns room_num.
int64_t GetOrCreateUserRoom(duckdb::Connection &con, const std::string &username,
                            const std::string &display_name);
// Convenience: the user's personal "Mail" room.
int64_t GetOrCreateMailRoom(duckdb::Connection &con, const std::string &username);
// Look up one of a user's personal rooms without creating it. Returns -1 when
// the user has no such room. Matches on the user-scoped internal key, so it can
// never return a public room that happens to share the display name.
int64_t FindUserRoom(duckdb::Connection &con, const std::string &username,
                     const std::string &display_name);
// Provision the full set of default personal rooms Citadel gives every user
// (Mail, Sent Items, Drafts, Trash, Calendar, Contacts, Notes, Tasks) with the
// correct default_view. Idempotent; call on login / user creation.
void EnsureUserRooms(duckdb::Connection &con, const std::string &username);

// ---- newsgroup naming (NNTP) --------------------------------------------
// Citadel's room <-> newsgroup mapping (serv_nntp.c). A room name is used
// verbatim when it is already a valid newsgroup name (no "ctdl." prefix, only
// [alnum.+-], at least one letter and at least one dot); otherwise it becomes
// "ctdl." + the lowercased name with every other byte escaped as "+HH".
std::string RoomToNewsgroup(const std::string &room_name);
// The inverse: strips "ctdl." and decodes "+HH"; other names pass through.
std::string NewsgroupToRoom(const std::string &newsgroup);

// ---- live sessions (presence) -------------------------------------------
// Sessions and instant messages live in DuckDB tables, not in process memory, so
// every front-end (native Citadel, telnet, XMPP, ...) sees the same presence.
struct SessionInfo {
	int64_t session_id = 0;
	std::string username;
	std::string host;
	std::string room;
	std::string last_cmd;
	std::string client; // e.g. "Citadel client protocol", "Telnet session"
	int64_t axlevel = 0;
	int64_t since = 0;
	int64_t last_seen = 0;
};

// Register a connection and return its session id (0 on failure). `host` is the
// peer address (net::ClientStream::PeerIp()); loopback is stored as "localhost",
// matching what a real Citadel server shows in RWHO.
int64_t RegisterSession(duckdb::Connection &con, const std::string &client, const std::string &host = "");
// Refresh the row after each command (also what makes the session visible in RWHO).
void TouchSession(duckdb::Connection &con, int64_t session_id, const std::string &username,
                  const std::string &room, const std::string &last_cmd, int64_t axlevel);
void UnregisterSession(duckdb::Connection &con, int64_t session_id);
std::vector<SessionInfo> ListSessions(duckdb::Connection &con);

// ---- express (instant) messages -----------------------------------------
struct Express {
	int64_t id = 0;
	std::string from_user;
	std::string text;
	int64_t sent_at = 0;
};

// Queue an instant message. Returns false if `to` is not a local user.
bool SendExpress(duckdb::Connection &con, const std::string &to, const std::string &from,
                 const std::string &text);
// Undelivered messages for a user, oldest first.
std::vector<Express> PendingExpress(duckdb::Connection &con, const std::string &user);
void MarkExpressDelivered(duckdb::Connection &con, int64_t id);

// ---- per-user room read state ------------------------------------------
RoomStats GetRoomStats(duckdb::Connection &con, const std::string &username, int64_t room_num);
// One query for many rooms. GetRoomStats costs three or four scalar queries, so
// per-room calls make drawing a room list O(4N); listings should use this.
// The result is parallel to `room_nums`.
std::vector<RoomStats> RoomStatsBulk(duckdb::Connection &con, const std::string &username,
                                     const std::vector<int64_t> &room_nums);
void SetLastRead(duckdb::Connection &con, const std::string &username, int64_t room_num, int64_t msgnum);

// Zap (forget) a room, or un-zap it. Refuses the Lobby, as Citadel does.
bool ZapRoom(duckdb::Connection &con, const std::string &username, int64_t room_num, bool zapped,
             std::string &err);
bool IsZapped(duckdb::Connection &con, const std::string &username, int64_t room_num);

// May this user read a room's contents? False only for a QR_PASSWORDED room
// whose password they have not yet given. Every front-end must ask before
// listing or serving messages — resolving a room by name or number is not by
// itself permission to read it.
bool RoomUnlocked(duckdb::Connection &con, const std::string &username, const Room &room);
// Record a correct password. Returns false if the password does not match.
bool UnlockRoom(duckdb::Connection &con, const std::string &username, const Room &room,
                const std::string &password);

// ---- access control lists (RFC 4314) ------------------------------------
//
// Rights are *derived* from the room's Citadel attributes (owner, aide,
// QR_PRIVATE/QR_READONLY/QR_PASSWORDED) and then unioned with any explicit
// citadel_room_acl row. Deriving rather than storing keeps qr_flags the single
// source of truth for ordinary permissions; the table only adds grants Citadel
// has nowhere else to put — above all "anyone" + `p`, which is what makes a
// room reachable by e-mail.
//
// Because the two are unioned, an ACL row can only widen access, never narrow
// it: revoking a user's read access is still a matter of the room's flags.

// The RFC 4314 rights letters in canonical order: lookup, read, keep-seen,
// write-flags, insert, post, create, delete-mailbox, delete-message, expunge,
// administer.
extern const char *const kAclRights;

// `username` may be empty or "anyone" for an unauthenticated/gateway caller, in
// which case nothing is derived and only stored grants apply.
std::string EffectiveRights(duckdb::Connection &con, const std::string &username, const Room &room);
// Replace the stored grant for one identifier. Empty `rights` removes the row.
bool SetRights(duckdb::Connection &con, const Room &room, const std::string &identifier,
               const std::string &rights, std::string &err);
// The stored grants only (identifier, rights) — what GETACL reports.
std::vector<std::pair<std::string, std::string>> ListRights(duckdb::Connection &con, const Room &room);

// May `username` post into `room`? An empty username is an anonymous/gateway
// sender and needs the explicit `p` (post) right; a logged-in user posts with
// `i` (insert), the right APPEND and COPY use. Every front-end that accepts a
// new message must ask — resolving a room is not permission to write to it.
bool CanPost(duckdb::Connection &con, const std::string &username, const Room &room);

// Resolve the local-part of a public room address ("room_the_lobby") to the
// room it names. Returns -1 unless the room exists, is public (not a mailbox,
// not QR_PRIVATE, not QR_PASSWORDED) *and* its ACL grants `p` to "anyone" —
// so no room is reachable by mail until an aide opts it in with SETACL.
int64_t ResolveMailRoom(duckdb::Connection &con, const std::string &local_part);

// ---- messages -----------------------------------------------------------
// Insert a message and point it into each of `rooms`. Bumps each room's
// highest_msg. Returns the new msgnum, or -1 on error (with err set).
int64_t InsertMessage(duckdb::Connection &con, const Message &msg, const std::vector<int64_t> &rooms,
                      std::string &err);
// Message numbers pointed into a room, ascending. filter: "all" | "new" | "old"
// | "last" | "first" | "gt" | "lt" (param is the count/threshold for the last four).
std::vector<int64_t> RoomMessages(duckdb::Connection &con, int64_t room_num, const std::string &filter,
                                  int64_t param, int64_t last_read);
bool LoadMessage(duckdb::Connection &con, int64_t msgnum, Message &out);
// True when `msgnum` is pointed into `room_num`. Every front-end that takes a
// message number from a client must check this against a room the caller is
// allowed to see before loading the message — LoadMessage itself has no notion
// of ownership.
bool MessageInRoom(duckdb::Connection &con, int64_t room_num, int64_t msgnum);
// Unlink a message from a room. The citadel_messages row is left behind (it may
// still be pointed into other rooms); an unreferenced row is harmless.
bool DeleteMessage(duckdb::Connection &con, int64_t room_num, int64_t msgnum, std::string &err);
// Point a message into `to_room`, unlinking it from `from_room` unless is_copy.
bool MoveMessage(duckdb::Connection &con, int64_t from_room, int64_t to_room, int64_t msgnum, bool is_copy,
                 std::string &err);

// ---- per-user string preferences ----------------------------------------
// The US_* bit field holds the boolean BBS toggles; this holds anything with a
// value. An unset preference returns `dflt`, which is how "follow the site
// default" is expressed — SetUserPref with an empty value clears the row.
std::string GetUserPref(duckdb::Connection &con, const std::string &username, const std::string &name,
                        const std::string &dflt = "");
bool SetUserPref(duckdb::Connection &con, const std::string &username, const std::string &name,
                 const std::string &value);

// ---- system messages ----------------------------------------------------
// Post a notice into the Aide room, authored by the node itself. This is the
// server's log-to-the-BBS channel: new users, aide actions, listeners starting.
// A no-op when the `qm_aide_log` setting is off, and it swallows its own
// errors — a system message must never fail the operation that triggered it.
void PostAideMessage(duckdb::Connection &con, const std::string &subject, const std::string &text);

} // namespace citadel
} // namespace quackmail
