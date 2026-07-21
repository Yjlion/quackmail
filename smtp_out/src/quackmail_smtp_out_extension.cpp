#define DUCKDB_EXTENSION_MAIN

#include "quackmail_smtp_out_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/mail_store.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"

namespace duckdb {
namespace {

using namespace quackmail;

ServerController g_smtp_out;

// Stub connection handler. The real outbound path drains quackmail_outbound and
// relays via SMTP rather than listening; this keeps the control interface
// uniform until that lands.
void HandleStub(DatabaseInstance &, net::ClientStream &stream) {
	stream.WriteLine("220 quackmail smtp_out relay (stub) not yet accepting connections");
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "qm_smtp_out", 2526, g_smtp_out, HandleStub);
}

} // namespace

void QuackmailSmtpOutExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailSmtpOutExtension::Name() {
	return "quackmail_smtp_out";
}
std::string QuackmailSmtpOutExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_SMTP_OUT
	return EXT_VERSION_QUACKMAIL_SMTP_OUT;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_smtp_out, loader) {
	duckdb::LoadInternal(loader);
}
}
