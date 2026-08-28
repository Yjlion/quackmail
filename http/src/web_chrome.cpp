#include "web.hpp"
#include "web_assets.hpp"
#include "web_i18n.hpp"

#include "quackmail/mime.hpp"
#include "quackmail/tz.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Message;
using quackmail::citadel::Room;

Ctx::Ctx(Connection &c, const http::Request &rq, http::Response &rs) : con(c), req(rq), resp(rs) {
}

std::string Ctx::Cap(size_t i) const {
	return i < captures.size() ? captures[i] : std::string();
}

namespace {

int64_t ToInt(const std::string &s, int64_t dflt) {
	if (s.empty()) {
		return dflt;
	}
	char *end = nullptr;
	long long v = std::strtoll(s.c_str(), &end, 10);
	if (end == s.c_str() || (end && *end != '\0')) {
		return dflt;
	}
	return (int64_t)v;
}

} // namespace

int64_t Ctx::ParamInt(const std::string &name, int64_t dflt) const {
	return ToInt(req.Param(name), dflt);
}

int64_t Ctx::FormInt(const std::string &name, int64_t dflt) const {
	return ToInt(req.Form(name), dflt);
}

unique_ptr<QueryResult> Exec(Connection &con, const std::string &sql, vector<Value> params) {
	auto stmt = con.Prepare(sql);
	if (stmt->HasError()) {
		return nullptr;
	}
	// `params` is a named lvalue here, which is what Execute's non-const
	// reference parameter needs — that is the whole reason for this wrapper.
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		return nullptr;
	}
	return r;
}

// ---- configuration -------------------------------------------------------

std::string ConfigStr(Connection &con, const char *name, const std::string &dflt) {
	return quackmail::citadel::GetConfig(con, name, dflt);
}

bool ConfigBool(Connection &con, const char *name, bool dflt) {
	std::string v = quackmail::citadel::GetConfig(con, name, dflt ? "1" : "0");
	return v == "1" || v == "true" || v == "yes" || v == "on";
}

// ---- escaping ------------------------------------------------------------

std::string T(const std::string &text) {
	return http::EscapeHtml(text);
}

std::string A(const std::string &value) {
	return http::EscapeAttr(value);
}

std::string RawHtml(const std::string &html) {
	return html;
}

// ---- small builders ------------------------------------------------------

std::string Cell(const std::string &text, const std::string &css_class, const std::string &label) {
	std::string out = "<td";
	if (!css_class.empty()) {
		out += " class=\"" + A(css_class) + "\"";
	}
	// Below the sidebar breakpoint qc.css turns every row into a card and every
	// cell labels itself from this attribute, so a phone gets "From: Ada" rather
	// than a column sheared off the side. Passing it is what makes a table
	// legible on a phone; omitting it leaves the cell unlabelled, not broken.
	if (!label.empty()) {
		out += " data-label=\"" + A(label) + "\"";
	}
	return out + ">" + T(text) + "</td>";
}

std::string Head(const std::string &text) {
	return "<th>" + T(text) + "</th>";
}

// ---- data tables ---------------------------------------------------------

Column::Column(const std::string &key_, const std::string &label_, const std::string &css_class_,
               bool numeric_)
    : key(key_), label(label_), css_class(css_class_), numeric(numeric_) {
}

Column Column::Num(const std::string &key, const std::string &label) {
	return Column(key, label, "num", true);
}

namespace {

// The href a sorting header points at: this page, this query, a different sort.
//
// Rebuilt from the matched path rather than echoed from the request target.
// The target is bytes a client chose and this ends up in an href; the path is
// what the router already matched, and every surviving query parameter goes
// back out exactly as encoded as it arrived.
std::string SortBase(const Ctx &ctx) {
	std::string out;
	const std::string &path = ctx.req.path;
	size_t pos = 0;
	while (true) {
		size_t slash = path.find('/', pos);
		out += http::PercentEncode(
		    path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos));
		if (slash == std::string::npos) {
			break;
		}
		out += "/";
		pos = slash + 1;
	}

	std::string kept;
	const std::string &query = ctx.req.query;
	size_t i = 0;
	while (i < query.size()) {
		size_t amp = query.find('&', i);
		std::string one = query.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
		std::string name = one.substr(0, one.find('='));
		if (!one.empty() && name != "sort" && name != "dir") {
			kept += (kept.empty() ? "" : "&") + one;
		}
		if (amp == std::string::npos) {
			break;
		}
		i = amp + 1;
	}
	return out + "?" + (kept.empty() ? "" : kept + "&");
}

} // namespace

Table::Table(Ctx &ctx, const std::string &id, const std::vector<Column> &columns)
    : id_(id), columns_(columns), base_(SortBase(ctx)) {
	std::string want = ctx.req.Param("sort");
	for (size_t i = 0; i < columns_.size(); i++) {
		if (!columns_[i].key.empty() && columns_[i].key == want) {
			sort_ = (int)i;
		}
	}
	desc_ = ctx.req.Param("dir") == "desc";
}

Table::Row Table::Add(const std::string &row_class) {
	RowData row;
	row.css_class = row_class;
	rows_.push_back(row);
	return Row(this, rows_.size() - 1);
}

bool Table::Sorted() const {
	return sort_ >= 0;
}

const std::string &Table::SortKey() const {
	static const std::string kNone;
	return sort_ >= 0 ? columns_[(size_t)sort_].key : kNone;
}

bool Table::Descending() const {
	return desc_;
}

bool Table::Empty() const {
	return rows_.empty();
}

void Table::ExtraClass(const std::string &css_class) {
	extra_class_ = css_class;
}

Table::Row &Table::Row::Text(const std::string &value) {
	Table::CellData cell;
	cell.html = T(value);
	cell.sort = value;
	table_->rows_[index_].cells.push_back(cell);
	return *this;
}

Table::Row &Table::Row::Html(const std::string &html, const std::string &sort_as) {
	Table::CellData cell;
	cell.html = html;
	cell.sort = sort_as;
	table_->rows_[index_].cells.push_back(cell);
	return *this;
}

Table::Row &Table::Row::Number(int64_t value) {
	Table::CellData cell;
	cell.html = T(std::to_string(value));
	cell.sort = std::to_string(value);
	table_->rows_[index_].cells.push_back(cell);
	return *this;
}

