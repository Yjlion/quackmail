#define DUCKDB_EXTENSION_MAIN

#include "quackmail_xmpp_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/citadel_store.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/util.hpp"
#include "quackmail/xmlstream.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace {

using namespace quackmail;

// xmpp (5222, STARTTLS) and xmpps (5223, implicit TLS). A real Citadel server
// listens only on 5222; the implicit-TLS port is free with the shared listener
// machinery, so it is offered too.
ServerController g_xmpp;
ServerController g_xmpps;

// How long to block waiting for client input before checking for instant
// messages that have to be pushed out as <message> stanzas.
constexpr int kPollMs = 500;

struct Xmpp {
	bool authed = false;
	std::string username;
	std::string node;      // server name, e.g. "quackcit"
	std::string resource;
	std::string client_jid; // user@node/resource

	// State accumulated while an <iq> is being read.
	std::string iq_id;
	std::string iq_type;
	std::string iq_from;
	std::string iq_to;
	std::string query_xmlns;
	std::string chardata;
	bool bind_requested = false;
	bool session_requested = false;
	bool vcard_requested = false;
	bool ping_requested = false;
	std::string sasl_mech;
	std::string nonsasl_user;
	std::string nonsasl_pass;

	// State accumulated while a <message> is being read.
	std::string message_to;
	std::string message_body;
	int html_depth = 0;

	std::string BareJid() const {
		return username + "@" + node;
	}
};

void Send(net::ClientStream &stream, const std::string &xml) {
	stream.Write(xml);
}

std::string Esc(const std::string &s) {
	return xmlstream::Escape(s);
}

// The stream header plus the features available at this point in the session.
void SendStreamHeader(net::ClientStream &stream, const Xmpp &s, ServerController &ctrl, int64_t session_id) {
	Send(stream, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
	             "<stream:stream from=\"" + Esc(s.node) + "\" id=\"" +
	                 std::to_string(session_id) +
	                 "\" version=\"1.0\" xmlns:stream=\"http://etherx.jabber.org/streams\" "
	                 "xmlns=\"jabber:client\">");
	Send(stream, "<stream:features>");
	if (!stream.IsTls() && ctrl.StartTlsEnabled()) {
		Send(stream, "<starttls xmlns=\"urn:ietf:params:xml:ns:xmpp-tls\"></starttls>");
	}
	if (!s.authed) {
		Send(stream, "<mechanisms xmlns=\"urn:ietf:params:xml:ns:xmpp-sasl\">"
		             "<mechanism>PLAIN</mechanism></mechanisms>");
		Send(stream, "<auth xmlns=\"http://jabber.org/features/iq-auth\"/>");
	}
	Send(stream, "<bind xmlns=\"urn:ietf:params:xml:ns:xmpp-bind\"/>");
	Send(stream, "<session xmlns=\"urn:ietf:params:xml:ns:xmpp-session\"/>");
	Send(stream, "</stream:features>");
}

// Everyone else who is logged in, which is what Citadel uses as the roster and
// as the presence list (it has no stored roster either).
std::vector<citadel::SessionInfo> OnlineOthers(Connection &con, const Xmpp &s) {
	std::vector<citadel::SessionInfo> out;
	for (auto &sess : citadel::ListSessions(con)) {
		if (sess.username.empty() || util::Upper(sess.username) == util::Upper(s.username)) {
			continue;
		}
		bool dup = false;
		for (auto &seen : out) {
			if (util::Upper(seen.username) == util::Upper(sess.username)) {
				dup = true;
				break;
			}
		}
		if (!dup) {
			out.push_back(sess);
		}
	}
	return out;
}

void SendRoster(Connection &con, net::ClientStream &stream, const Xmpp &s) {
	std::string humannode = citadel::GetConfig(con, "c_humannode", "QuackCit BBS");
	Send(stream, "<query xmlns=\"jabber:iq:roster\">");
	for (auto &other : OnlineOthers(con, s)) {
		Send(stream, "<item jid=\"" + Esc(other.username + "@" + s.node) + "\" name=\"" +
		                 Esc(other.username) + "\" subscription=\"both\"><group>" + Esc(humannode) +
		                 "</group></item>");
	}
	Send(stream, "</query>");
}

