#define DUCKDB_EXTENSION_MAIN

#include "quackmail_ftp_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/citadel_store.hpp"
#include "quackmail/filearea.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/net.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/util.hpp"

#include <cstdio>
#include <ctime>
#include <string>
#include <unistd.h>
#include <vector>

namespace duckdb {
namespace {

using namespace quackmail;
namespace filearea = quackmail::filearea;

// Two listeners over one implementation, the smtp_out/pop3 pattern: explicit
// AUTH TLS on the plaintext port (21; dev 1021) and implicit FTPS (990; dev
// 1990).
ServerController g_ftp;
ServerController g_ftps;

// How long to wait for a client to connect back to a passive port. A data
// channel nobody connects to must not hold the session thread for the kernel's
// own timeout.
const int kDataAcceptMs = 30000;

// The ceiling on one upload held in memory. The storage quota inside
// InsertMessage is the real limit; this is the one that stops a transfer before
// all of it is resident.
const size_t kMaxUpload = 64u * 1024 * 1024;

struct Ftp {
	Connection *con = nullptr;
	net::ClientStream *ctrl = nullptr;
	ServerController *server = nullptr;

	bool authed = false;
	std::string user_given; // USER before PASS
	std::string username;
	int64_t session_id = 0;

	// "" = the root (the list of file areas); otherwise the room we are in.
	bool in_room = false;
	citadel::Room room;

	bool tls = false;      // the control connection is protected
	bool prot_private = false; // PROT P: protect the data channel too

	// The passive listener, if one is pending. FTP allows exactly one at a time.
	int pasv_fd = -1;
	int pasv_port = 0;