std::string Table::Render() const {
	std::vector<size_t> order;
	order.reserve(rows_.size());
	for (size_t i = 0; i < rows_.size(); i++) {
		order.push_back(i);
	}
	if (sort_ >= 0) {
		size_t c = (size_t)sort_;
		bool numeric = columns_[c].numeric;
		bool desc = desc_;
		const std::vector<RowData> *rows = &rows_;
		auto value = [rows, c](size_t r) {
			return c < (*rows)[r].cells.size() ? (*rows)[r].cells[c].sort : std::string();
		};
		auto less = [&](size_t a, size_t b) {
			std::string x = value(a), y = value(b);
			if (numeric) {
				return std::strtod(x.c_str(), nullptr) < std::strtod(y.c_str(), nullptr);
			}
			return quackmail::util::Lower(x) < quackmail::util::Lower(y);
		};
		// stable_sort, so rows the active column cannot tell apart stay in the
		// order the handler put them in.
		std::stable_sort(order.begin(), order.end(),
		                 [&](size_t a, size_t b) { return desc ? less(b, a) : less(a, b); });
	}

	std::string out = "<div class=\"wrap\"><table class=\"datatable" +
	                  (extra_class_.empty() ? std::string() : " " + A(extra_class_)) +
	                  "\" data-table=\"" + A(id_) + "\">";
	// A <col> per column: the client-side resize writes widths here rather than
	// on every cell, and the reorder moves these in step with the headers.
	// Every column needs a *unique* handle, because the client-side reorder
	// addresses columns by it. A table can hold two columns with no sort key and
	// no heading (an action column at each end), so an unkeyed column falls back
	// to its position rather than to its label.
	auto handle = [&](size_t i) {
		return columns_[i].key.empty() ? "c" + std::to_string(i) : columns_[i].key;
	};
	out += "<colgroup>";
	for (size_t i = 0; i < columns_.size(); i++) {
		out += "<col data-col=\"" + A(handle(i)) + "\">";
	}
	out += "</colgroup><thead><tr>";
	for (size_t ci = 0; ci < columns_.size(); ci++) {
		const Column &col = columns_[ci];
		std::string classes = col.css_class;
		out += "<th scope=\"col\"";
		if (!classes.empty()) {
			out += " class=\"" + A(classes) + "\"";
		}
		out += " data-col=\"" + A(handle(ci)) + "\"";
		bool active = sort_ >= 0 && columns_[(size_t)sort_].key == col.key && !col.key.empty();
		if (!col.key.empty()) {
			out += " aria-sort=\"" + std::string(active ? (desc_ ? "descending" : "ascending") : "none") +
			       "\"";
		}
		out += ">";
		if (col.key.empty()) {
			out += T(col.label);
		} else {
			// Clicking the column already sorted on flips it.
			std::string dir = active && !desc_ ? "desc" : "asc";
			out += "<a class=\"sortlink\" href=\"" + A(base_ + "sort=" + http::PercentEncode(col.key) +
			                                            "&dir=" + dir) +
			       "\">" + T(col.label) + (active ? (desc_ ? " ▾" : " ▴") : "") + "</a>";
		}
		// The grip qc.js turns into a resize handle. Inert markup otherwise.
		out += "<span class=\"colgrip\" aria-hidden=\"true\"></span>";
		out += "</th>";
	}
	out += "</tr></thead><tbody>";
	for (size_t r : order) {
		const RowData &row = rows_[r];
		out += "<tr";
		if (!row.css_class.empty()) {
			out += " class=\"" + A(row.css_class) + "\"";
		}
		out += ">";
		for (size_t c = 0; c < columns_.size(); c++) {
			out += "<td";
			if (!columns_[c].css_class.empty()) {
				out += " class=\"" + A(columns_[c].css_class) + "\"";
			}
			// Never optional here: it is the column's own label, so a phone gets
			// a labelled card without the caller doing anything.
			out += " data-label=\"" + A(columns_[c].label) + "\">";
			out += c < row.cells.size() ? row.cells[c].html : std::string();
			out += "</td>";
		}
		out += "</tr>";
	}
	out += "</tbody></table></div>";
	return out;
}

// Pico styles a link as a button from role="button", not from a class, so the
// long-standing "btn" / "btn sec" classes are translated here rather than at a
// hundred call sites.
std::string PicoButtonClass(const std::string &css_class) {
	// Translate the three variants this tree has always used, and *keep*
	// anything else: callers attach hooks like "backtolist" to the same
	// attribute, and dropping them silently turns a styled control into one no
	// script and no media query can find.
	std::string out;
	size_t i = 0;
	while (i < css_class.size()) {
		size_t sp = css_class.find(' ', i);
		std::string word = css_class.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
		i = sp == std::string::npos ? css_class.size() : sp + 1;
		if (word.empty() || word == "btn") {
			continue; // "btn" becomes role="button"; see Link()
		}
		std::string mapped = word == "sec" ? "secondary" : word == "danger" ? "outline danger" : word;
		if (!out.empty()) {
			out += " ";
		}
		out += mapped;
	}
	return out;
}

std::string Link(const std::string &href, const std::string &label, const std::string &css_class) {
	std::string out = "<a href=\"" + A(href) + "\"";
	bool as_button = css_class.find("btn") != std::string::npos;
	std::string cls = as_button ? PicoButtonClass(css_class) : css_class;
	if (as_button) {
		out += " role=\"button\"";
	}
	if (!cls.empty()) {
		out += " class=\"" + A(cls) + "\"";
	}
	return out + ">" + T(label) + "</a>";
}

std::string Icon(const std::string &name) {
	// aria-hidden throughout: every icon in this interface sits beside its own
	// label, or beside a .vh one, so announcing it again would only be noise.
	return "<svg class=\"i\" aria-hidden=\"true\"><use href=\"#i-" + A(name) + "\"></use></svg>";
}

std::string TextInput(const std::string &name, const std::string &value, const std::string &type,
                      const std::string &placeholder) {
	std::string out = "<input type=\"" + A(type) + "\" name=\"" + A(name) + "\" value=\"" + A(value) + "\"";
	if (!placeholder.empty()) {
		out += " placeholder=\"" + A(placeholder) + "\"";
	}
	return out + ">";
}

std::string TextArea(const std::string &name, const std::string &value, int rows) {
	return "<textarea name=\"" + A(name) + "\" rows=\"" + std::to_string(rows) + "\">" + T(value) +
	       "</textarea>";
}

std::string Hidden(const std::string &name, const std::string &value) {
	return "<input type=\"hidden\" name=\"" + A(name) + "\" value=\"" + A(value) + "\">";
}

std::string Checkbox(const std::string &name, bool checked, const std::string &label) {
	return "<label class=\"chk\"><input type=\"checkbox\" name=\"" + A(name) + "\" value=\"1\"" +
	       (checked ? " checked" : "") + "> " + T(label) + "</label>";
}

std::string Select(const std::string &name, const std::vector<std::pair<std::string, std::string>> &options,
                   const std::string &selected) {
	std::string out = "<select name=\"" + A(name) + "\">";
	for (auto &opt : options) {
		out += "<option value=\"" + A(opt.first) + "\"" + (opt.first == selected ? " selected" : "") + ">" +
		       T(opt.second) + "</option>";
	}
	return out + "</select>";
}

std::string FormStart(const Ctx &ctx, const std::string &action, const std::string &css_class) {
	std::string out = "<form method=\"post\" action=\"" + A(action) + "\"";
	if (!css_class.empty()) {
		out += " class=\"" + A(css_class) + "\"";
	}
	out += ">";
	// Every form carries the synchronizer token; the router refuses any POST
	// without it, so leaving it out here would break the form loudly rather
	// than silently opening a CSRF hole.
	out += Hidden("_csrf", ctx.csrf);
	return out;
}

std::string FormEnd() {
	return "</form>";
}

std::string Button(const std::string &label, const std::string &css_class) {
	return IconButton(label, "", css_class);
}

