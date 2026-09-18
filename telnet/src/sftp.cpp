#include "sftp.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/filearea.hpp"
#include "quackmail/sshkeys.hpp"

#include <cstdio>
#include <ctime>

namespace quackmail {
namespace sshd {

using ssh::Reader;
using ssh::Writer;

namespace {

enum : uint8_t {
	FXP_INIT = 1,
	FXP_VERSION = 2,
	FXP_OPEN = 3,
	FXP_CLOSE = 4,
	FXP_READ = 5,
	FXP_WRITE = 6,
	FXP_LSTAT = 7,
	FXP_FSTAT = 8,
	FXP_SETSTAT = 9,
	FXP_FSETSTAT = 10,
	FXP_OPENDIR = 11,
	FXP_READDIR = 12,
	FXP_REMOVE = 13,
	FXP_MKDIR = 14,
	FXP_RMDIR = 15,
	FXP_REALPATH = 16,
	FXP_STAT = 17,
	FXP_RENAME = 18,
	FXP_READLINK = 19,
	FXP_SYMLINK = 20,
	FXP_STATUS = 101,
	FXP_HANDLE = 102,
	FXP_DATA = 103,
	FXP_NAME = 104,
	FXP_ATTRS = 105,
	FXP_EXTENDED = 200,
};

enum : uint32_t {
	FX_OK = 0,
	FX_EOF = 1,
	FX_NO_SUCH_FILE = 2,
	FX_PERMISSION_DENIED = 3,
	FX_FAILURE = 4,
	FX_BAD_MESSAGE = 5,
	FX_OP_UNSUPPORTED = 8,
};

enum : uint32_t {
	ATTR_SIZE = 0x1,
	ATTR_UIDGID = 0x2,
	ATTR_PERMISSIONS = 0x4,
	ATTR_ACMODTIME = 0x8,
	ATTR_EXTENDED = 0x80000000u,
};

enum : uint32_t {
	PF_READ = 0x1,
	PF_WRITE = 0x2,
	PF_APPEND = 0x4,
	PF_CREAT = 0x8,
	PF_TRUNC = 0x10,
	PF_EXCL = 0x20,
};

// The same ceiling the FTP listener puts on one upload. A write handle holds
// the whole file in memory until CLOSE, so this is also a memory bound.
constexpr size_t kMaxUpload = 64u * 1024 * 1024;
constexpr size_t kMaxRead = 256 * 1024;
constexpr size_t kMaxHandles = 64;
// The largest request we will buffer: a WRITE of the biggest chunk any client
// sends (OpenSSH: 32 KiB, WinSCP: up to 256 KiB) plus headers.
constexpr uint32_t kMaxRequest = 1024 * 1024;

struct Attrs {
	bool dir = false;
	bool writable = false;
	int64_t size = 0;
	int64_t mtime = 0;
};

std::string EncodeAttrs(const Attrs &a) {
	Writer w;
	w.U32(ATTR_SIZE | ATTR_PERMISSIONS | ATTR_ACMODTIME);
	w.U64((uint64_t)(a.size < 0 ? 0 : a.size));
	uint32_t perm = a.dir ? (0040000u | (a.writable ? 0755u : 0555u)) : (0100000u | (a.writable ? 0644u : 0444u));
	w.U32(perm);
	w.U32((uint32_t)a.mtime).U32((uint32_t)a.mtime);
	return w.Data();
}

// What `ls -l` would print; clients show it verbatim.
std::string LongName(const std::string &name, const Attrs &a, const std::string &owner) {
	char when[32] = "Jan  1  1970";
	time_t t = (time_t)a.mtime;
	struct tm tm {};
	if (gmtime_r(&t, &tm)) {
		strftime(when, sizeof(when), "%b %e %H:%M", &tm);
	}
	std::string perms = a.dir ? (a.writable ? "drwxr-xr-x" : "dr-xr-xr-x") : (a.writable ? "-rw-r--r--" : "-r--r--r--");
	std::string who = owner.empty() ? "citadel" : owner;
	for (char &c : who) {
		if (c == ' ') {
			c = '_';
		}
	}
	char buf[512];
	snprintf(buf, sizeof(buf), "%s    1 %-8.32s %-8s %12lld %s %s", perms.c_str(), who.c_str(), "citadel",
	         (long long)a.size, when, name.c_str());
	return buf;
}

// A path as its components, relative paths taken from "/" (the only working
// directory an SFTP session has). "." and ".." are resolved; ".." at the root
// stays at the root.
std::vector<std::string> SplitPath(const std::string &path) {
	std::vector<std::string> out;
	size_t i = 0;
	while (i <= path.size()) {
		size_t slash = path.find('/', i);
		std::string part = path.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
		if (part == "..") {
			if (!out.empty()) {
				out.pop_back();
			}
		} else if (!part.empty() && part != ".") {
			out.push_back(part);
		}
		if (slash == std::string::npos) {
			break;
		}
		i = slash + 1;
	}
	return out;
}

std::string JoinPath(const std::vector<std::string> &parts) {
	std::string out;
	for (auto &p : parts) {
		out += "/" + p;
	}
	return out.empty() ? "/" : out;
}

std::string Status(uint32_t id, uint32_t code, const std::string &msg) {
	return Writer().Byte(FXP_STATUS).U32(id).U32(code).String(msg).String("en").Data();
}

} // namespace

struct SftpServer::Handle {
	enum Kind { DIR, READ, WRITE } kind = DIR;
	std::vector<std::string> path;
	citadel::Room room;
	std::string content;
	std::string content_type;
	bool listed = false;
	bool dirty = false;
};

SftpServer::SftpServer(duckdb::Connection &con, const std::string &username) : con_(con), username_(username) {
}

SftpServer::~SftpServer() {
}

bool SftpServer::Feed(const std::string &data, std::string &out) {
	inbuf_ += data;
	while (inbuf_.size() >= 4) {
		uint32_t len = ((uint32_t)(uint8_t)inbuf_[0] << 24) | ((uint32_t)(uint8_t)inbuf_[1] << 16) |
		               ((uint32_t)(uint8_t)inbuf_[2] << 8) | (uint32_t)(uint8_t)inbuf_[3];
		if (len == 0 || len > kMaxRequest) {
			return false;
		}
		if (inbuf_.size() < 4 + (size_t)len) {
			break;
		}
		std::string packet = inbuf_.substr(4, len);
		inbuf_.erase(0, 4 + (size_t)len);
		std::string reply;
		Dispatch(packet, reply);
		if (!reply.empty()) {
			out += Writer().String(reply).Data();
		}
	}
	return true;
}

void SftpServer::Dispatch(const std::string &packet, std::string &out) {
	Reader r(packet);
	uint8_t type = 0;
	r.Byte(type);
	if (type == FXP_INIT) {
		initialized_ = true;
		// Version 3 and no extensions: in particular not posix-rename, so a
		// client's rename is our RENAME, which refuses to overwrite.
		out = Writer().Byte(FXP_VERSION).U32(3).Data();
		return;
	}
	uint32_t id = 0;
	if (!r.U32(id)) {
		return;
	}
	if (!initialized_) {
		out = Status(id, FX_BAD_MESSAGE, "INIT first");
		return;
	}

	// Resolve a path to (area, file). `depth` is 0 for "/", 1 for an area, 2
	// for a file. False means the path names nothing this user can see.
	auto resolve = [&](const std::vector<std::string> &parts, citadel::Room &room) -> bool {
		if (parts.empty()) {
			return true;
		}
		if (parts.size() > 2) {
			return false;
		}
		return filearea::FindArea(con_, username_, parts[0], room);
	};
	auto stat_path = [&](const std::vector<std::string> &parts, Attrs &a) -> bool {
		citadel::Room room;
		if (!resolve(parts, room)) {
			return false;
		}
		if (parts.size() < 2) {
			a.dir = true;
			a.writable = parts.empty() ? citadel::MayCreateRoom(con_, username_, 0)
			                           : filearea::CanUpload(con_, username_, room);
			return true;
		}
		filearea::File f;
		if (!filearea::StatFile(con_, room.room_num, parts[1], f)) {
			return false;
		}
		a.size = f.size;
		a.mtime = f.uploaded_at;
		a.writable = citadel::CanPost(con_, username_, room);
		return true;
	};

	switch (type) {
	case FXP_REALPATH: {
		std::string path;
		r.String(path);
		std::string norm = JoinPath(SplitPath(path));
		Attrs a;
		a.dir = true;
		out = Writer().Byte(FXP_NAME).U32(id).U32(1).String(norm).String(norm).U32(0).Data();
		return;
	}
	case FXP_STAT:
	case FXP_LSTAT: {
		std::string path;
		r.String(path);
		Attrs a;
		if (!stat_path(SplitPath(path), a)) {
			out = Status(id, FX_NO_SUCH_FILE, "No such file or directory");
			return;
		}
		out = Writer().Byte(FXP_ATTRS).U32(id).Raw(EncodeAttrs(a)).Data();
		return;
	}
	case FXP_FSTAT: {
		std::string h;
		r.String(h);
		auto it = handles_.find(h);
		if (it == handles_.end()) {
			out = Status(id, FX_FAILURE, "Invalid handle");
			return;
		}
		Attrs a;
		if (it->second->kind == Handle::DIR) {
			stat_path(it->second->path, a);
		} else {
			a.size = (int64_t)it->second->content.size();
			a.mtime = (int64_t)std::time(nullptr);
			a.writable = it->second->kind == Handle::WRITE;
		}
		out = Writer().Byte(FXP_ATTRS).U32(id).Raw(EncodeAttrs(a)).Data();
		return;
	}
	case FXP_SETSTAT:
	case FXP_FSETSTAT:
		// Nothing here has a mode, an owner or a settable time. Succeeding is
		// what lets `sftp put -p` and friends complete.
		out = Status(id, FX_OK, "");
		return;
	case FXP_OPENDIR: {
		std::string path;
		r.String(path);
		auto parts = SplitPath(path);
		citadel::Room room;
		if (parts.size() > 1 || !resolve(parts, room)) {
			out = Status(id, FX_NO_SUCH_FILE, "No such directory");
			return;
		}
		if (handles_.size() >= kMaxHandles) {
			out = Status(id, FX_FAILURE, "Too many open handles");
			return;
		}
		std::unique_ptr<Handle> hd(new Handle());
		hd->kind = Handle::DIR;
		hd->path = parts;
		hd->room = room;
		std::string key = std::to_string(next_handle_++);
		handles_[key] = std::move(hd);
		out = Writer().Byte(FXP_HANDLE).U32(id).String(key).Data();
		return;
	}
	case FXP_READDIR: {
		std::string h;
		r.String(h);
		auto it = handles_.find(h);
		if (it == handles_.end() || it->second->kind != Handle::DIR) {
			out = Status(id, FX_FAILURE, "Invalid handle");
			return;
		}
		Handle &hd = *it->second;
		if (hd.listed) {
			out = Status(id, FX_EOF, "");
			return;
		}
		hd.listed = true;
		Writer entries;
		uint32_t count = 0;
		Attrs self;
		stat_path(hd.path, self);
		for (const char *dot : {".", ".."}) {
			entries.String(dot).String(LongName(dot, self, "")).Raw(EncodeAttrs(self));
			count++;
		}
		if (hd.path.empty()) {
			for (auto &area : filearea::VisibleAreas(con_, username_)) {
				Attrs a;
				a.dir = true;
				a.writable = filearea::CanUpload(con_, username_, area);
				entries.String(area.display_name).String(LongName(area.display_name, a, "")).Raw(EncodeAttrs(a));
				count++;
			}
		} else {
			bool writable = citadel::CanPost(con_, username_, hd.room);
			for (auto &f : filearea::ListFiles(con_, hd.room.room_num)) {
				Attrs a;
				a.size = f.size;
				a.mtime = f.uploaded_at;
				a.writable = writable;
				entries.String(f.name).String(LongName(f.name, a, f.uploader)).Raw(EncodeAttrs(a));
				count++;
			}
		}
		out = Writer().Byte(FXP_NAME).U32(id).U32(count).Raw(entries.Data()).Data();
		return;
	}
	case FXP_OPEN: {
		std::string path;
		uint32_t pflags = 0;
		r.String(path);
		r.U32(pflags);
		auto parts = SplitPath(path);
		citadel::Room room;
		if (parts.size() != 2 || !resolve(parts, room)) {
			out = Status(id, FX_NO_SUCH_FILE, parts.size() < 2 ? "Not a file" : "No such file or directory");
			return;
		}
		if (handles_.size() >= kMaxHandles) {
			out = Status(id, FX_FAILURE, "Too many open handles");
			return;
		}
		std::unique_ptr<Handle> hd(new Handle());
		hd->path = parts;
		hd->room = room;
		filearea::File f;
		bool exists = filearea::StatFile(con_, room.room_num, parts[1], f);
		if (pflags & PF_WRITE) {
			if (pflags & PF_APPEND) {
				out = Status(id, FX_OP_UNSUPPORTED, "Appending to a stored file is not supported");
				return;
			}
			if (!filearea::CanUpload(con_, username_, room)) {
				out = Status(id, FX_PERMISSION_DENIED, "Uploads are not allowed here");
				return;
			}
			if (exists && (pflags & PF_EXCL)) {
				out = Status(id, FX_FAILURE, "File exists");
				return;
			}
			if (!exists && !(pflags & PF_CREAT)) {
				out = Status(id, FX_NO_SUCH_FILE, "No such file");
				return;
			}
			if (filearea::SanitizeName(parts[1]).empty()) {
				out = Status(id, FX_PERMISSION_DENIED, "That file name is not allowed");
				return;
			}
			hd->kind = Handle::WRITE;
			// Overwriting part of an existing file (no TRUNC) starts from its
			// current bytes, so a partial write does not lose the rest.
			if (exists && !(pflags & PF_TRUNC)) {
				filearea::File cur;
				filearea::GetFile(con_, room.room_num, parts[1], cur, hd->content);
				hd->content_type = cur.content_type;
			}
			// Created even if nothing is written: `touch`-style opens and a
			// zero-byte upload both expect a file to exist afterwards.
			hd->dirty = true;
		} else {
			if (!exists) {
				out = Status(id, FX_NO_SUCH_FILE, "No such file");
				return;
			}
			if (!filearea::CanDownload(con_, username_, room)) {
				out = Status(id, FX_PERMISSION_DENIED, "Downloads are not allowed here");
				return;
			}
			hd->kind = Handle::READ;
			if (!filearea::GetFile(con_, room.room_num, parts[1], f, hd->content)) {
				out = Status(id, FX_FAILURE, "Could not read the file");
				return;
			}
		}
		std::string key = std::to_string(next_handle_++);
		handles_[key] = std::move(hd);
		out = Writer().Byte(FXP_HANDLE).U32(id).String(key).Data();
		return;
	}
	case FXP_READ: {
		std::string h;
		uint64_t offset = 0;
		uint32_t len = 0;
		r.String(h);
		r.U64(offset);
		r.U32(len);
		auto it = handles_.find(h);
		if (it == handles_.end() || it->second->kind == Handle::DIR) {
			out = Status(id, FX_FAILURE, "Invalid handle");
			return;
		}
		const std::string &c = it->second->content;
		if (offset >= c.size()) {
			out = Status(id, FX_EOF, "");
			return;
		}
		size_t n = std::min<size_t>(std::min<size_t>(len, kMaxRead), c.size() - (size_t)offset);
		out = Writer().Byte(FXP_DATA).U32(id).String(c.substr((size_t)offset, n)).Data();
		return;
	}
	case FXP_WRITE: {
		std::string h, data;
		uint64_t offset = 0;
		r.String(h);
		r.U64(offset);
		r.String(data);
		auto it = handles_.find(h);
		if (!r.Ok() || it == handles_.end() || it->second->kind != Handle::WRITE) {
			out = Status(id, FX_FAILURE, "Invalid handle");
			return;
		}
		std::string &c = it->second->content;
		if (offset + data.size() > kMaxUpload) {
			out = Status(id, FX_FAILURE, "File too large");
			return;
		}
		if (c.size() < offset + data.size()) {
			c.resize((size_t)(offset + data.size()), '\0');
		}
		c.replace((size_t)offset, data.size(), data);
		it->second->dirty = true;
		out = Status(id, FX_OK, "");
		return;
	}
	case FXP_CLOSE: {
		std::string h;
		r.String(h);
		auto it = handles_.find(h);
		if (it == handles_.end()) {
			out = Status(id, FX_FAILURE, "Invalid handle");
			return;
		}
		std::unique_ptr<Handle> hd = std::move(it->second);
		handles_.erase(it);
		if (hd->kind == Handle::WRITE && hd->dirty) {
			// The store happens here, whole, which is where the quota inside
			// InsertMessage can refuse it — so the client hears about it.
			std::string err;
			std::string type = hd->content_type.empty() ? "application/octet-stream" : hd->content_type;
			if (filearea::PutFile(con_, hd->room.room_num, filearea::SanitizeName(hd->path[1]), hd->content, type, "",
			                      username_, err) < 0) {
				out = Status(id, FX_FAILURE, err.empty() ? "Could not store the file" : err);
				return;
			}
		}
		out = Status(id, FX_OK, "");
		return;
	}
	case FXP_REMOVE: {
		std::string path;
		r.String(path);
		auto parts = SplitPath(path);
		citadel::Room room;
		if (parts.size() != 2 || !resolve(parts, room)) {
			out = Status(id, FX_NO_SUCH_FILE, "No such file");
			return;
		}
		if (!citadel::CanPost(con_, username_, room)) {
			out = Status(id, FX_PERMISSION_DENIED, "You may not remove files here");
			return;
		}
		std::string err;
		out = filearea::RemoveFile(con_, room.room_num, parts[1], err)
		          ? Status(id, FX_OK, "")
		          : Status(id, FX_NO_SUCH_FILE, err.empty() ? "No such file" : err);
		return;
	}
	case FXP_RENAME: {
		std::string from, to;
		r.String(from);
		r.String(to);
		auto fp = SplitPath(from), tp = SplitPath(to);
		citadel::Room froom, troom;
		if (fp.size() != 2 || !resolve(fp, froom)) {
			out = Status(id, FX_NO_SUCH_FILE, "No such file");
			return;
		}
		if (tp.size() != 2 || !resolve(tp, troom)) {
			out = Status(id, FX_NO_SUCH_FILE, "No such directory to move it to");
			return;
		}
		if (!citadel::CanPost(con_, username_, froom) || !filearea::CanUpload(con_, username_, troom)) {
			out = Status(id, FX_PERMISSION_DENIED, "You may not move files there");
			return;
		}
		std::string dest = filearea::SanitizeName(tp[1]);
		filearea::File existing;
		if (dest.empty() || filearea::StatFile(con_, troom.room_num, dest, existing)) {
			// SFTP v3 RENAME never overwrites.
			out = Status(id, FX_FAILURE, "The destination already exists");
			return;
		}
		// A rename is a store under the new name and a remove of the old: the
		// name is the message's euid, not a column to update.
		filearea::File f;
		std::string content, err;
		if (!filearea::GetFile(con_, froom.room_num, fp[1], f, content)) {
			out = Status(id, FX_NO_SUCH_FILE, "No such file");
			return;
		}
		if (filearea::PutFile(con_, troom.room_num, dest, content, f.content_type, f.description, f.uploader, err) <
		    0) {
			out = Status(id, FX_FAILURE, err.empty() ? "Could not store the file" : err);
			return;
		}
		filearea::RemoveFile(con_, froom.room_num, fp[1], err);
		out = Status(id, FX_OK, "");
		return;
	}
	case FXP_MKDIR: {
		std::string path;
		r.String(path);
		auto parts = SplitPath(path);
		if (parts.size() != 1) {
			out = Status(id, FX_PERMISSION_DENIED, "A file area has no subdirectories");
			return;
		}
		// Creating a directory creates a room, which takes the same permission
		// FTP's MKD and WebDAV's MKCOL ask for.
		if (!citadel::MayCreateRoom(con_, username_, 0)) {
			out = Status(id, FX_PERMISSION_DENIED, "You may not create file areas");
			return;
		}
		citadel::Room clash;
		if (citadel::ResolveRoom(con_, username_, parts[0], clash)) {
			out = Status(id, FX_FAILURE, "There is already a room with that name");
			return;
		}
		std::string err;
		int64_t flags = citadel::QR_DIRECTORY | citadel::QR_UPLOAD | citadel::QR_DOWNLOAD | citadel::QR_VISDIR;
		int64_t room_num = citadel::CreateRoom(con_, parts[0], 0, flags, "", 0, err);
		if (room_num < 0) {
			out = Status(id, FX_FAILURE, err.empty() ? "Could not create the area" : err);
			return;
		}
		citadel::Room made;
		if (citadel::GetRoomByNum(con_, room_num, made)) {
			citadel::SetRights(con_, made, username_, citadel::kAclRights, err);
		}
		out = Status(id, FX_OK, "");
		return;
	}
	case FXP_RMDIR: {
		std::string path;
		r.String(path);
		auto parts = SplitPath(path);
		citadel::Room room;
		if (parts.size() != 1 || !resolve(parts, room)) {
			out = Status(id, FX_NO_SUCH_FILE, "No such directory");
			return;
		}
		if (!citadel::CanAdminister(con_, username_, room)) {
			out = Status(id, FX_PERMISSION_DENIED, "You may not remove that area");
			return;
		}
		// rmdir(2) semantics: only an empty directory. Removing an area deletes
		// the room and everything in it, which is too much for one keystroke in
		// a file manager that assumes the POSIX rule.
		if (!filearea::ListFiles(con_, room.room_num).empty()) {
			out = Status(id, FX_FAILURE, "Directory not empty");
			return;
		}
		std::string err;
		out = citadel::KillRoom(con_, room.room_num, err) ? Status(id, FX_OK, "")
		                                                    : Status(id, FX_FAILURE, err.empty() ? "Failed" : err);
		return;
	}
	case FXP_READLINK:
	case FXP_SYMLINK:
	case FXP_EXTENDED:
	default:
		out = Status(id, FX_OP_UNSUPPORTED, "Operation not supported");
		return;
	}
}

} // namespace sshd
} // namespace quackmail
