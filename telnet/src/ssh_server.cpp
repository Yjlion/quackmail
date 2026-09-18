#include "ssh_server.hpp"

#include "sftp.hpp"
#include "ssh_transport.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/citadel_store.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/sshkeys.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace quackmail {
namespace sshd {

using duckdb::Connection;
using duckdb::MaterializedQueryResult;
using duckdb::Value;
using ssh::Reader;
using ssh::Writer;

ShellRequest::ShellRequest() : cols(80), rows(24) {
}

namespace {

// How long an unauthenticated connection may take to log in (OpenSSH's
// LoginGraceTime default), and how many wrong answers it gets.
constexpr int kLoginGraceMs = 120 * 1000;
constexpr int kMaxAuthFailures = 6;

// Our receive window and the largest packet we accept on the channel.
constexpr uint32_t kWindow = 2 * 1024 * 1024;
constexpr uint32_t kMaxChannelPacket = 32 * 1024;

std::vector<std::string> MatchUsers(Connection &con, const std::string &sql, const std::string &arg) {
	std::vector<std::string> out;
	auto stmt = con.Prepare(sql);
	if (stmt->HasError()) {
		return out;
	}
	duckdb::vector<Value> params = {Value(arg)};
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
		out.push_back(mat.GetValue(0, i).ToString());
	}
	return out;
}

// Does a signature algorithm belong with a key type? rsa keys sign with the
// SHA-2 algorithms (RFC 8332), never with SHA-1 `ssh-rsa`.
bool AlgFitsKey(const std::string &alg, const std::string &type) {
	if (type == "ssh-rsa") {
		return alg == "rsa-sha2-256" || alg == "rsa-sha2-512";
	}
	return alg == type;
}

bool SendAll(int fd, const std::string &data) {
	size_t off = 0;
	while (off < data.size()) {
		ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		off += (size_t)n;
	}
	return true;
}

// Client keystrokes into the shell's telnet-framed input: a literal 0xFF is
// doubled, exactly as a telnet client would send it.
std::string TelnetEscape(const std::string &data) {
	std::string out;
	out.reserve(data.size());
	for (char c : data) {
		out.push_back(c);
		if ((unsigned char)c == 0xFF) {
			out.push_back(c);
		}
	}
	return out;
}

// A window-change as the shell already understands one: IAC SB NAWS.
std::string Naws(uint32_t cols, uint32_t rows) {
	cols = std::min<uint32_t>(cols, 0xFFFE);
	rows = std::min<uint32_t>(rows, 0xFFFE);
	std::string payload;
	payload.push_back((char)(cols >> 8));
	payload.push_back((char)cols);
	payload.push_back((char)(rows >> 8));
	payload.push_back((char)rows);
	std::string out = "\xFF\xFA\x1F";
	out += TelnetEscape(payload);
	out += "\xFF\xF0";
	return out;
}

class Session {
public:
	Session(duckdb::DatabaseInstance &db, net::ClientStream &stream, ShellRunner shell)
	    : db_(db), stream_(stream), shell_(shell), con_(db) {
	}
	~Session() {
		StopShell();
	}

	void Run() {
		store::EnsureSchema(con_);
		ssh::HostKey hk;
		std::string err;
		if (!ssh::SiteHostKey(con_, hk, err)) {
			return;
		}
		peer_ = stream_.PeerIp();
		tr_.reset(new Transport(stream_.Fd(), hk));
		if (!tr_->Handshake()) {
			return;
		}
		if (!Authenticate()) {
			return;
		}
		ChannelLoop();
	}

private:
	// ---- authentication ------------------------------------------------------

	bool Authenticate() {
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kLoginGraceMs);
		auto left = [&]() {
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
			                                                                std::chrono::steady_clock::now())
			              .count();
			return (int)std::max<int64_t>(ms, 1);
		};
		bool allow_password = citadel::GetConfig(con_, "qm_ssh_allow_password", "1") != "0";
		std::string methods = allow_password ? "publickey,password" : "publickey";
		auto failure = [&]() { return tr_->Write(Writer().Byte(USERAUTH_FAILURE).String(methods).Bool(false).Data()); };