std::string IconButton(const std::string &label, const std::string &icon, const std::string &css_class) {
	std::string cls = PicoButtonClass(css_class);
	std::string out = "<button";
	if (!cls.empty()) {
		out += " class=\"" + A(cls) + "\"";
	}
	out += ">";
	if (!icon.empty()) {
		out += Icon(icon);
	}
	return out + T(label) + "</button>";
}

std::string ButtonGroup(const std::string &inner) {
	// Pico draws role="group" as one segmented control, which is what turns the
	// scattered row of separately outlined boxes this interface used to emit
	// into a single bar.
	return "<div role=\"group\">" + inner + "</div>";
}

std::string Toolbar(const std::string &inner) {
	return "<div class=\"toolbar\">" + inner + "</div>";
}

// ---- flash ---------------------------------------------------------------

std::string FlashText(const std::string &slug) {
	// A fixed table, never free text out of the query string: reflecting a
	// parameter into a page is a text-injection vector even when escaped, and
	// one missed escape would make it XSS.
	if (slug == "saved") {
		return "Saved.";
	}
	if (slug == "created") {
		return "Created.";
	}
	if (slug == "deleted") {
		return "Deleted.";
	}
	if (slug == "sent") {
		return "Message sent.";
	}
	if (slug == "posted") {
		return "Message posted.";
	}
	if (slug == "moved") {
		return "Message moved.";
	}
	if (slug == "zapped") {
		return "Room forgotten. It will not appear in your room list again until you visit it.";
	}
	if (slug == "unzapped") {
		return "Room restored.";
	}
	if (slug == "marked") {
		return "Marked as read.";
	}
	if (slug == "flagged") {
		return "Flag updated.";
	}
	if (slug == "nothing") {
		return "Nothing was selected, so nothing happened.";
	}
	if (slug == "revoked") {
		return "Session revoked.";
	}
	if (slug == "password") {
		return "Password changed. Every other session for this account has been signed out.";
	}
	if (slug == "paged") {
		return "Message delivered.";
	}
	if (slug == "keygen") {
		return "Key generated. Publish the DNS record below before signing with it.";
	}
	if (slug == "queued") {
		return "Queued for another delivery attempt.";
	}
	if (slug == "activated") {
		return "Script activated.";
	}
	if (slug == "distributed") {
		return "Spooler run. Anything new in a list room is on the outbound queue.";
	}
	if (slug == "approved") {
		return "Approved and posted. The spooler will distribute it on its next pass.";
	}
	if (slug == "rejected") {
		return "Rejected. Nothing was sent.";
	}
	if (slug == "fetched") {
		return "Polled. Anything new is in the target room.";
	}
	if (slug == "feed_ok") {
		return "Connected and authenticated successfully.";
	}
	if (slug == "feed_failed") {
		return "The feed could not be reached — see its status in the table.";
	}
	if (slug == "room_created") {
		return "Room created. You administer it: the settings below decide who else can see it, read it "
		       "and post to it.";
	}
	if (slug == "invited") {
		return "Invitation sent. That address joins the list once somebody reading it follows the link.";
	}
	if (slug == "confirm_sent") {
		// Deliberately says nothing about whether the list or the subscription
		// exists: this page is anonymous, and a more helpful message would let
		// anyone test whether a given address is on a given list.
		return "If that list exists, a confirmation has been e-mailed to the address you gave. "
		       "Nothing changes until you follow the link in it.";
	}
	return std::string();
}

// ---- page shell ----------------------------------------------------------

