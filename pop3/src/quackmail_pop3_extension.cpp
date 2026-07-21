#define DUCKDB_EXTENSION_MAIN

#include "quackmail_pop3_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/mail_store.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/util.hpp"

namespace duckdb {
namespace {

using namespace quackmail;

ServerController g_pop3;

// Stub POP3 greeting + minimal command handling.
void HandleStub(DatabaseInstance &, net::ClientStream &stream) {
	stream.WriteLine("+OK quackmail POP3 (stub) ready");
	std::string line;
	while (stream.ReadLine(line, 8192)) {
		std::string upper = util::Upper(line);
		if (upper.rfind("QUIT", 0) == 0) {
			stream.WriteLine("+OK quackmail signing off");
			return;
		}
		if (upper.rfind("CAPA", 0) == 0) {
			stream.WriteLine("+OK capability list follows");
			stream.WriteLine("USER");
			stream.WriteLine(".");
			continue;
		}
		stream.WriteLine("-ERR command not implemented in stub");
	}
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "qm_pop3", 1110, g_pop3, HandleStub);
}

} // namespace

void QuackmailPop3Extension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailPop3Extension::Name() {
	return "quackmail_pop3";
}
std::string QuackmailPop3Extension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_POP3
	return EXT_VERSION_QUACKMAIL_POP3;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_pop3, loader) {
	duckdb::LoadInternal(loader);
}
}
