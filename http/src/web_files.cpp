// The file areas, in the browser.
//
// A directory room (QR_DIRECTORY) is shown as a file listing rather than as a
// message board: name, size, uploader, date, description, and the upload form
// when QR_UPLOAD allows one. The same rooms are served over WebDAV under
// /dav/files/, over FTP/FTPS, over SFTP and from the telnet `.R F` family;
// every one of them asks core/filearea.hpp rather than re-deriving what a file
// is, and so does this page.
//
// Files are addressed by `?name=` rather than by a path segment. A stored name
// is sanitized against path separators, but it may still hold characters that
// a path capture would have to percent-decode, and a query parameter is
// decoded exactly once by the request parser with no second opinion.

#include "dav.hpp"
#include "web.hpp"
#include "web_i18n.hpp"

#include "quackmail/filearea.hpp"

namespace duckdb {
namespace qmweb {

namespace {

using quackmail::citadel::Room;
namespace filearea = quackmail::filearea;

int64_t CapNum(Ctx &ctx, size_t i) {
	return (int64_t)std::strtoll(ctx.Cap(i).c_str(), nullptr, 10);
}

PageOpts FilesPage(const std::string &active) {
	PageOpts opts;
	opts.active = active;
	opts.wide = true;
	return opts;
}

// Resolve the room in the URL as a file area the caller can see. Answers the
// request and returns false when it is not one.
bool ResolveArea(Ctx &ctx, Room &room) {
	if (!ResolveRoomNumFor(ctx, CapNum(ctx, 0), room) || !filearea::IsFileArea(room)) {
		NotFound(ctx);
		return false;
	}
	if (!RequireUnlocked(ctx, room, RoomHref(room))) {
		return false;
	}
	if (!filearea::CanList(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "This directory's listing is not visible to you.");
		return false;
	}
	return true;
}

std::string FileHref(const Room &room, const std::string &action, const std::string &name) {
	return RoomHref(room, "/" + action + "?name=" + quackmail::http::PercentEncode(name));
}

} // namespace

// The room page of a directory room. Called from GetBbsRoom ahead of the
// view dispatch: a file area is a flag on the room, not a view, the same way
// the DAV router's KindForRoom decides it.
void FilesIndex(Ctx &ctx, const Room &room) {
	if (!filearea::CanList(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "This directory's listing is not visible to you.");
		return;
	}
	bool can_download = filearea::CanDownload(ctx.con, ctx.username, room);
	bool can_upload = filearea::CanUpload(ctx.con, ctx.username, room);
	bool can_delete = quackmail::citadel::CanPost(ctx.con, ctx.username, room);

	std::string toolbar = Link("/bbs/files", "All file areas", "btn sec");
	toolbar += Link(RoomHref(room, "?view=raw"), "Show as messages", "btn sec");
	if (room.mailbox_owner == 0 && quackmail::citadel::CanAdminister(ctx.con, ctx.username, room)) {
		toolbar += Link(RoomHref(room, "/settings"), "Room settings", "btn sec");
	}

	std::string body;
	if (!room.info.empty()) {
		body += "<p class=\"muted\">" + T(room.info) + "</p>";
	}

	auto files = filearea::ListFiles(ctx.con, room.room_num);
	int64_t total = 0;
	for (auto &f : files) {
		total += f.size;
	}
	body += "<p class=\"muted\">" + std::to_string(files.size()) +
	        (files.size() == 1 ? " file, " : " files, ") + T(FormatBytes(total)) + ".</p>";

	if (files.empty()) {
		body += "<p class=\"muted\">This directory is empty.</p>";
	} else {
		Table table(ctx, "file-area",
		            {Column("name", "Name"), Column::Num("size", "Size"), Column("type", "Type"),
		             Column("uploader", "Uploaded by"), Column("date", "Date", "", true),
		             Column("", "Description"), Column("", "")});
		for (auto &f : files) {
			std::string name_cell = can_download
			                            ? Link(FileHref(room, "download", f.name), f.name)
			                            : T(f.name);
			std::string actions;
			if (can_delete) {
				actions += FormStart(ctx, RoomHref(room, "/file/delete"), "inline") + Hidden("name", f.name) +
				           IconButton("Delete", "trash", "sec") + FormEnd();
			}
			std::string desc = f.description == f.name ? std::string() : f.description;
			std::string desc_cell = T(desc);
			if (can_delete) {
				desc_cell = FormStart(ctx, RoomHref(room, "/file/describe"), "inline filedesc") +
				            Hidden("name", f.name) + TextInput("description", desc, "text", "Add a description") +
				            Button("Save", "sec") + FormEnd();
			}
			table.Add()
			    .Html(Icon("file") + " " + name_cell, f.name)
			    .Html(T(FormatBytes(f.size)), std::to_string(f.size))
			    .Text(f.content_type)
			    .Text(f.uploader)
			    .Html(T(FormatTime(ctx, f.uploaded_at)), std::to_string(f.uploaded_at))
			    .Html(desc_cell, desc)
			    .Html(actions);
		}
		body += table.Render();
	}

	if (can_upload) {
		body += "<article class=\"upload\"><h3>Upload a file</h3>";
		body += "<form method=\"post\" action=\"" + A(RoomHref(room, "/upload")) +
		        "\" enctype=\"multipart/form-data\">" + Hidden("_csrf", ctx.csrf);
		body += "<input type=\"file\" name=\"file\" required>";
		body += "<label>Description " + TextInput("description", "", "text", "Optional") + "</label>";
		body += "<p class=\"muted\">Up to " + T(FormatBytes((int64_t)quackmail::http::Limits().max_body)) +
		        ". A file with the same name replaces the old one.</p>";
		body += IconButton("Upload", "plus") + "</form></article>";
	}

	// The same directory as a network drive. The href is what the DAV router
	// itself hands out for this collection, so the two cannot drift apart.
	std::string dav = SelfBaseUrl(ctx) + CollectionHref(DavKind::Files, ctx.username, room.room_num);
	body += "<details class=\"mount\"><summary>Open this directory in your file manager</summary>"
	        "<p>Connect to this address as a WebDAV (\"network drive\") location, signing in with your "
	        "QuackCit user name and password:</p><pre>" +
	        T(dav) +
	        "</pre><p class=\"muted\">The same files are also on FTP/FTPS and on SFTP, under the "
	        "directory named after this room.</p></details>";

	PageOpts opts = FilesPage("room:" + std::to_string(room.room_num));
	opts.view = (int)room.default_view;
	opts.toolbar = Toolbar(toolbar);
	Render(ctx, room.display_name, body, opts);
}

namespace {

// Every file area the caller can see, one line each.
void GetFilesIndex(Ctx &ctx) {
	auto areas = filearea::VisibleAreas(ctx.con, ctx.username);
	std::string body;
	if (areas.empty()) {
		body += "<p class=\"muted\">There are no file areas you can see. An aide makes one by turning on "
		        "the <em>Directory</em> flag in a room's settings.</p>";
		Render(ctx, Tr(ctx, "nav.files"), body, FilesPage("files"));
		return;
	}
	Table table(ctx, "file-areas",
	            {Column("name", "Directory"), Column::Num("files", "Files"), Column::Num("size", "Size"),
	             Column("", "Access")});
	for (auto &room : areas) {
		auto files = filearea::ListFiles(ctx.con, room.room_num);
		int64_t total = 0;
		for (auto &f : files) {
			total += f.size;
		}
		std::string access;
		if (filearea::CanDownload(ctx.con, ctx.username, room)) {
			access += "download";
		}
		if (filearea::CanUpload(ctx.con, ctx.username, room)) {
			access += access.empty() ? "upload" : ", upload";
		}
		if (access.empty()) {
			access = "list only";
		}
		table.Add()
		    .Html(Icon("folder") + " " + Link(RoomHref(room), room.display_name), room.display_name)
		    .Number((int64_t)files.size())
		    .Html(T(FormatBytes(total)), std::to_string(total))
		    .Text(access);
	}
	body += table.Render();
	Render(ctx, Tr(ctx, "nav.files"), body, FilesPage("files"));
}

void GetFileDownload(Ctx &ctx) {
	Room room;
	if (!ResolveArea(ctx, room)) {
		return;
	}
	// QR_DOWNLOAD is its own question: a directory may list what it will not
	// hand over.
	if (!filearea::CanDownload(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "Downloads are not allowed from this directory.");
		return;
	}
	filearea::File f;
	std::string content;
	if (!filearea::GetFile(ctx.con, room.room_num, ctx.req.Param("name"), f, content)) {
		NotFound(ctx);
		return;
	}
	// Always an attachment, whatever the uploader said it was: served inline, an
	// uploaded HTML file would be a page on this origin. The sandbox CSP is the
	// second line, for a browser that renders it anyway.
	SecurityHeaders(ctx, "default-src 'none'; sandbox");
	ctx.resp.Bytes(content, f.content_type.empty() ? "application/octet-stream" : f.content_type);
	ctx.resp.SetHeader("Content-Disposition",
	                   "attachment; filename=\"" + quackmail::http::SanitizeFilename(f.name) + "\"");
	ctx.resp.SetHeader("X-Content-Type-Options", "nosniff");
	ctx.resp.SetHeader("Cache-Control", "no-store");
}

void PostFileUpload(Ctx &ctx) {
	Room room;
	if (!ResolveArea(ctx, room)) {
		return;
	}
	if (!filearea::CanUpload(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "Uploads are not allowed in this directory.");
		return;
	}
	// The router has already checked the CSRF token inside the multipart body.
	std::vector<std::pair<std::string, std::string>> fields;
	std::vector<quackmail::http::FormFile> files;
	if (!quackmail::http::ParseMultipart(ctx.req.Header("Content-Type"), ctx.req.body, fields, files)) {
		BadRequest(ctx, "The upload form must be submitted as multipart/form-data.");
		return;
	}
	std::string description;
	for (auto &f : fields) {
		if (f.first == "description") {
			description = f.second;
		}
	}
	bool stored = false;
	for (auto &file : files) {
		if (file.field != "file" || file.filename.empty()) {
			continue;
		}
		std::string name = filearea::SanitizeName(file.filename);
		if (name.empty()) {
			continue;
		}
		std::string err;
		std::string type = file.content_type.empty() ? "application/octet-stream" : file.content_type;
		if (filearea::PutFile(ctx.con, room.room_num, name, file.content, type, description, ctx.username,
		                      err) > 0) {
			stored = true;
		}
	}
	RedirectTo(ctx, RoomHref(room), stored ? "uploaded" : "upload_failed");
}

void PostFileDelete(Ctx &ctx) {
	Room room;
	if (!ResolveArea(ctx, room)) {
		return;
	}
	// Removing a file is removing a message from the room, which is CanPost's
	// question in every other front door too (FTP DELE, DAV DELETE).
	if (!quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "You cannot remove files from this directory.");
		return;
	}
	std::string err;
	if (!filearea::RemoveFile(ctx.con, room.room_num, ctx.req.Form("name"), err)) {
		NotFound(ctx);
		return;
	}
	RedirectTo(ctx, RoomHref(room), "deleted");
}

void PostFileDescribe(Ctx &ctx) {
	Room room;
	if (!ResolveArea(ctx, room)) {
		return;
	}
	if (!quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "You cannot change files in this directory.");
		return;
	}
	std::string err;
	if (!filearea::DescribeFile(ctx.con, room.room_num, ctx.req.Form("name"), ctx.req.Form("description"),
	                            err)) {
		NotFound(ctx);
		return;
	}
	RedirectTo(ctx, RoomHref(room), "saved");
}

} // namespace

void RegisterFileRoutes(std::vector<Route> &out) {
	out.push_back({"GET", "/bbs/files", Role::User, GetFilesIndex});
	out.push_back({"GET", "/bbs/room/:n/download", Role::User, GetFileDownload});
	out.push_back({"POST", "/bbs/room/:n/upload", Role::User, PostFileUpload});
	out.push_back({"POST", "/bbs/room/:n/file/delete", Role::User, PostFileDelete});
	out.push_back({"POST", "/bbs/room/:n/file/describe", Role::User, PostFileDescribe});
}

} // namespace qmweb
} // namespace duckdb
