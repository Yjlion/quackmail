#define DUCKDB_EXTENSION_MAIN

#include "quackmail_telnet_extension.hpp"

#include "xmodem.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/citadel_msg.hpp"
#include "quackmail/citadel_store.hpp"
#include "quackmail/filearea.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/telnet.hpp"
#include "quackmail/util.hpp"
#include "quackmail/wildmat.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace duckdb {
namespace {

using namespace quackmail;

// telnet (23; dev 2300) and telnets (992; dev 2992, implicit TLS). A real
// Citadel install has no telnet listener of its own — the BBS experience is the
// text client talking the native protocol — so this module *is* that client,
// running server-side. The command set follows the `cmd=` table in citadel.rc,
// which is what the official client offers; the second field of each entry is
// the access level a command needs (0 anyone, 1 room aide, 2 aide).
ServerController g_telnet;
ServerController g_telnets;

constexpr size_t kMaxPostBytes = 256 * 1024;

int64_t NowEpoch() {
	return (int64_t)std::time(nullptr);
}

// A room the session has visited, with the read pointer it had on arrival, so
// <U>ngoto and <A>bandon can put it back the way it was.
struct Visit {
	int64_t room_num = 0;
	int64_t last_read = 0;

	Visit() = default;
	Visit(int64_t r, int64_t lr) : room_num(r), last_read(lr) {
	}
};

struct Bbs {
	int64_t session_id = 0;
	bool authed = false;
	std::string username;
	int64_t usernum = 0;
	int64_t axlevel = 0;
	int64_t flags = 0; // US_* bitmask, persisted in citadel_users.flags

	citadel::Room room;
	bool have_room = false;
	int64_t entry_last_read = 0; // read pointer when the current room was entered
	std::vector<Visit> ungoto;   // room history for <U>ngoto

	bool quiet = false;   // <Q>uiet mode: suppress instant messages
	bool stealth = false; // .Wholist Stealth: hide from the who-list

