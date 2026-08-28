#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace quackmail {
namespace mime {

struct ParsedMessage {
	std::string from;       // From: header address (raw)
	std::string subject;    // Subject: header (decoded as-is)
	std::string message_id; // Message-ID: header
	std::vector<std::pair<std::string, std::string>> headers;
};

// Parse an RFC-5322 message: split headers from body, unfold, and pull out a
// few well-known fields. Header names are returned as-is (case preserved), the
// convenience accessors are case-insensitive.
ParsedMessage Parse(const std::string &raw);

// Extract the bare address (<...>) from a header value like: Foo <a@b.com>.
std::string ExtractAddress(const std::string &header_value);

// ---------------------------------------------------------------------------
// RFC 2045 — Content-Transfer-Encoding codecs
// ---------------------------------------------------------------------------

// Decode a quoted-printable body (RFC 2045 §6.7): "=XX" hex escapes and soft
// line breaks ("=" at end of line). Invalid escapes are passed through literally.
std::string DecodeQuotedPrintable(const std::string &in);

// Encode bytes as quoted-printable (RFC 2045 §6.7), wrapping at 76 columns with
// soft line breaks. Intended for later outbound use; kept here for symmetry.
std::string EncodeQuotedPrintable(const std::string &in);

// Decode a body given its Content-Transfer-Encoding token. Recognizes (case-
// insensitively) "base64", "quoted-printable", and the identity encodings
// "7bit"/"8bit"/"binary" (and empty). Unknown encodings are returned unchanged.
std::string DecodeContentTransferEncoding(const std::string &encoding, const std::string &body);

// ---------------------------------------------------------------------------
// RFC 2047 — encoded-word decoding for header display
// ---------------------------------------------------------------------------

// Decode any "=?charset?B?...?=" / "=?charset?Q?...?=" encoded words in a header
// value, collapsing linear whitespace between adjacent encoded words (RFC 2047
// §6.2). Charset handling: UTF-8/US-ASCII pass through; ISO-8859-1 (Latin-1) is
// transcoded to UTF-8; other charsets yield the raw decoded bytes.
std::string DecodeEncodedWords(const std::string &in);

// The inverse, for headers we compose ourselves (digest subjects, the subject
// of a message synthesized from a feed item). Pure ASCII without '=?' is
// returned unchanged, so the common case stays readable on the wire; anything
// else becomes one or more "=?UTF-8?B?...?=" words, split so no line exceeds
// RFC 2047's 75-character limit per encoded word.
std::string EncodeEncodedWord(const std::string &in);

// Transcode a Latin-1 (ISO-8859-1) byte string to UTF-8.
std::string Latin1ToUtf8(const std::string &in);

// ---------------------------------------------------------------------------
// RFC 2045 — structured header fields
// ---------------------------------------------------------------------------

struct ContentType {
	std::string type;    // e.g. "text" (lowercased); empty if unparsed
	std::string subtype; // e.g. "plain" (lowercased)
	std::vector<std::pair<std::string, std::string>> params; // names lowercased

	// Case-insensitive parameter lookup; returns "" when absent.
	std::string Param(const std::string &name) const;
	// "type/subtype" (lowercased), or "" when type is empty.
	std::string Mime() const;
};

// Parse a Content-Type / Content-Disposition style value into a token plus
// parameters (RFC 2045 §5.1). Handles quoted-string values and comments.
ContentType ParseContentType(const std::string &value);

// ---------------------------------------------------------------------------
// RFC 822 / 2822 / 5322 — address lists and dates
// ---------------------------------------------------------------------------

struct Address {
	std::string name; // display name (RFC 2047-decoded), may be empty
	std::string addr; // addr-spec "local@domain", may be empty for group labels
};

// Parse an address-list header value (From/To/Cc/...) per RFC 5322 §3.4.
// Understands display names, angle-addr, quoted strings, comments, and groups
// ("Group: a@x, b@y;"). Group members are returned as individual addresses.
std::vector<Address> ParseAddressList(const std::string &value);

// Parse an RFC 5322 §3.3 date ("Mon, 02 Jan 2006 15:04:05 -0700", obsolete
// 2-digit years and named zones included) into Unix epoch seconds. Returns
// false when the value cannot be parsed.
bool ParseDate(const std::string &value, int64_t &epoch_seconds);

// ---------------------------------------------------------------------------
// RFC 2046 — MIME entity tree
// ---------------------------------------------------------------------------

struct MimeEntity {
	std::vector<std::pair<std::string, std::string>> headers;
	ContentType content_type;   // defaulted to text/plain when absent
	std::string charset;        // Content-Type charset param (lowercased), default us-ascii
	std::string encoding;       // Content-Transfer-Encoding token (lowercased), default 7bit
	std::string disposition;    // Content-Disposition token (lowercased), may be empty
	std::string filename;       // name/filename param from type/disposition, may be empty
	std::string content_id;     // Content-ID (angle brackets stripped), may be empty
	std::string body_raw;       // undecoded body bytes for a leaf part
	std::string body_decoded;   // transfer-decoded body bytes for a leaf part
	std::vector<MimeEntity> children; // sub-parts for multipart / message-rfc822

