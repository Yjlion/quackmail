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

// Reserved room numbers (fixed, seeded ids) — match Citadel's low room numbers.
constexpr int64_t kLobbyRoom = 0;
constexpr int64_t kAideRoom = 1;

// Citadel default_view codes (VIEW_*): what kind of content a room holds.
enum RoomView {
	VIEW_BBS = 0,       // ordinary message board
	VIEW_MAILBOX = 1,   // mail folder
	VIEW_ADDRESSBOOK = 2,
	VIEW_CALENDAR = 3,
	VIEW_TASKS = 4,
	VIEW_NOTES = 5,
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
// True when `addr` is deliverable locally: its domain (if present) matches the
// configured c_fqdn and its local-part is a known local user. Used by the SMTP
// front-ends to accept local mail vs. reject unknown users / deny relay.
bool IsLocalUser(duckdb::Connection &con, const std::string &addr);

// ---- floors -------------------------------------------------------------
std::vector<Floor> ListFloors(duckdb::Connection &con);
int64_t CreateFloor(duckdb::Connection &con, const std::string &name, std::string &err);

// ---- rooms --------------------------------------------------------------
// Resolve a client-supplied room name (matches a public room's display name, or
// the logged-in user's own mailbox room). Returns false if not found/visible.
bool ResolveRoom(duckdb::Connection &con, const std::string &username, const std::string &wanted, Room &out);
bool GetRoomByNum(duckdb::Connection &con, int64_t room_num, Room &out);
std::vector<Room> ListRooms(duckdb::Connection &con, const std::string &username, int64_t floor,
                            const std::string &which); // which: "all" | "new" | "old"
int64_t CreateRoom(duckdb::Connection &con, const std::string &display_name, int64_t floor, int64_t qr_flags,
                   const std::string &password, int64_t mailbox_owner, std::string &err);
bool KillRoom(duckdb::Connection &con, int64_t room_num, std::string &err);
// Get (creating if needed) a personal room owned by a user, identified by its
// display name (e.g. "Mail", or a Sieve fileinto folder). Returns room_num.
int64_t GetOrCreateUserRoom(duckdb::Connection &con, const std::string &username,
                            const std::string &display_name);
// Convenience: the user's personal "Mail" room.
int64_t GetOrCreateMailRoom(duckdb::Connection &con, const std::string &username);
// Provision the full set of default personal rooms Citadel gives every user
// (Mail, Sent Items, Drafts, Trash, Calendar, Contacts, Notes, Tasks) with the
// correct default_view. Idempotent; call on login / user creation.
void EnsureUserRooms(duckdb::Connection &con, const std::string &username);

// ---- per-user room read state ------------------------------------------
RoomStats GetRoomStats(duckdb::Connection &con, const std::string &username, int64_t room_num);
void SetLastRead(duckdb::Connection &con, const std::string &username, int64_t room_num, int64_t msgnum);

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

} // namespace citadel
} // namespace quackmail
