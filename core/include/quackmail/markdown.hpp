#pragma once

#include <functional>
#include <string>

namespace quackmail {
namespace markdown {

// A bounded Markdown subset, for wiki pages stored as `text/x-markdown`.
//
// Written here rather than vendored. This tree takes no third-party
// dependencies — the Public Suffix List and the time zone database are
// *generated and committed*, not fetched — and CommonMark is a specification
// large enough that a conforming implementation would be the largest single
// thing in `core/`. What a wiki actually needs is much smaller.
//
// Two properties matter more than coverage:
//
//  1. **It is bounded.** One line-oriented pass, no regular expressions, no
//     backtracking, an explicit nesting cap and an output-size cap. The input is
//     a page somebody typed and the output is re-served to everyone who can read
//     the room, so a quadratic case here is a denial of service with a byline.
//
//  2. **Raw HTML in the source is escaped, not passed through.** That is a
//     deliberate departure from CommonMark, and it removes the injection surface
//     at the source instead of relying on the sanitizer to catch it. The output
//     is still put through `html::SanitizeForCompose` before it reaches a page,
//     on the same terms as everything else this server stores and re-serves.
//
// Supported: ATX headings (`#`..`######`), fenced (```) and indented code,
// blockquotes, unordered (`-`, `*`, `+`) and ordered (`1.`) lists with one level
// of nesting, paragraphs, thematic breaks (`---`), pipe tables, `**strong**`,
// `*emphasis*`, `` `code` ``, `[text](url)`, `![alt](url)`, autolinked bare URLs,
// and `[[Wiki Links]]`.
//
// Not supported, on purpose: setext headings, reference links, footnotes,
// nested blockquotes, and HTML blocks.

// Resolve a `[[Wiki Link]]` to an href. Given the page name exactly as written,
// it returns the URL to link to, or "" to render the link as plain text. A null
// function renders every wiki link as plain text, which is what a non-wiki
// caller wants.
using WikiLink = std::function<std::string(const std::string &page)>;

// Does the page named by a wiki link exist? Used only to mark a link to a page
// nobody has written yet, which is the affordance that makes a wiki a wiki. A
// null function treats every link as existing.
using WikiExists = std::function<bool(const std::string &page)>;

struct Options {
	WikiLink link;
	WikiExists exists;
	// The class put on a link to a page that does not exist yet.
	std::string wanted_class = "wanted";
	// Hard ceilings. Past either, rendering stops and the remainder is emitted
	// as escaped plain text rather than being silently dropped.
	size_t max_input = 1u << 20;   // 1 MiB of source
	size_t max_output = 1u << 22;  // 4 MiB of HTML
	int max_nesting = 8;
};

std::string Render(const std::string &src, const Options &opts = Options());

// The inline half on its own: emphasis, code spans, links and wiki links, with
// everything else escaped. Exposed because a wiki page title and a table cell
// need it without the block grammar around them.
std::string RenderInline(const std::string &src, const Options &opts = Options());

} // namespace markdown
} // namespace quackmail
