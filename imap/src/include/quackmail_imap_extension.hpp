#pragma once

#include "duckdb.hpp"

namespace duckdb {

// IMAP4rev1 retrieval gateway (minimal subset). Maps mailboxes to Citadel rooms
// (INBOX -> the user's Mail room) and serves LOGIN/LIST/SELECT/FETCH/STORE/
// EXPUNGE from citadel_messages + citadel_msg_flags. Deep IMAP is a later phase.
class QuackmailImapExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