	bool Expert() const {
		return (flags & citadel::US_EXPERT) != 0;
	}
	bool FloorMode() const {
		return (flags & citadel::US_FLOORS) != 0;
	}
	bool Paginate() const {
		return (flags & citadel::US_PAGINATOR) != 0;
	}
	bool IsAide() const {
		return axlevel >= citadel::kAideAxLevel;
	}
};

// ---------------------------------------------------------------- rendering

std::string TimeString(int64_t epoch) {
	time_t t = (time_t)epoch;
	struct tm tm {};
	localtime_r(&t, &tm);
	char buf[64];
	if (std::strftime(buf, sizeof buf, "%a %b %e %H:%M:%S %Y", &tm) == 0) {
		return "";
	}
	return buf;
}

// The room prompt character: ']' if the room is a directory, '>' otherwise (the
// same rule as room_prompt() in the text client).
char PromptChar(const citadel::Room &room) {
	return (room.qr_flags & citadel::QR_DIRECTORY) ? ']' : '>';
}

// ---- help ----------------------------------------------------------------
//
// Compiled-in text, overridable from a `Help` room. Real Citadel reads help out
// of a directory on disk; this server is a single loadable extension with no
// data directory to install, which is the same reason http/assets/ is compiled
// in.
//
// The override is a euid-keyed message in a room named Help, so an aide can
// rewrite a topic from any front-end that can post — and a site that never
// touches it still ships working help rather than an empty screen. Nothing has
// to be seeded for that to be true.

struct HelpTopic {
	const char *key;
	const char *title;
	const char *body;
};

const HelpTopic kHelpTopics[] = {
    {"rooms", "Rooms and floors",
     "Rooms hold messages; floors group rooms.\n\n"
     "  <G>oto      the next room with unread messages\n"
     "  <S>kip      leave this room unread and move on\n"
     "  <K>nown     list the rooms you can see\n"
     "  <Z>ap       forget a room; it stops appearing in <G>oto\n"
     "  <U>ngoto    go back to the room you came from\n"
     "  <A>bandon   leave without marking anything read\n"
     "  <;>         floor commands: <;G>oto, <;K>nown, <;C>onfigure\n"},
    {"messages", "Reading and writing",
     "  <N>ew       unread messages in this room\n"
     "  <F>orward   every message, oldest first\n"
     "  <R>everse   every message, newest first\n"
     "  <L>ast five the five most recent\n"
     "  <E>nter     post a message\n"
     "  <D>elete    remove one (yours, or any if you are an aide)\n"},
    {"files", "File directories",
     "A room may be a file directory. In one:\n\n"
     "  <.RF>       list the files\n"
     "  <.RFG>      fetch one -- type it out, Xmodem, or base64\n"
     "  <.EF>       upload one\n"
     "  <.AFD>      delete one (aide)\n"
     "  <.AFE>      change a description (aide)\n\n"
     "Xmodem-1K with CRC is the transfer protocol. The same files are\n"
     "reachable over WebDAV and FTP, which is what to use for anything\n"
     "large or for a whole directory at once.\n"},
    {"chat", "Talking to people",
     "  <W>ho       who is online now\n"
     "  <P>age      send someone an instant message\n"
     "  <C>hat      hold a conversation\n"
     "  <Q>uiet     stop other people paging you\n\n"
     "Chat, paging, the web chat page and XMPP are all the same\n"
     "conversation: a message sent from here reaches someone reading\n"
     "their mail in a browser, and their reply comes back here.\n"},
    {"account", "Your account",
     "  <.EP>       change your password\n"
     "  <.EC>       terminal width, height, expert mode\n"
     "  <.EG>       registration details\n"
     "  <.EB>       your biography\n"
     "  <X>         toggle expert mode (hides the menu)\n"},
};

// A topic's override, if an aide wrote one. "" means use the compiled text.
std::string HelpOverride(Connection &con, const std::string &key) {
	citadel::Room help_room;
	if (!citadel::ResolveRoom(con, std::string(), "Help", help_room)) {
		return std::string();
	}
	int64_t msgnum = citadel::FindByEuid(con, help_room.room_num, "help/" + key);
	if (msgnum <= 0) {
		return std::string();
	}
	citadel::Message msg;
	if (!citadel::LoadMessage(con, msgnum, msg)) {
		return std::string();
	}
	return citadel::BodyText(msg);
}

void ShowHelp(Connection &con, telnet::Session &t, const std::string &want) {
	t.ResetPager();
    std::string key = quackmail::util::Lower(want);
	if (key.empty()) {
		std::string out = "\nHelp topics:\n\n";
		for (const auto &h : kHelpTopics) {
			std::string k = h.key;
			k.resize(std::max<size_t>(k.size(), 12), ' ');
			out += "  " + k + h.title + "\n";
		}
		out += "\nType .H <topic>, or <?> for the command menu.\n";
		t.Page(out);
		return;
	}
	for (const auto &h : kHelpTopics) {
		if (key != h.key) {
			continue;
		}
		std::string body = HelpOverride(con, h.key);
		t.Page("\n" + std::string(h.title) + "\n\n" + (body.empty() ? std::string(h.body) : body));
		return;
	}
	t.Write("\nNo help on \"" + want + "\". Type .H for the list of topics.\n");
}

void ShowMenu(telnet::Session &t, const Bbs &s) {
	t.Write("-----------------------------------------------------------------------\n"
	        "Room cmds:    <K>nown rooms, <G>oto next room, <.G>oto a specific room,\n"
	        "              <S>kip this room, <A>bandon, <U>ngoto, <Z>ap (forget)\n"
	        "              <+>/<-> next/previous room, <>>/<<> next/previous floor\n"
	        "Message cmds: <N>ew msgs, <F>orward read, <O>ld msgs, <R>everse read,\n"
	        "              <L>ast five msgs, <E>nter a message, <D>elete a message\n"
	        "General cmds: <?> help, <T>erminate, <W>ho is online, <P>age a user,\n"
	        "              <C>hat, <M>ail, <I>nfo, <Q>uiet mode\n"
	        "Floors:       <;> floor commands (<;C>onfigure, <;G>oto, <;K>nown)\n"
	        "Dot cmds:     <.K>nown, <.R>ead, <.E>nter, <.W>holist, <.H>elp");
	if (s.IsAide()) {
		t.Write(", <.A>dmin");
	}
	t.Write("\n"
	        "Misc:         <X> toggle eXpert mode\n"
	        "\n"
	        " (Type <X> to hide this menu)\n"
	        "-----------------------------------------------------------------------\n");
}

// Re-read the user's persisted preferences into the session.
void LoadUser(Connection &con, Bbs &s) {
	citadel::UserInfo info;
	if (!citadel::GetUser(con, s.username, info)) {
		return;
	}
	s.usernum = info.usernum;
	s.axlevel = info.axlevel;
	s.flags = info.flags;
}

// Push the user's persisted preferences onto the terminal. Called after login
// and again whenever <.EC>onfigure changes them.
void ApplyPrefs(Connection &con, Bbs &s, telnet::Session &t) {
	// US_COLOR has been settable from .EC and the web console since those
	// existed; this is what finally makes it do something. telnet::Session still
	// refuses to emit escapes to a terminal that called itself dumb.
	t.SetColor((s.flags & citadel::US_COLOR) != 0);
	if (!s.Paginate()) {
		t.SetPageSize(0);
		return;
	}
	int height = t.Height();
	if (!t.HaveNaws()) {
		citadel::UserInfo info;
		if (citadel::GetUser(con, s.username, info)) {
			t.SetSize((int)info.screenwidth, (int)info.screenheight);
			height = (int)info.screenheight;
		}
	}
	t.SetPageSize(height);
	t.ResetPager();
}

// ------------------------------------------------------------------- rooms

void EnterRoom(Connection &con, Bbs &s, telnet::Session &t, const citadel::Room &room, bool remember) {
	if (remember && s.have_room) {
		s.ungoto.push_back(Visit(s.room.room_num, s.entry_last_read));
		if (s.ungoto.size() > 64) {
			s.ungoto.erase(s.ungoto.begin());
		}
	}
	s.room = room;
	s.have_room = true;
	auto st = citadel::GetRoomStats(con, s.username, room.room_num);
	s.entry_last_read = st.last_read;
	// Opening a forgotten room brings it back, which is what Citadel does.
	if (citadel::IsZapped(con, s.username, room.room_num)) {
		std::string err;
		citadel::ZapRoom(con, s.username, room.room_num, false, err);
	}
	t.Write("\n" + t.Colour(telnet::Session::Attr::Banner) + room.display_name + PromptChar(room) + "  " +
	        std::to_string(st.new_count) + " new of " + std::to_string(st.total) + " messages" +
	        t.Colour(telnet::Session::Attr::Reset) + "\n");
	if (!room.info.empty()) {
		t.Write(room.info + "\n");
	}
}

// Enforce QR_PASSWORDED before letting a session into a room. Without this any
// passworded room is enterable simply by naming it.
bool Unlock(Connection &con, Bbs &s, telnet::Session &t, const citadel::Room &room) {
	if (citadel::RoomUnlocked(con, s.username, room)) {
		return true;
	}
	std::string pw;
	if (!t.Prompt("\nEnter room password: ", pw)) {
		return false;
	}
	if (!citadel::UnlockRoom(con, s.username, room, pw)) {
		t.Write("Wrong password.\n");
		return false;
	}
	return true;
}

bool GotoNamed(Connection &con, Bbs &s, telnet::Session &t, const std::string &wanted) {
	citadel::Room room;
	if (!citadel::ResolveRoom(con, s.username, wanted, room)) {
		t.Write("\nNo such room.\n");
		return false;
	}
	if (!Unlock(con, s, t, room)) {
		return false;
	}
	EnterRoom(con, s, t, room, true);
	return true;
}

// The rooms this session walks, in order. In floor mode the current floor comes
// first, so <G>oto finishes a floor before moving on, as the text client does.
std::vector<citadel::Room> ScanOrder(Connection &con, Bbs &s) {
	auto rooms = citadel::ListRooms(con, s.username, -1, "all");
	if (!s.FloorMode() || !s.have_room) {
		return rooms;
	}
	std::vector<citadel::Room> ordered;
	for (auto &r : rooms) {
		if (r.floor_num == s.room.floor_num) {
			ordered.push_back(r);
		}
	}
	for (auto &r : rooms) {
		if (r.floor_num != s.room.floor_num) {
			ordered.push_back(r);
		}
	}
	return ordered;
}

// <G>oto: mark this room read, then move to the next one holding unread
// messages. <S>kip does the same walk without touching the read pointer —
// that difference is the whole point of having both.
void GotoNext(Connection &con, Bbs &s, telnet::Session &t, bool mark_read) {
	if (s.have_room && mark_read) {
		auto st = citadel::GetRoomStats(con, s.username, s.room.room_num);
		citadel::SetLastRead(con, s.username, s.room.room_num, st.highest);
	}
	auto rooms = ScanOrder(con, s);
	if (rooms.empty()) {
		t.Write("\nNo rooms.\n");
		return;
	}
	size_t start = 0;
	if (s.have_room) {
		for (size_t i = 0; i < rooms.size(); i++) {
			if (rooms[i].room_num == s.room.room_num) {
				start = i + 1;
				break;
			}
		}
	}
	std::vector<int64_t> nums;
	for (auto &r : rooms) {
		nums.push_back(r.room_num);
	}
	auto stats = citadel::RoomStatsBulk(con, s.username, nums);
	for (size_t n = 0; n < rooms.size(); n++) {
		size_t idx = (start + n) % rooms.size();
		if (stats[idx].new_count > 0) {
			if (!Unlock(con, s, t, rooms[idx])) {
				continue;
			}
			EnterRoom(con, s, t, rooms[idx], true);
			return;
		}
	}
	t.Write("\nNo rooms with new messages. ");
	citadel::Room lobby;
	if (citadel::ResolveRoom(con, s.username, "Lobby", lobby)) {
		EnterRoom(con, s, t, lobby, true);
	}
}

// <+> / <-> — the next or previous room in the list, unread or not.
void StepRoom(Connection &con, Bbs &s, telnet::Session &t, int delta) {
	auto rooms = ScanOrder(con, s);
	if (rooms.empty()) {
		t.Write("\nNo rooms.\n");
		return;
	}
	size_t here = 0;
	for (size_t i = 0; i < rooms.size(); i++) {
		if (s.have_room && rooms[i].room_num == s.room.room_num) {
			here = i;
			break;
		}
	}
	size_t next = (here + rooms.size() + (size_t)((delta > 0) ? 1 : -1)) % rooms.size();
	if (Unlock(con, s, t, rooms[next])) {
		EnterRoom(con, s, t, rooms[next], true);
	}
}

// <>> / <<> — the first room on the next or previous floor.
void StepFloor(Connection &con, Bbs &s, telnet::Session &t, int delta) {
	auto floors = citadel::ListFloors(con);
	if (floors.empty()) {
		t.Write("\nNo floors.\n");
		return;
	}
	size_t here = 0;
	for (size_t i = 0; i < floors.size(); i++) {
		if (s.have_room && floors[i].floor_num == s.room.floor_num) {
			here = i;
			break;
		}
	}
	for (size_t n = 1; n <= floors.size(); n++) {
		size_t idx = (here + floors.size() + n * (size_t)((delta > 0) ? 1 : floors.size() - 1)) %
		             floors.size();
		auto rooms = citadel::ListRooms(con, s.username, floors[idx].floor_num, "all");
		if (rooms.empty()) {
			continue;
		}
		t.Write("\nFloor: " + floors[idx].name + "\n");
		if (Unlock(con, s, t, rooms[0])) {
			EnterRoom(con, s, t, rooms[0], true);
		}
		return;
	}
	t.Write("\nNo other floor has rooms.\n");
}

// Print a set of rooms in the text client's two-column-ish flowed layout.
bool DumpRooms(telnet::Session &t, const std::string &title, const std::vector<std::string> &list) {
	if (list.empty()) {
		return true;
	}
	std::string out = "\n   " + title + "\n";
	std::string line;
	for (auto &e : list) {
		if (line.size() + e.size() > 72) {
			out += line + "\n";
			line.clear();
		}
		line += e;
	}
	if (!line.empty()) {
		out += line + "\n";
	}
	return t.Page(out);
}

std::string RoomEntry(const citadel::Room &room) {
	return room.display_name + PromptChar(room) + "  ";
}

// <K>nown rooms. In floor mode the listing is grouped by floor, which is what
// the ;Configure floor mode toggle is for.
void KnownRooms(Connection &con, Bbs &s, telnet::Session &t) {
	t.ResetPager();
	auto rooms = citadel::ListRooms(con, s.username, -1, "all");
	std::vector<int64_t> nums;
	for (auto &r : rooms) {
		nums.push_back(r.room_num);
	}
	auto stats = citadel::RoomStatsBulk(con, s.username, nums);

	if (!s.FloorMode()) {
		std::vector<std::string> with_new, without_new;
		for (size_t i = 0; i < rooms.size(); i++) {
			(stats[i].new_count > 0 ? with_new : without_new).push_back(RoomEntry(rooms[i]));
		}
		if (!DumpRooms(t, "Rooms with unread messages:", with_new)) {
			return;
		}
		DumpRooms(t, "No unseen messages in:", without_new);
		return;
	}

	for (auto &floor : citadel::ListFloors(con)) {
		std::vector<std::string> here;
		for (size_t i = 0; i < rooms.size(); i++) {
			if (rooms[i].floor_num == floor.floor_num) {
				std::string entry = RoomEntry(rooms[i]);
				if (stats[i].new_count > 0) {
					entry = "*" + entry;
				}
				here.push_back(entry);
			}
		}
		if (!DumpRooms(t, "Floor: " + floor.name, here)) {
			return;
		}
	}
	t.Write("\n(* marks a room with unread messages.)\n");
}

// .Known <filter> — the citadel.rc "Known" submenu: Anonymous, Directory,
// Match:, preferred Only, Private, Read only, Zapped, Floors.
void KnownFiltered(Connection &con, Bbs &s, telnet::Session &t, char which) {
	t.ResetPager();
	if (which == 'F') {
		std::vector<std::string> names;
		for (auto &f : citadel::ListFloors(con)) {
			names.push_back(f.name + " (" + std::to_string(f.room_count) + ")  ");
		}
		DumpRooms(t, "Floors:", names);
		return;
	}
	if (which == 'Z') {
		std::vector<std::string> names;
		for (auto &r : citadel::ListRooms(con, s.username, -1, "zapped")) {
			names.push_back(RoomEntry(r));
		}
		if (names.empty()) {
			t.Write("\nYou have not forgotten any rooms.\n");
			return;
		}
		DumpRooms(t, "Forgotten rooms:", names);
		return;
	}

	std::string pattern;
	if (which == 'M') {
		if (!t.Prompt("\nMatch rooms: ", pattern) || pattern.empty()) {
			return;
		}
	}

	int64_t want = 0;
	const char *title = "Rooms:";
	switch (which) {
	case 'D':
		want = citadel::QR_DIRECTORY;
		title = "Directory rooms:";
		break;
	case 'P':
		want = citadel::QR_PRIVATE;
		title = "Private rooms:";
		break;
	case 'R':
		want = citadel::QR_READONLY;
		title = "Read-only rooms:";
		break;
	case 'O':
		want = citadel::QR_PREFONLY;
		title = "Preferred-only rooms:";
		break;
	case 'A':
		// Citadel's "anonymous rooms" are the guess-name ones.
		want = citadel::QR_GUESSNAME;
		title = "Anonymous rooms:";
		break;
	case 'M':
		title = "Matching rooms:";
		break;
	default:
		break;
	}

	std::vector<std::string> names;
	for (auto &r : citadel::ListRooms(con, s.username, -1, "all")) {
		if (want != 0 && !(r.qr_flags & want)) {
			continue;
		}
		if (which == 'M' && !WildmatMatch(util::Upper(r.display_name), util::Upper(pattern))) {
			continue;
		}
		names.push_back(RoomEntry(r));
	}
	if (names.empty()) {
		t.Write("\nNone.\n");
		return;
	}
	DumpRooms(t, title, names);
}

void Ungoto(Connection &con, Bbs &s, telnet::Session &t) {
	if (s.ungoto.empty()) {
		t.Write("\nNo rooms to ungoto.\n");
		return;
	}
	Visit prev = s.ungoto.back();
	s.ungoto.pop_back();
	// Ungoto restores the read pointer as well as the room: the point is to
	// undo the visit, not merely to walk backwards.
	citadel::SetLastRead(con, s.username, prev.room_num, prev.last_read);
	citadel::Room room;
	if (citadel::GetRoomByNum(con, prev.room_num, room)) {
		EnterRoom(con, s, t, room, false);
	}
}

// <A>bandon: leave this room as if the visit had not happened.
void Abandon(Connection &con, Bbs &s, telnet::Session &t) {
	if (s.have_room) {
		citadel::SetLastRead(con, s.username, s.room.room_num, s.entry_last_read);
	}
	GotoNext(con, s, t, false);
}

void ZapRoom(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.have_room) {
		t.Write("\nNot in a room.\n");
		return;
	}
	bool sure = false;
	if (!t.YesNo("\nForget room '" + s.room.display_name + "'?", false, sure) || !sure) {
		return;
	}
	std::string err;
	if (!citadel::ZapRoom(con, s.username, s.room.room_num, true, err)) {
		t.Write("\n" + err + "\n");
		return;
	}
	t.Write("\nRoom forgotten. It will reappear if you goto it by name.\n");
	GotoNext(con, s, t, false);
}