// Announce every online user to this client (Citadel's wholist presence dump).
void SendPresenceDump(Connection &con, net::ClientStream &stream, const Xmpp &s) {
	for (auto &other : OnlineOthers(con, s)) {
		Send(stream, "<presence from=\"" + Esc(other.username + "@" + s.node) + "\" to=\"" +
		                 Esc(s.client_jid) + "\"></presence>");
	}
}

// Drain queued instant messages into <message> stanzas. Shared with the native
// Citadel protocol's SEXP/GEXP through the citadel_express table.
void PushIncomingMessages(Connection &con, net::ClientStream &stream, const Xmpp &s) {
	if (!s.authed || s.client_jid.empty()) {
		return;
	}
	for (auto &e : citadel::PendingExpress(con, s.username)) {
		Send(stream, "<message to=\"" + Esc(s.client_jid) + "\" from=\"" +
		                 Esc(e.from_user + "@" + s.node) + "\" type=\"chat\"><body>" + Esc(e.text) +
		                 "</body></message>");
		citadel::MarkExpressDelivered(con, e.id);
	}
}

void SendIqError(net::ClientStream &stream, const std::string &id, const std::string &from) {
	Send(stream, "<iq type=\"error\"" + (from.empty() ? "" : " from=\"" + Esc(from) + "\"") + " id=\"" +
	                 Esc(id) + "\"><error code=\"503\" type=\"cancel\">"
	                           "<service-unavailable xmlns=\"urn:ietf:params:xml:ns:xmpp-stanzas\"/>"
	                           "</error></iq>");
}

// SASL PLAIN: the payload is "\0<authcid>\0<password>".
bool SaslPlain(Connection &con, const std::string &payload, std::string &user) {
	std::string decoded;
	if (!util::Base64Decode(payload, decoded)) {
		return false;
	}
	size_t first = decoded.find('\0');
	if (first == std::string::npos) {
		return false;
	}
	size_t second = decoded.find('\0', first + 1);
	if (second == std::string::npos) {
		return false;
	}
	std::string authcid = decoded.substr(first + 1, second - first - 1);
	std::string password = decoded.substr(second + 1);
	if (!auth::Verify(con, authcid, password)) {
		return false;
	}
	user = authcid;
	return true;
}

