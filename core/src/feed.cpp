#include "quackmail/feed.hpp"

#include "quackmail/mime.hpp"
#include "quackmail/util.hpp"
#include "quackmail/xmlstream.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace quackmail {
namespace feed {

namespace {

std::string Trim(const std::string &s) {
	size_t a = 0, b = s.size();
	while (a < b && std::isspace((unsigned char)s[a])) {
		a++;
	}
	while (b > a && std::isspace((unsigned char)s[b - 1])) {
		b--;
	}
	return s.substr(a, b - a);
}

// Atom dates are ISO 8601 ("2026-07-28T12:34:56Z", or with a ±hh:mm offset);
// RSS dates are RFC 2822, which mime::ParseDate already handles.
bool ParseIso8601(const std::string &in, int64_t &epoch) {
	int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
	if (std::sscanf(in.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) < 5) {
		return false;
	}
	if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) {
		return false;
	}
	std::tm tm {};
	tm.tm_year = y - 1900;
	tm.tm_mon = mo - 1;
	tm.tm_mday = d;
	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_sec = s;
	// timegm, not mktime: the parsed fields are UTC, and mktime would apply the
	// server's local zone to them.
	std::time_t t = timegm(&tm);
	if (t == (std::time_t)-1) {
		return false;
	}
	// Apply an explicit offset, if the string carries one after the seconds.
	auto tpos = in.find('T');
	if (tpos != std::string::npos) {
		std::string rest = in.substr(tpos);
		auto sign = rest.find_first_of("+-");
		if (sign != std::string::npos && sign + 5 < rest.size() + 1) {
			int oh = 0, om = 0;
			if (std::sscanf(rest.c_str() + sign + 1, "%2d:%2d", &oh, &om) == 2) {
				int off = oh * 3600 + om * 60;
				t -= (rest[sign] == '+' ? off : -off);
			}
		}
	}
	epoch = (int64_t)t;
	return true;
}

int64_t ParseAnyDate(const std::string &in) {
	std::string v = Trim(in);
	if (v.empty()) {
		return 0;
	}
	int64_t epoch = 0;
	if (ParseIso8601(v, epoch)) {
		return epoch;
	}
	if (mime::ParseDate(v, epoch)) {
		return epoch;
	}
	return 0;
}

// Does a string of markup look like HTML rather than plain text? Feeds are
// wildly inconsistent about declaring it, so the content decides.
bool LooksHtml(const std::string &s) {
	static const char *const kTags[] = {"<p", "<br", "<a ", "<div", "<img", "<ul", "<h1", "<h2", "<em",
	                                    "<strong", "<blockquote"};
	std::string lower = util::Lower(s);
	for (const char *t : kTags) {
		if (lower.find(t) != std::string::npos) {
			return true;
		}
	}
	return false;
}

} // namespace

bool Parse(const std::string &xml, Feed &out) {
	out = Feed();

	xmlstream::Tokenizer tok;
	tok.Feed(xml);

	std::vector<std::string> stack; // local element names, outermost first
	std::string text;               // text accumulated inside the current element
	Item item;
	bool in_item = false;
	bool in_author = false;
	bool saw_root = false;
	std::string root;
	// Atom carries the link as an attribute, so it is captured at START rather
	// than from the element's text.
	std::string pending_link;

	auto at = [&](size_t from_end) -> std::string {
		return stack.size() > from_end ? stack[stack.size() - 1 - from_end] : std::string();
	};

	xmlstream::Event ev;
	while (tok.Next(ev)) {
		if (ev.kind == xmlstream::Event::START) {
			std::string name = util::Lower(ev.LocalName());
			// The namespace prefix is dropped by LocalName, so RSS's
			// content:encoded and dc:creator arrive as "encoded" and "creator".
			if (!saw_root) {
				root = name;
				saw_root = true;
				if (root != "rss" && root != "feed" && root != "rdf") {
					return false;
				}
			}
			stack.push_back(name);
			text.clear();

			if (name == "item" || name == "entry") {
				item = Item();
				in_item = true;
				pending_link.clear();
			} else if (name == "author") {
				in_author = true;
			} else if (name == "link" && in_item) {
				// Atom: <link rel="alternate" href="..."/>. RSS puts the URL in
				// the element text instead, handled at END.
				std::string rel = util::Lower(ev.Attr("rel"));
				std::string href = ev.Attr("href");
				if (!href.empty() && (rel.empty() || rel == "alternate")) {
					pending_link = href;
				}
			} else if ((name == "content" || name == "summary" || name == "title") && in_item) {
				// Atom declares the payload type on the element.
				if (util::Lower(ev.Attr("type")).find("html") != std::string::npos) {
					item.html = true;
				}
			}
			continue;
		}

		if (ev.kind == xmlstream::Event::TEXT) {
			text += ev.text;
			continue;
		}

		// END. The tokenizer has already entity-decoded TEXT events (and left
		// CDATA literal, which is what CDATA means), so `text` needs only
		// trimming.
		std::string name = util::Lower(ev.LocalName());
		std::string value = Trim(text);
		text.clear();
		if (!stack.empty()) {
			stack.pop_back();
		}

		if (name == "item" || name == "entry") {
			if (!pending_link.empty()) {
				item.link = pending_link;
			}
			if (item.guid.empty()) {
				// Order of preference: an explicit identifier, then the link,
				// then the title. Something must identify the item or every
				// poll would re-post it.
				item.guid = !item.link.empty() ? item.link : item.title;
			}
			if (!item.html && (LooksHtml(item.content) || LooksHtml(item.summary))) {
				item.html = true;
			}
			if (!item.guid.empty() || !item.title.empty()) {
				out.items.push_back(item);
			}
			in_item = false;
			continue;
		}
		if (name == "author") {
			in_author = false;
			// RSS <author> holds an address directly; Atom nests <name>.
			if (in_item && item.author.empty() && !value.empty()) {
				item.author = value;
			}
			continue;
		}

		if (in_item) {
			if (name == "title") {
				item.title = value;
			} else if (name == "link") {
				if (item.link.empty() && !value.empty()) {
					item.link = value;
				}
			} else if (name == "guid" || name == "id") {
				item.guid = value;
			} else if (name == "pubdate" || name == "published" || name == "updated" || name == "date") {
				if (item.published == 0) {
					item.published = ParseAnyDate(value);
				}
			} else if (name == "description" || name == "summary") {
				item.summary = value;
			} else if (name == "encoded" || name == "content") {
				item.content = value;
			} else if (name == "creator") {
				item.author = value;
			} else if (name == "name" && in_author) {
				item.author = value;
			} else if (name == "email" && in_author && !value.empty()) {
				item.author = item.author.empty() ? value : (item.author + " <" + value + ">");
			}
			continue;
		}

		// Channel-level metadata. `at(0)` is the parent now that this element
		// has been popped, which keeps a <title> inside <image> from becoming
		// the feed's title.
		std::string parent = at(0);
		if (parent == "channel" || parent == "feed") {
			if (name == "title") {
				out.title = value;
			} else if (name == "link" && out.link.empty()) {
				out.link = value;
			} else if (name == "description" || name == "subtitle") {
				out.description = value;
			}
		}
	}

	return saw_root;
}