// ---------------------------------------------------------------- messages

// Render one message into `out`, which the caller then feeds to the pager.
void ShowMessage(const citadel::Message &msg, const citadel::Room &room, telnet::Session &t,
                 std::string &out) {
	out += "\n" + t.Colour(telnet::Session::Attr::Header) + " [#" + std::to_string(msg.msgnum) + "] " +
	       TimeString(msg.msgtime) + " from " + msg.author;
	if (!msg.recipient.empty()) {
		out += " to " + msg.recipient;
	}
	out += " in " + room.display_name + "\n";
	if (!msg.subject.empty()) {
		out += "Subject: " + msg.subject + "\n";
	}
	out += t.Colour(telnet::Session::Attr::Body);
	std::string body = citadel::BodyText(msg);
	out += body;
	if (!body.empty() && body.back() != '\n') {
		out += "\n";
	}
	out += t.Colour(telnet::Session::Attr::Reset);
}

// which: "new" | "old" | "all"; reverse walks newest-first; `last` caps the count.
void ReadMessages(Connection &con, Bbs &s, telnet::Session &t, const std::string &which, bool reverse,
                  int64_t last) {
	if (!s.have_room) {
		t.Write("\nNot in a room.\n");
		return;
	}
	t.ResetPager();
	auto st = citadel::GetRoomStats(con, s.username, s.room.room_num);
	std::string filter = which;
	int64_t param = 0;
	if (last > 0) {
		filter = "last";
		param = last;
	}
	auto nums = citadel::RoomMessages(con, s.room.room_num, filter, param, st.last_read);
	if (reverse) {
		std::reverse(nums.begin(), nums.end());
	}
	if (nums.empty()) {
		t.Write("\nNo messages.\n");
		return;
	}
	int64_t highest = st.last_read;
	for (int64_t n : nums) {
		citadel::Message msg;
		if (!citadel::LoadMessage(con, n, msg)) {
			continue;
		}
		std::string out;
		ShowMessage(msg, s.room, t, out);
		if (!t.Page(out)) {
			break; // the reader pressed S(top)
		}
		highest = std::max(highest, n);
	}
	// Only a forward pass through new or all messages advances the pointer.
	// Reading *old* messages must not mark the room read to its maximum, which
	// is what the previous unconditional SetLastRead did.
	if (!reverse && which != "old") {
		citadel::SetLastRead(con, s.username, s.room.room_num, highest);
	}
}

void EnterMessage(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.have_room) {
		t.Write("\nNot in a room.\n");
		return;
	}
	if (!citadel::CanPost(con, s.username, s.room)) {
		t.Write("\nYou may not enter messages in this type of room.\n");
		return;
	}

	std::string recipient;
	if (s.room.mailbox_owner > 0) {
		if (!t.Prompt("\nEnter recipient: ", recipient)) {
			return;
		}
		if (!recipient.empty() && citadel::GetOrAssignUserNum(con, recipient) <= 0) {
			t.Write("No such user.\n");
			return;
		}
	}

	std::string subject;
	if (!t.Prompt("\nSubject: ", subject)) {
		return;
	}
	t.Write("\nEnter your message. End with a '.' on a line by itself.\n");

	std::string body;
	std::string line;
	while (t.ReadLine(line)) {
		if (line == ".") {
			break;
		}
		if (body.size() + line.size() + 1 > kMaxPostBytes) {
			t.Write("Message too large; aborted.\n");
			return;
		}
		body += line;
		body += '\n';
	}

	citadel::Message msg;
	msg.author = s.username;
	msg.author_usernum = s.usernum;
	msg.recipient = recipient;
	msg.msgtime = NowEpoch();
	msg.format_type = 0;
	msg.subject = subject;
	msg.origin_room = s.room.display_name;
	msg.raw = body;

	std::vector<int64_t> rooms = {s.room.room_num};
	if (!recipient.empty()) {
		int64_t rcpt_room = citadel::GetOrCreateMailRoom(con, recipient);
		if (rcpt_room >= 0 && rcpt_room != s.room.room_num) {
			rooms.push_back(rcpt_room);
		}
	}
	std::string err;
	int64_t msgnum = citadel::InsertMessage(con, msg, rooms, err);
	if (msgnum < 0) {
		t.Write("Message not saved: " + err + "\n");
	} else {
		t.Write("Message saved.\n");
	}
}

// <D>elete a message from the current room. A user may remove their own; an
// aide may remove any. In a personal mailbox everything is the owner's.
void DeleteMessage(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.have_room) {
		t.Write("\nNot in a room.\n");
		return;
	}
	int64_t msgnum = 0;
	if (!t.PromptInt("\nDelete which message number?", 0, msgnum) || msgnum <= 0) {
		return;
	}
	citadel::Message msg;
	if (!citadel::MessageInRoom(con, s.room.room_num, msgnum) ||
	    !citadel::LoadMessage(con, msgnum, msg)) {
		t.Write("No such message in this room.\n");
		return;
	}
	if (s.room.mailbox_owner == 0 && !s.IsAide() && msg.author != s.username) {
		t.Write("You may only delete your own messages here.\n");
		return;
	}
	std::string err;
	if (!citadel::DeleteMessage(con, s.room.room_num, msgnum, err)) {
		t.Write("Not deleted: " + err + "\n");
		return;
	}
	t.Write("Message deleted.\n");
}

// .Admin Message edit: — move or copy a message to another room.
void MoveMessage(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.have_room) {
		t.Write("\nNot in a room.\n");
		return;
	}
	int64_t msgnum = 0;
	if (!t.PromptInt("\nMove which message number?", 0, msgnum) || msgnum <= 0) {
		return;
	}
	std::string target;
	if (!t.Prompt("Move to which room? ", target) || target.empty()) {
		return;
	}
	citadel::Room dest;
	if (!citadel::ResolveRoom(con, s.username, target, dest)) {
		t.Write("No such room.\n");
		return;
	}
	bool copy = false;
	if (!t.YesNo("Leave a copy behind?", false, copy)) {
		return;
	}
	std::string err;
	if (!citadel::MoveMessage(con, s.room.room_num, dest.room_num, msgnum, copy, err)) {
		t.Write("Not moved: " + err + "\n");
		return;
	}
	t.Write(copy ? "Message copied.\n" : "Message moved.\n");
}

// ------------------------------------------------------- presence + paging

// One fixed-width listing cell: pad short values, truncate long ones so the
// columns stay aligned on an 80-column terminal.
std::string Col(std::string text, size_t width) {
	if (text.size() >= width) {
		text.resize(width - 1);
		text += " ";
		return text;
	}
	text.resize(width, ' ');
	return text;
}

void WhoIsOnline(Connection &con, Bbs &s, telnet::Session &t, bool long_form) {
	t.ResetPager();
	std::string out = "\n" + t.Colour(telnet::Session::Attr::Header) +
	                  " Session  User             Room             From               Client" +
	                  t.Colour(telnet::Session::Attr::Reset) + "\n";
	for (auto &sess : citadel::ListSessions(con)) {
		std::string user = sess.username.empty() ? "(not logged in)" : sess.username;
		out += Col(" " + std::to_string(sess.session_id), 9) + Col(user, 17) + Col(sess.room, 17) +
		       Col(sess.host, 19) + sess.client;
		if (long_form) {
			out += "  (" + sess.last_cmd + ", since " + TimeString(sess.since) + ")";
		}
		out += "\n";
	}
	if (s.stealth) {
		out += "\n(You are in stealth mode; others do not see you here.)\n";
	}
	t.Page(out);
}

void PageUser(Connection &con, Bbs &s, telnet::Session &t) {
	std::string to;
	if (!t.Prompt("\nEnter user name: ", to) || to.empty()) {
		return;
	}
	std::string text;
	if (!t.Prompt("Enter message: ", text)) {
		return;
	}
	if (citadel::SendExpress(con, to, s.username, text)) {
		t.Write("Message sent.\n");
	} else {
		t.Write("No such user.\n");
	}
}