void HandleXmpp(DatabaseInstance &db, net::ClientStream &stream, ServerController &ctrl) {
	Connection con(db);
	store::EnsureSchema(con);

	Xmpp s;
	s.node = citadel::GetConfig(con, "c_nodename", "quackcit");
	int64_t session_id = citadel::RegisterSession(con, ctrl.ImplicitTls() ? "XMPPS session" : "XMPP session");

	xmlstream::Tokenizer tok;
	bool running = true;
	bool restart_stream = false;

	while (running) {
		// Push anything waiting for this user before blocking again.
		PushIncomingMessages(con, stream, s);

		if (!stream.WaitReadable(kPollMs)) {
			continue;
		}
		std::string chunk;
		if (!stream.ReadAvailable(chunk)) {
			break;
		}
		tok.Feed(chunk);

		xmlstream::Event ev;
		while (running && tok.Next(ev)) {
			std::string el = ev.LocalName();

			if (ev.kind == xmlstream::Event::TEXT) {
				s.chardata += ev.text;
				continue;
			}

			if (ev.kind == xmlstream::Event::START) {
				if (el == "stream") {
					SendStreamHeader(stream, s, ctrl, session_id);
				} else if (el == "starttls") {
					if (stream.IsTls() || !ctrl.StartTlsEnabled()) {
						Send(stream, "<failure xmlns=\"urn:ietf:params:xml:ns:xmpp-tls\"/>");
					} else {
						Send(stream, "<proceed xmlns=\"urn:ietf:params:xml:ns:xmpp-tls\"/>");
						std::string terr;
						if (!stream.StartTls(ctrl.TlsCtx(), terr)) {
							running = false;
							break;
						}
						restart_stream = true; // TLS begins a new XML document
					}
				} else if (el == "auth") {
					s.sasl_mech = util::Upper(ev.Attr("mechanism"));
					s.chardata.clear();
				} else if (el == "iq") {
					s.iq_id = ev.Attr("id");
					s.iq_type = util::Upper(ev.Attr("type"));
					s.iq_from = ev.Attr("from");
					s.iq_to = ev.Attr("to");
					s.query_xmlns.clear();
					s.bind_requested = false;
					s.session_requested = false;
					s.vcard_requested = false;
					s.ping_requested = false;
				} else if (el == "query") {
					s.query_xmlns = ev.Attr("xmlns");
				} else if (el == "bind") {
					s.bind_requested = true;
				} else if (el == "session") {
					s.session_requested = true;
				} else if (el == "vCard") {
					s.vcard_requested = true;
				} else if (el == "ping") {
					s.ping_requested = true;
				} else if (el == "message") {
					s.message_to = ev.Attr("to");
					s.message_body.clear();
					s.html_depth = 0;
				} else if (el == "html") {
					s.html_depth++;
				}
				s.chardata.clear();
				continue;
			}

			// --- END elements: this is where stanzas are acted on ------------
			if (el == "stream") {
				Send(stream, "</stream:stream>");
				running = false;
				break;
			} else if (el == "resource") {
				s.resource = s.chardata;
			} else if (el == "username") {
				s.nonsasl_user = s.chardata; // legacy jabber:iq:auth
			} else if (el == "password") {
				s.nonsasl_pass = s.chardata;
			} else if (el == "html") {
				s.html_depth--;
			} else if (el == "body" && s.html_depth == 0) {
				s.message_body = s.chardata;
			} else if (el == "auth") {
				std::string user;
				if (s.sasl_mech != "PLAIN") {
					Send(stream, "<failure xmlns=\"urn:ietf:params:xml:ns:xmpp-sasl\">"
					             "<invalid-mechanism/></failure>");
				} else if (!SaslPlain(con, s.chardata, user)) {
					Send(stream, "<failure xmlns=\"urn:ietf:params:xml:ns:xmpp-sasl\">"
					             "<not-authorized/></failure>");
				} else {
					s.authed = true;
					s.username = user;
					citadel::EnsureUserRooms(con, user);
					citadel::TouchSession(con, session_id, user, "", "xmpp", citadel::GetAxLevel(con, user));
					Send(stream, "<success xmlns=\"urn:ietf:params:xml:ns:xmpp-sasl\"/>");
					restart_stream = true; // a successful SASL restarts the stream
				}
			} else if (el == "message") {
				if (s.authed && !s.message_to.empty() && !s.message_body.empty()) {
					// The JID's local part is the Citadel user name.
					std::string to = s.message_to;
					size_t at = to.find('@');
					if (at != std::string::npos) {
						to = to.substr(0, at);
					}
					citadel::SendExpress(con, to, s.username, s.message_body);
				}
				s.message_to.clear();
				s.message_body.clear();
			} else if (el == "presence") {
				SendPresenceDump(con, stream, s);
			} else if (el == "iq") {
				std::string from_domain = s.client_jid.empty() ? s.node : s.BareJid();
				if (s.iq_type == "GET" && s.query_xmlns == "jabber:iq:roster") {
					Send(stream, "<iq type=\"result\" from=\"" + Esc(s.BareJid()) + "\" id=\"" +
					                 Esc(s.iq_id) + "\">");
					SendRoster(con, stream, s);
					Send(stream, "</iq>");
				} else if (s.iq_type == "GET" && s.query_xmlns == "jabber:iq:auth") {
					Send(stream, "<iq type=\"result\" from=\"" + Esc(s.node) + "\" id=\"" + Esc(s.iq_id) +
					                 "\"><query xmlns=\"jabber:iq:auth\">"
					                 "<username/><password/><resource/></query></iq>");
				} else if (s.iq_type == "SET" && s.query_xmlns == "jabber:iq:auth") {
					// Legacy non-SASL authentication.
					if (auth::Verify(con, s.nonsasl_user, s.nonsasl_pass)) {
						s.authed = true;
						s.username = s.nonsasl_user;
						citadel::EnsureUserRooms(con, s.username);
						citadel::TouchSession(con, session_id, s.username, "", "xmpp",
						                      citadel::GetAxLevel(con, s.username));
						Send(stream, "<iq type=\"result\" id=\"" + Esc(s.iq_id) + "\"/>");
					} else {
						Send(stream, "<iq type=\"error\" id=\"" + Esc(s.iq_id) +
						                 "\"><error code=\"401\" type=\"auth\">"
						                 "<not-authorized xmlns=\"urn:ietf:params:xml:ns:xmpp-stanzas\"/>"
						                 "</error></iq>");
					}
					s.nonsasl_pass.clear();
				} else if (s.query_xmlns.rfind("http://jabber.org/protocol/disco#", 0) == 0) {
					Send(stream, "<iq type=\"result\" from=\"" + Esc(s.node) + "\" id=\"" + Esc(s.iq_id) +
					                 "\"><query xmlns=\"" + Esc(s.query_xmlns) + "\"/></iq>");
				} else if (s.bind_requested && s.authed) {
					if (s.resource.empty()) {
						s.resource = std::to_string(session_id);
					}
					s.client_jid = s.BareJid() + "/" + s.resource;
					Send(stream, "<iq type=\"result\" id=\"" + Esc(s.iq_id) +
					                 "\"><bind xmlns=\"urn:ietf:params:xml:ns:xmpp-bind\"><jid>" +
					                 Esc(s.client_jid) + "</jid></bind></iq>");
				} else if (s.session_requested) {
					Send(stream, "<iq type=\"result\" id=\"" + Esc(s.iq_id) + "\"></iq>");
				} else if (s.ping_requested) {
					Send(stream, "<iq type=\"result\" from=\"" + Esc(s.node) + "\" id=\"" + Esc(s.iq_id) +
					                 "\"/>");
				} else if (s.vcard_requested) {
					Send(stream, "<iq type=\"result\" id=\"" + Esc(s.iq_id) + "\" to=\"" +
					                 Esc(s.iq_from) + "\"><vCard xmlns=\"vcard-temp\"><fn>" +
					                 Esc(s.username) + "</fn><nickname>" + Esc(s.username) +
					                 "</nickname></vCard></iq>");
				} else {
					SendIqError(stream, s.iq_id, from_domain);
				}
				s.iq_id.clear();
				s.iq_type.clear();
				s.query_xmlns.clear();
				s.bind_requested = false;
				s.session_requested = false;
				s.vcard_requested = false;
				s.ping_requested = false;
			}
			s.chardata.clear();

			if (restart_stream) {
				// STARTTLS and SASL both begin a fresh XML document: drop any
				// buffered bytes and wait for the client's new stream header.
				restart_stream = false;
				tok.Reset();
				break;
			}
		}
	}

	citadel::UnregisterSession(con, session_id);
}

void HandleXmppConn(DatabaseInstance &db, net::ClientStream &stream) {
	HandleXmpp(db, stream, g_xmpp);
}
void HandleXmppsConn(DatabaseInstance &db, net::ClientStream &stream) {
	HandleXmpp(db, stream, g_xmpps);
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "qm_xmpp", 5222, g_xmpp, HandleXmppConn);
	RegisterServerControls(loader, "qm_xmpps", 5223, g_xmpps, HandleXmppsConn);
}

} // namespace

void QuackmailXmppExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailXmppExtension::Name() {
	return "quackmail_xmpp";
}
std::string QuackmailXmppExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_XMPP
	return EXT_VERSION_QUACKMAIL_XMPP;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_xmpp, loader) {
	duckdb::LoadInternal(loader);
}
}