namespace {

// The critical stylesheet. Emitted *before* the stylesheet links, unlike the
// theme below, and the ordering is the whole design:
//
//   1. this block   — a fallback palette and the layout skeleton
//   2. pico.css, qc.css
//   3. ThemeCss()   — the per-user override
//
// What stays here is exactly what a page cannot be read without, so it renders
// legibly on the very first packet and stays legible if /static is unreachable
// behind a misconfigured proxy. It declares the same `--pico-*` names Pico
// does, at plain `:root` specificity, so Pico overrides every one of them the
// moment it arrives — putting this block *after* the links instead would
// silently defeat Pico's own palette. The theme has to stay after them because
// it is per-user and therefore not a cacheable asset.
const char *kCriticalCss = R"CSS(
:root { color-scheme: light dark;
  --pico-background-color:#fbfbfa; --pico-color:#1c1b19;
  --pico-muted-color:#6b6a66; --pico-muted-border-color:#e0dfdb;
  --pico-card-background-color:#ffffff; --pico-primary:#8a5a2b;
  --qc-sidebar:15.5rem; --qc-header:3.25rem; }
@media (prefers-color-scheme: dark) { :root {
  --pico-background-color:#17171a; --pico-color:#e9e8e4;
  --pico-muted-color:#9d9c97; --pico-muted-border-color:#2e2e33;
  --pico-card-background-color:#1f1f23; --pico-primary:#c98d54; } }
* { box-sizing:border-box; }
body { margin:0; background:var(--pico-background-color); color:var(--pico-color);
  font:16px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif; }
.navtoggle { position:absolute; opacity:0; pointer-events:none; }
.app { display:grid; grid-template-columns:var(--qc-sidebar) 1fr;
  grid-template-rows:auto 1fr; min-height:100dvh; }
header.top { grid-column:1 / -1; display:flex; align-items:center; gap:.75rem;
  height:var(--qc-header); padding:0 1rem;
  border-bottom:1px solid var(--pico-muted-border-color); }
main { padding:1.1rem 1rem 3rem; min-width:0; }
/* Signed out, SidebarFor() emits no <nav> at all, so without this the grid's
   first (15.5rem) column is the only free cell and main — login form included —
   collapses into that narrow gutter instead of the full page. */
body.anon .app { grid-template-columns:1fr; }
body.anon main { display:flex; align-items:center; justify-content:center; }
@media (max-width:52rem) { .app { grid-template-columns:1fr; } }
)CSS";

// ---- icons ---------------------------------------------------------------

// One <symbol> sprite, emitted once per page by Render(), so every `use` below
// it is same-document. That is deliberate: cross-document `use` is still
// unevenly supported, and an external sprite would have to satisfy the CSP as
// well. A stroke-only set inherits currentColor, so an icon is the right colour
// in six themes without a single per-theme rule.
//
// Geometry is a 24x24 box; qc.css sizes them in `em` so an icon tracks the text
// it sits beside.
const char *kIconSprite = R"SVG(<svg class="sprite" aria-hidden="true" hidden xmlns="http://www.w3.org/2000/svg"><defs>
<symbol id="i-menu" viewBox="0 0 24 24"><path d="M3 6h18M3 12h18M3 18h18"/></symbol>
<symbol id="i-search" viewBox="0 0 24 24"><circle cx="11" cy="11" r="7"/><path d="M20 20l-3.5-3.5"/></symbol>
<symbol id="i-inbox" viewBox="0 0 24 24"><path d="M3 13h5l1.5 3h5L16 13h5"/><path d="M5 5h14l2 8v5a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1v-5z"/></symbol>
<symbol id="i-mail" viewBox="0 0 24 24"><rect x="3" y="5" width="18" height="14" rx="2"/><path d="M3 7l9 6 9-6"/></symbol>
<symbol id="i-send" viewBox="0 0 24 24"><path d="M21 3L10 14"/><path d="M21 3l-7 18-4-7-7-4z"/></symbol>
<symbol id="i-edit" viewBox="0 0 24 24"><path d="M4 20h4L20 8l-4-4L4 16z"/></symbol>
<symbol id="i-file" viewBox="0 0 24 24"><path d="M14 3H7a1 1 0 0 0-1 1v16a1 1 0 0 0 1 1h10a1 1 0 0 0 1-1V7z"/><path d="M14 3v4h4"/></symbol>
<symbol id="i-trash" viewBox="0 0 24 24"><path d="M4 7h16M10 7V5h4v2M6 7l1 13h10l1-13"/></symbol>
<symbol id="i-folder" viewBox="0 0 24 24"><path d="M3 7a1 1 0 0 1 1-1h5l2 2h8a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1z"/></symbol>
<symbol id="i-archive" viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="4" rx="1"/><path d="M5 8v11a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1V8M10 12h4"/></symbol>
<symbol id="i-calendar" viewBox="0 0 24 24"><rect x="3" y="5" width="18" height="16" rx="2"/><path d="M3 10h18M8 3v4M16 3v4"/></symbol>
<symbol id="i-users" viewBox="0 0 24 24"><circle cx="9" cy="8" r="3"/><path d="M3 20a6 6 0 0 1 12 0M16 5.5a3 3 0 0 1 0 5M17 14c2.5.6 4 2.6 4 6"/></symbol>
<symbol id="i-user" viewBox="0 0 24 24"><circle cx="12" cy="8" r="3.5"/><path d="M5 20a7 7 0 0 1 14 0"/></symbol>
<symbol id="i-check" viewBox="0 0 24 24"><path d="M4 12l5 5L20 6"/></symbol>
<symbol id="i-tasks" viewBox="0 0 24 24"><path d="M4 7l2 2 3-3M4 17l2 2 3-3M12 8h8M12 18h8"/></symbol>
<symbol id="i-note" viewBox="0 0 24 24"><path d="M5 4h14a1 1 0 0 1 1 1v9l-6 6H5a1 1 0 0 1-1-1V5a1 1 0 0 1 1-1z"/><path d="M20 14h-6v6"/></symbol>
<symbol id="i-home" viewBox="0 0 24 24"><path d="M3 11l9-7 9 7"/><path d="M5 10v9a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-9"/></symbol>
<symbol id="i-globe" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a15 15 0 0 1 0 18a15 15 0 0 1 0-18"/></symbol>
<symbol id="i-book" viewBox="0 0 24 24"><path d="M4 5a2 2 0 0 1 2-2h13v16H6a2 2 0 0 0-2 2z"/><path d="M4 19a2 2 0 0 1 2-2h13"/></symbol>
<symbol id="i-settings" viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3M4.9 4.9l2.2 2.2M16.9 16.9l2.2 2.2M19.1 4.9l-2.2 2.2M7.1 16.9l-2.2 2.2"/></symbol>
<symbol id="i-filter" viewBox="0 0 24 24"><path d="M3 5h18l-7 8v6l-4 2v-8z"/></symbol>
<symbol id="i-shield" viewBox="0 0 24 24"><path d="M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6z"/></symbol>
<symbol id="i-monitor" viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="12" rx="1"/><path d="M8 20h8M12 16v4"/></symbol>
<symbol id="i-reply" viewBox="0 0 24 24"><path d="M9 7L4 12l5 5"/><path d="M4 12h9a7 7 0 0 1 7 7v1"/></symbol>
<symbol id="i-replyall" viewBox="0 0 24 24"><path d="M8 7l-5 5 5 5M13 7l-5 5 5 5"/><path d="M8 12h7a6 6 0 0 1 6 6v1"/></symbol>
<symbol id="i-forward" viewBox="0 0 24 24"><path d="M15 7l5 5-5 5"/><path d="M20 12h-9a7 7 0 0 0-7 7v1"/></symbol>
<symbol id="i-flag" viewBox="0 0 24 24"><path d="M5 21V4M5 4h12l-2 4 2 4H5"/></symbol>
<symbol id="i-star" viewBox="0 0 24 24"><path d="M12 3l2.8 5.7 6.2.9-4.5 4.4 1.1 6.2L12 17.3 6.4 20.2l1.1-6.2L3 9.6l6.2-.9z"/></symbol>
<symbol id="i-clip" viewBox="0 0 24 24"><path d="M20 11l-8.5 8.5a4.5 4.5 0 0 1-6.4-6.4L13 5a3 3 0 0 1 4.2 4.2l-8 8a1.5 1.5 0 0 1-2.1-2.1l7.5-7.5"/></symbol>
<symbol id="i-plus" viewBox="0 0 24 24"><path d="M12 5v14M5 12h14"/></symbol>
<symbol id="i-x" viewBox="0 0 24 24"><path d="M6 6l12 12M18 6L6 18"/></symbol>
<symbol id="i-left" viewBox="0 0 24 24"><path d="M15 5l-7 7 7 7"/></symbol>
<symbol id="i-right" viewBox="0 0 24 24"><path d="M9 5l7 7-7 7"/></symbol>
<symbol id="i-signout" viewBox="0 0 24 24"><path d="M14 5H6a1 1 0 0 0-1 1v12a1 1 0 0 0 1 1h8"/><path d="M17 8l4 4-4 4M21 12H10"/></symbol>
<symbol id="i-rss" viewBox="0 0 24 24"><path d="M5 19a1 1 0 1 0 0-2 1 1 0 0 0 0 2M5 12a7 7 0 0 1 7 7M5 5a14 14 0 0 1 14 14"/></symbol>
</defs></svg>)SVG";

// The sidebar. Grouped, because a flat list stopped scaling somewhere around
// the fifteenth page and this module now has roughly thirty.
//
// `active` marks the current item with aria-current, which is both the
// accessible answer and the hook the stylesheet colours.
std::string SidebarFor(const Ctx &ctx, const std::string &active) {
	if (!ctx.Authed()) {
		return std::string();
	}
	std::string out = "<nav class=\"sidebar\" aria-label=\"Sections\">";

	// `label_key` is a Tr() catalog key, not literal text — every static nav
	// label in this sidebar goes through the message catalog.
	auto item = [&](const char *href, const char *label_key, const char *key, const char *icon,
	                int64_t count = 0) {
		std::string extra = (active == key) ? " aria-current=\"page\"" : "";
		out += "<a href=\"" + A(href) + "\"" + extra + ">" + Icon(icon) + "<span>" +
		       T(Tr(ctx, label_key)) + "</span>";
		// The same bubble an unread room gets, so a page waiting reads the same
		// as a room with something new in it.
		if (count > 0) {
			out += "<span class=\"count\">" + std::to_string(count) + "</span>";
		}
		out += "</a>";
	};
	auto group = [&](const char *label_key) {
		out += "<div class=\"group\"><span class=\"label\">" + T(Tr(ctx, label_key)) + "</span>";
	};
	auto endgroup = [&]() { out += "</div>"; };

	// A room's own link, with its unread count riding along on the right — the
	// `.count` span the stylesheet has always described and nothing emitted.
	// Marked current by room number, so the folder you are reading is the one
	// highlighted rather than the section it belongs to.
	std::string room_key;
	auto room_link = [&](const Room &room, const std::string &label, int64_t unread,
	                     const char *icon) {
		std::string href = "/bbs/room/" + std::to_string(room.room_num);
		std::string key = "room:" + std::to_string(room.room_num);
		std::string extra = (active == key) ? " aria-current=\"page\"" : "";
		if (!extra.empty()) {
			room_key = key;
		}
		out += "<a href=\"" + A(href) + "\"" + extra + ">" + Icon(icon) + "<span>" + T(label) +
		       "</span>";
		if (unread > 0) {
			out += "<span class=\"count\">" + std::to_string(unread) + "</span>";
		}
		out += "</a>";
	};

	// One listing for the whole sidebar. This replaces the four FindUserRoom
	// lookups the groupware group used to cost — those rooms are personal rooms
	// and are already in here — so the counts below arrive for fewer queries
	// than the sidebar ran before them, not more.
	auto rooms = quackmail::citadel::ListRooms(ctx.con, ctx.username, -1, "all");

	static const char *kGroupwareRooms[] = {"Calendar", "Contacts", "Tasks", "Notes"};

	// One definition of "a mail folder", shared with the move targets and the
	// listing itself — see MailFoldersFrom.
	std::vector<Room> folders = MailFoldersFrom(rooms);

	group("nav.mail");
	item("/mail/compose", "nav.compose", "compose", "edit");
	{
		std::vector<int64_t> nums;
		for (auto &f : folders) {
			nums.push_back(f.room_num);
		}
		// \Seen, not the last-read pointer: this has to be the same count the
		// folder's own listing shows in bold, and a high-water mark cannot skip
		// a message somebody left unread behind one they opened.
		auto unseen = UnseenCounts(ctx, nums);
		for (size_t i = 0; i < folders.size(); i++) {
			// "Mail" is what the store calls it and "Inbox" is what a person
			// does. The other folders are already named the way they read — and
			// are room names, not UI copy, so they do not go through Tr().
			const std::string &name = folders[i].display_name;
			std::string label = name == "Mail" ? Tr(ctx, "nav.inbox") : name;
			const char *icon = name == "Mail"        ? "inbox"
			                   : name == "Sent Items" ? "send"
			                   : name == "Drafts"     ? "file"
			                   : name == "Trash"      ? "trash"
			                                          : "folder";
			room_link(folders[i], label, unseen[i], icon);
		}
	}
	item("/mail/", "nav.all_folders", "mail", "mail");
	endgroup();

	// The user's own groupware rooms, linked by number because that is how rooms
	// are addressed. EnsureUserRooms provisions these at first login; one that
	// somehow does not exist is simply omitted rather than linked to a 404.
	{
		std::string groupware;
		static const char *kKeys[] = {"calendar", "contacts", "tasks", "notes"};
		static const char *kIcons[] = {"calendar", "users", "tasks", "note"};
		for (size_t g = 0; g < 4; g++) {
			for (auto &r : rooms) {
				if (r.mailbox_owner == 0 || r.display_name != kGroupwareRooms[g]) {
					continue;
				}
				std::string href = "/bbs/room/" + std::to_string(r.room_num);
				std::string key = "room:" + std::to_string(r.room_num);
				bool current = active == kKeys[g] || active == key;
				if (current) {
					room_key = key;
				}
				groupware += "<a href=\"" + A(href) + "\"" +
				             (current ? " aria-current=\"page\"" : "") + ">" + Icon(kIcons[g]) +
				             "<span>" + T(r.display_name) + "</span></a>";
				break;
			}
		}
		if (!groupware.empty()) {
			group("nav.groupware");
			out += groupware;
			endgroup();
		}
	}

	group("nav.rooms");
	{
		// Rooms with something new in them, most unread first. Capped, because
		// this is rendered on every page: `qm_web_sidebar_rooms` is the ceiling
		// and 0 turns the listing — and the query behind it — off entirely.
		int64_t limit = (int64_t)std::strtoll(ConfigStr(ctx.con, "qm_web_sidebar_rooms", "10").c_str(),
		                                      nullptr, 10);
		std::vector<std::pair<Room, int64_t>> unread;
		if (limit > 0) {
			std::vector<Room> public_rooms;
			std::vector<int64_t> nums;
			for (auto &r : rooms) {
				if (r.mailbox_owner == 0) {
					public_rooms.push_back(r);
					nums.push_back(r.room_num);
				}
			}
			auto stats = quackmail::citadel::RoomStatsBulk(ctx.con, ctx.username, nums);
			for (size_t i = 0; i < public_rooms.size(); i++) {
				if (stats[i].new_count > 0) {
					unread.push_back({public_rooms[i], stats[i].new_count});
				}
			}
			std::sort(unread.begin(), unread.end(),
			          [](const std::pair<Room, int64_t> &a, const std::pair<Room, int64_t> &b) {
				          return a.second > b.second;
			          });
			if ((int64_t)unread.size() > limit) {
				unread.resize((size_t)limit);
			}
		}
		// "All rooms" also stands in for a room that is not itself listed, so a
		// room page never leaves the whole sidebar unmarked.
		bool in_a_room = active.rfind("room:", 0) == 0;
		bool listed = false;
		for (auto &u : unread) {
			listed = listed || active == "room:" + std::to_string(u.first.room_num);
		}
		std::string extra =
		    (active == "bbs" || (in_a_room && !listed && room_key.empty())) ? " aria-current=\"page\"" : "";
		out += "<a href=\"/bbs/\"" + extra + ">" + Icon("globe") + "<span>" +
		       T(Tr(ctx, "nav.all_rooms")) + "</span></a>";
		for (auto &u : unread) {
			room_link(u.first, u.first.display_name, u.second, "home");
		}
	}
	item("/search", "nav.search", "search", "search");
	if (MayCreateRooms(ctx)) {
		item("/bbs/new", "nav.create_room", "newroom", "plus");
	}
	item("/bbs/who", "nav.who_online", "who", "user");
	// One indexed count(*) per authed render, beside the room-unread query that
	// already runs here.
	item("/chat", "chat.title", "chat", "user",
	     quackmail::citadel::PendingExpressCount(ctx.con, ctx.username));
	endgroup();

	group("nav.you");
	item("/prefs", "nav.preferences", "prefs", "settings");
	item("/prefs/sieve", "nav.filters", "sieve", "filter");
	item("/prefs/sessions", "nav.sessions", "sessions", "monitor");
	endgroup();

	// Same gate as the router applies to every /admin route, so the link never
	// points at a 403.
	if (ctx.IsAide() && ConfigBool(ctx.con, "qm_web_admin_enabled", false)) {
		group("nav.system");
		item("/admin/", "nav.admin", "admin", "shield");
		endgroup();
	}

	out += "</nav>";
	return out;
}

} // namespace

