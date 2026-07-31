#include "quackmail/davxml.hpp"

#include "quackmail/http.hpp"
#include "quackmail/xmlstream.hpp"

#include <cctype>

namespace quackmail {
namespace dav {

const char *const kNsDav = "DAV:";
const char *const kNsCalDav = "urn:ietf:params:xml:ns:caldav";
const char *const kNsCardDav = "urn:ietf:params:xml:ns:carddav";
const char *const kNsCalServer = "http://calendarserver.org/ns/";
const char *const kNsApple = "http://apple.com/ns/ical/";

namespace {

struct NsEntry {
	const char *uri;
	const char *prefix;
};

const NsEntry kRegistry[] = {
    {kNsDav, "D"},
    {kNsCalDav, "C"},
    {kNsCardDav, "CARD"},
    {kNsCalServer, "CS"},
    {kNsApple, "A"},
};

bool IsSafeNameByte(unsigned char c) {
	return std::isalnum(c) || c == '.' || c == '_' || c == '-';
}

int HexVal(unsigned char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

std::string LocalOf(const std::string &qname) {
	size_t colon = qname.find(':');
	return colon == std::string::npos ? qname : qname.substr(colon + 1);
}

std::string PrefixOf(const std::string &qname) {
	size_t colon = qname.find(':');
	return colon == std::string::npos ? std::string() : qname.substr(0, colon);
}

} // namespace

std::string PrefixFor(const std::string &ns) {
	for (const auto &e : kRegistry) {
		if (ns == e.uri) {
			return e.prefix;
		}
	}
	return std::string();
}

// ---- Node ----------------------------------------------------------------

Node::Node() {
}

std::string Node::Attr(const std::string &key) const {
	for (const auto &a : attrs) {
		if (a.first == key) {
			return a.second;
		}
	}
	return std::string();
}

bool Node::Is(const std::string &want_ns, const std::string &want_name) const {
	return ns == want_ns && name == want_name;
}

const Node *Node::Child(const std::string &child_ns, const std::string &child_name) const {
	for (const auto &c : children) {
		if (c.Is(child_ns, child_name)) {
			return &c;
		}
	}
	return nullptr;
}

std::vector<const Node *> Node::Children(const std::string &child_ns, const std::string &child_name) const {
	std::vector<const Node *> out;
	for (const auto &c : children) {
		if (c.Is(child_ns, child_name)) {
			out.push_back(&c);
		}
	}
	return out;
}

// ---- reading -------------------------------------------------------------

bool ParseDoc(const std::string &xml, Node &out) {
	xmlstream::Tokenizer tk;
	tk.Feed(xml);

	out = Node();
	std::vector<Node *> stack;
	std::vector<std::string> qnames; // the qualified name each open element was written with
	// One namespace scope per open element: (prefix, uri), with "" for the
	// default namespace. Resolution walks this outward from the innermost.
	std::vector<std::vector<std::pair<std::string, std::string>>> scopes;

	bool have_root = false;
	size_t nodes = 0;

	auto resolve = [&](const std::string &prefix) -> std::string {
		for (size_t i = scopes.size(); i > 0; i--) {
			for (const auto &d : scopes[i - 1]) {
				if (d.first == prefix) {
					return d.second;
				}
			}
		}
		return std::string();
	};

	xmlstream::Event ev;
	while (tk.Next(ev)) {
		if (ev.kind == xmlstream::Event::TEXT) {
			if (!stack.empty()) {
				stack.back()->text += ev.text;
			}
			continue;
		}

		if (ev.kind == xmlstream::Event::END) {
			if (stack.empty() || qnames.back() != ev.name) {
				return false;
			}
			stack.pop_back();
			qnames.pop_back();
			scopes.pop_back();
			continue;
		}

		// START.
		//
		// The tokenizer skips <?...?> and <!-- --> but not <!DOCTYPE, which it
		// would hand back as an element named "!DOCTYPE" with no closing tag.
		// Refuse it outright: a DAV body has no legitimate DTD, and accepting one
		// is the front door to entity expansion.
		if (ev.name.empty() || ev.name[0] == '!') {
			return false;
		}
		if (++nodes > kMaxNodes || stack.size() >= kMaxDepth) {
			return false;
		}
		if (have_root && stack.empty()) {
			return false; // a second root element
		}

		// This element's namespace declarations have to be in scope before its
		// own name is resolved: <D:propfind xmlns:D="DAV:"> declares the prefix it
		// is itself written with.
		std::vector<std::pair<std::string, std::string>> decls;
		for (const auto &a : ev.attrs) {
			if (a.first == "xmlns") {
				decls.emplace_back(std::string(), a.second);
			} else if (a.first.rfind("xmlns:", 0) == 0) {
				decls.emplace_back(a.first.substr(6), a.second);
			}
		}
		scopes.push_back(std::move(decls));

		Node node;
		node.ns = resolve(PrefixOf(ev.name));
		node.name = LocalOf(ev.name);
		for (const auto &a : ev.attrs) {
			if (a.first == "xmlns" || a.first.rfind("xmlns:", 0) == 0) {
				continue;
			}
			node.attrs.push_back(a);
		}

		Node *placed = nullptr;
		if (stack.empty()) {
			out = std::move(node);
			placed = &out;
			have_root = true;
		} else {
			stack.back()->children.push_back(std::move(node));
			placed = &stack.back()->children.back();
		}
		stack.push_back(placed);
		qnames.push_back(ev.name);
	}

	return have_root && stack.empty();
}

// ---- writing -------------------------------------------------------------

Writer::Writer() {
}

void Writer::FlushOpen() {
	if (pending_) {
		out_ += '>';
		pending_ = false;
	}
}

void Writer::StartDoc(const std::string &ns, const std::string &name) {
	out_ += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
	std::string qname = PrefixFor(ns) + ":" + name;
	out_ += "<" + qname;
	for (const auto &e : kRegistry) {
		out_ += " xmlns:" + std::string(e.prefix) + "=\"" + xmlstream::Escape(e.uri) + "\"";
	}
	stack_.push_back(qname);
	pending_ = true;
}

void Writer::Open(const std::string &ns, const std::string &name) {
	FlushOpen();
	std::string prefix = PrefixFor(ns);
	if (prefix.empty()) {
		// A namespace we do not know — a property a client asked for by name.
		// It still has to be echoed back inside the 404 propstat, so invent a
		// prefix and declare it on the element itself.
		prefix = "X" + std::to_string(gensym_++);
		std::string qname = prefix + ":" + name;
		out_ += "<" + qname + " xmlns:" + prefix + "=\"" + xmlstream::Escape(ns) + "\"";
		stack_.push_back(qname);
		pending_ = true;
		return;
	}
	std::string qname = prefix + ":" + name;
	out_ += "<" + qname;
	stack_.push_back(qname);
	pending_ = true;
}

void Writer::Attr(const std::string &name, const std::string &value) {
	if (!pending_) {
		return; // an attribute after the element has content is a caller bug
	}
	out_ += " " + name + "=\"" + xmlstream::Escape(value) + "\"";
}

void Writer::Text(const std::string &text) {
	FlushOpen();
	out_ += xmlstream::Escape(text);
}

void Writer::Empty(const std::string &ns, const std::string &name) {
	Open(ns, name);
	Close();
}

void Writer::TextElem(const std::string &ns, const std::string &name, const std::string &text) {
	Open(ns, name);
	Text(text);
	Close();
}

void Writer::RawFragment(const std::string &fragment) {
	FlushOpen();
	out_ += fragment;
}

void Writer::Close() {
	if (stack_.empty()) {
		return;
	}
	if (pending_) {
		out_ += "/>";
		pending_ = false;
	} else {
		out_ += "</" + stack_.back() + ">";
	}
	stack_.pop_back();
}

std::string StatusLine(int status) {
	return "HTTP/1.1 " + std::to_string(status) + " " + http::StatusText(status);
}

// ---- resource names ------------------------------------------------------

std::string NameForEuid(const std::string &euid) {
	static const char *kHex = "0123456789ABCDEF";
	std::string out;
	out.reserve(euid.size() + 8);
	for (size_t i = 0; i < euid.size(); i++) {
		unsigned char c = (unsigned char)euid[i];
		// A '.' is safe everywhere except at the front, where leaving it alone
		// would let an euid of "." or ".." encode to itself — and those are the
		// two segments NormalizePath rejects outright, so the resource would be
		// addressable by no URL at all. Escaping only the leading one keeps
		// "abc@example.com" readable while making that shape unreachable.
		if (IsSafeNameByte(c) && !(c == '.' && i == 0)) {
			out += (char)c;
			continue;
		}
		out += '~';
		out += kHex[c >> 4];
		out += kHex[c & 0x0F];
	}
	return out;
}

std::string EuidForName(const std::string &name) {
	std::string out;
	out.reserve(name.size());
	for (size_t i = 0; i < name.size(); i++) {
		if (name[i] != '~' || i + 2 >= name.size()) {
			out += name[i];
			continue;
		}
		int hi = HexVal((unsigned char)name[i + 1]);
		int lo = HexVal((unsigned char)name[i + 2]);
		if (hi < 0 || lo < 0) {
			out += name[i]; // not an escape after all; keep the byte
			continue;
		}
		out += (char)((hi << 4) | lo);
		i += 2;
	}
	return out;
}

} // namespace dav
} // namespace quackmail
