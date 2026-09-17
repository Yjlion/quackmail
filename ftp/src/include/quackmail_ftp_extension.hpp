#pragma once

#include "duckdb.hpp"

namespace duckdb {

// FTP and FTPS over the Citadel file areas.
//
// A third front door onto the store in core/filearea.hpp — the same rooms, the
// same QR_UPLOAD/QR_DOWNLOAD/QR_VISDIR flags and the same permission questions
// the telnet shell and /dav/files/ ask. Nothing about a file is stored twice.
//
// The filesystem a client sees is two levels deep and no deeper: "/" is the file
// areas visible to this user, "/<room>/" is its files. There are no
// subdirectories, because a Citadel room has no sub-rooms.
class QuackmailFtpExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
