#pragma once

#include <string>
#include <utility>
#include <vector>

namespace quackmail {
namespace dav {

// The pure half of WebDAV: an XML document reader, an XML writer, and the
// resource-name encoding. No sockets, no database, no routing — the same split
// core/http.cpp makes against http/, and for the same reason: everything here
// is reachable from a SQL scalar function and so is testable from test/sql/.
//
// The protocol itself (collections, properties, the verbs) lives in
// http/src/dav_*.cpp, because it needs rooms and permissions.

// ---- namespaces ----------------------------------------------------------
//
// A fixed registry. Clients pick their own prefixes on the way in — which is
// why the reader resolves prefixes to URIs and matching is never on a prefix —
// but on the way out we choose, and a stable short prefix keeps a multistatus
// readable in a packet capture.

extern const char *const kNsDav;       // "DAV:"
extern const char *const kNsCalDav;    // "urn:ietf:params:xml:ns:caldav"
extern const char *const kNsCardDav;   // "urn:ietf:params:xml:ns:carddav"
extern const char *const kNsCalServer; // "http://calendarserver.org/ns/"
// Apple's iCal namespace. Not a standard, but calendar-color and calendar-order
// are what every client actually stores its per-collection decoration in, and a
// PROPPATCH we reject is one iOS retries forever.
extern const char *const kNsApple; // "http://apple.com/ns/ical/"

// The prefix this writer emits for a namespace URI ("D", "C", "CARD", "CS"),
// or "" for one it does not know.
std::string PrefixFor(const std::string &ns);

// ---- reading -------------------------------------------------------------

// One element of a parsed request body. `ns` is the resolved namespace *URI*,
// never the prefix the client used; `name` is the local name.
struct Node {
	std::string ns;
	std::string name;
	std::vector<std::pair<std::string, std::string>> attrs; // non-xmlns attributes, in order
	std::string text;                                       // direct character data, concatenated
	std::vector<Node> children;

	Node();

	// Attribute lookup by unqualified name; "" when absent.
	std::string Attr(const std::string &key) const;
	// The first child matching {ns, name}, or nullptr.
	const Node *Child(const std::string &ns, const std::string &name) const;
	// Every child matching {ns, name}, in document order.
	std::vector<const Node *> Children(const std::string &ns, const std::string &name) const;
	// Whether this element is {ns, name}.
	bool Is(const std::string &want_ns, const std::string &want_name) const;
};

// Parse a whole document into `out` (the root element). Returns false for
// malformed input, for an unterminated document, or when a limit below is
// exceeded — a DAV body arrives from the network, so this refuses rather than
// recurses.
//
// An empty body is *not* an error at this layer: several DAV verbs treat "no
// body" as a defined request (PROPFIND with no body means allprop). Callers
// check for emptiness before calling.
bool ParseDoc(const std::string &xml, Node &out);

// Limits ParseDoc enforces. Deliberately small: no legitimate PROPFIND or
// REPORT body comes near them.
constexpr size_t kMaxDepth = 32;
constexpr size_t kMaxNodes = 20000;

// ---- writing -------------------------------------------------------------

// A minimal element-tree serializer. Not a general XML writer: it emits exactly
// the shape DAV responses take, with every text node escaped through
// xmlstream::Escape and no way to inject raw markup by accident.
class Writer {
public:
	Writer();

	// Begin the document with an <?xml?> declaration and a root element that
	// declares every namespace in the registry. Called once, first.
	void StartDoc(const std::string &ns, const std::string &name);

	void Open(const std::string &ns, const std::string &name);
	// An attribute on the element most recently opened. Must come before any
	// child or text is written into it.
	void Attr(const std::string &name, const std::string &value);
	// Character data, escaped.
	void Text(const std::string &text);
	// An empty element: <D:collection/>.
	void Empty(const std::string &ns, const std::string &name);
	// An element containing only text: <D:href>...</D:href>.
	void TextElem(const std::string &ns, const std::string &name, const std::string &text);
	void Close();

	// Append markup this module already produced — the escaped output of another
	// Writer, spliced in whole. It has the alarming name because it is the one
	// way to put bytes in without escaping them, and the only legitimate caller
	// is a builder assembling propstat blocks it wrote itself.
	void RawFragment(const std::string &fragment);

	// Everything written so far. Closes nothing — call Close() for each Open()
	// first; unbalanced output is a bug, not a shape to tolerate.
	const std::string &Str() const {
		return out_;
	}

private:
	void FlushOpen();

	std::string out_;
	std::vector<std::string> stack_; // qualified names still open
	bool pending_ = false;           // an Open() whose '>' has not been written
	int gensym_ = 0;                 // counter for prefixes we invent
};

// The "HTTP/1.1 207 Multi-Status" status line a <D:status> element carries.
std::string StatusLine(int status);

// ---- resource names ------------------------------------------------------
//
// A DAV resource name has to survive http::NormalizePath, which percent-decodes
// *before* splitting the path into segments. So an euid containing '/' would
// become two segments and one containing ".." would be rejected outright —
// percent-encoding cannot help, because it is undone before the split happens.
//
// The encoding below therefore emits only unreserved characters (RFC 3986):
// [A-Za-z0-9._-] pass through, and every other byte becomes ~HH. '~' is itself
// unreserved, so percent-decoding leaves the escape intact and the round trip is
// exact. Ordinary UIDs stay readable: "abc-123@example.com" encodes to
// "abc-123~40example.com".
std::string NameForEuid(const std::string &euid);
// The inverse. A malformed escape is passed through literally rather than
// dropped, so decoding never silently loses bytes — the same rule
// http::PercentDecode follows.
std::string EuidForName(const std::string &name);

} // namespace dav
} // namespace quackmail