// Instant messages are shown before each prompt, like the text client does.
void ShowPendingExpress(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.authed || s.quiet) {
		return;
	}
	for (auto &e : citadel::PendingExpress(con, s.username)) {
		t.Write("\n---\nMessage from " + e.from_user + ":\n" + e.text + "\n---\n");
		citadel::MarkExpressDelivered(con, e.id);
	}
}

// ---- chat ----------------------------------------------------------------
//
// Built on citadel_express rather than on a channel of its own, and that is the
// whole design: the web /chat view, XMPP, native SEXP/GEXP and this are then one
// conversation instead of four. A user paged from a terminal sees the reply in
// their browser.
//
// The cost is that there is no room-wide chat the way real Citadel's CHAT verb
// has: a message here is addressed to somebody. Talking to "everyone" fans out
// one row per person currently online, which is a handful of rows and keeps
// every other client in the conversation rather than inventing a channel table
// only this front-end understands.

const int64_t kChatWindowSeconds = 7 * 24 * 3600;
const int64_t kChatScrollback = 40;

void ShowChatLines(Bbs &s, telnet::Session &t, const std::vector<citadel::ExpressLine> &lines) {
	for (const auto &l : lines) {
		const bool mine = l.from_user == s.username;
		t.Write(t.Colour(mine ? telnet::Session::Attr::Dim : telnet::Session::Attr::Notice) +
		        (mine ? "-> " + l.to_user : "<- " + l.from_user) + ": " +
		        t.Colour(telnet::Session::Attr::Reset) + l.text + "\n");
	}
}

void ChatMode(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.authed) {
		t.Write("\nYou must be logged in to chat.\n");
		return;
	}

	// Who to talk to. Defaulting to the most recent correspondent is what makes
	// answering a page one keystroke rather than retyping a name.
	auto recent = citadel::ExpressCorrespondents(con, s.username, kChatWindowSeconds);
	std::string with;
	if (!recent.empty()) {
		std::string list;
		for (size_t i = 0; i < recent.size() && i < 8; i++) {
			list += (i ? ", " : "") + recent[i];
		}
		t.Write("\nRecent: " + list + "\n");
	}
    if (!t.Prompt("Chat with (. for everyone online) [" +
                  (recent.empty() ? std::string("") : recent.front()) + "]: ",
                  with, recent.empty() ? std::string() : recent.front())) {
		return;
	}
	if (with.empty()) {
		return;
	}
	const bool broadcast = with == ".";
	if (!broadcast && citadel::GetOrAssignUserNum(con, with) == 0) {
		// GetOrAssignUserNum returns 0 for a user that does not exist; it does
		// not create one.
		t.Write("\nNo such user.\n");
		return;
	}

	t.Write("\nChat" + (broadcast ? std::string(" with everyone online")
	                               : " with " + with) +
	        ". Enter a blank line to leave, /r to refresh.\n\n");
	ShowChatLines(s, t, citadel::ExpressHistory(con, s.username, broadcast ? std::string() : with,
	                                            kChatWindowSeconds, kChatScrollback));

	int64_t seen = citadel::ExpressChangeToken(con, s.username);
	citadel::MarkExpressDeliveredThrough(con, s.username, seen);

	for (;;) {
		// A blocking ReadLine means incoming lines only appear when this user
		// types. That is the honest limit of a line-oriented terminal without a
		// second thread, and /r is the escape hatch — the same shape the web
		// view's poll has, driven by hand.
		std::string line;
		if (!t.Prompt("> ", line)) {
			return;
		}
		// Keep the session alive: a long conversation must not be reaped out
		// from under the person having it.
		citadel::TouchSession(con, s.session_id, s.stealth ? std::string() : s.username,
		                      (s.stealth || !s.have_room) ? std::string() : s.room.display_name,
		                      "chat", s.axlevel);

		if (line.empty()) {
			t.Write("\nLeaving chat.\n");
			return;
		}
		if (line != "/r") {
			if (broadcast) {
				int sent = 0;
				for (const auto &sess : citadel::ListSessions(con)) {
					if (sess.username.empty() || sess.username == s.username) {
						continue;
					}
					if (citadel::SendExpress(con, sess.username, s.username, line)) {
						sent++;
					}
				}
				if (sent == 0) {
					t.Write("Nobody else is online.\n");
				}
			} else if (!citadel::SendExpress(con, with, s.username, line)) {
				t.Write("That user is no longer reachable.\n");
				return;
			}
		}

		int64_t now_token = citadel::ExpressChangeToken(con, s.username);
		if (now_token != seen) {
			auto lines = citadel::ExpressHistory(con, s.username,
			                                     broadcast ? std::string() : with,
			                                     kChatWindowSeconds, kChatScrollback);
			// Only what arrived since the last screen, or every refresh would
			// repeat the whole transcript.
			std::vector<citadel::ExpressLine> fresh;
			for (const auto &l : lines) {
				if (l.id > seen && l.from_user != s.username) {
					fresh.push_back(l);
				}
			}
			ShowChatLines(s, t, fresh);
			seen = now_token;
			citadel::MarkExpressDeliveredThrough(con, s.username, seen);
		}
	}
}

// ------------------------------------------------- registration and config

// The eight REGI fields, in the order cmd_regi reads them in the real server.
void EnterRegistration(Connection &con, Bbs &s, telnet::Session &t) {
	citadel::Registration reg;
	citadel::GetRegistration(con, s.username, reg);
	t.Write("\nPlease enter your registration information.\n");
	struct Field {
		const char *label;
		std::string *slot;
	};
	Field fields[] = {
	    {"Real name", &reg.real_name}, {"Street address", &reg.street}, {"City/town", &reg.city},
	    {"State/province", &reg.state}, {"Postal code", &reg.zipcode},  {"Telephone", &reg.phone},
	    {"E-mail address", &reg.email}, {"Country", &reg.country},
	};
	for (auto &f : fields) {
		std::string value;
		if (!t.Prompt(std::string(f.label) + " [" + *f.slot + "]: ", value, *f.slot)) {
			return;
		}
		*f.slot = value;
	}
	if (!citadel::SetRegistration(con, s.username, reg)) {
		t.Write("Registration not saved.\n");
		return;
	}
	LoadUser(con, s);
	t.Write("Registration saved.\n");
}

void EnterBio(Connection &con, Bbs &s, telnet::Session &t) {
	t.Write("\nEnter your biography. End with a '.' on a line by itself.\n");
	std::string bio, line;
	while (t.ReadLine(line)) {
		if (line == ".") {
			break;
		}
		if (bio.size() + line.size() > 16 * 1024) {
			t.Write("Too long; aborted.\n");
			return;
		}
		bio += line;
		bio += '\n';
	}
	t.Write(citadel::SetBio(con, s.username, bio) ? "Biography saved.\n" : "Not saved.\n");
}

void ReadBio(Connection &con, Bbs &s, telnet::Session &t) {
	std::string who;
	if (!t.Prompt("\nRead whose biography? [" + s.username + "]: ", who, s.username)) {
		return;
	}
	citadel::Registration reg;
	if (!citadel::GetRegistration(con, who, reg) || reg.bio.empty()) {
		t.Write("No biography on file.\n");
		return;
	}
	t.ResetPager();
	t.Page("\n" + reg.bio + "\n");
}

// The US_USER_SET bits, which is exactly what the web console's preferences
// page edits — the two front-ends read and write the same column.
void EnterConfiguration(Connection &con, Bbs &s, telnet::Session &t) {
	struct Opt {
		int64_t bit;
		const char *question;
	};
	Opt opts[] = {
	    {citadel::US_EXPERT, "Expert mode (hide the menu)"},
	    {citadel::US_PAGINATOR, "Pause after each screenful"},
	    {citadel::US_FLOORS, "Show floors"},
	    {citadel::US_LASTOLD, "Print the last old message with new ones"},
	    {citadel::US_NOPROMPT, "Skip the prompt after each message"},
	    {citadel::US_PROMPTCTL, "<N>ext and <S>top work at the message prompt"},
	    {citadel::US_DISAPPEAR, "Use disappearing message prompts"},
	    {citadel::US_UNLISTED, "Be unlisted in the user directory"},
	    {citadel::US_COLOR, "ANSI colour"},
	};
	int64_t flags = s.flags;
	for (auto &o : opts) {
		bool on = false;
		if (!t.YesNo(std::string("\n") + o.question + "?", (flags & o.bit) != 0, on)) {
			return;
		}
		flags = on ? (flags | o.bit) : (flags & ~o.bit);
	}
	int64_t width = 0, height = 0;
	if (!t.PromptInt("\nScreen width", t.Width(), width)) {
		return;
	}
	if (!t.PromptInt("Screen height", t.Height(), height)) {
		return;
	}
	// Only the user-settable bits move; US_NEEDVALID and US_PERM are an aide's.
	s.flags = (s.flags & ~citadel::kUserSettableFlags) | (flags & citadel::kUserSettableFlags);
	citadel::SetUserFlags(con, s.username, s.flags);
	citadel::SetScreenSize(con, s.username, width, height);
	t.SetSize((int)width, (int)height);
	ApplyPrefs(con, s, t);
	t.Write("Configuration saved.\n");
}

void ReadConfiguration(Connection &con, Bbs &s, telnet::Session &t) {
	citadel::UserInfo info;
	citadel::GetUser(con, s.username, info);
	std::string out = "\nYour configuration:\n";
	auto flag = [&](const char *label, int64_t bit) {
		out += std::string("  ") + label + ": " + ((s.flags & bit) ? "yes" : "no") + "\n";
	};
	flag("Expert mode", citadel::US_EXPERT);
	flag("Paginator", citadel::US_PAGINATOR);
	flag("Floor mode", citadel::US_FLOORS);
	flag("Last old with new", citadel::US_LASTOLD);
	flag("Unlisted", citadel::US_UNLISTED);
	flag("ANSI colour", citadel::US_COLOR);
	flag("Registered", citadel::US_REGIS);
	out += "  Screen: " + std::to_string(info.screenwidth) + "x" + std::to_string(info.screenheight) +
	       "\n";
	out += "  Access level: " + std::to_string(s.axlevel) + (s.IsAide() ? " (aide)" : "") + "\n";
	t.Write(out);
}