	bool IsMultipart() const { return content_type.type == "multipart"; }
};

// Recursively parse a MIME entity (headers + body). For multipart bodies the
// children are split on the boundary; for message/rfc822 the single enclosed
// message is parsed as one child. Leaf bodies are transfer-decoded.
MimeEntity ParseEntity(const std::string &raw);

struct MimePart {
	std::string section;      // IMAP-style part number: "1", "1.2", "2.1.3"
	std::string content_type; // "type/subtype"
	std::string charset;
	std::string encoding;
	std::string filename;
	int64_t size_bytes = 0;   // size of decoded content
	std::string content;      // decoded leaf content ("" for multipart nodes)
};

// Flatten an entity tree into a depth-first list of parts with IMAP body
// section numbers (RFC 3501 §6.4.5 numbering), for FETCH BODYSTRUCTURE reuse.
std::vector<MimePart> FlattenParts(const MimeEntity &root);

// ---- building ------------------------------------------------------------
//
// Assembling a message is the inverse of ParseEntity, and it lives here rather
// than in a front-end because there were three hand-rolled copies of it: the web
// composer, the mailing-list digest builder and the feed importer. It also means
// the exact bytes are assertable from sqllogictest through `qm_mime_build`.

// A header list. Named here rather than reusing http::Headers, which is the same
// shape but belongs to a layer MIME must not depend on.
using HeaderList = std::vector<std::pair<std::string, std::string>>;

struct BuildPart {
	std::string content_type;      // "text/plain", "text/html", "image/png"
	std::string charset = "utf-8"; // ignored for non-text parts
	std::string content;           // decoded bytes; encoding is chosen below
	std::string filename;          // set to make this an attachment
	std::string content_id;        // set to make this referable as cid:<id>
	std::string disposition;       // "inline" | "attachment" | "" (decide from the above)

	BuildPart();
};

// Assemble a message from headers and parts, choosing the nesting for you:
//
//   one text part                    -> text/plain (or text/html)
//   text + html                      -> multipart/alternative
//   the above + inline (cid:) parts  -> multipart/related
//   any of the above + attachments   -> multipart/mixed
//
// So a plain note stays a single-part message rather than being wrapped in a
// pointless multipart, and a rich message with an inline image and a PDF nests
// mixed(related(alternative(plain, html), image), pdf) — which is the order every
// mail client expects to find them in.
//
// Boundaries are random *and verified absent from every part's bytes*: a part
// that happened to contain the boundary string would otherwise truncate the
// message at that point, and an attacker who controls an attachment controls
// those bytes.
//
// `headers` are emitted verbatim in order, minus any Content-Type,
// Content-Transfer-Encoding or MIME-Version — the framing is decided here, and
// letting a caller also set it is how the two end up disagreeing.
std::string BuildMessage(const HeaderList &headers, const std::vector<BuildPart> &parts);

// Choose a transfer encoding for a part: 8bit for text that is already clean,
// quoted-printable for text with long lines or control characters, base64 for
// anything binary, and 8bit for message/* whatever it contains (RFC 2046
// §5.2.1 permits nothing else around an embedded message). Exposed because the
// choice is worth asserting.
std::string ChooseEncoding(const BuildPart &part);

} // namespace mime
} // namespace quackmail