		std::string p;
		if (!tr_->Read(p, left())) {
			return false;
		}
		Reader sr(p);
		uint8_t msg = 0;
		std::string service;
		sr.Byte(msg);
		sr.String(service);
		if (msg != SERVICE_REQUEST || service != "ssh-userauth") {
			tr_->Disconnect(SERVICE_NOT_AVAILABLE, "expected ssh-userauth");
			return false;
		}
		tr_->Write(Writer().Byte(SERVICE_ACCEPT).String("ssh-userauth").Data());

		int failures = 0;
		while (true) {
			if (!tr_->Read(p, left())) {
				return false;
			}
			Reader r(p);
			std::string user, svc, method;
			r.Byte(msg);
			if (msg != USERAUTH_REQUEST) {
				continue; // e.g. a stray global request; nothing else is valid yet
			}
			r.String(user);
			r.String(svc);
			r.String(method);
			if (!r.Ok() || svc != "ssh-connection") {
				tr_->Disconnect(SERVICE_NOT_AVAILABLE, "unknown service");
				return false;
			}
			std::string canonical;
			bool known = ResolveUser(con_, user, canonical);

			bool counted = false;
			if (method == "password") {
				bool change = false;
				std::string pw;
				r.Bool(change);
				r.String(pw);
				if (allow_password && !change && known && auth::Verify(con_, canonical, pw)) {
					return Succeed(canonical);
				}
				counted = true;
			} else if (method == "publickey") {
				bool has_sig = false;
				std::string alg, blob;
				r.Bool(has_sig);
				r.String(alg);
				r.String(blob);
				ssh::PublicKey key;
				std::string kerr;
				bool usable = r.Ok() && ssh::ParseKeyBlob(blob, key, kerr) && AlgFitsKey(alg, key.type) && known &&
				              ssh::UserHasKey(con_, canonical, blob);
				if (!has_sig) {
					// "Would this key do?" — answered truthfully only for a key
					// that is registered to this very user.
					if (usable) {
						tr_->Write(Writer().Byte(USERAUTH_PK_OK).String(alg).String(blob).Data());
						continue;
					}
				} else {
					std::string sig;
					r.String(sig);
					Reader sg(sig);
					std::string sig_alg;
					sg.String(sig_alg);
					// RFC 4252 §7: the signature covers the session identifier and
					// this request as sent, with the user name the client typed.
					std::string signed_data = Writer()
					                              .String(tr_->SessionId())
					                              .Byte(USERAUTH_REQUEST)
					                              .String(user)
					                              .String(svc)
					                              .String("publickey")
					                              .Bool(true)
					                              .String(alg)
					                              .String(blob)
					                              .Data();
					if (usable && r.Ok() && sig_alg == alg && ssh::VerifySignature(blob, sig, signed_data)) {
						ssh::UserHasKey(con_, canonical, blob, true);
						return Succeed(canonical);
					}
					counted = true;
				}
			}
			// "none" (a client asking which methods exist) and unknown methods are
			// answered with the list and not counted against the client.
			if (counted && ++failures >= kMaxAuthFailures) {
				tr_->Disconnect(NO_MORE_AUTH_METHODS, "Too many authentication failures");
				return false;
			}
			if (counted) {
				// Slows a password guesser down without holding a lock.
				std::this_thread::sleep_for(std::chrono::milliseconds(300 * failures));
			}
			if (!failure()) {
				return false;
			}
		}
	}

	bool Succeed(const std::string &canonical) {
		username_ = canonical;
		citadel::EnsureUserRooms(con_, username_);
		return tr_->Write(std::string(1, (char)USERAUTH_SUCCESS));
	}

	// ---- the connection protocol --------------------------------------------

	void ChannelLoop() {
		while (true) {
			// Housekeeping first: flush what the window allows, then decide what
			// to wait on. The shell socket is only watched while there is room to
			// forward its output, which is the backpressure for a slow client.
			if (!Flush()) {
				return;
			}
			if (closing_ && (peer_closed_ || std::chrono::steady_clock::now() > close_deadline_)) {
				return;
			}
			bool watch_shell = shell_fd_ >= 0 && !shell_eof_ && pending_.empty() && remote_window_ > 0;
			struct pollfd pfd[2] {};
			pfd[0].fd = tr_->Fd();
			pfd[0].events = POLLIN;
			pfd[1].fd = watch_shell ? shell_fd_ : -1;
			pfd[1].events = POLLIN;
			if (!tr_->Buffered()) {
				int pr = ::poll(pfd, 2, closing_ ? 500 : 60000);
				if (pr < 0 && errno != EINTR) {
					return;
				}
				if (pr <= 0) {
					continue;
				}
			} else {
				pfd[0].revents = POLLIN;
			}
			if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
				std::string p;
				if (!tr_->Read(p, 60000)) {
					return;
				}
				if (!HandlePacket(p)) {
					return;
				}
			}
			if (watch_shell && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
				char buf[kMaxChannelPacket];
				size_t want = std::min<size_t>({sizeof(buf), (size_t)remote_window_, (size_t)remote_max_packet_});
				ssize_t n = ::recv(shell_fd_, buf, want, 0);
				if (n > 0) {
					pending_.append(buf, (size_t)n);
				} else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
					// The shell ended (the user typed .Q, or it hung up).
					shell_eof_ = true;
					CloseChannel(0);
				}
			}
		}
	}

	// Send as much of `pending_` as the client's window allows.
	bool Flush() {
		while (!pending_.empty() && remote_window_ > 0 && channel_open_) {
			size_t n = std::min<size_t>({pending_.size(), (size_t)remote_window_, (size_t)remote_max_packet_});
			if (!tr_->Write(Writer().Byte(CHANNEL_DATA).U32(remote_id_).String(pending_.substr(0, n)).Data())) {
				return false;
			}
			pending_.erase(0, n);
			remote_window_ -= (uint32_t)n;
		}
		if (close_after_flush_ && pending_.empty() && channel_open_) {
			close_after_flush_ = false;
			SendClose();
		}
		return true;
	}

	void CloseChannel(uint32_t exit_status) {
		if (!channel_open_ || closing_) {
			return;
		}
		exit_status_ = exit_status;
		close_after_flush_ = true;
	}

	void SendClose() {
		tr_->Write(Writer()
		               .Byte(CHANNEL_REQUEST)
		               .U32(remote_id_)
		               .String("exit-status")
		               .Bool(false)
		               .U32(exit_status_)
		               .Data());
		tr_->Write(Writer().Byte(CHANNEL_EOF).U32(remote_id_).Data());
		tr_->Write(Writer().Byte(CHANNEL_CLOSE).U32(remote_id_).Data());
		closing_ = true;
		close_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	}

	bool Reply(bool want, bool ok) {
		if (!want) {
			return true;
		}
		return tr_->Write(Writer().Byte(ok ? CHANNEL_SUCCESS : CHANNEL_FAILURE).U32(remote_id_).Data());
	}

	bool HandlePacket(const std::string &p) {
		Reader r(p);
		uint8_t msg = 0;
		r.Byte(msg);
		switch (msg) {
		case CHANNEL_OPEN: {
			std::string type;
			uint32_t sender = 0, window = 0, maxpkt = 0;
			r.String(type);
			r.U32(sender);
			r.U32(window);
			r.U32(maxpkt);
			if (type != "session" || channel_open_ || closing_) {
				// 1 = administratively prohibited. Forwarding channels and a
				// second session are both refused here.
				return tr_->Write(Writer()
				                      .Byte(CHANNEL_OPEN_FAILURE)
				                      .U32(sender)
				                      .U32(1)
				                      .String(type == "session" ? "one session per connection"
				                                                : "channel type not supported")
				                      .String("en")
				                      .Data());
			}
			channel_open_ = true;
			remote_id_ = sender;
			remote_window_ = window;
			remote_max_packet_ = std::max<uint32_t>(std::min<uint32_t>(maxpkt, kMaxChannelPacket), 1024);
			local_window_ = kWindow;
			return tr_->Write(Writer()
			                      .Byte(CHANNEL_OPEN_CONFIRMATION)
			                      .U32(sender)
			                      .U32(0)
			                      .U32(kWindow)
			                      .U32(kMaxChannelPacket)
			                      .Data());
		}
		case CHANNEL_REQUEST: {
			uint32_t recipient = 0;
			std::string type;
			bool want = false;
			r.U32(recipient);
			r.String(type);
			r.Bool(want);
			if (!channel_open_) {
				return true;
			}
			if (type == "pty-req") {
				uint32_t cols = 0, rows = 0, pw = 0, ph = 0;
				r.String(term_);
				r.U32(cols);
				r.U32(rows);
				r.U32(pw);
				r.U32(ph);
				cols_ = (int)cols;
				rows_ = (int)rows;
				pty_ = true;
				if (term_.empty()) {
					term_ = "vt100";
				}
				return Reply(want, mode_ == Mode::None);
			}
			if (type == "window-change") {
				uint32_t cols = 0, rows = 0;
				r.U32(cols);
				r.U32(rows);
				if (mode_ == Mode::Shell && shell_fd_ >= 0 && cols > 0 && rows > 0) {
					SendAll(shell_fd_, Naws(cols, rows));
				}
				return true;
			}
			if (type == "shell" && mode_ == Mode::None) {
				StartShell();
				return Reply(want, shell_fd_ >= 0);
			}
			if (type == "subsystem" && mode_ == Mode::None) {
				std::string name;
				r.String(name);
				if (name == "sftp") {
					mode_ = Mode::Sftp;
					sftp_.reset(new SftpServer(con_, username_));
					session_id_ = citadel::RegisterSession(con_, "SFTP session", peer_);
					citadel::TouchSession(con_, session_id_, username_, "", "SFTP", 0);
					return Reply(want, true);
				}
				return Reply(want, false);
			}
			// exec, env, x11-req, auth-agent-req, signal, ...: not offered.
			return Reply(want, false);
		}
		case CHANNEL_DATA: {
			uint32_t recipient = 0;
			std::string data;
			r.U32(recipient);
			r.String(data);
			if (!r.Ok() || !channel_open_) {
				return true;
			}
			if (data.size() > local_window_) {
				tr_->Disconnect(PROTOCOL_ERROR, "window exceeded");
				return false;
			}
			local_window_ -= (uint32_t)data.size();
			if (mode_ == Mode::Shell && shell_fd_ >= 0) {
				SendAll(shell_fd_, TelnetEscape(data));
			} else if (mode_ == Mode::Sftp && sftp_) {
				std::string out;
				if (!sftp_->Feed(data, out)) {
					CloseChannel(1);
				}
				pending_ += out;
				if (session_id_ > 0) {
					citadel::TouchSession(con_, session_id_, username_, "", "SFTP", 0);
				}
			}
			if (local_window_ < kWindow / 2) {
				uint32_t add = kWindow - local_window_;
				local_window_ = kWindow;
				return tr_->Write(Writer().Byte(CHANNEL_WINDOW_ADJUST).U32(remote_id_).U32(add).Data());
			}
			return true;
		}
		case CHANNEL_EXTENDED_DATA: {
			uint32_t recipient = 0, code = 0;
			std::string data;
			r.U32(recipient);
			r.U32(code);
			r.String(data);
			local_window_ -= std::min<uint32_t>(local_window_, (uint32_t)data.size());
			return true;
		}
		case CHANNEL_WINDOW_ADJUST: {
			uint32_t recipient = 0, add = 0;
			r.U32(recipient);
			r.U32(add);
			uint64_t w = (uint64_t)remote_window_ + add;
			remote_window_ = (uint32_t)std::min<uint64_t>(w, 0xFFFFFFFFu);
			return true;
		}
		case CHANNEL_EOF:
			// The client will send no more input: let the shell see EOF, or end
			// an SFTP session.
			if (mode_ == Mode::Shell && shell_fd_ >= 0) {
				::shutdown(shell_fd_, SHUT_WR);
			} else {
				CloseChannel(0);
			}
			return true;
		case CHANNEL_CLOSE:
			peer_closed_ = true;
			if (!closing_ && channel_open_) {
				tr_->Write(Writer().Byte(CHANNEL_CLOSE).U32(remote_id_).Data());
			}
			channel_open_ = false;
			closing_ = true;
			return false;
		case GLOBAL_REQUEST: {
			std::string name;
			bool want = false;
			r.String(name);
			r.Bool(want);
			// keepalive@openssh.com arrives here with want_reply set, and
			// REQUEST_FAILURE is the answer OpenSSH expects from a server that
			// does not implement it.
			return !want || tr_->Write(std::string(1, (char)REQUEST_FAILURE));
		}
		case USERAUTH_REQUEST:
			// RFC 4252 §5.1: ignored once authenticated.
			return true;
		default:
			return true;
		}
	}

	// ---- the shell bridge -----------------------------------------------------

	void StartShell() {
		int sv[2];
		if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
			return;
		}
		shell_fd_ = sv[0];
		mode_ = Mode::Shell;
		ShellRequest req;
		req.username = username_;
		req.peer_ip = peer_;
		req.term = pty_ ? term_ : std::string();
		req.cols = cols_ > 0 ? cols_ : 80;
		req.rows = rows_ > 0 ? rows_ : 24;
		int bbs_fd = sv[1];
		duckdb::DatabaseInstance *db = &db_;
		ShellRunner run = shell_;
		shell_thread_ = std::thread([db, run, bbs_fd, req]() {
			// The stream owns bbs_fd and closes it on the way out, which is what
			// the loop above sees as the shell ending.
			net::ClientStream s(bbs_fd);
			run(*db, s, req);
		});
	}

	void StopShell() {
		if (shell_fd_ >= 0) {
			// Wake the shell out of whatever read it is in; it sees EOF, unwinds,
			// and releases its connection before we return and the database can
			// go away underneath it.
			::shutdown(shell_fd_, SHUT_RDWR);
		}
		if (shell_thread_.joinable()) {
			shell_thread_.join();
		}
		if (shell_fd_ >= 0) {
			::close(shell_fd_);
			shell_fd_ = -1;
		}
		if (session_id_ > 0) {
			citadel::UnregisterSession(con_, session_id_);
			session_id_ = 0;
		}
	}

	enum class Mode { None, Shell, Sftp };

	duckdb::DatabaseInstance &db_;
	net::ClientStream &stream_;
	ShellRunner shell_;
	Connection con_;
	std::unique_ptr<Transport> tr_;
	std::string peer_;
	std::string username_;

	Mode mode_ = Mode::None;
	bool channel_open_ = false;
	bool closing_ = false;
	bool close_after_flush_ = false;
	bool peer_closed_ = false;
	std::chrono::steady_clock::time_point close_deadline_;
	uint32_t exit_status_ = 0;
	uint32_t remote_id_ = 0;
	uint32_t remote_window_ = 0;
	uint32_t remote_max_packet_ = kMaxChannelPacket;
	uint32_t local_window_ = 0;
	std::string pending_;

	bool pty_ = false;
	std::string term_;
	int cols_ = 0;
	int rows_ = 0;
	int shell_fd_ = -1;
	bool shell_eof_ = false;
	std::thread shell_thread_;

	std::unique_ptr<SftpServer> sftp_;
	int64_t session_id_ = 0;
};

} // namespace