PageOpts::PageOpts() {
}

void SecurityHeaders(Ctx &ctx, const std::string &csp) {
	// default-src 'none' plus a per-response nonce, rather than 'unsafe-inline'.
	// The nonce is three lines of work and it is the difference between a
	// reflected-XSS bug being inert and being execution.
	std::string policy = csp;
	if (policy.empty()) {
		// 'self' covers /static; the nonce covers the critical CSS and the theme
		// override that have to arrive inline. In CSP3 the two coexist — a nonce
		// does not suppress host sources, only 'strict-dynamic' does — so this
		// is additive rather than a weakening.
		//
		// connect-src is what htmx's XHR needs, and it needs to be stated: with
		// default-src 'none' and no connect-src, the fetch falls back to 'none'
		// and every pane swap is blocked. 'self' and nothing else — this page
		// has no reason to talk to another origin, and saying so is what keeps
		// an injected script from exfiltrating to one.
		//
		// Note that the CSP for a *message* body (web_mail.cpp's HTML-part
		// route) is passed in explicitly and must never gain 'self': that frame
		// renders markup written by whoever sent the mail.
		policy = "default-src 'none'; script-src 'self' 'nonce-" + ctx.nonce + "'; style-src 'self' 'nonce-" +
		         ctx.nonce +
		         "'; img-src 'self' data:; connect-src 'self'; frame-src 'self'; form-action 'self'; "
		         "frame-ancestors 'none'; base-uri 'none'";
	}
	ctx.resp.SetHeader("Content-Security-Policy", policy);
	ctx.resp.SetHeader("X-Content-Type-Options", "nosniff");
	ctx.resp.SetHeader("X-Frame-Options", "DENY");
	ctx.resp.SetHeader("Referrer-Policy", "no-referrer");
	// Mail must not survive in a shared or kiosk browser's disk cache.
	ctx.resp.SetHeader("Cache-Control", "private, no-store");
	if (ctx.tls && ConfigBool(ctx.con, "qm_web_hsts", false)) {
		ctx.resp.SetHeader("Strict-Transport-Security", "max-age=31536000");
	}
}

