#pragma once

#include <string>
#include <utility>
#include <vector>

namespace quackmail {
namespace vnote {

// vNote — the format Citadel stores a sticky note in, as a text/vnote part of an
// ordinary message.
//
// It is vCard 2.1's grammar with its own property names, which is why the line
// handling comes from contentline.hpp rather than being written again here:
//
//   BEGIN:VNOTE
//   VERSION:1.1
//   UID:...
//   SUMMARY:the title
//   BODY:the text
//   X-OUTLOOK-COLOR:#FFFF88
//   X-OUTLOOK-LEFT:0
//   X-OUTLOOK-TOP:0
//   X-OUTLOOK-WIDTH:300
//   X-OUTLOOK-HEIGHT:200
//   END:VNOTE
//
// The alternative was storing notes as iCalendar VJOURNAL, which would have
// needed no new code at all — but then WebCit and the Citadel clients could not
// read them, and cross-protocol readability is the whole reason groupware
// objects are stored as ordinary messages. Transcribed from
// libcitadel/lib/vnote.c on the parity oracle.
//
// As with vCard, unknown properties survive a round trip: the geometry and
// colour fields exist so that a note placed on a desktop pinboard by another
// client keeps its position after being edited here.

struct Note {
	std::string uid;
	std::string summary;
	std::string body;
	// "#RRGGBB", as Outlook and Citadel write it. Empty when unset.
	std::string color;
	// "text/html" or "text/x-markdown" — a QuackCit extension (X-QM-FORMAT),
	// absent from every note some other Citadel client or an older QuackCit
	// wrote. Empty means "plain text", rendered escaped rather than through a
	// sanitizer: an existing note's body must not suddenly start rendering as
	// HTML just because this field was added.
	std::string content_type;

	// Every property, in the order it arrived, so emitting preserves what this
	// struct does not model. The four fields above are projections of it.
	std::vector<std::pair<std::string, std::string>> props;

	Note();
};

// Parse a file that may hold several notes. False when there is no BEGIN:VNOTE.
bool Parse(const std::string &text, std::vector<Note> &out);
bool ParseOne(const std::string &text, Note &out);

// Serialize, folded at 75 octets on a UTF-8 boundary.
std::string Emit(const Note &note);

// A one-line label for a list. SUMMARY, else the body's first line, else
// "(untitled)" — a note with no label at all still has to be clickable.
std::string TitleOf(const Note &note);

// The euid the note is stored under: its UID, or a stable content-derived one.
std::string EuidFor(const Note &note);
std::string NewUid(const std::string &fqdn);

} // namespace vnote
} // namespace quackmail