bool ResolveUser(Connection &con, const std::string &typed, std::string &canonical) {
	if (typed.empty() || typed.size() > 64) {
		return false;
	}
	const char *base = "SELECT username FROM quackmail_users WHERE enabled = true AND ";
	auto exact = MatchUsers(con, std::string(base) + "username = $1", typed);
	if (exact.size() == 1) {
		canonical = exact[0];
		return true;
	}
	// Case-insensitive, then with the stand-ins for a space. Each only when it
	// names exactly one account: an ambiguous login matches nobody.
	auto folded = MatchUsers(con, std::string(base) + "lower(username) = lower($1)", typed);
	if (folded.size() == 1) {
		canonical = folded[0];
		return true;
	}
	std::string spaced = typed;
	std::replace(spaced.begin(), spaced.end(), '_', ' ');
	std::replace(spaced.begin(), spaced.end(), '.', ' ');
	if (spaced != typed) {
		auto sp = MatchUsers(con, std::string(base) + "lower(username) = lower($1)", spaced);
		if (sp.size() == 1) {
			canonical = sp[0];
			return true;
		}
	}
	return false;
}

void Serve(duckdb::DatabaseInstance &db, net::ClientStream &stream, ShellRunner shell) {
	Session s(db, stream, shell);
	s.Run();
}

} // namespace sshd
} // namespace quackmail
