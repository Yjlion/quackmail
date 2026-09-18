#include "quackmail/filearea.hpp"

#include "quackmail/http.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/util.hpp"

#include <ctime>

namespace quackmail {
namespace filearea {

File::File() {
}

namespace {

// The euid a file is keyed by. Prefixed so a file cannot collide with a
// groupware object that happens to share its name: a vCard's euid is its UID,
// and "file/" is not a legal UID prefix for one.
std::string EuidFor(const std::string &name) {
	return "file/" + name;
}

int64_t NowSeconds() {
	return (int64_t)std::time(nullptr);
}

// Pull the single attachment part back out of a stored file message.
bool PartOf(const std::string &raw, mime::MimePart &out) {
	mime::MimeEntity ent = mime::ParseEntity(raw);
	std::vector<mime::MimePart> parts = mime::FlattenParts(ent);
	for (auto &p : parts) {
		if (!p.filename.empty()) {
			out = p;
			return true;
		}
	}
	// A file stored as a bare single part, with the filename only on the
	// message. Tolerated on read so a hand-made message is still listable.
	if (!parts.empty()) {
		out = parts.front();
		return true;
	}
	return false;
}

bool FileFromMessage(const citadel::Message &msg, File &out, std::string *content) {
	if (msg.euid.rfind("file/", 0) != 0) {
		return false;
	}
	out.msgnum = msg.msgnum;
	out.name = msg.euid.substr(5);
	out.description = msg.subject == out.name ? std::string() : msg.subject;
	out.uploader = msg.author;
	out.uploaded_at = msg.msgtime;

	mime::MimePart part;
	if (PartOf(msg.raw, part)) {
		out.content_type = part.content_type.empty() ? "application/octet-stream" : part.content_type;
		out.size = (int64_t)part.content.size();
		if (content) {
			*content = part.content;
		}
	} else {
		out.content_type = "application/octet-stream";
		out.size = 0;
		if (content) {
			content->clear();
		}
	}
	return true;
}

} // namespace

std::string SanitizeName(const std::string &name) {
	// One rule, shared with the mail attachment path, rather than a third
	// opinion about what a filename may contain. It already strips path
	// separators, control characters and leading dots.
	std::string out = http::SanitizeFilename(name);
	// SanitizeFilename falls back to "attachment" for a name with nothing left;
	// here an unusable name has to be refused rather than silently renamed, so
	// that fallback is undone.
	if (out == "attachment" && name.find("attachment") == std::string::npos) {
		return std::string();
	}
	return out;
}

bool IsFileArea(const citadel::Room &room) {
	return (room.qr_flags & citadel::QR_DIRECTORY) != 0;
}

bool CanList(duckdb::Connection &con, const std::string &user, const citadel::Room &room) {
	if (!IsFileArea(room)) {
		return false;
	}
	std::string rights = citadel::EffectiveRights(con, user, room);
	if (rights.find('r') == std::string::npos) {
		return false;
	}
	// QR_VISDIR is what makes the *names* public where the contents are not. A
	// room with neither VISDIR nor DOWNLOAD has a directory only its writers can
	// see, which is how an upload-only drop box is expressed.
	return (room.qr_flags & citadel::QR_VISDIR) != 0 ||
	       (room.qr_flags & citadel::QR_DOWNLOAD) != 0 ||
	       citadel::CanPost(con, user, room);
}

bool CanDownload(duckdb::Connection &con, const std::string &user, const citadel::Room &room) {
	if (!IsFileArea(room) || (room.qr_flags & citadel::QR_DOWNLOAD) == 0) {
		return false;
	}
	return citadel::EffectiveRights(con, user, room).find('r') != std::string::npos;
}

bool CanUpload(duckdb::Connection &con, const std::string &user, const citadel::Room &room) {
	if (!IsFileArea(room) || (room.qr_flags & citadel::QR_UPLOAD) == 0) {
		return false;
	}
	// Deliberately CanPost: depositing a file in a room *is* posting to it, and
	// Citadel has exactly one answer to "may this person write here".
	return citadel::CanPost(con, user, room);
}

std::vector<citadel::Room> VisibleAreas(duckdb::Connection &con, const std::string &user) {
	std::vector<citadel::Room> out;
	for (auto &room : citadel::ListRooms(con, user, -1, "all")) {
		if (!IsFileArea(room) || !CanList(con, user, room) || !citadel::RoomUnlocked(con, user, room)) {
			continue;
		}
		out.push_back(room);
	}
	return out;
}

bool FindArea(duckdb::Connection &con, const std::string &user, const std::string &name,
              citadel::Room &out) {
	std::string want = util::Lower(name);
	for (auto &room : VisibleAreas(con, user)) {
		if (util::Lower(room.display_name) == want) {
			out = room;
			return true;
		}
	}
	return false;
}

std::vector<File> ListFiles(duckdb::Connection &con, int64_t room_num) {
	std::vector<File> out;
	for (int64_t msgnum : citadel::RoomMessages(con, room_num, "all", 0, 0)) {
		citadel::Message msg;
		if (!citadel::LoadMessage(con, msgnum, msg)) {
			continue;
		}
		File f;
		if (FileFromMessage(msg, f, nullptr)) {
			out.push_back(std::move(f));
		}
	}
	return out;
}

bool StatFile(duckdb::Connection &con, int64_t room_num, const std::string &name, File &out) {
	std::string ignored;
	return GetFile(con, room_num, name, out, ignored);
}

bool GetFile(duckdb::Connection &con, int64_t room_num, const std::string &name, File &out,
             std::string &content) {
	int64_t msgnum = citadel::FindByEuid(con, room_num, EuidFor(name));
	if (msgnum <= 0) {
		return false;
	}
	citadel::Message msg;
	if (!citadel::LoadMessage(con, msgnum, msg)) {
		return false;
	}
	return FileFromMessage(msg, out, &content);
}

int64_t PutFile(duckdb::Connection &con, int64_t room_num, const std::string &name,
                const std::string &content, const std::string &content_type,
                const std::string &description, const std::string &uploader, std::string &err) {
	std::string clean = SanitizeName(name);
	if (clean.empty()) {
		err = "That filename cannot be stored.";
		return -1;
	}

	mime::BuildPart part;
	part.content_type = content_type.empty() ? "application/octet-stream" : content_type;
	part.content = content;
	part.filename = clean;
	part.disposition = "attachment";

	mime::HeaderList headers;
	headers.push_back({"Subject", description.empty() ? clean : description});
	headers.push_back({"From", uploader});

	citadel::Message msg;
	msg.euid = EuidFor(clean);
	msg.subject = description.empty() ? clean : description;
	msg.author = uploader;
	msg.msgtime = NowSeconds();
	msg.format_type = 4;
	msg.raw = mime::BuildMessage(headers, {part});

	// UpsertByEuid, so re-uploading a file replaces it rather than accumulating
	// a second copy under the same name. The storage quota is enforced inside
	// InsertMessage underneath this, which is why no front door checks it.
	return citadel::UpsertByEuid(con, msg, room_num, err);
}

bool RemoveFile(duckdb::Connection &con, int64_t room_num, const std::string &name,
                std::string &err) {
	int64_t msgnum = citadel::FindByEuid(con, room_num, EuidFor(name));
	if (msgnum <= 0) {
		err = "No such file.";
		return false;
	}
	return citadel::DeleteMessage(con, room_num, msgnum, err);
}

bool DescribeFile(duckdb::Connection &con, int64_t room_num, const std::string &name,
                  const std::string &description, std::string &err) {
	File f;
	std::string content;
	if (!GetFile(con, room_num, name, f, content)) {
		err = "No such file.";
		return false;
	}
	// Rewritten rather than UPDATEd: the description is the message subject, and
	// the subject is also inside the stored MIME. Two places to change it is one
	// place to forget.
	return PutFile(con, room_num, f.name, content, f.content_type, description, f.uploader, err) >= 0;
}

} // namespace filearea
} // namespace quackmail