	std::string rename_from; // RNFR, waiting for RNTO
};

void Reply(Ftp &s, int code, const std::string &text) {
	s.ctrl->WriteLine(std::to_string(code) + " " + text);
}

// A multi-line reply: "code-first line", then continuation, then "code last".
void ReplyMulti(Ftp &s, int code, const std::vector<std::string> &lines) {
	for (size_t i = 0; i + 1 < lines.size(); i++) {
		s.ctrl->WriteLine(std::to_string(code) + "-" + lines[i]);
	}
	s.ctrl->WriteLine(std::to_string(code) + " " + (lines.empty() ? std::string() : lines.back()));
}

// core/util.hpp has no trim, and adding one there would rebuild every extension
// for four lines. FTP needs it because a control line arrives CRLF-terminated
// and clients pad arguments inconsistently.
std::string Trim(const std::string &in) {
	size_t b = in.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return std::string();
	}
	size_t e = in.find_last_not_of(" \t\r\n");
	return in.substr(b, e - b + 1);
}

std::string ConfigStr(Connection &con, const std::string &key, const std::string &dflt) {
	return citadel::GetConfig(con, key, dflt);
}

bool ConfigBool(Connection &con, const std::string &key, bool dflt) {
	std::string v = ConfigStr(con, key, dflt ? "1" : "0");
	return !(v == "0" || util::Lower(v) == "false" || v.empty());
}

int ConfigInt(Connection &con, const std::string &key, int dflt) {
	std::string v = ConfigStr(con, key, "");
	if (v.empty()) {
		return dflt;
	}
	char *end = nullptr;
	long n = std::strtol(v.c_str(), &end, 10);
	if (end == v.c_str() || *end != '\0' || n < 0 || n > 65535) {
		return dflt;
	}
	return (int)n;
}

// ---- the filesystem view -------------------------------------------------
//
// Two levels, and no deeper: a Citadel room has no sub-rooms. "/" lists the file
// areas this user can see; "/<room>/" lists its files.

// The file areas visible to this user, by display name.
std::vector<citadel::Room> VisibleAreas(Ftp &s) {
	std::vector<citadel::Room> out;
	for (auto &room : citadel::ListRooms(*s.con, s.username, -1, "all")) {
		if (!filearea::IsFileArea(room)) {
			continue;
		}
		if (!filearea::CanList(*s.con, s.username, room)) {
			continue;
		}
		if (!citadel::RoomUnlocked(*s.con, s.username, room)) {
			continue;
		}
		out.push_back(room);
	}
	return out;
}

bool FindArea(Ftp &s, const std::string &name, citadel::Room &out) {
	for (auto &room : VisibleAreas(s)) {
		if (util::Lower(room.display_name) == util::Lower(name)) {
			out = room;
			return true;
		}
	}
	return false;
}

std::string CurrentPath(const Ftp &s) {
	return s.in_room ? "/" + s.room.display_name : "/";
}

// ---- the data channel ----------------------------------------------------

// Open the data connection a PASV/EPSV set up. Returns null and answers the
// client if there is none or it never connects.
std::unique_ptr<net::ClientStream> OpenData(Ftp &s) {
	if (s.pasv_fd < 0) {
		Reply(s, 425, "Use PASV or EPSV first.");
		return nullptr;
	}
	std::string err;
	int fd = net::AcceptOnce(s.pasv_fd, kDataAcceptMs, err);
	::close(s.pasv_fd);
	s.pasv_fd = -1;
	if (fd < 0) {
		Reply(s, 425, "Can't open data connection: " + err);
		return nullptr;
	}
	auto data = make_uniq<net::ClientStream>(fd);
	if (s.prot_private) {
		// PROT P: the data channel is wrapped in TLS from the control
		// connection's context.
		std::string terr;
		if (!data->AcceptTls(s.server->TlsCtx(), terr)) {
			Reply(s, 425, "Can't protect the data connection: " + terr);
			return nullptr;
		}
	}
	return data;
}

void ClosePasv(Ftp &s) {
	if (s.pasv_fd >= 0) {
		::close(s.pasv_fd);
		s.pasv_fd = -1;
	}
}

// ---- listings ------------------------------------------------------------

std::string TimeMlsd(int64_t when) {
	struct tm tm_v {};
	time_t t = (time_t)when;
	gmtime_r(&t, &tm_v);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", &tm_v);
	return buf;
}

std::string TimeLs(int64_t when) {
	struct tm tm_v {};
	time_t t = (time_t)when;
	gmtime_r(&t, &tm_v);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%b %d %Y", &tm_v);
	return buf;
}

// The Unix-ls form, for the clients that still parse it. MLSD below is the
// machine-readable one and is what anything modern uses.
std::string LsLine(const std::string &name, bool dir, int64_t size, int64_t when,
                   const std::string &owner) {
	char buf[512];
	std::snprintf(buf, sizeof(buf), "%s%s 1 %-8s %-8s %12lld %s %s",
	              dir ? "d" : "-", dir ? "rwxr-xr-x" : "rw-r--r--", owner.c_str(), "ftp",
	              (long long)size, TimeLs(when).c_str(), name.c_str());
	return buf;
}

std::string MlsdLine(const std::string &name, bool dir, int64_t size, int64_t when) {
	std::string out = "type=" + std::string(dir ? "dir" : "file") + ";";
	if (!dir) {
		out += "size=" + std::to_string(size) + ";";
	}
	out += "modify=" + TimeMlsd(when) + "; " + name;
	return out;
}

void SendListing(Ftp &s, bool names_only, bool mlsd) {
	auto data = OpenData(s);
	if (!data) {
		return;
	}
	Reply(s, 150, "Here comes the directory listing.");

	std::string body;
	if (!s.in_room) {
		for (auto &room : VisibleAreas(s)) {
			if (names_only) {
				body += room.display_name + "\r\n";
			} else if (mlsd) {
				body += MlsdLine(room.display_name, true, 0, 0) + "\r\n";
			} else {
				body += LsLine(room.display_name, true, 0, 0, "citadel") + "\r\n";
			}
		}
	} else {
		for (auto &f : filearea::ListFiles(*s.con, s.room.room_num)) {
			if (names_only) {
				body += f.name + "\r\n";
			} else if (mlsd) {
				body += MlsdLine(f.name, false, f.size, f.uploaded_at) + "\r\n";
			} else {
				body += LsLine(f.name, false, f.size, f.uploaded_at, f.uploader) + "\r\n";
			}
		}
	}
	data->Write(body);
	data.reset();
	Reply(s, 226, "Directory send OK.");
}

// ---- commands ------------------------------------------------------------

void CmdPasv(Ftp &s, bool extended) {
	ClosePasv(s);
	int low = ConfigInt(*s.con, "qm_ftp_pasv_low", 0);
	int high = ConfigInt(*s.con, "qm_ftp_pasv_high", 0);
	std::string err;
	int port = 0;
	int fd = net::ListenEphemeral(s.server->Host(), low, high, port, err);
	if (fd < 0) {
		Reply(s, 425, "Can't open a data port: " + err);
		return;
	}
	s.pasv_fd = fd;
	s.pasv_port = port;

	if (extended) {
		// RFC 2428. The delimiter form with no address is what a client behind
		// NAT wants: it reuses the control connection's peer address.
		Reply(s, 229, "Entering Extended Passive Mode (|||" + std::to_string(port) + "|)");
		return;
	}
	// The classic h1,h2,h3,h4,p1,p2 form. The address is the one the listener is
	// bound to; ServerController binds AF_INET, so there is always one.
	std::string host = s.server->Host();
	if (host.empty() || host == "0.0.0.0") {
		host = "127.0.0.1";
	}
	std::string dotted = host;
	for (auto &ch : dotted) {
		if (ch == '.') {
			ch = ',';
		}
	}
	Reply(s, 227, "Entering Passive Mode (" + dotted + "," + std::to_string(port / 256) + "," +
	                  std::to_string(port % 256) + ").");
}

void CmdRetr(Ftp &s, const std::string &arg) {
	if (!s.in_room) {
		Reply(s, 550, "Not inside a file area.");
		return;
	}
	if (!filearea::CanDownload(*s.con, s.username, s.room)) {
		Reply(s, 550, "Downloads are not enabled in this area.");
		return;
	}
	filearea::File f;
	std::string content;
	if (!filearea::GetFile(*s.con, s.room.room_num, arg, f, content)) {
		Reply(s, 550, "No such file.");
		return;
	}
	auto data = OpenData(s);
	if (!data) {
		return;
	}
	Reply(s, 150, "Opening data connection for " + f.name + " (" + std::to_string(f.size) +
	                  " bytes).");
	data->Write(content);
	data.reset();
	Reply(s, 226, "Transfer complete.");
}

void CmdStor(Ftp &s, const std::string &arg) {
	if (!s.in_room) {
		Reply(s, 550, "Not inside a file area.");
		return;
	}
	if (!filearea::CanUpload(*s.con, s.username, s.room)) {
		Reply(s, 550, "Uploads are not enabled in this area.");
		return;
	}
	std::string clean = filearea::SanitizeName(arg);
	if (clean.empty()) {
		Reply(s, 553, "That filename cannot be stored.");
		return;
	}
	auto data = OpenData(s);
	if (!data) {
		return;
	}
	Reply(s, 150, "Ok to send data.");

	std::string content;
	std::string chunk;
	while (data->ReadAvailable(chunk, 65536)) {
		content += chunk;
		if (content.size() > kMaxUpload) {
			data.reset();
			Reply(s, 552, "File too large.");
			return;
		}
	}
	data.reset();

	std::string err;
	if (filearea::PutFile(*s.con, s.room.room_num, clean, content, std::string(), std::string(),
	                      s.username, err) < 0) {
		// Over quota is transient everywhere else in this server, for the same
		// reason: a client that retries should succeed once room is made.
		Reply(s, 452, err.empty() ? "Insufficient storage." : err);
		return;
	}
	Reply(s, 226, "Transfer complete.");
}

void CmdMkd(Ftp &s, const std::string &arg) {
	if (s.in_room) {
		Reply(s, 550, "A file area has no subdirectories.");
		return;
	}
	// Creating a directory creates a room, so it takes the same permission
	// creating one from the web or over MKCOL takes.
	if (!citadel::MayCreateRoom(*s.con, s.username, 0)) {
		Reply(s, 550, "You may not create file areas here.");
		return;
	}
	citadel::Room clash;
	if (citadel::ResolveRoom(*s.con, s.username, arg, clash)) {
		Reply(s, 550, "There is already a room with that name.");
		return;
	}
	std::string err;
	int64_t flags = citadel::QR_DIRECTORY | citadel::QR_UPLOAD | citadel::QR_DOWNLOAD |
	                citadel::QR_VISDIR;
	int64_t room_num = citadel::CreateRoom(*s.con, arg, 0, flags, "", 0, err);
	if (room_num < 0) {
		Reply(s, 550, err.empty() ? "Could not create the area." : err);
		return;
	}
	citadel::Room made;
	if (citadel::GetRoomByNum(*s.con, room_num, made)) {
		// The creator's grant, without which a non-aide holds derived rights
		// only and cannot write to what they just made.
		citadel::SetRights(*s.con, made, s.username, citadel::kAclRights, err);
	}
	Reply(s, 257, "\"/" + arg + "\" created.");
}

void CmdDele(Ftp &s, const std::string &arg) {
	if (!s.in_room) {
		Reply(s, 550, "Not inside a file area.");
		return;
	}
	if (!citadel::CanPost(*s.con, s.username, s.room)) {
		Reply(s, 550, "You may not remove files here.");
		return;
	}
	std::string err;
	if (!filearea::RemoveFile(*s.con, s.room.room_num, arg, err)) {
		Reply(s, 550, err.empty() ? "No such file." : err);
		return;
	}
	Reply(s, 250, "Delete operation successful.");
}

void CmdCwd(Ftp &s, const std::string &arg) {
	std::string want = arg;
	// Only the two levels exist, so the path handling is deliberately small:
	// anything that is not "/" or an area name is a 550 rather than an attempt
	// to walk a tree that has no depth.
	while (want.size() > 1 && want.back() == '/') {
		want.pop_back();
	}
	if (want.empty() || want == "/" || want == "..") {
		s.in_room = false;
		Reply(s, 250, "Directory successfully changed.");
		return;
	}
	if (want.front() == '/') {
		want.erase(0, 1);
	}
	if (want.find('/') != std::string::npos) {
		Reply(s, 550, "A file area has no subdirectories.");
		return;
	}
	citadel::Room room;
	if (!FindArea(s, want, room)) {
		Reply(s, 550, "No such file area.");
		return;
	}
	s.room = room;
	s.in_room = true;
	Reply(s, 250, "Directory successfully changed.");
}

// ---- the session ---------------------------------------------------------

void HandleFtp(DatabaseInstance &db, net::ClientStream &stream, ServerController &ctrl) {
	Connection con(db);
	store::EnsureSchema(con);

	Ftp s;
	s.con = &con;
	s.ctrl = &stream;
	s.server = &ctrl;
	s.tls = ctrl.ImplicitTls();

	const bool allow_cleartext = ConfigBool(con, "qm_ftp_allow_cleartext", false);

	Reply(s, 220, ConfigStr(con, "c_humannode", "QuackCit") + " FTP service ready.");

	std::string line;
	while (stream.ReadLine(line, 4096)) {
		std::string verb = util::Upper(Trim(line));
		std::string arg;
		size_t sp = verb.find(' ');
		if (sp != std::string::npos) {
			arg = Trim(line.substr(line.find(' ') + 1));
			verb = verb.substr(0, sp);
		}

		if (verb == "QUIT") {
			Reply(s, 221, "Goodbye.");
			break;
		}
		if (verb == "NOOP") {
			Reply(s, 200, "NOOP ok.");
			continue;
		}
		if (verb == "AUTH") {
			if (s.tls) {
				Reply(s, 534, "Already protected.");
				continue;
			}
			if (util::Upper(arg) != "TLS" && util::Upper(arg) != "TLS-C" && util::Upper(arg) != "SSL") {
				Reply(s, 504, "Only AUTH TLS is supported.");
				continue;
			}
			if (!ctrl.StartTlsEnabled()) {
				Reply(s, 431, "TLS is not configured on this server.");
				continue;
			}
			Reply(s, 234, "Proceed with negotiation.");
			std::string terr;
			if (!stream.StartTls(ctrl.TlsCtx(), terr)) {
				break;
			}
			s.tls = true;
			continue;
		}
		if (verb == "PBSZ") {
			// RFC 4217: over TLS the only legal size is 0.
			Reply(s, 200, "PBSZ=0");
			continue;
		}
		if (verb == "PROT") {
			if (!s.tls) {
				Reply(s, 503, "AUTH TLS first.");
				continue;
			}
			std::string want = util::Upper(arg);
			if (want == "P") {
				s.prot_private = true;
				Reply(s, 200, "Data channel protection set to private.");
			} else if (want == "C") {
				s.prot_private = false;
				Reply(s, 200, "Data channel protection set to clear.");
			} else {
				Reply(s, 504, "Only PROT C and PROT P are supported.");
			}
			continue;
		}
		if (verb == "FEAT") {
			ReplyMulti(s, 211,
			           {"Features:", " UTF8", " MLST type*;size*;modify*;", " MLSD", " SIZE",
			            " MDTM", " EPSV", " AUTH TLS", " PBSZ", " PROT", "End"});
			continue;
		}
		if (verb == "OPTS") {
			// "OPTS UTF8 ON" is what every client sends first; everything else
			// is refused rather than silently accepted.
			Reply(s, util::Upper(arg).rfind("UTF8", 0) == 0 ? 200 : 501,
			      util::Upper(arg).rfind("UTF8", 0) == 0 ? "Always in UTF8 mode." : "Option not understood.");
			continue;
		}

		if (verb == "USER") {
			s.user_given = arg;
			s.authed = false;
			if (!s.tls && !allow_cleartext) {
				// Every other protocol in this tree has a TLS story, and a
				// password over cleartext FTP is the one thing this server
				// should not invite by default. qm_ftp_allow_cleartext opts in.
				Reply(s, 534, "Policy requires AUTH TLS before authentication.");
				continue;
			}
			Reply(s, 331, "Please specify the password.");
			continue;
		}
		if (verb == "PASS") {
			if (s.user_given.empty()) {
				Reply(s, 503, "Login with USER first.");
				continue;
			}
			if (!s.tls && !allow_cleartext) {
				Reply(s, 534, "Policy requires AUTH TLS before authentication.");
				continue;
			}
			if (!auth::Verify(con, s.user_given, arg)) {
				Reply(s, 530, "Login incorrect.");
				s.user_given.clear();
				continue;
			}
			s.username = s.user_given;
			s.authed = true;
			citadel::EnsureUserRooms(con, s.username);
			s.session_id = citadel::RegisterSession(con, ctrl.ImplicitTls() ? "FTPS session" : "FTP session",
			                                        stream.PeerIp());
			Reply(s, 230, "Login successful.");
			continue;
		}

		if (!s.authed) {
			Reply(s, 530, "Please login with USER and PASS.");
			continue;
		}
		citadel::TouchSession(con, s.session_id, s.username, CurrentPath(s), verb, 0);

		if (verb == "SYST") {
			Reply(s, 215, "UNIX Type: L8");
		} else if (verb == "TYPE") {
			// Every transfer here is binary. Answering 200 to TYPE A and then
			// sending bytes unchanged is what every modern server does; refusing
			// it breaks clients that send it out of habit.
			Reply(s, 200, "Switching to Binary mode.");
		} else if (verb == "PWD" || verb == "XPWD") {
			Reply(s, 257, "\"" + CurrentPath(s) + "\" is the current directory.");
		} else if (verb == "CWD" || verb == "XCWD") {
			CmdCwd(s, arg);
		} else if (verb == "CDUP" || verb == "XCUP") {
			s.in_room = false;
			Reply(s, 250, "Directory successfully changed.");
		} else if (verb == "PASV") {
			CmdPasv(s, false);
		} else if (verb == "EPSV") {
			CmdPasv(s, true);
		} else if (verb == "PORT" || verb == "EPRT") {
			// Active mode makes the server dial an address the client names,
			// which is a port-scanning primitive and useless behind NAT anyway.
			Reply(s, 502, "Active mode is not supported; use PASV or EPSV.");
		} else if (verb == "LIST") {
			SendListing(s, false, false);
		} else if (verb == "NLST") {
			SendListing(s, true, false);
		} else if (verb == "MLSD") {
			SendListing(s, false, true);
		} else if (verb == "MLST") {
			filearea::File f;
			if (s.in_room && filearea::StatFile(con, s.room.room_num, arg, f)) {
				ReplyMulti(s, 250, {"Listing " + f.name,
				                    " " + MlsdLine(f.name, false, f.size, f.uploaded_at), "End"});
			} else {
				Reply(s, 550, "No such file.");
			}
		} else if (verb == "RETR") {
			CmdRetr(s, arg);
		} else if (verb == "STOR") {
			CmdStor(s, arg);
		} else if (verb == "DELE") {
			CmdDele(s, arg);
		} else if (verb == "SIZE") {
			filearea::File f;
			if (s.in_room && filearea::StatFile(con, s.room.room_num, arg, f)) {
				Reply(s, 213, std::to_string(f.size));
			} else {
				Reply(s, 550, "Could not get file size.");
			}
		} else if (verb == "MDTM") {
			filearea::File f;
			if (s.in_room && filearea::StatFile(con, s.room.room_num, arg, f)) {
				Reply(s, 213, TimeMlsd(f.uploaded_at));
			} else {
				Reply(s, 550, "Could not get file modification time.");
			}
		} else if (verb == "MKD" || verb == "XMKD") {
			CmdMkd(s, arg);
		} else if (verb == "RMD" || verb == "XRMD") {
			citadel::Room room;
			if (s.in_room || !FindArea(s, arg, room)) {
				Reply(s, 550, "No such file area.");
			} else if (!citadel::CanAdminister(con, s.username, room)) {
				Reply(s, 550, "You may not remove that area.");
			} else {
				std::string err;
				Reply(s, citadel::KillRoom(con, room.room_num, err) ? 250 : 550,
				      err.empty() ? "Remove directory operation successful." : err);
			}
		} else if (verb == "RNFR") {
			filearea::File f;
			if (s.in_room && filearea::StatFile(con, s.room.room_num, arg, f)) {
				s.rename_from = f.name;
				Reply(s, 350, "Ready for RNTO.");
			} else {
				Reply(s, 550, "No such file.");
			}
		} else if (verb == "RNTO") {
			if (s.rename_from.empty()) {
				Reply(s, 503, "RNFR required first.");
			} else if (!filearea::CanUpload(con, s.username, s.room)) {
				Reply(s, 550, "You may not write here.");
			} else {
				// A rename is a store under the new name and a remove of the
				// old: the name is the message's euid, not a column to update.
				filearea::File f;
				std::string content;
				std::string err;
				std::string to = filearea::SanitizeName(arg);
				if (to.empty() ||
				    !filearea::GetFile(con, s.room.room_num, s.rename_from, f, content)) {
					Reply(s, 550, "Rename failed.");
				} else if (filearea::PutFile(con, s.room.room_num, to, content, f.content_type,
				                             f.description, f.uploader, err) < 0) {
					Reply(s, 550, err.empty() ? "Rename failed." : err);
				} else {
					filearea::RemoveFile(con, s.room.room_num, s.rename_from, err);
					Reply(s, 250, "Rename successful.");
				}
				s.rename_from.clear();
			}
		} else {
			Reply(s, 502, "Command not implemented.");
		}
	}

	ClosePasv(s);
	if (s.session_id > 0) {
		citadel::UnregisterSession(con, s.session_id);
	}
}

void HandleFtpConn(DatabaseInstance &db, net::ClientStream &stream) {
	HandleFtp(db, stream, g_ftp);
}
void HandleFtpsConn(DatabaseInstance &db, net::ClientStream &stream) {
	HandleFtp(db, stream, g_ftps);
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "qm_ftp", 1021, g_ftp, HandleFtpConn);
	RegisterServerControls(loader, "qm_ftps", 1990, g_ftps, HandleFtpsConn);
}

} // namespace

void QuackmailFtpExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailFtpExtension::Name() {
	return "quackmail_ftp";
}
std::string QuackmailFtpExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_FTP
	return EXT_VERSION_QUACKMAIL_FTP;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_ftp, loader) {
	duckdb::LoadInternal(loader);
}
}
