#include "web_i18n.hpp"

namespace duckdb {
namespace qmweb {

std::string EffectiveLocale(const Ctx &ctx) {
	std::string want;
	if (ctx.Authed()) {
		want = quackmail::citadel::GetUserPref(ctx.con, ctx.username, "web_locale");
	}
	if (want.empty()) {
		want = ConfigStr(ctx.con, "qm_default_locale", "en");
	}
	// Only "en" exists today; anything else — a stale pref, a typo'd config
	// value — falls back rather than serving a locale with no catalog.
	if (want != "en") {
		return "en";
	}
	return want;
}

std::vector<std::pair<std::string, std::string>> LocaleOptions() {
	return {{"", "Follow the site default"}, {"en", "English"}};
}

namespace {

struct Msg {
	const char *key;
	const char *en;
};

// One row per string, one column per language. Only "en" exists so far — see
// web_i18n.hpp for what adding a second one looks like. Grouped by the page it
// belongs to, matching the four surfaces this first pass covers: the login
// screen, the sidebar, the mail view, and compose.
const Msg kMessages[] = {
    {"login.title_prefix", "Sign in to "},
    {"login.username", "User name"},
    {"login.password", "Password"},
    {"login.signin", "Sign in"},

    {"nav.mail", "Mail"},
    {"nav.compose", "Compose"},
    {"nav.inbox", "Inbox"},
    {"nav.all_folders", "All folders"},
    {"nav.groupware", "Groupware"},
    {"nav.rooms", "Rooms"},
    {"nav.all_rooms", "All rooms"},
    {"nav.search", "Search"},
    {"nav.create_room", "Create a room"},
    {"nav.who_online", "Who is online"},
    {"nav.you", "You"},
    {"nav.preferences", "Preferences"},
    {"nav.filters", "Filters"},
    {"nav.sessions", "Signed-in browsers"},
    {"nav.system", "System"},
    {"nav.admin", "Admin console"},

    {"mail.title", "Mail"},
    {"mail.write", "Write a message"},
    {"mail.folder", "Folder"},
    {"mail.unread", "Unread"},
    {"mail.total", "Total"},
    {"mail.footer", "These are ordinary Citadel rooms — the same messages are visible over IMAP, "
                    "POP3 and the BBS."},

    {"mailbox.search_folder", "Search this folder"},
    {"mailbox.all", "All"},
    {"mailbox.mark_all_read", "Mark all read"},
    {"mailbox.empty", "This folder is empty."},
    {"mailbox.subject", "Subject"},
    {"mailbox.from", "From"},
    {"mailbox.date", "Date"},
    {"mailbox.size", "Size"},
    {"pager.page", "Page"},
    {"pager.of", "of"},
    {"pager.newer", "Newer"},
    {"pager.older", "Older"},

    {"compose.title", "Write a message"},
    {"compose.to", "To"},
    {"compose.cc", "Cc"},
    {"compose.subject", "Subject"},
    {"compose.message", "Message"},
    {"compose.formatted_text", "Formatted text"},
    {"compose.attachment", "Attachment"},
    {"compose.send", "Send"},
    {"compose.save_draft", "Save as draft"},
    {"compose.cancel", "Cancel"},
    {"compose.address_book", "Address book"},
};

} // namespace

std::string Tr(const Ctx &ctx, const std::string &key) {
	// Resolved now (not cached): a locale switch takes effect on the very next
	// page, and this is cheap enough — one preference lookup per page render,
	// same cost EffectiveTz already pays — not to bother memoizing.
	EffectiveLocale(ctx);
	for (auto &m : kMessages) {
		if (key == m.key) {
			return m.en;
		}
	}
	// No catalog entry: return the key itself. Visible and obviously wrong in
	// review or in a screenshot, never empty and never a crash.
	return key;
}

} // namespace qmweb
} // namespace duckdb
