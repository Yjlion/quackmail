#pragma once

// SFTP (draft-ietf-secsh-filexfer-02, protocol version 3 — the one every
// client speaks) over the Citadel file areas.
//
// The tree is the one FTP shows: "/" lists the file areas the user can see,
// and "/<area>/<file>" is a file. There is no deeper level, because a file
// area has no subdirectories. Everything goes through core/filearea.hpp, so
// SFTP, FTP, WebDAV, the web file view and the telnet `.R F` commands can never
// disagree about what a file is or who may touch it.
//
// A file is a message, stored whole. A write handle therefore buffers the
// upload and stores it on CLOSE — which is also when the storage quota inside
// InsertMessage gets its say — and a read handle holds the file it opened.

#include "duckdb.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace quackmail {
namespace sshd {

class SftpServer {
public:
	SftpServer(duckdb::Connection &con, const std::string &username);
	~SftpServer();

	// Consume channel data. Every complete request is answered, the replies
	// appended to `out`. False when the stream is not SFTP, and the channel
	// should be closed.
	bool Feed(const std::string &data, std::string &out);

private:
	struct Handle;

	void Dispatch(const std::string &packet, std::string &out);

	duckdb::Connection &con_;
	std::string username_;
	std::string inbuf_;
	bool initialized_ = false;
	uint32_t next_handle_ = 1;
	std::map<std::string, std::unique_ptr<Handle>> handles_;
};

} // namespace sshd
} // namespace quackmail
