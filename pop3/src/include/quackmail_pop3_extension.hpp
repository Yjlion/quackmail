#pragma once

#include "duckdb.hpp"

namespace duckdb {

// POP3 retrieval gateway. Serves each user's Citadel Mail room
// (citadel_room_msgs -> citadel_messages) over USER/PASS/STAT/LIST/UIDL/RETR/
// DELE/RSET/QUIT.
class QuackmailPop3Extension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