void ReadUserList(Connection &con, Bbs &s, telnet::Session &t) {
	t.ResetPager();
	std::string out = "\n User                 Num  Ax  Calls  Posts  Last call\n";
	for (auto &u : citadel::ListUsers(con)) {
		// US_UNLISTED hides a user from everyone but an aide.
		if ((u.flags & citadel::US_UNLISTED) && !s.IsAide() && u.username != s.username) {
			continue;
		}
		std::string name = u.username;
		name.resize(std::max<size_t>(name.size(), 21), ' ');
		std::string num = std::to_string(u.usernum);
		num.resize(std::max<size_t>(num.size(), 5), ' ');
		std::string ax = std::to_string(u.axlevel);
		ax.resize(std::max<size_t>(ax.size(), 4), ' ');
		std::string calls = std::to_string(u.times_called);
		calls.resize(std::max<size_t>(calls.size(), 7), ' ');
		std::string posts = std::to_string(u.num_posts);
		posts.resize(std::max<size_t>(posts.size(), 7), ' ');
		out += " " + name + num + ax + calls + posts + TimeString(u.last_call) + "\n";
	}
	t.Page(out);
}

void ReadSystemInfo(Connection &con, telnet::Session &t) {
	std::string out = "\nSystem information:\n";
	const char *keys[] = {"c_humannode", "c_nodename", "c_fqdn", "c_bbs_city", "c_sysadm", "c_version"};
	for (const char *k : keys) {
		out += std::string("  ") + k + ": " + citadel::GetConfig(con, k, "") + "\n";
	}
	t.Write(out);
}

void EnterPassword(Connection &con, Bbs &s, telnet::Session &t) {
	std::string current;
	t.Write("\nCurrent password: ");
	if (!t.ReadLine(current, true)) {
		return;
	}
	t.Write("\n");
	if (!auth::Verify(con, s.username, current)) {
		t.Write("Wrong password.\n");
		return;
	}
	std::string pw, again;
	t.Write("New password: ");
	if (!t.ReadLine(pw, true)) {
		return;
	}
	t.Write("\nRepeat new password: ");
	if (!t.ReadLine(again, true)) {
		return;
	}
	t.Write("\n");
	if (pw.size() < 6) {
		t.Write("Choose a password of at least six characters.\n");
		return;
	}
	if (pw != again) {
		t.Write("They do not match.\n");
		return;
	}
	std::string err;
	t.Write(auth::AddUser(con, s.username, pw, err) ? std::string("Password changed.\n")
	                                                : ("Not changed: " + err + "\n"));
}

// ------------------------------------------------------------------ floors

void CreateFloorCmd(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.IsAide()) {
		t.Write("\nHigher access required.\n");
		return;
	}
	std::string name;
	if (!t.Prompt("\nName for the new floor: ", name) || name.empty()) {
		return;
	}
	std::string err;
	if (citadel::CreateFloor(con, name, err) < 0) {
		t.Write("Not created: " + err + "\n");
		return;
	}
	t.Write("Floor created.\n");
}

void EditFloorCmd(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.IsAide() || !s.have_room) {
		t.Write("\nHigher access required.\n");
		return;
	}
	citadel::Floor floor;
	if (!citadel::GetFloor(con, s.room.floor_num, floor)) {
		t.Write("\nNo such floor.\n");
		return;
	}
	std::string name;
	if (!t.Prompt("\nFloor name [" + floor.name + "]: ", name, floor.name)) {
		return;
	}
	std::string err;
	t.Write(citadel::RenameFloor(con, floor.floor_num, name, err) ? std::string("Floor renamed.\n")
	                                                             : ("Not renamed: " + err + "\n"));
}

void KillFloorCmd(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.IsAide() || !s.have_room) {
		t.Write("\nHigher access required.\n");
		return;
	}
	bool sure = false;
	if (!t.YesNo("\nDelete this floor?", false, sure) || !sure) {
		return;
	}
	std::string err;
	t.Write(citadel::KillFloor(con, s.room.floor_num, err) ? std::string("Floor deleted.\n")
	                                                      : ("Not deleted: " + err + "\n"));
}

void GotoFloor(Connection &con, Bbs &s, telnet::Session &t, bool skip) {
	std::string name;
	if (!t.Prompt("\nGoto which floor? ", name) || name.empty()) {
		return;
	}
	for (auto &f : citadel::ListFloors(con)) {
		if (util::Upper(f.name).rfind(util::Upper(name), 0) != 0) {
			continue;
		}
		auto rooms = citadel::ListRooms(con, s.username, f.floor_num, "all");
		if (rooms.empty()) {
			t.Write("\nThat floor has no rooms you can see.\n");
			return;
		}
		if (skip && s.have_room) {
			// Skipping leaves the current room unread, unlike a plain goto.
			citadel::SetLastRead(con, s.username, s.room.room_num, s.entry_last_read);
		}
		if (Unlock(con, s, t, rooms[0])) {
			EnterRoom(con, s, t, rooms[0], true);
		}
		return;
	}
	t.Write("\nNo such floor.\n");
}

void ZapFloor(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.have_room) {
		t.Write("\nNot in a room.\n");
		return;
	}
	bool sure = false;
	if (!t.YesNo("\nForget every room on this floor?", false, sure) || !sure) {
		return;
	}
	int64_t count = 0;
	for (auto &r : citadel::ListRooms(con, s.username, s.room.floor_num, "all")) {
		std::string err;
		if (citadel::ZapRoom(con, s.username, r.room_num, true, err)) {
			count++;
		}
	}
	t.Write("\n" + std::to_string(count) + " room(s) forgotten.\n");
	GotoNext(con, s, t, false);
}

// ------------------------------------------------------- room administration

void EnterNewRoom(Connection &con, Bbs &s, telnet::Session &t) {
	std::string name;
	if (!t.Prompt("\nName for the new room: ", name) || name.empty()) {
		return;
	}
	int64_t flags = 0;
	bool yes = false;
	if (!t.YesNo("Private room?", false, yes)) {
		return;
	}
	if (yes) {
		flags |= citadel::QR_PRIVATE;
	}
	std::string password;
	if (!t.YesNo("Password protected?", false, yes)) {
		return;
	}
	if (yes) {
		flags |= citadel::QR_PASSWORDED;
		if (!t.Prompt("Room password: ", password)) {
			return;
		}
	}
	if (!t.YesNo("Read only?", false, yes)) {
		return;
	}
	if (yes) {
		flags |= citadel::QR_READONLY;
	}
	int64_t floor = s.have_room ? s.room.floor_num : 0;
	if (!t.PromptInt("Floor number", floor, floor)) {
		return;
	}
	std::string err;
	int64_t num = citadel::CreateRoom(con, name, floor, flags, password, 0, err);
	if (num < 0) {
		t.Write("Not created: " + err + "\n");
		return;
	}
	t.Write("Room created.\n");
	citadel::Room room;
	if (citadel::GetRoomByNum(con, num, room)) {
		EnterRoom(con, s, t, room, true);
	}
}

void EditRoom(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.IsAide() || !s.have_room) {
		t.Write("\nHigher access required.\n");
		return;
	}
	citadel::Room room = s.room;
	std::string value;
	if (!t.Prompt("\nRoom name [" + room.display_name + "]: ", value, room.display_name)) {
		return;
	}
	room.display_name = value;
	int64_t n = 0;
	if (!t.PromptInt("Floor number", room.floor_num, n)) {
		return;
	}
	room.floor_num = n;
	if (!t.PromptInt("List order", room.listorder, n)) {
		return;
	}
	room.listorder = n;
	// The full set the web interface offers. It used to stop at 5, so a room
	// created here could not be made a blog, a journal or a wiki at all, and
	// editing one that was silently offered a shorter list than it belonged to.
	if (!t.PromptInt("Default view (0 board, 1 mail, 2 contacts, 3 calendar, 4 tasks,\n"
	                 "              5 notes, 6 wiki, 7 calendar list, 8 journal, 10 blog)",
	                 room.default_view, n)) {
		return;
	}
	room.default_view = n;

	struct Opt {
		int64_t bit;
		const char *question;
	};
	Opt opts[] = {
	    {citadel::QR_PRIVATE, "Private"},         {citadel::QR_PASSWORDED, "Password protected"},
	    {citadel::QR_GUESSNAME, "Guess-name"},    {citadel::QR_READONLY, "Read only"},
	    {citadel::QR_DIRECTORY, "Directory"},     {citadel::QR_PERMANENT, "Never auto-purge"},
	    {citadel::QR_PREFONLY, "Preferred only"}, {citadel::QR_NETWORK, "Network shared"},
	};
	for (auto &o : opts) {
		bool on = false;
		if (!t.YesNo(std::string(o.question) + "?", (room.qr_flags & o.bit) != 0, on)) {
			return;
		}
		room.qr_flags = on ? (room.qr_flags | o.bit) : (room.qr_flags & ~o.bit);
	}
	if (room.qr_flags & citadel::QR_PASSWORDED) {
		if (!t.Prompt("Room password [" + room.password + "]: ", value, room.password)) {
			return;
		}
		room.password = value;
	}
	std::string err;
	if (!citadel::UpdateRoom(con, room, err)) {
		t.Write("Not saved: " + err + "\n");
		return;
	}
	s.room = room;
	t.Write("Room saved.\n");
}

void EnterRoomInfo(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.IsAide() || !s.have_room) {
		t.Write("\nHigher access required.\n");
		return;
	}
	t.Write("\nEnter the room info file. End with a '.' on a line by itself.\n");
	std::string info, line;
	while (t.ReadLine(line)) {
		if (line == ".") {
			break;
		}
		info += line;
		info += '\n';
	}
	citadel::Room room = s.room;
	room.info = info;
	std::string err;
	if (!citadel::UpdateRoom(con, room, err)) {
		t.Write("Not saved: " + err + "\n");
		return;
	}
	s.room = room;
	t.Write("Info file saved.\n");
}