// Named themes. Pico is a ~500-variable custom-property sheet and everything in
// qc.css reads those variables, so a theme is a `:root` override plus the
// `data-theme` attribute Pico keys its own light and dark blocks on — no second
// stylesheet, no per-theme rules to keep in sync.
//
// "auto" sets no attribute at all, which is what leaves Pico following the OS.
// Light and dark are therefore native and cost nothing but the attribute; only
// the three custom skins carry any CSS.
struct Theme {
	const char *name;
	const char *label;
	// "" | "light" | "dark" — the value of the <html data-theme> attribute,
	// which for a custom skin selects the base Pico palette it overrides.
	const char *base;
	const char *css; // "" = Pico unmodified
};

const Theme kThemes[] = {
    {"auto", "Follow my system", "", ""},
    {"light", "Always light", "light", ""},
    {"dark", "Always dark", "dark", ""},
    {"sepia", "Sepia", "light",
     ":root{--pico-background-color:#f4ecd8;--pico-color:#3b2f2a;--pico-muted-color:#7a6a5d;"
     "--pico-muted-border-color:#ddd0b5;--pico-card-background-color:#fbf5e6;"
     "--pico-card-sectioning-background-color:#efe5cc;--pico-primary:#8a5a2b;"
     "--pico-primary-background:#8a5a2b;--pico-primary-hover-background:#734a22;"
     "--pico-primary-inverse:#fbf5e6;--pico-primary-border:#8a5a2b;"
     "--pico-primary-hover:#734a22;--pico-secondary-hover-background:#e7dbc0;"
     "--pico-form-element-background-color:#fbf5e6;--pico-form-element-border-color:#ddd0b5;"
     "--pico-code-background-color:#efe5cc;--pico-del-color:#9c3a12;--pico-ins-color:#4a6b34}"},
    {"slate", "Slate", "dark",
     ":root{--pico-background-color:#1b2027;--pico-color:#dfe4ea;--pico-muted-color:#93a1b0;"
     "--pico-muted-border-color:#2c3440;--pico-card-background-color:#222933;"
     "--pico-card-sectioning-background-color:#262e39;--pico-primary:#6fa8d6;"
     "--pico-primary-background:#6fa8d6;--pico-primary-hover-background:#8bbbe2;"
     "--pico-primary-inverse:#131820;--pico-primary-border:#6fa8d6;"
     "--pico-primary-hover:#8bbbe2;--pico-secondary-hover-background:#2c3440;"
     "--pico-form-element-background-color:#1f252d;--pico-form-element-border-color:#2c3440;"
     "--pico-code-background-color:#262e39;--pico-del-color:#e08a6a;--pico-ins-color:#7fc8a0}"},
    {"amber", "Amber on black", "dark",
     ":root{--pico-background-color:#0b0b0b;--pico-color:#ffb642;--pico-muted-color:#a8752a;"
     "--pico-muted-border-color:#3a2a10;--pico-card-background-color:#121008;"
     "--pico-card-sectioning-background-color:#171308;--pico-primary:#ffd08a;"
     "--pico-primary-background:#ffd08a;--pico-primary-hover-background:#ffdfae;"
     "--pico-primary-inverse:#0b0b0b;--pico-primary-border:#ffd08a;"
     "--pico-primary-hover:#ffdfae;--pico-secondary-hover-background:#1d1708;"
     "--pico-form-element-background-color:#121008;--pico-form-element-border-color:#3a2a10;"
     "--pico-code-background-color:#171308;--pico-del-color:#ff7043;--pico-ins-color:#b8d94a}"},
};

// The theme in force: the signed-in user's choice, else the site default.
// Never null — an unrecognized name falls through to "auto".
const Theme &ResolveTheme(Ctx &ctx) {
	std::string want;
	if (ctx.Authed()) {
		want = quackmail::citadel::GetUserPref(ctx.con, ctx.username, "web_theme");
	}
	if (want.empty() || want == "auto") {
		want = ConfigStr(ctx.con, "qm_web_theme", "auto");
	}
	for (auto &t : kThemes) {
		if (want == t.name) {
			return t;
		}
	}
	return kThemes[0];
}

std::vector<std::pair<std::string, std::string>> ThemeOptions() {
	std::vector<std::pair<std::string, std::string>> out;
	for (auto &t : kThemes) {
		out.push_back({t.name, t.label});
	}
	return out;
}

void Render(Ctx &ctx, const std::string &title, const std::string &body, int status) {
	Render(ctx, title, body, PageOpts(), status);
}

