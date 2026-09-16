#pragma once

// File areas: the QR_UPLOAD / QR_DOWNLOAD / QR_VISDIR room flags, made real.
//
// Those three bits have been defined in citadel_store.hpp since the beginning
// and nothing has ever read them. The only file-area behaviour anywhere in the
// tree was the telnet prompt printing ']' for a QR_DIRECTORY room.
//
// **A file is a message.** A directory room's files are ordinary euid-keyed
// messages whose body is a one-part MIME message with a filename — the same
// shape as a mail attachment, and the same shape the wiki already uses for its
// binary payloads (core/src/wiki.cpp). That is the choice that matters here,
// and it is what makes storage quotas, the DAV tombstones, the room ACL and
// KillRoom's cleanup all apply to files without any of them being told about
// files. A separate table would have had to re-earn every one of those.
//
// Three front doors read this: the telnet `.Read File` family, WebDAV under
// /dav/files/, and the FTP listener. They ask this API rather than each
// re-deriving "a file is an attachment part", for the same reason every
// front-end that accepts a message asks CanPost rather than re-deriving that.

#include "duckdb.hpp"
#include "quackmail/citadel_store.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace quackmail {
namespace filearea {

// One file in a directory room.
struct File {
	int64_t msgnum = 0;
	std::string name;        // as uploaded, already sanitized
	std::string description; // the message subject, if the uploader gave one
	std::string uploader;
	int64_t uploaded_at = 0;
	int64_t size = 0; // the decoded payload, not the stored MIME
	std::string content_type;

	File();
};

// Is this room a file area at all?
bool IsFileArea(const citadel::Room &room);

// May `user` see that this room's files exist?
//
// QR_VISDIR is the "visible directory" bit: without it, a user who cannot
// download cannot see the listing either. With it, the names are public even
// where the contents are not — which is the point of it.
bool CanList(duckdb::Connection &con, const std::string &user, const citadel::Room &room);

// May `user` fetch a file's contents? QR_DOWNLOAD plus the room's own read
// right.
bool CanDownload(duckdb::Connection &con, const std::string &user, const citadel::Room &room);

// May `user` deposit one? QR_UPLOAD plus CanPost — deliberately CanPost and not
// a new predicate, because depositing a file in a room is posting to it, and
// Citadel has exactly one answer to "may this person write here".
bool CanUpload(duckdb::Connection &con, const std::string &user, const citadel::Room &room);

// Every file in the room, oldest first. Does not check permission: the caller
// has already asked CanList, and this is also what an admin path wants.
std::vector<File> ListFiles(duckdb::Connection &con, int64_t room_num);

// One file by name. False when there is no such file.
bool StatFile(duckdb::Connection &con, int64_t room_num, const std::string &name, File &out);

// One file by name, with its bytes.
bool GetFile(duckdb::Connection &con, int64_t room_num, const std::string &name, File &out,
             std::string &content);

// Store a file, replacing any file of the same name. Returns the msgnum, or -1
// with `err` set.
//
// The name is sanitized here rather than by each caller: it arrives from a
// remote client over three different protocols and becomes a resource name in
// all of them. Storage quota is not checked here either — it is enforced inside
// InsertMessage, which is the choke point every front door already goes
// through.
int64_t PutFile(duckdb::Connection &con, int64_t room_num, const std::string &name,
                const std::string &content, const std::string &content_type,
                const std::string &description, const std::string &uploader, std::string &err);

// Remove a file by name. False when there was none, or with `err` set.
bool RemoveFile(duckdb::Connection &con, int64_t room_num, const std::string &name,
                std::string &err);

// Change a file's description without touching its bytes.
bool DescribeFile(duckdb::Connection &con, int64_t room_num, const std::string &name,
                  const std::string &description, std::string &err);

// The name a file is stored and served under: no path separators, no leading
// dot, nothing that could climb out of the room it is in. Returns "" for a name
// with nothing usable left, which the caller must refuse rather than store.
std::string SanitizeName(const std::string &name);

} // namespace filearea
} // namespace quackmail