std::string ToRfc822(const Feed &f, const Item &item, const std::string &feed_name,
                     const std::string &from_domain, const std::string &subject_prefix,
                     const std::string &author_override) {
	std::string domain = from_domain.empty() ? "localhost" : from_domain;
	int64_t when = item.published > 0 ? item.published : (int64_t)std::time(nullptr);

	std::string display = author_override;
	if (display.empty()) {
		display = !item.author.empty() ? item.author
		                               : (!f.title.empty() ? f.title : feed_name);
	}
	// The address is the feed, not the item's author: the author string is
	// free text out of somebody else's XML, and putting it in the addr-spec
	// would let a feed forge a header address. It goes in the display name,
	// where it is quoted and encoded.
	std::string from_addr = "feed-" + feed_name + "@" + domain;

	std::string subject = item.title.empty() ? "(no title)" : item.title;
	if (!subject_prefix.empty() && subject.find(subject_prefix) == std::string::npos) {
		subject = subject_prefix + " " + subject;
	}

	// A stable Message-ID derived from the guid, so the same item re-fetched
	// after a database restore threads as one message rather than two.
	std::string id = util::Sha256Hex(feed_name + "\n" + item.guid).substr(0, 32);

	std::string body_text = !item.content.empty() ? item.content : item.summary;
	std::string head;
	head += "Date: " + util::RfcDate(when) + "\r\n";
	head += "From: \"" + mime::EncodeEncodedWord(display) + "\" <" + from_addr + ">\r\n";
	head += "To: " + from_addr + "\r\n";
	head += "Subject: " + mime::EncodeEncodedWord(subject) + "\r\n";
	head += "Message-ID: <feed." + id + "@" + domain + ">\r\n";
	if (!item.link.empty()) {
		head += "X-Feed-Link: " + item.link + "\r\n";
	}
	if (!f.title.empty()) {
		head += "X-Feed-Title: " + mime::EncodeEncodedWord(f.title) + "\r\n";
	}
	head += "Auto-Submitted: auto-generated\r\n";
	head += "Precedence: bulk\r\n";
	head += "MIME-Version: 1.0\r\n";

	// A plain-text link line always goes first, so the item is useful in a
	// terminal client that renders no HTML at all — which is most of the ways
	// this BBS is read.
	std::string plain;
	if (!item.link.empty()) {
		plain += item.link + "\r\n\r\n";
	}
	if (item.html) {
		std::string boundary = "=_qc_feed_" + util::RandomHex(16);
		head += "Content-Type: multipart/alternative; boundary=\"" + boundary + "\"\r\n";
		std::string summary_text = item.summary.empty() ? item.title : item.summary;
		std::string body;
		body += "--" + boundary + "\r\n";
		body += "Content-Type: text/plain; charset=\"UTF-8\"\r\n\r\n";
		body += plain + summary_text + "\r\n\r\n";
		body += "--" + boundary + "\r\n";
		body += "Content-Type: text/html; charset=\"UTF-8\"\r\n\r\n";
		body += body_text + "\r\n";
		body += "--" + boundary + "--\r\n";
		return head + "\r\n" + body;
	}

	head += "Content-Type: text/plain; charset=\"UTF-8\"\r\n";
	return head + "\r\n" + plain + body_text + "\r\n";
}

} // namespace feed
} // namespace quackmail