void KillRoomCmd(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.IsAide() || !s.have_room) {
		t.Write("\nHigher access required.\n");
		return;
	}
	bool sure = false;
	if (!t.YesNo("\nDelete room '" + s.room.display_name + "' and its messages?", false, sure) || !sure) {
		return;
	}
	std::string err;
	if (!citadel::KillRoom(con, s.room.room_num, err)) {
		t.Write("Not deleted: " + err + "\n");
		return;
	}
	t.Write("Room deleted.\n");
	citadel::PostAideMessage(con, "Room deleted: " + s.room.display_name,
	                         "The room and its message pointers were removed.\n\nBy: " + s.username +
	                             " (BBS shell)\n");
	s.have_room = false;
	citadel::Room lobby;
	if (citadel::ResolveRoom(con, s.username, "Lobby", lobby)) {
		EnterRoom(con, s, t, lobby, false);
	}
}

void WhoKnowsRoom(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.have_room) {
		t.Write("\nNot in a room.\n");
		return;
	}
	t.ResetPager();
	std::string out = "\nUsers who can see '" + s.room.display_name + "':\n";
	for (auto &u : citadel::ListUsers(con)) {
		// A private room is visible to aides and, if it is a mailbox, its owner.
		bool visible = !(s.room.qr_flags & citadel::QR_PRIVATE) || u.axlevel >= citadel::kAideAxLevel ||
		               (s.room.mailbox_owner > 0 && s.room.mailbox_owner == u.usernum);
		if (visible) {
			out += "  " + u.username + "\n";
		}
	}
	t.Page(out);
}

void EditUser(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.IsAide()) {
		t.Write("\nHigher access required.\n");
		return;
	}
	std::string who;
	if (!t.Prompt("\nEdit which user? ", who) || who.empty()) {
		return;
	}
	citadel::UserInfo info;
	if (!citadel::GetUser(con, who, info)) {
		t.Write("No such user.\n");
		return;
	}
	int64_t level = 0;
	if (!t.PromptInt("Access level (0-6)", info.axlevel, level)) {
		return;
	}
	std::string err;
	if (!citadel::SetAxLevel(con, who, level, err)) {
		t.Write("Not changed: " + err + "\n");
		return;
	}
	bool needs_valid = false;
	if (!t.YesNo("Awaiting validation?", (info.flags & citadel::US_NEEDVALID) != 0, needs_valid)) {
		return;
	}
	int64_t flags = needs_valid ? (info.flags | citadel::US_NEEDVALID)
	                            : (info.flags & ~(int64_t)citadel::US_NEEDVALID);
	citadel::SetUserFlags(con, who, flags);
	citadel::PostAideMessage(con, "User edited: " + who,
	                         "User: " + who + "\nAccess level: " + std::to_string(level) +
	                             "\nAwaiting validation: " + (needs_valid ? "yes" : "no") + "\n\nBy: " +
	                             s.username + " (BBS shell)\n");
	t.Write("User saved.\n");
}

void DeleteUser(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.IsAide()) {
		t.Write("\nHigher access required.\n");
		return;
	}
	std::string who;
	if (!t.Prompt("\nDelete which user? ", who) || who.empty()) {
		return;
	}
	if (who == s.username) {
		t.Write("You cannot delete the account you are logged in as.\n");
		return;
	}
	bool sure = false;
	if (!t.YesNo("Really delete '" + who + "'?", false, sure) || !sure) {
		return;
	}
	std::string err;
	if (auth::RemoveUser(con, who, err)) {
		t.Write("User deleted.\n");
		citadel::PostAideMessage(con, "User removed: " + who,
		                         "The account and its credentials were deleted.\n\nUser: " + who +
		                             "\nBy: " + s.username + " (BBS shell)\n");
	} else {
		t.Write("Not deleted: " + err + "\n");
	}
}

void ValidateUsers(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.IsAide()) {
		t.Write("\nHigher access required.\n");
		return;
	}
	bool any = false;
	for (auto &u : citadel::ListUsers(con)) {
		if (!(u.flags & citadel::US_NEEDVALID)) {
			continue;
		}
		any = true;
		citadel::Registration reg;
		citadel::GetRegistration(con, u.username, reg);
		t.Write("\n" + u.username + " (#" + std::to_string(u.usernum) + ")\n");
		if (!reg.real_name.empty()) {
			t.Write("  " + reg.real_name + ", " + reg.city + " " + reg.state + " " + reg.country + "\n");
		}
		int64_t level = 0;
		if (!t.PromptInt("  Access level to grant (0 to leave pending)", 4, level)) {
			return;
		}
		if (level <= 0) {
			continue;
		}
		std::string err;
		citadel::SetAxLevel(con, u.username, level, err);
		citadel::SetUserFlags(con, u.username, u.flags & ~(int64_t)citadel::US_NEEDVALID);
		citadel::PostAideMessage(con, "User validated: " + u.username,
		                         "User: " + u.username + "\nAccess level granted: " +
		                             std::to_string(level) + "\n\nBy: " + s.username +
		                             " (BBS shell)\n");
		t.Write("  Validated.\n");
	}
	if (!any) {
		t.Write("\nNo users are awaiting validation.\n");
	}
}

// ------------------------------------------------------------------- login

bool Login(Connection &con, Bbs &s, telnet::Session &t) {
	for (int attempt = 0; attempt < 3; attempt++) {
		std::string name;
		if (!t.Prompt("\nEnter your name (or 'new' to register): ", name)) {
			return false;
		}
		if (name.empty()) {
			continue;
		}

		bool fresh = false;
		if (util::Upper(name) == "NEW") {
			std::string newname;
			if (!t.Prompt("Enter a user name: ", newname) || newname.empty()) {
				continue;
			}
			if (citadel::GetOrAssignUserNum(con, newname) > 0) {
				t.Write("That user already exists.\n");
				continue;
			}
			t.Write("Enter a password: ");
			std::string pw;
			if (!t.ReadLine(pw, true)) {
				return false;
			}
			t.Write("\n");
			std::string err;
			if (!auth::AddUser(con, newname, pw, err)) {
				t.Write("Could not create account: " + err + "\n");
				continue;
			}
			citadel::PostAideMessage(con, "New user: " + newname,
			                         "A new account was registered from the BBS shell.\n\nUser: " +
			                             newname + "\n");
			name = newname;
			fresh = true;
		} else {
			if (citadel::GetOrAssignUserNum(con, name) <= 0) {
				t.Write("No such user.\n");
				continue;
			}
			t.Write("Password: ");
			std::string pw;
			if (!t.ReadLine(pw, true)) {
				return false;
			}
			t.Write("\n");
			if (!auth::Verify(con, name, pw)) {
				t.Write("Wrong password.\n");
				continue;
			}
		}

		s.authed = true;
		s.username = name;
		citadel::EnsureUserRooms(con, name);
		LoadUser(con, s);
		citadel::RecordCall(con, name);
		if (fresh) {
			// A stock BBS asks a new account to register before it lets them in.
			EnterRegistration(con, s, t);
		}
		return true;
	}
	t.Write("\nToo many failed attempts.\n");
	return false;
}

// ------------------------------------------------------------ command loop

// What a command did to the session, so a nested dispatcher can end it.
enum class Outcome { Continue, Quit };

// ";" — the floor submenu from citadel.rc.
Outcome FloorCommand(Connection &con, Bbs &s, telnet::Session &t, const std::string &rest) {
	std::string up = util::Upper(rest);
	char c = up.empty() ? '?' : up[0];
	switch (c) {
	case 'C': { // ;Configure floor mode
		bool on = false;
		if (t.YesNo("\nDo you want to use floor mode?", s.FloorMode(), on)) {
			s.flags = on ? (s.flags | citadel::US_FLOORS) : (s.flags & ~(int64_t)citadel::US_FLOORS);
			citadel::SetUserFlags(con, s.username, s.flags);
			// "Floor mode now ON/OFF" — user_functions.c:872-876.
			t.Write(on ? "Floor mode now ON\n" : "Floor mode now OFF\n");
		}
		break;
	}
	case 'G':
		GotoFloor(con, s, t, false);
		break;
	case 'S':
		GotoFloor(con, s, t, true);
		break;
	case 'Z':
		ZapFloor(con, s, t);
		break;
	case 'K':
		KnownFiltered(con, s, t, 'F');
		break;
	case 'A': { // ;Admin Create/Edit/Kill floor
		char sub = up.size() > 1 ? up[1] : '?';
		if (sub == 'C') {
			CreateFloorCmd(con, s, t);
		} else if (sub == 'E') {
			EditFloorCmd(con, s, t);
		} else if (sub == 'K') {
			KillFloorCmd(con, s, t);
		} else {
			t.Write("\nFloor admin: <;AC>reate, <;AE>dit, <;AK>ill.\n");
		}
		break;
	}
	default:
		t.Write("\nFloor commands: <;C>onfigure floor mode, <;G>oto floor, <;S>kip to floor,\n"
		        "                <;Z>ap floor, <;K>nown rooms by floor");
		if (s.IsAide()) {
			t.Write(", <;A>dmin");
		}
		t.Write("\n");
		break;
	}
	return Outcome::Continue;
}

// ".Admin ..." — the aide submenu.
// ---- file areas ----------------------------------------------------------
//
// A directory room's files are messages, so everything below is the file store
// in core/ plus a terminal in front of it. The permission questions are asked
// there too rather than re-derived here — the same rule that has every
// front-end ask CanPost instead of working it out again.

namespace filearea = quackmail::filearea;

// A size a person can read at a glance, which is the only reason a listing
// exists at all.
std::string SizeLabel(int64_t n) {
	char buf[32];
	if (n < 1024) {
		std::snprintf(buf, sizeof(buf), "%lld B", (long long)n);
	} else if (n < 1024 * 1024) {
		std::snprintf(buf, sizeof(buf), "%.1f KB", (double)n / 1024.0);
	} else {
		std::snprintf(buf, sizeof(buf), "%.1f MB", (double)n / (1024.0 * 1024.0));
	}
	return buf;
}

