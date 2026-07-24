#pragma once

#include <string>

namespace quackmail {

// RFC 3977 §4 wildmat matching, as NNTP clients use it in LIST/NEWGROUPS.
// A pattern is a comma-separated list of items, each optionally prefixed with
// '!' to negate it; the last item that matches decides. Within an item, '*'
// matches any run of characters, '?' matches one, and [...] is a character
// class (with a leading '^' or '!' negating it). Matching is case sensitive,
// which is fine because newsgroup names are already normalized to lower case.
bool WildmatMatch(const std::string &text, const std::string &pattern);

} // namespace quackmail
