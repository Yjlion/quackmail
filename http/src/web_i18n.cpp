#include "web_i18n.hpp"

#include <cstdlib>
#include <cctype>
#include <unordered_map>

namespace duckdb {
namespace qmweb {

namespace {

// One row per string, one column per language. Adding a language is a column
// here plus a row in kLocales — no call site changes, which is the whole point
// of routing every user-facing string through Tr().
//
// An empty cell is not a bug: it means "not translated yet", and Tr() serves the
// English string for it rather than the key. That is what lets a language land
// incrementally instead of all at once.
struct Msg {
	const char *key;
	const char *en;
	const char *de;
	const char *fr;
};

struct Locale {
	const char *code;
	const char *label; // the endonym
	size_t column;     // which of Msg's language fields
};

const Locale kLocales[] = {
    {"en", "English", 0},
    {"de", "Deutsch", 1},
    {"fr", "Français", 2},
};

const char *Column(const Msg &m, size_t col) {
	switch (col) {
	case 1:
		return m.de;
	case 2:
		return m.fr;
	default:
		return m.en;
	}
}

// Grouped by the surface each string belongs to. Keys are `page.thing`.
const Msg kMessages[] = {
    // ---- login ------------------------------------------------------------
    {"login.title_prefix", "Sign in to ", "Anmelden bei ", "Connexion à "},
    {"login.username", "User name", "Benutzername", "Nom d’utilisateur"},
    {"login.password", "Password", "Passwort", "Mot de passe"},
    {"login.signin", "Sign in", "Anmelden", "Se connecter"},

    // ---- chrome and navigation ---------------------------------------------
    {"nav.skip", "Skip to content", "Zum Inhalt springen", "Aller au contenu"},
    {"nav.sections", "Sections", "Bereiche", "Sections"},
    {"nav.signout", "Sign out", "Abmelden", "Se déconnecter"},
    {"nav.search", "Search", "Suchen", "Rechercher"},
    {"nav.search_messages", "Search messages", "Nachrichten durchsuchen", "Rechercher des messages"},
    {"nav.mail", "Mail", "E-Mail", "Courrier"},
    {"nav.compose", "Compose", "Verfassen", "Écrire"},
    {"nav.inbox", "Inbox", "Posteingang", "Boîte de réception"},
    {"nav.all_folders", "All folders", "Alle Ordner", "Tous les dossiers"},
    {"nav.groupware", "Groupware", "Groupware", "Travail collaboratif"},
    {"nav.rooms", "Rooms", "Räume", "Salons"},
    {"nav.all_rooms", "All rooms", "Alle Räume", "Tous les salons"},
    {"nav.create_room", "Create a room", "Raum erstellen", "Créer un salon"},
    {"nav.who_online", "Who is online", "Wer ist online", "Qui est en ligne"},
    {"nav.you", "You", "Sie", "Vous"},
    {"nav.preferences", "Preferences", "Einstellungen", "Préférences"},
    {"nav.filters", "Filters", "Filter", "Filtres"},
    {"nav.sessions", "Signed-in browsers", "Angemeldete Browser", "Navigateurs connectés"},
    {"nav.system", "System", "System", "Système"},
    {"nav.admin", "Admin console", "Administration", "Console d’administration"},

    {"warn.plaintext", "This connection is not encrypted. Your password and mail are visible to "
                       "anything on the network path.",
     "Diese Verbindung ist nicht verschlüsselt. Ihr Passwort und Ihre E-Mails sind für alles auf "
     "dem Netzwerkpfad sichtbar.",
     "Cette connexion n’est pas chiffrée. Votre mot de passe et vos messages sont visibles par "
     "tout ce qui se trouve sur le trajet réseau."},

    // ---- mail overview ------------------------------------------------------
    {"mail.title", "Mail", "E-Mail", "Courrier"},
    {"mail.write", "Write a message", "Nachricht schreiben", "Écrire un message"},
    {"mail.folder", "Folder", "Ordner", "Dossier"},
    {"mail.unread", "Unread", "Ungelesen", "Non lus"},
    {"mail.total", "Total", "Gesamt", "Total"},
    {"mail.footer", "These are ordinary Citadel rooms — the same messages are visible over IMAP, "
                    "POP3 and the BBS.",
     "Dies sind gewöhnliche Citadel-Räume — dieselben Nachrichten sind über IMAP, POP3 und die "
     "Mailbox sichtbar.",
     "Ce sont des salons Citadel ordinaires — les mêmes messages sont visibles via IMAP, POP3 et "
     "le BBS."},

    // ---- a mail folder ------------------------------------------------------
    {"mailbox.search_folder", "Search this folder", "Diesen Ordner durchsuchen",
     "Rechercher dans ce dossier"},
    {"mailbox.all", "All", "Alle", "Tous"},
    {"mailbox.mark_all_read", "Mark all read", "Alle als gelesen markieren", "Tout marquer comme lu"},
    {"mailbox.empty", "This folder is empty.", "Dieser Ordner ist leer.", "Ce dossier est vide."},
    {"mailbox.subject", "Subject", "Betreff", "Objet"},
    {"mailbox.from", "From", "Von", "De"},
    {"mailbox.date", "Date", "Datum", "Date"},
    {"mailbox.size", "Size", "Größe", "Taille"},
    {"mailbox.no_subject", "(no subject)", "(kein Betreff)", "(sans objet)"},
    {"mailbox.gone", "That message is no longer in this folder.",
     "Diese Nachricht ist nicht mehr in diesem Ordner.",
     "Ce message ne se trouve plus dans ce dossier."},
    {"mailbox.select_one", "Select this message", "Diese Nachricht auswählen",
     "Sélectionner ce message"},
    {"mailbox.select_all", "Select every message on this page",
     "Alle Nachrichten auf dieser Seite auswählen",
     "Sélectionner tous les messages de cette page"},
    {"mailbox.with_selected", "With the selected:", "Mit der Auswahl:", "Avec la sélection :"},
    {"mailbox.move", "Move", "Verschieben", "Déplacer"},
    {"mailbox.mark_read", "Mark read", "Als gelesen markieren", "Marquer comme lu"},
    {"mailbox.mark_unread", "Mark unread", "Als ungelesen markieren", "Marquer comme non lu"},
    {"mailbox.flag", "Flag", "Markieren", "Marquer"},
    {"mailbox.clear_flag", "Clear flag", "Markierung entfernen", "Retirer la marque"},
    {"mailbox.to_trash", "Move to Trash", "In den Papierkorb", "Mettre à la corbeille"},
    {"mailbox.delete_forever", "Delete permanently", "Endgültig löschen", "Supprimer définitivement"},
    {"mailbox.more_in_thread", "%1 more in this conversation",
     "%1 weitere in dieser Unterhaltung", "%1 de plus dans cette conversation"},
    {"mailbox.threaded", "Group by conversation", "Nach Unterhaltung gruppieren",
     "Grouper par conversation"},
    {"mailbox.count_one", "%1 message", "%1 Nachricht", "%1 message"},
    {"mailbox.count_other", "%1 messages", "%1 Nachrichten", "%1 messages"},

    // ---- one message --------------------------------------------------------
    {"msg.to", "To", "An", "À"},
    {"msg.html_alt", "This message also has an HTML version; the text above is the same message.",
     "Diese Nachricht hat auch eine HTML-Fassung; der Text oben ist dieselbe Nachricht.",
     "Ce message a aussi une version HTML ; le texte ci-dessus est le même message."},
    {"msg.html_only", "This message is HTML only.", "Diese Nachricht ist nur in HTML vorhanden.",
     "Ce message est uniquement en HTML."},
    {"msg.reply", "Reply", "Antworten", "Répondre"},
    {"msg.reply_all", "Reply all", "Allen antworten", "Répondre à tous"},
    {"msg.forward", "Forward", "Weiterleiten", "Transférer"},
    {"msg.flag", "Flag", "Markieren", "Marquer"},
    {"msg.move", "Move", "Verschieben", "Déplacer"},
    {"msg.trash", "Move to Trash", "In den Papierkorb", "Mettre à la corbeille"},
    {"msg.source", "View source", "Quelltext anzeigen", "Voir la source"},
    {"msg.back", "Back", "Zurück", "Retour"},
    {"msg.delete", "Delete", "Löschen", "Supprimer"},

    // ---- paging -------------------------------------------------------------
    {"pager.page", "Page", "Seite", "Page"},
    {"pager.of", "of", "von", "sur"},
    {"pager.newer", "Newer", "Neuer", "Plus récents"},
    {"pager.older", "Older", "Älter", "Plus anciens"},

    // ---- compose ------------------------------------------------------------
    {"compose.title", "Write a message", "Nachricht schreiben", "Écrire un message"},
    {"compose.to", "To", "An", "À"},
    {"compose.cc", "Cc", "Cc", "Cc"},
    {"compose.subject", "Subject", "Betreff", "Objet"},
    {"compose.message", "Message", "Nachricht", "Message"},
    {"compose.formatted_text", "Formatted text", "Formatierter Text", "Texte mis en forme"},
    {"compose.attachment", "Attachment", "Anhang", "Pièce jointe"},
    {"compose.send", "Send", "Senden", "Envoyer"},
    {"compose.save_draft", "Save as draft", "Als Entwurf speichern", "Enregistrer comme brouillon"},
    {"compose.cancel", "Cancel", "Abbrechen", "Annuler"},
    {"compose.address_book", "Address book", "Adressbuch", "Carnet d’adresses"},

    // ---- the keyboard help overlay ------------------------------------------
    {"keys.title", "Keyboard shortcuts", "Tastenkürzel", "Raccourcis clavier"},
    {"keys.close", "Close", "Schließen", "Fermer"},
    {"keys.move", "Move down and up", "Nach unten und oben", "Descendre et monter"},
    {"keys.open", "Open the selected message", "Ausgewählte Nachricht öffnen",
     "Ouvrir le message sélectionné"},
    {"keys.back", "Back to the list", "Zurück zur Liste", "Revenir à la liste"},
    {"keys.select", "Select the message", "Nachricht auswählen", "Sélectionner le message"},
    {"keys.compose", "Compose", "Verfassen", "Écrire"},
    {"keys.replies", "Reply, reply all, forward", "Antworten, allen antworten, weiterleiten",
     "Répondre, répondre à tous, transférer"},
    {"keys.trash", "Move to Trash", "In den Papierkorb", "Mettre à la corbeille"},
    {"keys.flag", "Flag", "Markieren", "Marquer"},
    {"keys.search", "Search", "Suchen", "Rechercher"},
    {"keys.goto", "Go to the inbox", "Zum Posteingang", "Aller à la boîte de réception"},
    {"keys.help", "This list", "Diese Liste", "Cette liste"},
};

// key -> row, built once. The catalog is consulted on the order of a hundred
// times per render, so the linear scan this replaces cost the size of the
// catalog times the number of strings on the page.
const std::unordered_map<std::string, const Msg *> &Index() {
	static const std::unordered_map<std::string, const Msg *> index = [] {
		std::unordered_map<std::string, const Msg *> m;
		m.reserve(sizeof(kMessages) / sizeof(kMessages[0]) * 2);
		for (auto &msg : kMessages) {
			m.emplace(msg.key, &msg);
		}
		return m;
	}();
	return index;
}

size_t ColumnFor(const std::string &code) {
	for (auto &l : kLocales) {
		if (code == l.code) {
			return l.column;
		}
	}
	return 0;
}

// The best acceptable language this build can actually serve. Deliberately
// small: q-values are honoured, because a browser that sends `de;q=0.9, en;q=0.8`
// means it, but anything past that — script subtags, ranges, "*" — is reduced to
// a primary subtag or skipped.
std::string NegotiateLocale(const std::string &header) {
	std::string best;
	double best_q = 0.0;
	size_t i = 0;
	while (i < header.size()) {
		size_t comma = header.find(',', i);
		std::string item =
		    header.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
		i = comma == std::string::npos ? header.size() : comma + 1;

		double q = 1.0;
		size_t semi = item.find(';');
		if (semi != std::string::npos) {
			size_t eq = item.find('=', semi);
			if (eq != std::string::npos) {
				q = strtod(item.c_str() + eq + 1, nullptr);
			}
			item = item.substr(0, semi);
		}
		// Trim, lower-case, and cut "de-CH" down to "de".
		std::string tag;
		for (char c : item) {
			if (c == ' ' || c == '\t') {
				continue;
			}
			if (c == '-' || c == '_') {
				break;
			}
			tag += (char)tolower((unsigned char)c);
		}
		if (tag.empty() || q <= best_q) {
			continue;
		}
		if (KnownLocale(tag)) {
			best = tag;
			best_q = q;
		}
	}
	return best;
}

} // namespace

bool KnownLocale(const std::string &code) {
	for (auto &l : kLocales) {
		if (code == l.code) {
			return true;
		}
	}
	return false;
}

std::string EffectiveLocale(const Ctx &ctx) {
	if (ctx.Authed()) {
		std::string want = quackmail::citadel::GetUserPref(ctx.con, ctx.username, "web_locale");
		if (KnownLocale(want)) {
			return want;
		}
	}
	// Unset means "negotiate". A value an operator set is a decision and stands
	// even against a header that disagrees; that is the difference between the
	// two, and why the header is consulted only after both.
	std::string site = ConfigStr(ctx.con, "qm_default_locale", "");
	if (KnownLocale(site)) {
		return site;
	}
	std::string negotiated = NegotiateLocale(ctx.req.Header("Accept-Language"));
	if (!negotiated.empty()) {
		return negotiated;
	}
	return "en";
}

std::vector<std::pair<std::string, std::string>> LocaleOptions() {
	std::vector<std::pair<std::string, std::string>> out;
	out.push_back({"", "Follow the site default"});
	for (auto &l : kLocales) {
		out.push_back({l.code, l.label});
	}
	return out;
}

std::string Tr(const Ctx &ctx, const std::string &key) {
	// Resolved per call rather than cached: a locale switch takes effect on the
	// very next page, and this is one preference lookup per render — the same
	// cost EffectiveTz already pays.
	auto &index = Index();
	auto it = index.find(key);
	if (it == index.end()) {
		// No catalog entry at all. Returning the key is loud on the page and in
		// a screenshot, which is what gets a missing string noticed.
		return key;
	}
	const char *text = Column(*it->second, ColumnFor(EffectiveLocale(ctx)));
	if (text == nullptr || text[0] == '\0') {
		// Present but untranslated in this locale. English reads as a mixed
		// page; the key reads as debris.
		return it->second->en;
	}
	return text;
}

std::string TrN(const Ctx &ctx, const std::string &key_one, const std::string &key_other, int64_t n) {
	// English and German treat only 1 as singular; French counts 0 that way too.
	// Two forms is all these three need — a language with more would want a
	// third key, not a different rule here.
	std::string locale = EffectiveLocale(ctx);
	bool one = locale == "fr" ? (n >= -1 && n <= 1) : (n == 1 || n == -1);
	return Tr(ctx, one ? key_one : key_other);
}

std::string TrF(const Ctx &ctx, const std::string &key, const std::vector<std::string> &args) {
	std::string tmpl = Tr(ctx, key);
	std::string out;
	out.reserve(tmpl.size() + 16);
	for (size_t i = 0; i < tmpl.size(); i++) {
		if (tmpl[i] != '%' || i + 1 >= tmpl.size() || tmpl[i + 1] < '1' || tmpl[i + 1] > '9') {
			out += tmpl[i];
			continue;
		}
		size_t which = (size_t)(tmpl[i + 1] - '1');
		if (which < args.size()) {
			out += args[which];
		} else {
			// A placeholder with no argument stays as written rather than
			// vanishing, so the mismatch is visible instead of silent.
			out += tmpl[i];
			out += tmpl[i + 1];
		}
		i++;
	}
	return out;
}

} // namespace qmweb
} // namespace duckdb
