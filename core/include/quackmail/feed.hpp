#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace quackmail {
namespace feed {

// RSS 2.0 / RSS 1.0 (RDF) / Atom parsing, over the incremental XML tokenizer
// already in core for XMPP. No new dependency: the extension build has no
// expat, and a feed is small enough that a streaming tokenizer driven to
// completion is a perfectly good document parser.
//
// Parse() is a pure function over a string, which is the point — it makes every
// shape of feed assertable from SQL with no network in the loop, the same trick
// dkim::Verify's injectable KeyLookup plays.

struct Item {
	std::string guid;      // <guid> / <id> / the link, in that order of preference
	std::string link;
	std::string title;
	std::string author;
	int64_t published = 0; // unix seconds; 0 when the feed gave no usable date
	std::string summary;   // <description> / <summary>
	std::string content;   // <content:encoded> / <content>, when richer than summary
	bool html = false;     // the body above is HTML rather than plain text
};

struct Feed {
	std::string title;
	std::string link;
	std::string description;
	std::vector<Item> items;
};

// Returns false when the document is not a feed we recognise. Malformed XML
// yields whatever was parsed before the damage rather than throwing: a feed
// that is half-broken half the time is still worth reading.
bool Parse(const std::string &xml, Feed &out);

// Compose the RFC822 message a feed item becomes. `from_domain` is the site's
// c_fqdn, used for the From: address and Message-ID; `feed_name` names the feed
// in the From: display name.
std::string ToRfc822(const Feed &f, const Item &item, const std::string &feed_name,
                     const std::string &from_domain, const std::string &subject_prefix,
                     const std::string &author_override);

} // namespace feed
} // namespace quackmail
