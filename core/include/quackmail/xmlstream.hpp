#pragma once

#include <string>
#include <vector>

namespace quackmail {
namespace xmlstream {

// An incremental XML tokenizer, sized for XMPP stanzas.
//
// XMPP is a never-ending XML document: the peer sends an opening
// <stream:stream> and then stanzas arrive one at a time, so a document-oriented
// parser cannot be used and the extension build has no expat. This produces
// start/end/text events from whatever bytes have arrived so far, and reports
// "need more data" instead of blocking.
struct Event {
	enum Kind { START, END, TEXT } kind = TEXT;
	std::string name;                                        // element name, prefix included
	std::vector<std::pair<std::string, std::string>> attrs;   // in document order
	std::string text;                                        // TEXT payload

	// Attribute lookup (case sensitive, like XML); "" when absent.
	std::string Attr(const std::string &key) const;
	// The element name without its namespace prefix ("stream:stream" -> "stream").
	std::string LocalName() const;
};

class Tokenizer {
public:
	void Feed(const std::string &data) {
		buf_ += data;
	}
	// Pop the next event. Returns false when more bytes are needed.
	bool Next(Event &out);
	// Drop any buffered input (used when the stream restarts after STARTTLS or
	// SASL, which begins a brand new XML document).
	void Reset() {
		buf_.clear();
		pos_ = 0;
		pending_end_.clear();
	}

private:
	std::string buf_;
	size_t pos_ = 0;
	std::string pending_end_; // second half of a self-closing element
};

// Escape text for inclusion in an XML attribute or element body.
std::string Escape(const std::string &in);

} // namespace xmlstream
} // namespace quackmail