void Render(Ctx &ctx, const std::string &title, const std::string &body, const PageOpts &opts,
            int status) {
	SecurityHeaders(ctx);

	std::string node = ConfigStr(ctx.con, "c_humannode", "QuackCit");
	const Theme &theme = ResolveTheme(ctx);

	std::string page = "<!doctype html><html lang=\"" + A(EffectiveLocale(ctx)) + "\"";
	// Pico keys its own light and dark blocks on this attribute, so pinning a
	// theme costs the attribute and nothing else. "auto" deliberately emits no
	// attribute at all: that is the state Pico's prefers-color-scheme block is
	// written for.
	if (theme.base[0] != '\0') {
		page += " data-theme=\"" + A(theme.base) + "\"";
	}
	// The keyboard overlay is built in the browser, so its labels have to reach
	// it as data rather than as markup — this is what keeps them in the message
	// catalog with everything else instead of hard-coded in qc.js. "keys|text",
	// entries separated by "~", because it has to survive an HTML attribute.
	if (ctx.Authed()) {
		std::string keys;
		auto row = [&](const char *combo, const char *key) {
			if (!keys.empty()) {
				keys += "~";
			}
			keys += std::string(combo) + "|" + Tr(ctx, key);
		};
		row("j k", "keys.move");
		row("Enter", "keys.open");
		row("u", "keys.back");
		row("x", "keys.select");
		row("c", "keys.compose");
		row("r a f", "keys.replies");
		row("#", "keys.trash");
		row("s", "keys.flag");
		row("/", "keys.search");
		row("g i", "keys.goto");
		row("?", "keys.help");
		page += " data-keys=\"" + A(keys) + "\"";
		page += " data-keys-title=\"" + A(Tr(ctx, "keys.title")) + "\"";
		page += " data-keys-close=\"" + A(Tr(ctx, "keys.close")) + "\"";
	}
	page += "><head><meta charset=\"utf-8\">";
	page += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
	page += "<title>" + T(title.empty() ? node : title + " — " + node) + "</title>";
	// Three blocks, in this order, and the order is the design — see kCriticalCss.
	// The skeleton must precede the sheets so Pico can override its fallback
	// palette; the theme must follow them so it wins. Both inline blocks are
	// nonced, because style-src has no 'unsafe-inline' and an unnonced <style>
	// would be dropped in silence.
	page += "<style nonce=\"" + A(ctx.nonce) + "\">" + RawHtml(kCriticalCss) + "</style>";
	page += "<link rel=\"stylesheet\" href=\"" + A(AssetUrl("pico.css")) + "\">";
	page += "<link rel=\"stylesheet\" href=\"" + A(AssetUrl("qc.css")) + "\">";
	if (theme.css[0] != '\0') {
		page += "<style nonce=\"" + A(ctx.nonce) + "\">" + RawHtml(theme.css) + "</style>";
	}
	// Last, so a page's own rules win over the theme's. This is where a handler
	// puts CSS it can only know at render time; see PageOpts::style.
	if (!opts.style.empty()) {
		page += "<style nonce=\"" + A(ctx.nonce) + "\">" + RawHtml(opts.style) + "</style>";
	}
	page += "<script nonce=\"" + A(ctx.nonce) + "\" src=\"" + A(AssetUrl("htmx.min.js")) +
	        "\" defer></script>";
	page += "<script nonce=\"" + A(ctx.nonce) + "\" src=\"" + A(AssetUrl("qc.js")) + "\" defer></script>";
	if (!opts.script.empty()) {
		page += "<script nonce=\"" + A(ctx.nonce) + "\" src=\"" + A(AssetUrl(opts.script.c_str())) +
		        "\" defer></script>";
	}
	page += "</head>";

	std::string body_class;
	if (!ctx.Authed()) {
		body_class += " anon";
	}
	if (opts.wide) {
		body_class += " wide";
	}
	// The two-pane mail layout is a fixed-height shell rather than a flowing
	// document, so it has to be opted into: applying it everywhere would break
	// the calendar and the wiki, which want the page to grow.
	if (opts.panes) {
		body_class += " panes";
	}
	if (opts.view >= 0) {
		body_class += " view-" + std::to_string(opts.view);
	}
	// Density is a mail-list thing, not a general page thing — scoping it to
	// mail rooms (rather than a page-wide setting) keeps it from touching
	// Notes/Tasks/Calendar/Wiki, which have their own layouts already.
	if (ctx.Authed() &&
	    (opts.view == quackmail::citadel::VIEW_MAILBOX || opts.view == quackmail::citadel::VIEW_DRAFTS)) {
		std::string layout = quackmail::citadel::GetUserPref(ctx.con, ctx.username, "web_mail_layout");
		if (layout == "compact" || layout == "wide") {
			body_class += " layout-" + layout;
		}
	}
	page += "<body" + (body_class.empty() ? "" : " class=\"" + A(body_class.substr(1)) + "\"") + ">";
	// The sprite first, so every `use` in the page below resolves against a
	// symbol that is already in the document.
	page += RawHtml(kIconSprite);
	page += "<a class=\"skip\" href=\"#main\">" + T(Tr(ctx, "nav.skip")) + "</a>";
	// The checkbox precedes .app so the stylesheet can reach the sidebar with a
	// sibling selector: the mobile menu needs no script at all.
	page += "<input type=\"checkbox\" id=\"navtoggle\" class=\"navtoggle\">";
	page += "<div class=\"app\">";

	page += "<header class=\"top\">";
	if (ctx.Authed()) {
		page += "<label for=\"navtoggle\" class=\"navbtn\" title=\"" + A(Tr(ctx, "nav.sections")) +
		        "\">" + Icon("menu") + "<span class=\"vh\">" + T(Tr(ctx, "nav.sections")) + "</span></label>";
	}
	page += "<span class=\"brand\">" + T(node) + "</span>";
	if (ctx.Authed()) {
		// A GET form, so it carries no CSRF token and needs none — and a search
		// stays linkable and bookmarkable, which a POST would take away. It is
		// the one control that belongs on every page rather than in the sidebar:
		// finding a message is not a section of the site.
		page += "<form method=\"get\" action=\"/search\" class=\"topsearch\" role=\"search\">";
		page += "<label class=\"vh\" for=\"topq\">" + T(Tr(ctx, "nav.search_messages")) + "</label>";
		page += "<input id=\"topq\" type=\"search\" name=\"q\" value=\"" +
		        A(ctx.req.path == "/search" ? ctx.req.Param("q") : std::string()) +
		        "\" placeholder=\"" + A(Tr(ctx, "nav.search")) + "\">";
		page += "</form>";
		page += "<span class=\"who\">" + T(ctx.username);
		page += FormStart(ctx, "/logout", "inline") +
		        IconButton(Tr(ctx, "nav.signout"), "signout", "sec") + FormEnd();
		page += "</span>";
	}
	page += "</header>";

	page += SidebarFor(ctx, opts.active);
	page += "<main id=\"main\"><div class=\"inner\">";

	// A plaintext session is a real risk worth naming on the page itself, not
	// only in the docs.
	if (!ctx.tls && ctx.Authed()) {
		page += "<div class=\"warnbar\">" + Icon("shield") + T(Tr(ctx, "warn.plaintext")) + "</div>";
	}
	std::string flash = FlashText(ctx.req.Param("ok"));
	if (!flash.empty()) {
		page += "<div class=\"flash\">" + T(flash) + "</div>";
	}
	std::string problem = FlashText(ctx.req.Param("err"));
	if (!problem.empty()) {
		page += "<div class=\"flash err\">" + T(problem) + "</div>";
	}
	// A panes page owns its own heading: the title belongs inside the scrolling
	// list pane, not above the fixed-height shell, so emitting it here as well
	// would put the folder name on the page twice.
	if (!title.empty() && !opts.panes) {
		page += "<h1>" + T(title) + "</h1>";
	}
	if (!opts.toolbar.empty()) {
		page += RawHtml(opts.toolbar);
	}
	page += RawHtml(body);
	page += "</div></main></div></body></html>";

	ctx.resp.Html(page, status);
}

