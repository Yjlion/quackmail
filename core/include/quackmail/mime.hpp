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

} // namespace mime
} // namespace quackmail
