#define DUCKDB_EXTENSION_MAIN

#include "quackmail_managesieve_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/mail_store.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/util.hpp"

namespace duckdb {
namespace {

using namespace quackmail;

ServerController g_managesieve;

// Stub ManageSieve greeting: advertise capabilities, then decline commands.
void HandleStub(DatabaseInstance &, net::ClientStream &stream) {
	stream.WriteLine("\"IMPLEMENTATION\" \"quackmail (stub)\"");
	stream.WriteLine("\"SIEVE\" \"fileinto\"");
	stream.WriteLine("\"STARTTLS\"");
	stream.WriteLine("OK \"ManageSieve ready\"");
	std::string line;
	while (stream.ReadLine(line, 8192)) {
		std::string upper = util::Upper(line);
		if (upper.rfind("LOGOUT", 0) == 0) {
			stream.WriteLine("OK \"Bye\"");
			return;
		}
		stream.WriteLine("NO \"command not implemented in stub\"");
	}
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "qm_managesieve", 4190, g_managesieve, HandleStub);
}

} // namespace

void QuackmailManagesieveExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailManagesieveExtension::Name() {
	return "quackmail_managesieve";
}
std::string QuackmailManagesieveExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_MANAGESIEVE
	return EXT_VERSION_QUACKMAIL_MANAGESIEVE;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_managesieve, loader) {
	duckdb::LoadInternal(loader);
}
}
