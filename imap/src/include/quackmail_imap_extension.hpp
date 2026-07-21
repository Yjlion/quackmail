#pragma once

#include "duckdb.hpp"

namespace duckdb {

// IMAP4rev1 retrieval (stub). Serves mailboxes backed by quackmail_messages /
// quackmail_message_flags. Full command set is a later iteration.
class QuackmailImapExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