// Is the current room a file area this user may look at? Writes the refusal
// itself, so every caller is one `if`.
bool RequireFileArea(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.have_room || !filearea::IsFileArea(s.room)) {
		t.Write("\nThis room is not a file directory.\n");
		return false;
	}
	if (!filearea::CanList(con, s.username, s.room)) {
		t.Write("\nYou may not list the files in this room.\n");
		return false;
	}
	return true;
}

void ReadFileDirectory(Connection &con, Bbs &s, telnet::Session &t) {
	if (!RequireFileArea(con, s, t)) {
		return;
	}
	t.ResetPager();
	auto files = filearea::ListFiles(con, s.room.room_num);
	if (files.empty()) {
		t.Write("\nThere are no files in this room.\n");
		return;
	}
	std::string out = "\n Name                             Size        Uploaded by\n";
	for (const auto &f : files) {
		std::string name = f.name;
		name.resize(std::max<size_t>(name.size(), 33), ' ');
		std::string size = SizeLabel(f.size);
		size.resize(std::max<size_t>(size.size(), 12), ' ');
		out += " " + name + size + f.uploader + "\n";
		if (!f.description.empty()) {
			out += "     " + f.description + "\n";
		}
	}
	out += "\n" + std::to_string(files.size()) + " file(s). <.RFG>et to download.\n";
	t.Page(out);
}

// Does this look like something a person can read on a terminal?
bool LooksTextual(const filearea::File &f, const std::string &content) {
	if (f.content_type.rfind("text/", 0) == 0) {
		return true;
	}
	// Otherwise decide from the bytes rather than from a header a client chose:
    // a NUL anywhere is the reliable tell, and so is a high proportion of
	// control characters.
	size_t odd = 0;
	for (unsigned char c : content) {
		if (c == 0) {
			return false;
		}
		if (c < 0x09 || (c > 0x0D && c < 0x20)) {
			odd++;
		}
	}
	return content.empty() || odd * 32 < content.size();
}

void ReadFileGet(Connection &con, Bbs &s, telnet::Session &t, const std::string &arg) {
	if (!RequireFileArea(con, s, t)) {
		return;
	}
	if (!filearea::CanDownload(con, s.username, s.room)) {
		t.Write("\nDownloads are not enabled in this room.\n");
		return;
	}
	std::string name = arg;
	if (name.empty() && !t.Prompt("\nFilename: ", name)) {
		return;
	}
	if (name.empty()) {
		return;
	}
	filearea::File f;
	std::string content;
	if (!filearea::GetFile(con, s.room.room_num, name, f, content)) {
		t.Write("\nNo such file.\n");
		return;
	}

	// Three ways out, in the order that suits a terminal. Typing a text file is
	// what a BBS reader usually wants; Xmodem is for anything else; base64 is
	// the fallback for a terminal with no transfer protocol at all, and is
	// pasteable into `base64 -d` on the other side.
	std::string how;
	std::string dflt = LooksTextual(f, content) ? "T" : "X";
	if (!t.Prompt("\n<T>ype it out, <X>modem, <B>ase64, <Q>uit [" + dflt + "]: ", how, dflt)) {
		return;
	}
	char c = how.empty() ? dflt[0] : (char)std::toupper((unsigned char)how[0]);
	if (c == 'Q') {
		return;
	}
	if (c == 'T') {
		t.ResetPager();
		t.Page("\n" + content + (content.empty() || content.back() == '\n' ? "" : "\n"));
		return;
	}
	if (c == 'B') {
		t.Write("\n---- begin " + f.name + " ----\n");
		std::string b64 = quackmail::util::Base64Encode(content);
		for (size_t at = 0; at < b64.size(); at += 76) {
			t.Write(b64.substr(at, 76) + "\n");
		}
		t.Write("---- end " + f.name + " ----\n");
		return;
	}
	t.Write("\nStart your Xmodem receive now.\n");
	if (!quackmail::xmodem::Send(t, content)) {
		t.Write("\nTransfer failed or was cancelled.\n");
		return;
	}
	t.Write("\nTransfer complete: " + f.name + " (" + SizeLabel(f.size) + ").\n");
}

void EnterFile(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.have_room || !filearea::IsFileArea(s.room)) {
		t.Write("\nThis room is not a file directory.\n");
		return;
	}
	if (!filearea::CanUpload(con, s.username, s.room)) {
		t.Write("\nUploads are not enabled in this room, or you may not post here.\n");
		return;
	}
	std::string name;
	if (!t.Prompt("\nFilename: ", name) || name.empty()) {
		return;
	}
	if (filearea::SanitizeName(name).empty()) {
		t.Write("\nThat filename cannot be stored.\n");
		return;
	}
	std::string desc;
	if (!t.Prompt("Description (optional): ", desc)) {
		return;
	}

	std::string how;
	if (!t.Prompt("<X>modem or <B>ase64 paste [X]: ", how, "X")) {
		return;
	}
	std::string content;
	if (!how.empty() && std::toupper((unsigned char)how[0]) == 'B') {
		t.Write("\nPaste the base64 now; end with a line containing a single dot.\n");
		std::string b64;
		for (;;) {
			std::string line;
			if (!t.ReadLine(line)) {
				return;
			}
			if (line == ".") {
				break;
			}
			b64 += line;
		}
		if (!quackmail::util::Base64Decode(b64, content)) {
			t.Write("\nThat is not valid base64; nothing was stored.\n");
			return;
		}
	} else {
		t.Write("\nStart your Xmodem send now.\n");
		// Bounded so a peer cannot fill the database from a terminal. The
		// storage quota inside InsertMessage is the real ceiling; this is the
		// one that stops the transfer before it is all in memory.
		if (!quackmail::xmodem::Receive(t, content, 32u * 1024 * 1024)) {
			t.Write("\nTransfer failed or was cancelled.\n");
			return;
		}
	}

	std::string err;
	if (filearea::PutFile(con, s.room.room_num, name, content, std::string(), desc, s.username,
	                      err) < 0) {
		t.Write("\n" + (err.empty() ? std::string("The file could not be stored.") : err) + "\n");
		return;
	}
	t.Write("\nStored " + name + " (" + SizeLabel((int64_t)content.size()) + ").\n");
}

// .Admin File: tidying up somebody else's upload, which is a room-admin job.
void AdminFile(Connection &con, Bbs &s, telnet::Session &t, char sub) {
	if (!s.have_room || !filearea::IsFileArea(s.room)) {
		t.Write("\nThis room is not a file directory.\n");
		return;
	}
	if (sub != 'D' && sub != 'E') {
		t.Write("\nAdmin file: <.AFD>elete, <.AFE>dit description\n");
		return;
	}
	std::string name;
	if (!t.Prompt("\nFilename: ", name) || name.empty()) {
		return;
	}
	std::string err;
	if (sub == 'D') {
		if (!filearea::RemoveFile(con, s.room.room_num, name, err)) {
			t.Write("\n" + (err.empty() ? std::string("No such file.") : err) + "\n");
			return;
		}
		t.Write("\nDeleted " + name + ".\n");
		return;
	}
	std::string desc;
	if (!t.Prompt("New description: ", desc)) {
		return;
	}
	if (!filearea::DescribeFile(con, s.room.room_num, name, desc, err)) {
		t.Write("\n" + (err.empty() ? std::string("No such file.") : err) + "\n");
		return;
	}
	t.Write("\nDescription updated.\n");
}

void AdminCommand(Connection &con, Bbs &s, telnet::Session &t, const std::string &up) {
	if (!s.IsAide()) {
		t.Write("\nHigher access required.\n");
		return;
	}
	char c = up.empty() ? '?' : up[0];
	switch (c) {
	case 'K':
		KillRoomCmd(con, s, t);
		break;
	case 'E':
		EditRoom(con, s, t);
		break;
	case 'W':
		WhoKnowsRoom(con, s, t);
		break;
	case 'I':
		EnterRoomInfo(con, s, t);
		break;
	case 'M':
		MoveMessage(con, s, t);
		break;
	case 'V':
		ValidateUsers(con, s, t);
		break;
	case 'U': {
		char sub = up.size() > 1 ? up[1] : 'E';
		if (sub == 'D') {
			DeleteUser(con, s, t);
		} else {
			EditUser(con, s, t);
		}
		break;
	}
	case 'F':
		AdminFile(con, s, t, up.size() > 1 ? up[1] : '?');
		break;
	default:
		t.Write("\nAdmin: <.AK>ill room, <.AE>dit room, <.AW>ho knows room, <.AI>nfo file,\n"
		        "       <.AM>essage move, <.AUE>dit user, <.AUD>elete user, <.AV>alidate users,\n"
		        "       <.AFD>elete file, <.AFE>dit file description\n");
		break;
	}
}

