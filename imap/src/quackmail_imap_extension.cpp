#define DUCKDB_EXTENSION_MAIN

#include "quackmail_imap_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/mail_store.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/util.hpp"

namespace duckdb {
namespace {

using namespace quackmail;

ServerController g_imap;

// Stub IMAP greeting + a minimal, honest response to any command.
void HandleStub(DatabaseInstance &, net::ClientStream &stream) {
	stream.WriteLine("* OK [CAPABILITY IMAP4rev1] quackmail IMAP (stub) ready");
	std::string line;
	while (stream.ReadLine(line, 8192)) {
		std::string tag = line.substr(0, line.find(' '));
		if (tag.empty()) {
			tag = "*";
		}
		std::string upper = util::Upper(line);
		if (upper.find("LOGOUT") != std::string::npos) {
			stream.WriteLine("* BYE quackmail logging out");
			stream.WriteLine(tag + " OK LOGOUT completed");
			return;
		}
		stream.WriteLine(tag + " NO command not implemented in stub");
	}
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "qm_imap", 1143, g_imap, HandleStub);
}

} // namespace

void QuackmailImapExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailImapExtension::Name() {
	return "quackmail_imap";
}
std::string QuackmailImapExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_IMAP
	return EXT_VERSION_QUACKMAIL_IMAP;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_imap, loader) {
	duckdb::LoadInternal(loader);
}
}