void RedirectTo(Ctx &ctx, const std::string &path, const std::string &flash) {
	SecurityHeaders(ctx);
	std::string target = path;
	if (!flash.empty()) {
		target += (target.find('?') == std::string::npos ? "?ok=" : "&ok=") + http::PercentEncode(flash);
	}
	// Relative Location values are legal (RFC 7231) and mean the Host header
	// never has to be trusted, which deletes the open-redirect class here.
	ctx.resp.Redirect(target, 303);
}

void ErrorPage(Ctx &ctx, int status, const std::string &title, const std::string &detail) {
	std::string body = "<p>" + T(detail) + "</p>";
	if (!ctx.Authed()) {
		body += "<p>" + Link("/login", "Sign in") + "</p>";
	}
	Render(ctx, title, body, status);
}

void NotFound(Ctx &ctx) {
	ErrorPage(ctx, 404, "Not found", "There is nothing at this address.");
}

void Forbidden(Ctx &ctx, const std::string &why) {
	ErrorPage(ctx, 403, "Not permitted", why);
}

void BadRequest(Ctx &ctx, const std::string &why) {
	ErrorPage(ctx, 400, "Bad request", why);
}

// ---- shared helpers ------------------------------------------------------

bool ResolveRoomFor(Ctx &ctx, const std::string &name, Room &out) {
	if (name.empty() || !ctx.Authed()) {
		return false;
	}
	return quackmail::citadel::ResolveRoom(ctx.con, ctx.username, name, out);
}

bool ResolveRoomNumFor(Ctx &ctx, int64_t room_num, Room &out) {
	if (!ctx.Authed() || room_num < 0) {
		return false;
	}
	if (!quackmail::citadel::GetRoomByNum(ctx.con, room_num, out)) {
		return false;
	}
	// GetRoomByNum applies no visibility rules at all, so they are applied here
	// — the same ones ListRooms uses, or a user could read anyone's mailbox by
	// guessing a room number.
	int64_t usernum = quackmail::citadel::GetOrAssignUserNum(ctx.con, ctx.username);
	if (out.mailbox_owner != 0 && out.mailbox_owner != usernum) {
		return false; // someone else's personal room; not even an aide browses those
	}
	if (!ctx.IsAide() && (out.qr_flags & quackmail::citadel::QR_PRIVATE) && out.mailbox_owner != usernum) {
		// An invitation-only room is reachable by whoever the access list names,
		// which is the same rule ListRooms applies — `l` is RFC 4314's lookup
		// right. Without this a private room would be invisible even to the
		// person who created it and holds every right on it.
		return quackmail::citadel::EffectiveRights(ctx.con, ctx.username, out).find('l') !=
		       std::string::npos;
	}
	return true;
}

bool RequireUnlocked(Ctx &ctx, const Room &room, const std::string &back_path) {
	if (quackmail::citadel::RoomUnlocked(ctx.con, ctx.username, room)) {
		return true;
	}
	std::string body = "<p class=\"muted\">This room is password protected.</p>";
	body += FormStart(ctx, "/bbs/room/" + std::to_string(room.room_num) + "/unlock");
	body += Hidden("next", back_path);
	body += "<label class=\"field\"><span>Room password</span>" + TextInput("password", "", "password") +
	        "</label>";
	body += "<p>" + Button("Enter room") + "</p>";
	body += FormEnd();
	Render(ctx, room.display_name, body, 403);
	return false;
}

bool LoadMessageIn(Ctx &ctx, const Room &room, int64_t msgnum, Message &out) {
	// The membership check is the access control. citadel::LoadMessage takes a
	// bare message number and knows nothing about who may read it, so a handler
	// that skips this reads anyone's mail.
	if (msgnum <= 0 || !quackmail::citadel::MessageInRoom(ctx.con, room.room_num, msgnum)) {
		return false;
	}
	return quackmail::citadel::LoadMessage(ctx.con, msgnum, out);
}

std::string EffectiveTz(Ctx &ctx) {
	std::string want;
	if (ctx.Authed()) {
		want = quackmail::citadel::GetUserPref(ctx.con, ctx.username, "web_tz");
	}
	if (want.empty()) {
		want = ConfigStr(ctx.con, "qm_default_tz", "UTC");
	}
	// A pref naming a zone the bundled database has never heard of — a stale
	// name, a typo set by hand — falls back rather than rendering nothing.
	if (want.empty() || !quackmail::tz::IsKnown(want)) {
		return "UTC";
	}
	return want;
}

std::string EffectiveDateFormat(Ctx &ctx) {
	std::string want;
	if (ctx.Authed()) {
		want = quackmail::citadel::GetUserPref(ctx.con, ctx.username, "web_date_format");
	}
	if (want.empty()) {
		want = ConfigStr(ctx.con, "qm_default_date_format", "iso");
	}
	if (want != "us" && want != "eu") {
		return "iso";
	}
	return want;
}

std::string FormatTimeIn(int64_t epoch_seconds, const std::string &tzid, const std::string &date_format) {
	if (epoch_seconds <= 0) {
		return "—";
	}
	// Break the wall clock down here rather than through localtime_r, which
	// would answer in the server's zone regardless of what was asked for.
	int64_t wall = tzid.empty() ? epoch_seconds : quackmail::tz::FromUtc(tzid, epoch_seconds);
	int64_t days = wall / 86400;
	int64_t rem = wall % 86400;
	if (rem < 0) {
		rem += 86400;
		days -= 1;
	}
	// Days-since-epoch to a civil date, via gmtime_r on a value already shifted
	// into the target zone — so the "UTC" it reports is the local wall clock.
	std::time_t t = (std::time_t)(days * 86400);
	struct tm tm {};
	gmtime_r(&t, &tm);
	int year = tm.tm_year + 1900, mon = tm.tm_mon + 1, day = tm.tm_mday;
	int hour = (int)(rem / 3600), minute = (int)((rem % 3600) / 60);
	char buf[48];
	if (date_format == "us") {
		std::snprintf(buf, sizeof buf, "%02d/%02d/%04d %02d:%02d", mon, day, year, hour, minute);
	} else if (date_format == "eu") {
		std::snprintf(buf, sizeof buf, "%02d/%02d/%04d %02d:%02d", day, mon, year, hour, minute);
	} else {
		std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", year, mon, day, hour, minute);
	}
	return buf;
}

std::string FormatTime(Ctx &ctx, int64_t epoch_seconds) {
	return FormatTimeIn(epoch_seconds, EffectiveTz(ctx), EffectiveDateFormat(ctx));
}

std::string FormatBytes(int64_t bytes) {
	char buf[32];
	if (bytes < 1024) {
		std::snprintf(buf, sizeof buf, "%lld B", (long long)bytes);
	} else if (bytes < 1024 * 1024) {
		std::snprintf(buf, sizeof buf, "%.1f KB", bytes / 1024.0);
	} else {
		std::snprintf(buf, sizeof buf, "%.1f MB", bytes / (1024.0 * 1024.0));
	}
	return buf;
}

std::string DecodeHeader(const std::string &raw) {
	// Decode first. Escaping happens later, at interpolation: doing it the
	// other way round lets an encoded-word decode back into a live tag after
	// the escaping has already run.
	return quackmail::mime::DecodeEncodedWords(raw);
}

} // namespace qmweb
} // namespace duckdb