// A "." command, e.g. ".Goto Lobby": `rest` is whatever followed the dot.
Outcome DotCommand(Connection &con, Bbs &s, telnet::Session &t, const std::string &rest) {
	std::string up = util::Upper(rest);
	std::string arg;
	size_t sp = rest.find(' ');
	if (sp != std::string::npos) {
		arg = rest.substr(sp + 1);
	}
	char c = up.empty() ? '?' : up[0];
	char sub = up.size() > 1 ? up[1] : '\0';

	switch (c) {
	case 'G': // .Goto <room>
		if (arg.empty() && !t.Prompt("\nGoto which room? ", arg)) {
			return Outcome::Continue;
		}
		GotoNamed(con, s, t, arg);
		break;
	case 'S': // .Skip ... goto:
		if (s.have_room) {
			citadel::SetLastRead(con, s.username, s.room.room_num, s.entry_last_read);
		}
		if (arg.empty() && !t.Prompt("\nSkip to which room? ", arg)) {
			return Outcome::Continue;
		}
		GotoNamed(con, s, t, arg);
		break;
	case 'U': // .Ungoto:
		Ungoto(con, s, t);
		break;
	case 'K': // .Known <filter>
		if (sub == '\0') {
			KnownRooms(con, s, t);
		} else {
			KnownFiltered(con, s, t, sub);
		}
		break;
	case 'R': // .Read ...
		switch (sub) {
		case 'U':
			ReadUserList(con, s, t);
			break;
		case 'B':
			ReadBio(con, s, t);
			break;
		case 'C':
			ReadConfiguration(con, s, t);
			break;
		case 'S':
			ReadSystemInfo(con, t);
			break;
		case 'N':
			ReadMessages(con, s, t, "new", false, 0);
			break;
		case 'O':
			ReadMessages(con, s, t, "old", true, 0);
			break;
		case 'L':
			ReadMessages(con, s, t, "all", false, 5);
			break;
		case 'F':
			// .RF lists, .RFG fetches — the third letter, the same way .AU
			// already splits into .AUE and .AUD.
			if (up.size() > 2 && up[2] == 'G') {
				ReadFileGet(con, s, t, arg);
			} else {
				ReadFileDirectory(con, s, t);
			}
			break;
		default:
			t.Write("\nRead: <.RU>ser list, <.RB>io, <.RC>onfiguration, <.RS>ystem info,\n"
			        "      <.RN>ew, <.RO>ld, <.RL>ast five, <.RF>ile directory, <.RFG>et file\n");
			break;
		}
		break;
	case 'E': // .Enter ...
		switch (sub) {
		case 'P':
			EnterPassword(con, s, t);
			break;
		case 'C':
			EnterConfiguration(con, s, t);
			break;
		case 'G':
			EnterRegistration(con, s, t);
			break;
		case 'B':
			EnterBio(con, s, t);
			break;
		case 'R':
			EnterNewRoom(con, s, t);
			break;
		case 'M':
			EnterMessage(con, s, t);
			break;
		case 'F':
			EnterFile(con, s, t);
			break;
		default:
			t.Write("\nEnter: <.EP>assword, <.EC>onfiguration, re<.EG>istration, <.EB>io,\n"
			        "       a new <.ER>oom, <.EM>essage, a <.EF>ile\n");
			break;
		}
		break;
	case 'W': // .Wholist Long/Active/Stealth
		if (sub == 'S') {
			s.stealth = !s.stealth;
			t.Write(s.stealth ? "\nStealth mode now ON\n" : "\nStealth mode now OFF\n");
		} else {
			WhoIsOnline(con, s, t, true);
		}
		break;
	case 'A': // .Admin ...
		AdminCommand(con, s, t, up.substr(1));
		break;
	case 'T': // .Terminate and Quit
		t.Write("\nGoodbye.\n");
		return Outcome::Quit;
	case 'H':
		// A topic if one was named, the topic list otherwise. <?> is still the
		// command menu; this is the prose.
		ShowHelp(con, t, arg);
		break;
	default:
		t.Write("\nUnknown command.\n");
		break;
	}
	return Outcome::Continue;
}

void HandleTelnet(DatabaseInstance &db, net::ClientStream &stream, ServerController &ctrl) {
	Connection con(db);
	store::EnsureSchema(con);

	telnet::Session t(stream);
	t.Negotiate();

	Bbs s;
	s.session_id = citadel::RegisterSession(con, ctrl.ImplicitTls() ? "Telnets session" : "Telnet session",
	                                       stream.PeerIp());

	std::string humannode = citadel::GetConfig(con, "c_humannode", "QuackCit BBS");
	std::string city = citadel::GetConfig(con, "c_bbs_city", "");
	std::string version = citadel::GetConfig(con, "c_version", "QuackCit");

	t.Write("\n" + humannode + (city.empty() ? "" : " - " + city) + "\n" + version + "\n");

	if (!Login(con, s, t)) {
		citadel::UnregisterSession(con, s.session_id);
		return;
	}
	t.Write("\nWelcome, " + s.username + ".\n");
	ApplyPrefs(con, s, t);

	citadel::Room lobby;
	if (citadel::ResolveRoom(con, s.username, "Lobby", lobby)) {
		EnterRoom(con, s, t, lobby, false);
	}

	bool running = true;
	while (running) {
		ShowPendingExpress(con, s, t);
		if (!s.Expert()) {
			ShowMenu(t, s);
		}
		// A stealth session is registered but reports no room, so it does not
		// show up meaningfully in the who-list.
		citadel::TouchSession(con, s.session_id, s.stealth ? std::string() : s.username,
		                      (s.stealth || !s.have_room) ? std::string() : s.room.display_name, "idle",
		                      s.axlevel);

		t.ResetPager();
		t.Write("\n" + t.Colour(telnet::Session::Attr::Prompt) +
		        (s.have_room ? s.room.display_name : std::string("(no room)")) +
		        std::string(1, s.have_room ? PromptChar(s.room) : '>') +
		        t.Colour(telnet::Session::Attr::Reset) + " ");

		// A negotiated telnet client sends one keystroke at a time; a raw socket
		// (nc, expect harnesses, our tests) sends whole lines. Read accordingly,
		// otherwise the trailing CRLF of a line-mode command would be swallowed
		// by whatever prompt the command opens.
		char cmd = 0;
		std::string rest;
		if (t.Echoing()) {
			int ch = t.GetChar();
			if (ch < 0) {
				break;
			}
			cmd = (char)ch;
			// '.' and ';' both introduce a longer command; echo the lead-in and
			// read the remainder as a line.
			if (cmd == '.' || cmd == ';') {
				t.Write(std::string(1, cmd));
				if (!t.ReadLine(rest)) {
					break;
				}
			} else {
				t.Write(std::string(1, cmd));
				t.Write("\n");
			}
		} else {
			std::string cmdline;
			if (!t.ReadLine(cmdline)) {
				break;
			}
			if (cmdline.empty()) {
				continue;
			}
			cmd = cmdline[0];
			rest = cmdline.substr(1);
		}

		citadel::TouchSession(con, s.session_id, s.stealth ? std::string() : s.username,
		                      (s.stealth || !s.have_room) ? std::string() : s.room.display_name,
		                      std::string(1, cmd), s.axlevel);

		switch (util::Upper(std::string(1, cmd))[0]) {
		case '?':
			ShowMenu(t, s);
			break;
		case 'K':
			KnownRooms(con, s, t);
			break;
		case 'G':
			GotoNext(con, s, t, true);
			break;
		case 'S':
			// Skip: advance without marking this room read. That difference
			// from <G>oto is the entire point of the command.
			GotoNext(con, s, t, false);
			break;
		case 'A':
			Abandon(con, s, t);
			break;
		case 'U':
			Ungoto(con, s, t);
			break;
		case 'Z':
			ZapRoom(con, s, t);
			break;
		case '+':
			StepRoom(con, s, t, 1);
			break;
		case '-':
			StepRoom(con, s, t, -1);
			break;
		case '>':
			StepFloor(con, s, t, 1);
			break;
		case '<':
			StepFloor(con, s, t, -1);
			break;
		case 'M':
			GotoNamed(con, s, t, "Mail");
			break;
		case 'N':
			ReadMessages(con, s, t, "new", false, 0);
			break;
		case 'O':
			ReadMessages(con, s, t, "old", true, 0);
			break;
		case 'F':
			ReadMessages(con, s, t, "all", false, 0);
			break;
		case 'R':
			ReadMessages(con, s, t, "all", true, 0);
			break;
		case 'L':
			ReadMessages(con, s, t, "all", false, 5);
			break;
		case 'E':
			EnterMessage(con, s, t);
			break;
		case 'D':
			DeleteMessage(con, s, t);
			break;
		case 'W':
			WhoIsOnline(con, s, t, false);
			break;
		case 'P':
			PageUser(con, s, t);
			break;
		case 'C':
			ChatMode(con, s, t);
			break;
		case 'I':
			if (s.have_room && !s.room.info.empty()) {
				t.Write("\n" + s.room.info + "\n");
			} else {
				t.Write("\nNo info file for this room.\n");
			}
			break;
		case 'Q':
			// Wording from client_chat.c in the real text client.
			s.quiet = !s.quiet;
			t.Write(s.quiet ? "\nQuiet mode enabled (no other users may page you)\n"
			                : "\nQuiet mode disabled (other users may page you)\n");
			break;
		case 'X':
			// "Expert mode now ON/OFF" — user_functions.c:861-865.
			s.flags ^= citadel::US_EXPERT;
			citadel::SetUserFlags(con, s.username, s.flags);
			t.Write(s.Expert() ? "\nExpert mode now ON\n" : "\nExpert mode now OFF\n");
			break;
		case ';':
			FloorCommand(con, s, t, rest);
			break;
		case '.':
			if (DotCommand(con, s, t, rest) == Outcome::Quit) {
				running = false;
			}
			break;
		case 'T':
			t.Write("\nGoodbye.\n");
			running = false;
			break;
		default:
			t.Write("\nUnknown command. Press ? for help.\n");
			break;
		}
	}

	citadel::UnregisterSession(con, s.session_id);
}

void HandleTelnetConn(DatabaseInstance &db, net::ClientStream &stream) {
	HandleTelnet(db, stream, g_telnet);
}
void HandleTelnetsConn(DatabaseInstance &db, net::ClientStream &stream) {
	HandleTelnet(db, stream, g_telnets);
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "qm_telnet", 2300, g_telnet, HandleTelnetConn);
	RegisterServerControls(loader, "qm_telnets", 2992, g_telnets, HandleTelnetsConn);
}

} // namespace

void QuackmailTelnetExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string QuackmailTelnetExtension::Name() {
	return "quackmail_telnet";
}

std::string QuackmailTelnetExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_TELNET
	return EXT_VERSION_QUACKMAIL_TELNET;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_telnet, loader) {
	duckdb::LoadInternal(loader);
}
}
