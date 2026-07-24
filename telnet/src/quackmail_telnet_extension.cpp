#define DUCKDB_EXTENSION_MAIN

#include "quackmail_telnet_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/citadel_msg.hpp"
#include "quackmail/citadel_store.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/telnet.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

namespace duckdb {
namespace {

using namespace quackmail;

// telnet (23; dev 2300) and telnets (992; dev 2992, implicit TLS). A real
// Citadel install has no telnet listener of its own — the BBS experience is the
// text client talking the native protocol — so this module *is* that client,
// running server-side. Its menu follows citadel.rc, which defines what the
// official client offers.
ServerController g_telnet;
ServerController g_telnets;

constexpr size_t kMaxPostBytes = 256 * 1024;

int64_t NowEpoch() {
	return (int64_t)std::time(nullptr);
}

struct Bbs {
	int64_t session_id = 0;
	bool authed = false;
	std::string username;
	int64_t usernum = 0;
	int64_t axlevel = 0;

	citadel::Room room;
	bool have_room = false;
	bool expert = false;             // <X> hides the menu
	std::vector<int64_t> ungoto;     // room history for <U>ngoto
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

// The room prompt character: ']' for directory rooms, '>' otherwise (the same
// rule as room_prompt() in the text client).
char PromptChar(const citadel::Room &room) {
	return (room.qr_flags & citadel::QR_DIRECTORY) ? ']' : '>';
}

void ShowMenu(telnet::Session &t) {
	t.Write(
	    "-----------------------------------------------------------------------\n"
	    "Room cmds:    <K>nown rooms, <G>oto next room, <.G>oto a specific room,\n"
	    "              <S>kip this room, <U>ngoto (move back)\n"
	    "Message cmds: <N>ew msgs, <F>orward read, <R>everse read, <O>ld msgs,\n"
	    "              <L>ast five msgs, <E>nter a message\n"
	    "General cmds: <?> help, <T>erminate, <W>ho is online, <P>age a user,\n"
	    "              <M>ail\n"
	    "Misc:         <X> toggle eXpert mode\n"
	    "\n"
	    " (Type <X> to hide this menu)\n"
	    "-----------------------------------------------------------------------\n");
}

// ------------------------------------------------------------------- rooms

void EnterRoom(Connection &con, Bbs &s, telnet::Session &t, const citadel::Room &room, bool remember) {
	if (remember && s.have_room) {
		s.ungoto.push_back(s.room.room_num);
	}
	s.room = room;
	s.have_room = true;
	auto st = citadel::GetRoomStats(con, s.username, room.room_num);
	t.Write("\n" + room.display_name + PromptChar(room) + "  " + std::to_string(st.new_count) +
	        " new of " + std::to_string(st.total) + " messages\n");
	if (!room.info.empty()) {
		t.Write(room.info + "\n");
	}
}

bool GotoNamed(Connection &con, Bbs &s, telnet::Session &t, const std::string &wanted) {
	citadel::Room room;
	if (!citadel::ResolveRoom(con, s.username, wanted, room)) {
		t.Write("\nNo such room.\n");
		return false;
	}
	EnterRoom(con, s, t, room, true);
	return true;
}

// <G>oto: the next room holding unread messages, wrapping around to the Lobby.
void GotoNext(Connection &con, Bbs &s, telnet::Session &t) {
	auto rooms = citadel::ListRooms(con, s.username, -1, "all");
	// Start scanning just past the current room so repeated <G> walks the list.
	size_t start = 0;
	if (s.have_room) {
		for (size_t i = 0; i < rooms.size(); i++) {
			if (rooms[i].room_num == s.room.room_num) {
				start = i + 1;
				break;
			}
		}
	}
	for (size_t n = 0; n < rooms.size(); n++) {
		const auto &r = rooms[(start + n) % rooms.size()];
		if (citadel::GetRoomStats(con, s.username, r.room_num).new_count > 0) {
			EnterRoom(con, s, t, r, true);
			return;
		}
	}
	t.Write("\nNo rooms with new messages. ");
	citadel::Room lobby;
	if (citadel::ResolveRoom(con, s.username, "Lobby", lobby)) {
		EnterRoom(con, s, t, lobby, true);
	}
}

// <K>nown rooms, grouped the way the text client groups them.
void KnownRooms(Connection &con, Bbs &s, telnet::Session &t) {
	auto rooms = citadel::ListRooms(con, s.username, -1, "all");
	std::vector<std::string> with_new, without_new;
	for (auto &r : rooms) {
		std::string entry = r.display_name + PromptChar(r) + "  ";
		if (citadel::GetRoomStats(con, s.username, r.room_num).new_count > 0) {
			with_new.push_back(entry);
		} else {
			without_new.push_back(entry);
		}
	}
	auto dump = [&t](const std::string &title, const std::vector<std::string> &list) {
		if (list.empty()) {
			return;
		}
		t.Write("\n   " + title + "\n");
		std::string line;
		for (auto &e : list) {
			if (line.size() + e.size() > 72) {
				t.Write(line + "\n");
				line.clear();
			}
			line += e;
		}
		if (!line.empty()) {
			t.Write(line + "\n");
		}
	};
	dump("Rooms with unread messages:", with_new);
	dump("No unseen messages in:", without_new);
}

void Ungoto(Connection &con, Bbs &s, telnet::Session &t) {
	if (s.ungoto.empty()) {
		t.Write("\nNo rooms to ungoto.\n");
		return;
	}
	int64_t prev = s.ungoto.back();
	s.ungoto.pop_back();
	citadel::Room room;
	if (citadel::GetRoomByNum(con, prev, room)) {
		EnterRoom(con, s, t, room, false);
	}
}

// ---------------------------------------------------------------- messages

void ShowMessage(telnet::Session &t, const citadel::Message &msg, const citadel::Room &room) {
	t.Write("\n [#" + std::to_string(msg.msgnum) + "] " + TimeString(msg.msgtime) + " from " + msg.author);
	if (!msg.recipient.empty()) {
		t.Write(" to " + msg.recipient);
	}
	t.Write(" in " + room.display_name + "\n");
	if (!msg.subject.empty()) {
		t.Write("Subject: " + msg.subject + "\n");
	}
	std::string body = citadel::BodyText(msg);
	t.Write(body);
	if (!body.empty() && body.back() != '\n') {
		t.Write("\n");
	}
}

// which: "new" | "old" | "all"; reverse walks newest-first; `last` caps the count.
void ReadMessages(Connection &con, Bbs &s, telnet::Session &t, const std::string &which, bool reverse,
                  int64_t last) {
	if (!s.have_room) {
		t.Write("\nNot in a room.\n");
		return;
	}
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
		ShowMessage(t, msg, s.room);
		highest = std::max(highest, n);
	}
	// Reading marks the room read up to the highest message shown.
	citadel::SetLastRead(con, s.username, s.room.room_num, highest);
}

void EnterMessage(Connection &con, Bbs &s, telnet::Session &t) {
	if (!s.have_room) {
		t.Write("\nNot in a room.\n");
		return;
	}
	if (s.room.qr_flags & citadel::QR_READONLY) {
		t.Write("\nYou may not enter messages in this type of room.\n");
		return;
	}

	std::string recipient;
	if (s.room.mailbox_owner > 0) {
		t.Write("\nEnter recipient: ");
		if (!t.ReadLine(recipient)) {
			return;
		}
		if (!recipient.empty() && citadel::GetOrAssignUserNum(con, recipient) <= 0) {
			t.Write("No such user.\n");
			return;
		}
	}

	t.Write("\nSubject: ");
	std::string subject;
	if (!t.ReadLine(subject)) {
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

// ------------------------------------------------------- presence + paging

void WhoIsOnline(Connection &con, telnet::Session &t) {
	t.Write("\n Session  User                 Room                 Client\n");
	for (auto &sess : citadel::ListSessions(con)) {
		std::string user = sess.username.empty() ? "(not logged in)" : sess.username;
		std::string line = " " + std::to_string(sess.session_id);
		line.resize(9, ' ');
		user.resize(std::max<size_t>(user.size(), 21), ' ');
		std::string room = sess.room;
		room.resize(std::max<size_t>(room.size(), 21), ' ');
		t.Write(line + user + room + sess.client + "\n");
	}
}

void PageUser(Connection &con, Bbs &s, telnet::Session &t) {
	t.Write("\nEnter user name: ");
	std::string to;
	if (!t.ReadLine(to) || to.empty()) {
		return;
	}
	t.Write("Enter message: ");
	std::string text;
	if (!t.ReadLine(text)) {
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
	if (!s.authed) {
		return;
	}
	for (auto &e : citadel::PendingExpress(con, s.username)) {
		t.Write("\n---\nMessage from " + e.from_user + ":\n" + e.text + "\n---\n");
		citadel::MarkExpressDelivered(con, e.id);
	}
}

// ------------------------------------------------------------------- login

bool Login(Connection &con, Bbs &s, telnet::Session &t) {
	for (int attempt = 0; attempt < 3; attempt++) {
		t.Write("\nEnter your name (or 'new' to register): ");
		std::string name;
		if (!t.ReadLine(name)) {
			return false;
		}
		if (name.empty()) {
			continue;
		}

		if (util::Upper(name) == "NEW") {
			t.Write("Enter a user name: ");
			std::string newname;
			if (!t.ReadLine(newname) || newname.empty()) {
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
			name = newname;
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
		s.usernum = citadel::GetOrAssignUserNum(con, name);
		s.axlevel = citadel::GetAxLevel(con, name);
		citadel::EnsureUserRooms(con, name);
		return true;
	}
	t.Write("\nToo many failed attempts.\n");
	return false;
}

// ------------------------------------------------------------ command loop

// A "." command, e.g. ".Goto Lobby": `rest` is whatever followed the dot.
void DotCommand(Connection &con, Bbs &s, telnet::Session &t, const std::string &rest) {
	std::string up = util::Upper(rest);
	std::string arg;
	size_t sp = rest.find(' ');
	if (sp != std::string::npos) {
		arg = rest.substr(sp + 1);
	}
	if (up.rfind("G", 0) == 0) { // .Goto <room>
		if (arg.empty()) {
			t.Write("\nGoto which room? ");
			t.ReadLine(arg);
		}
		GotoNamed(con, s, t, arg);
	} else if (up.rfind("K", 0) == 0) {
		KnownRooms(con, s, t);
	} else if (up.rfind("T", 0) == 0) {
		t.Write("\nGoodbye.\n");
	} else if (up.rfind("H", 0) == 0) {
		ShowMenu(t);
	} else {
		t.Write("\nUnknown command.\n");
	}
}

void HandleTelnet(DatabaseInstance &db, net::ClientStream &stream, ServerController &ctrl) {
	Connection con(db);
	store::EnsureSchema(con);

	telnet::Session t(stream);
	t.Negotiate();

	Bbs s;
	s.session_id = citadel::RegisterSession(con, ctrl.ImplicitTls() ? "Telnets session" : "Telnet session");

	std::string humannode = citadel::GetConfig(con, "c_humannode", "QuackCit BBS");
	std::string city = citadel::GetConfig(con, "c_bbs_city", "");
	std::string version = citadel::GetConfig(con, "c_version", "QuackCit");

	t.Write("\n" + humannode + (city.empty() ? "" : " - " + city) + "\n" + version + "\n");

	if (!Login(con, s, t)) {
		citadel::UnregisterSession(con, s.session_id);
		return;
	}
	t.Write("\nWelcome, " + s.username + ".\n");

	citadel::Room lobby;
	if (citadel::ResolveRoom(con, s.username, "Lobby", lobby)) {
		EnterRoom(con, s, t, lobby, false);
	}

	bool running = true;
	while (running) {
		ShowPendingExpress(con, s, t);
		if (!s.expert) {
			ShowMenu(t);
		}
		citadel::TouchSession(con, s.session_id, s.username,
		                      s.have_room ? s.room.display_name : std::string(), "idle", s.axlevel);

		t.Write("\n" + (s.have_room ? s.room.display_name : std::string("(no room)")) +
		        std::string(1, s.have_room ? PromptChar(s.room) : '>') + " ");

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
			if (ch == '\r' || ch == '\n') {
				continue;
			}
			cmd = (char)std::toupper((unsigned char)ch);
			if (cmd == '.') {
				t.Write("."); // the rest of a dot command is typed as a line
				if (!t.ReadLine(rest)) {
					break;
				}
			} else {
				t.Write(std::string(1, cmd) + "\n");
			}
		} else {
			std::string cmdline;
			if (!t.ReadLine(cmdline)) {
				break;
			}
			if (cmdline.empty()) {
				continue;
			}
			cmd = (char)std::toupper((unsigned char)cmdline[0]);
			rest = cmdline.substr(1);
		}
		citadel::TouchSession(con, s.session_id, s.username,
		                      s.have_room ? s.room.display_name : std::string(), std::string(1, cmd),
		                      s.axlevel);

		switch (cmd) {
		case '?':
			ShowMenu(t);
			break;
		case 'K':
			KnownRooms(con, s, t);
			break;
		case 'G':
			GotoNext(con, s, t);
			break;
		case 'S': // skip: leave the room unread
			GotoNext(con, s, t);
			break;
		case 'U':
			Ungoto(con, s, t);
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
		case 'W':
			WhoIsOnline(con, t);
			break;
		case 'P':
			PageUser(con, s, t);
			break;
		case 'X':
			s.expert = !s.expert;
			t.Write(s.expert ? "\nExpert mode ON\n" : "\nExpert mode OFF\n");
			break;
		case '.':
			DotCommand(con, s, t, rest);
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
