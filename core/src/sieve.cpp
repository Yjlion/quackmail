#include "quackmail/sieve.hpp"

#include "quackmail/wildmat.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>

namespace quackmail {
namespace sieve {

using duckdb::Connection;
using duckdb::Value;

namespace {

std::string ToLower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string Trim(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return "";
	}
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

// ---------------------------------------------------------------------------
// Lexer (RFC 5228 §8.1)
// ---------------------------------------------------------------------------

struct Token {
	enum Kind {
		END,
		IDENTIFIER, // require, if, header, fileinto, ...
		TAG,        // :contains, :is, :over
		NUMBER,
		STRING,
		LBRACKET, // [
		RBRACKET, // ]
		LPAREN,   // (
		RPAREN,   // )
		LBRACE,   // {
		RBRACE,   // }
		COMMA,
		SEMICOLON,
	};
	Kind kind = END;
	std::string text;  // identifier/tag name (without ':'), or string contents
	long long number = 0;
	int line = 1;
};

class Lexer {
public:
	explicit Lexer(const std::string &src) : src_(src) {
	}

	// Tokenize the whole script. Returns false with `err` on a lexical error.
	bool Run(std::vector<Token> &out, std::string &err) {
		while (true) {
			SkipWhitespaceAndComments();
			if (pos_ >= src_.size()) {
				Token t;
				t.kind = Token::END;
				t.line = line_;
				out.push_back(t);
				return true;
			}
			Token t;
			t.line = line_;
			char c = src_[pos_];

			switch (c) {
			case '[':
				t.kind = Token::LBRACKET;
				pos_++;
				out.push_back(t);
				continue;
			case ']':
				t.kind = Token::RBRACKET;
				pos_++;
				out.push_back(t);
				continue;
			case '(':
				t.kind = Token::LPAREN;
				pos_++;
				out.push_back(t);
				continue;
			case ')':
				t.kind = Token::RPAREN;
				pos_++;
				out.push_back(t);
				continue;
			case '{':
				t.kind = Token::LBRACE;
				pos_++;
				out.push_back(t);
				continue;
			case '}':
				t.kind = Token::RBRACE;
				pos_++;
				out.push_back(t);
				continue;
			case ',':
				t.kind = Token::COMMA;
				pos_++;
				out.push_back(t);
				continue;
			case ';':
				t.kind = Token::SEMICOLON;
				pos_++;
				out.push_back(t);
				continue;
			default:
				break;
			}

			if (c == '"') {
				if (!ReadQuotedString(t, err)) {
					return false;
				}
				out.push_back(t);
				continue;
			}
			if (c == ':') {
				pos_++;
				std::string name;
				while (pos_ < src_.size() && (std::isalnum((unsigned char)src_[pos_]) || src_[pos_] == '_' ||
				                              src_[pos_] == '.' || src_[pos_] == '-')) {
					name += src_[pos_++];
				}
				if (name.empty()) {
					err = "stray ':' on line " + std::to_string(line_);
					return false;
				}
				t.kind = Token::TAG;
				t.text = ToLower(name);
				out.push_back(t);
				continue;
			}
			if (std::isdigit((unsigned char)c)) {
				long long v = 0;
				while (pos_ < src_.size() && std::isdigit((unsigned char)src_[pos_])) {
					v = v * 10 + (src_[pos_++] - '0');
				}
				// RFC 5228 §2.4.2: a K/M/G suffix scales the number.
				if (pos_ < src_.size()) {
					char suffix = (char)std::tolower((unsigned char)src_[pos_]);
					if (suffix == 'k') {
						v *= 1024;
						pos_++;
					} else if (suffix == 'm') {
						v *= 1024 * 1024;
						pos_++;
					} else if (suffix == 'g') {
						v *= 1024LL * 1024LL * 1024LL;
						pos_++;
					}
				}
				t.kind = Token::NUMBER;
				t.number = v;
				out.push_back(t);
				continue;
			}
			if (std::isalpha((unsigned char)c) || c == '_') {
				std::string name;
				while (pos_ < src_.size() &&
				       (std::isalnum((unsigned char)src_[pos_]) || src_[pos_] == '_')) {
					name += src_[pos_++];
				}
				// "text:" starts a multi-line literal running to a lone ".".
				if (ToLower(name) == "text" && pos_ < src_.size() && src_[pos_] == ':') {
					pos_++;
					if (!ReadMultiline(t, err)) {
						return false;
					}
					out.push_back(t);
					continue;
				}
				t.kind = Token::IDENTIFIER;
				t.text = ToLower(name);
				out.push_back(t);
				continue;
			}

			err = std::string("unexpected character '") + c + "' on line " + std::to_string(line_);
			return false;
		}
	}

private:
	void SkipWhitespaceAndComments() {
		while (pos_ < src_.size()) {
			char c = src_[pos_];
			if (c == '\n') {
				line_++;
				pos_++;
			} else if (std::isspace((unsigned char)c)) {
				pos_++;
			} else if (c == '#') {
				while (pos_ < src_.size() && src_[pos_] != '\n') {
					pos_++;
				}
			} else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '*') {
				pos_ += 2;
				while (pos_ + 1 < src_.size() && !(src_[pos_] == '*' && src_[pos_ + 1] == '/')) {
					if (src_[pos_] == '\n') {
						line_++;
					}
					pos_++;
				}
				pos_ = std::min(pos_ + 2, src_.size());
			} else {
				return;
			}
		}
	}

	bool ReadQuotedString(Token &t, std::string &err) {
		pos_++; // opening quote
		std::string value;
		while (pos_ < src_.size() && src_[pos_] != '"') {
			if (src_[pos_] == '\\' && pos_ + 1 < src_.size()) {
				pos_++; // only '\\' and '\"' are defined; others pass through
			}
			if (src_[pos_] == '\n') {
				line_++;
			}
			value += src_[pos_++];
		}
		if (pos_ >= src_.size()) {
			err = "unterminated string starting on line " + std::to_string(t.line);
			return false;
		}
		pos_++; // closing quote
		t.kind = Token::STRING;
		t.text = value;
		return true;
	}

	bool ReadMultiline(Token &t, std::string &err) {
		// Skip to the end of the "text:" line.
		while (pos_ < src_.size() && src_[pos_] != '\n') {
			pos_++;
		}
		if (pos_ < src_.size()) {
			pos_++;
			line_++;
		}
		std::string value;
		while (pos_ < src_.size()) {
			size_t eol = src_.find('\n', pos_);
			std::string raw_line =
			    src_.substr(pos_, eol == std::string::npos ? std::string::npos : eol - pos_);
			pos_ = eol == std::string::npos ? src_.size() : eol + 1;
			line_++;
			std::string stripped = raw_line;
			if (!stripped.empty() && stripped.back() == '\r') {
				stripped.pop_back();
			}
			if (stripped == ".") {
				t.kind = Token::STRING;
				t.text = value;
				return true;
			}
			// Dot-stuffing, as in SMTP.
			if (stripped.size() >= 2 && stripped[0] == '.' && stripped[1] == '.') {
				stripped.erase(0, 1);
			}
			value += stripped;
			value += "\r\n";
		}
		err = "unterminated 'text:' block";
		return false;
	}

	const std::string &src_;
	size_t pos_ = 0;
	int line_ = 1;
};

// ---------------------------------------------------------------------------
// AST
// ---------------------------------------------------------------------------

struct Test;
using TestPtr = std::unique_ptr<Test>;

struct Test {
	enum Kind {
		TRUE,
		FALSE,
		NOT,
		ANYOF,
		ALLOF,
		HEADER,
		ADDRESS,
		ENVELOPE,
		EXISTS,
		SIZE,
		BODY,
	};
	Kind kind = TRUE;
	std::vector<TestPtr> children; // NOT/ANYOF/ALLOF
	std::vector<std::string> names;  // header names / envelope parts
	std::vector<std::string> keys;   // values to match against
	std::string match_type = "is";   // is | contains | matches
	std::string comparator = "i;ascii-casemap";
	std::string address_part = "all"; // all | localpart | domain
	std::string size_relation;        // over | under
	long long size_limit = 0;
};

struct Command;
using CommandPtr = std::unique_ptr<Command>;

struct Command {
	enum Kind { REQUIRE, IF, STOP, KEEP, DISCARD, FILEINTO, REDIRECT, REJECT };
	Kind kind = STOP;
	// IF: a chain of (test, block) branches plus an optional trailing else.
	struct Branch {
		TestPtr test; // null for the trailing `else`
		std::vector<CommandPtr> body;
	};
	std::vector<Branch> branches;
	std::string argument; // fileinto folder / redirect address / reject reason
	bool create = false;  // fileinto :create
	bool copy = false;    // :copy — act without cancelling the implicit keep
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

class Parser {
public:
	Parser(std::vector<Token> tokens) : t_(std::move(tokens)) {
	}

	bool ParseScript(std::vector<CommandPtr> &out, std::string &err) {
		while (Peek().kind != Token::END) {
			CommandPtr c;
			if (!ParseCommand(c, err)) {
				return false;
			}
			if (c) {
				out.push_back(std::move(c));
			}
		}
		return true;
	}

private:
	const Token &Peek(size_t ahead = 0) const {
		size_t i = std::min(pos_ + ahead, t_.size() - 1);
		return t_[i];
	}
	const Token &Next() {
		const Token &t = t_[std::min(pos_, t_.size() - 1)];
		if (pos_ < t_.size() - 1) {
			pos_++;
		}
		return t;
	}
	bool Accept(Token::Kind k) {
		if (Peek().kind == k) {
			Next();
			return true;
		}
		return false;
	}
	bool Expect(Token::Kind k, const char *what, std::string &err) {
		if (Accept(k)) {
			return true;
		}
		err = std::string("expected ") + what + " on line " + std::to_string(Peek().line);
		return false;
	}

	// A string, or a bracketed list of strings.
	bool ParseStringList(std::vector<std::string> &out, std::string &err) {
		if (Peek().kind == Token::STRING) {
			out.push_back(Next().text);
			return true;
		}
		if (!Expect(Token::LBRACKET, "a string or string list", err)) {
			return false;
		}
		while (true) {
			if (Peek().kind != Token::STRING) {
				err = "expected a string inside the list on line " + std::to_string(Peek().line);
				return false;
			}
			out.push_back(Next().text);
			if (Accept(Token::COMMA)) {
				continue;
			}
			break;
		}
		return Expect(Token::RBRACKET, "']'", err);
	}

	// Consume the tagged arguments common to the match tests.
	void ParseMatchTags(Test &test) {
		while (Peek().kind == Token::TAG) {
			std::string tag = Peek().text;
			if (tag == "is" || tag == "contains" || tag == "matches") {
				test.match_type = tag;
				Next();
			} else if (tag == "all" || tag == "localpart" || tag == "domain") {
				test.address_part = tag;
				Next();
			} else if (tag == "comparator") {
				Next();
				if (Peek().kind == Token::STRING) {
					test.comparator = ToLower(Next().text);
				}
			} else if (tag == "over" || tag == "under") {
				test.size_relation = tag;
				Next();
			} else {
				// An unrecognised tag with no argument; skip it rather than
				// failing the whole script.
				Next();
			}
		}
	}

	bool ParseTest(TestPtr &out, std::string &err) {
		if (Peek().kind != Token::IDENTIFIER) {
			err = "expected a test on line " + std::to_string(Peek().line);
			return false;
		}
		std::string name = Next().text;
		auto test = std::unique_ptr<Test>(new Test());

		if (name == "true") {
			test->kind = Test::TRUE;
			out = std::move(test);
			return true;
		}
		if (name == "false") {
			test->kind = Test::FALSE;
			out = std::move(test);
			return true;
		}
		if (name == "not") {
			test->kind = Test::NOT;
			TestPtr child;
			if (!ParseTest(child, err)) {
				return false;
			}
			test->children.push_back(std::move(child));
			out = std::move(test);
			return true;
		}
		if (name == "anyof" || name == "allof") {
			test->kind = name == "anyof" ? Test::ANYOF : Test::ALLOF;
			if (!Expect(Token::LPAREN, "'(' after anyof/allof", err)) {
				return false;
			}
			while (true) {
				TestPtr child;
				if (!ParseTest(child, err)) {
					return false;
				}
				test->children.push_back(std::move(child));
				if (Accept(Token::COMMA)) {
					continue;
				}
				break;
			}
			if (!Expect(Token::RPAREN, "')'", err)) {
				return false;
			}
			out = std::move(test);
			return true;
		}
		if (name == "size") {
			test->kind = Test::SIZE;
			ParseMatchTags(*test);
			if (Peek().kind != Token::NUMBER) {
				err = "size test needs a number on line " + std::to_string(Peek().line);
				return false;
			}
			test->size_limit = Next().number;
			if (test->size_relation.empty()) {
				err = "size test needs :over or :under on line " + std::to_string(Peek().line);
				return false;
			}
			out = std::move(test);
			return true;
		}
		if (name == "exists") {
			test->kind = Test::EXISTS;
			if (!ParseStringList(test->names, err)) {
				return false;
			}
			out = std::move(test);
			return true;
		}
		if (name == "header" || name == "address" || name == "envelope") {
			test->kind = name == "header" ? Test::HEADER
			                              : (name == "address" ? Test::ADDRESS : Test::ENVELOPE);
			ParseMatchTags(*test);
			if (!ParseStringList(test->names, err)) {
				return false;
			}
			ParseMatchTags(*test);
			if (!ParseStringList(test->keys, err)) {
				return false;
			}
			out = std::move(test);
			return true;
		}
		if (name == "body") {
			test->kind = Test::BODY;
			ParseMatchTags(*test);
			if (!ParseStringList(test->keys, err)) {
				return false;
			}
			out = std::move(test);
			return true;
		}

		err = "unknown test '" + name + "' on line " + std::to_string(Peek().line);
		return false;
	}

	bool ParseBlock(std::vector<CommandPtr> &out, std::string &err) {
		if (!Expect(Token::LBRACE, "'{'", err)) {
			return false;
		}
		while (Peek().kind != Token::RBRACE) {
			if (Peek().kind == Token::END) {
				err = "unterminated block";
				return false;
			}
			CommandPtr c;
			if (!ParseCommand(c, err)) {
				return false;
			}
			if (c) {
				out.push_back(std::move(c));
			}
		}
		return Expect(Token::RBRACE, "'}'", err);
	}

	bool ParseCommand(CommandPtr &out, std::string &err) {
		if (Peek().kind != Token::IDENTIFIER) {
			err = "expected a command on line " + std::to_string(Peek().line);
			return false;
		}
		std::string name = Next().text;
		auto cmd = std::unique_ptr<Command>(new Command());

		if (name == "require") {
			cmd->kind = Command::REQUIRE;
			std::vector<std::string> ignored;
			if (!ParseStringList(ignored, err)) {
				return false;
			}
			// The capability list is advisory here: an unimplemented extension
			// simply has no effect, which is safer than refusing to filter.
			if (!Expect(Token::SEMICOLON, "';'", err)) {
				return false;
			}
			out = std::move(cmd);
			return true;
		}

		if (name == "if") {
			cmd->kind = Command::IF;
			while (true) {
				Command::Branch branch;
				if (!ParseTest(branch.test, err)) {
					return false;
				}
				if (!ParseBlock(branch.body, err)) {
					return false;
				}
				cmd->branches.push_back(std::move(branch));

				if (Peek().kind == Token::IDENTIFIER && Peek().text == "elsif") {
					Next();
					continue;
				}
				if (Peek().kind == Token::IDENTIFIER && Peek().text == "else") {
					Next();
					Command::Branch tail; // null test marks the else arm
					if (!ParseBlock(tail.body, err)) {
						return false;
					}
					cmd->branches.push_back(std::move(tail));
				}
				break;
			}
			out = std::move(cmd);
			return true;
		}

		if (name == "elsif" || name == "else") {
			err = "'" + name + "' without a matching 'if' on line " + std::to_string(Peek().line);
			return false;
		}

		if (name == "stop") {
			cmd->kind = Command::STOP;
		} else if (name == "keep") {
			cmd->kind = Command::KEEP;
		} else if (name == "discard") {
			cmd->kind = Command::DISCARD;
		} else if (name == "fileinto") {
			cmd->kind = Command::FILEINTO;
		} else if (name == "redirect") {
			cmd->kind = Command::REDIRECT;
		} else if (name == "reject" || name == "ereject") {
			cmd->kind = Command::REJECT;
		} else {
			err = "unknown command '" + name + "' on line " + std::to_string(Peek().line);
			return false;
		}

		// Tagged arguments, then the optional string argument.
		while (Peek().kind == Token::TAG) {
			std::string tag = Next().text;
			if (tag == "create") {
				cmd->create = true;
			} else if (tag == "copy") {
				cmd->copy = true;
			}
		}
		if (Peek().kind == Token::STRING) {
			cmd->argument = Next().text;
		}
		if (!Expect(Token::SEMICOLON, "';'", err)) {
			return false;
		}
		out = std::move(cmd);
		return true;
	}

	std::vector<Token> t_;
	size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

struct Context {
	const mime::ParsedMessage *msg = nullptr;
	const std::string *raw = nullptr;
	const Envelope *env = nullptr;
};

// All header values for `name`, case-insensitively (a header may repeat).
std::vector<std::string> HeaderValues(const mime::ParsedMessage &msg, const std::string &name) {
	std::vector<std::string> out;
	std::string want = ToLower(Trim(name));
	for (auto &h : msg.headers) {
		if (ToLower(Trim(h.first)) == want) {
			out.push_back(Trim(h.second));
		}
	}
	return out;
}

bool MatchOne(const std::string &value, const std::string &key, const std::string &match_type,
              const std::string &comparator) {
	// i;octet is case sensitive; every other comparator we support folds case.
	bool fold = comparator != "i;octet";
	std::string v = fold ? ToLower(value) : value;
	std::string k = fold ? ToLower(key) : key;

	if (match_type == "contains") {
		return v.find(k) != std::string::npos;
	}
	if (match_type == "matches") {
		// Sieve's :matches uses '*' and '?' with the same meaning as wildmat,
		// so the existing matcher covers it. Escape wildmat's extra syntax
		// (',' separates alternatives, '!' negates) so it cannot leak in.
		std::string pattern;
		for (char c : k) {
			if (c == ',' || c == '!' || c == '[' || c == ']') {
				pattern += '\\';
			}
			pattern += c;
		}
		return WildmatMatch(v, pattern);
	}
	return v == k; // :is
}

// Apply the address-part selector to an addr-spec.
std::string AddressPart(const std::string &addr, const std::string &part) {
	if (part == "localpart") {
		auto at = addr.rfind('@');
		return at == std::string::npos ? addr : addr.substr(0, at);
	}
	if (part == "domain") {
		auto at = addr.rfind('@');
		return at == std::string::npos ? "" : addr.substr(at + 1);
	}
	return addr;
}

bool EvalTest(const Test &test, const Context &ctx);

bool MatchAny(const std::vector<std::string> &values, const std::vector<std::string> &keys,
              const std::string &match_type, const std::string &comparator) {
	for (auto &v : values) {
		for (auto &k : keys) {
			if (MatchOne(v, k, match_type, comparator)) {
				return true;
			}
		}
	}
	return false;
}

// Test carries unique_ptr children, so it is move-only; pass its fields rather
// than copying it to tweak the match type.
bool MatchAny(const std::vector<std::string> &values, const Test &test) {
	return MatchAny(values, test.keys, test.match_type, test.comparator);
}

bool EvalTest(const Test &test, const Context &ctx) {
	switch (test.kind) {
	case Test::TRUE:
		return true;
	case Test::FALSE:
		return false;
	case Test::NOT:
		return test.children.empty() ? false : !EvalTest(*test.children[0], ctx);
	case Test::ANYOF:
		for (auto &c : test.children) {
			if (EvalTest(*c, ctx)) {
				return true;
			}
		}
		return false;
	case Test::ALLOF:
		for (auto &c : test.children) {
			if (!EvalTest(*c, ctx)) {
				return false;
			}
		}
		return true;

	case Test::EXISTS:
		for (auto &n : test.names) {
			if (HeaderValues(*ctx.msg, n).empty()) {
				return false; // every named header must be present
			}
		}
		return true;

	case Test::SIZE: {
		long long size = (long long)ctx.raw->size();
		return test.size_relation == "over" ? size > test.size_limit : size < test.size_limit;
	}

	case Test::HEADER: {
		std::vector<std::string> values;
		for (auto &n : test.names) {
			auto vs = HeaderValues(*ctx.msg, n);
			values.insert(values.end(), vs.begin(), vs.end());
		}
		return MatchAny(values, test);
	}

	case Test::ADDRESS: {
		std::vector<std::string> values;
		for (auto &n : test.names) {
			for (auto &raw_value : HeaderValues(*ctx.msg, n)) {
				for (auto &a : mime::ParseAddressList(raw_value)) {
					if (!a.addr.empty()) {
						values.push_back(AddressPart(a.addr, test.address_part));
					}
				}
			}
		}
		return MatchAny(values, test);
	}

	case Test::ENVELOPE: {
		std::vector<std::string> values;
		for (auto &n : test.names) {
			std::string part = ToLower(Trim(n));
			// The envelope test names transaction parts, not header names.
			if (part == "from") {
				values.push_back(AddressPart(ctx.env->mail_from, test.address_part));
			} else if (part == "to") {
				values.push_back(AddressPart(ctx.env->rcpt_to, test.address_part));
			}
		}
		return MatchAny(values, test);
	}

	case Test::BODY: {
		// The body is everything after the header/body separator.
		auto sep = ctx.raw->find("\r\n\r\n");
		size_t start = sep == std::string::npos ? ctx.raw->find("\n\n") : sep + 4;
		if (sep == std::string::npos && start != std::string::npos) {
			start += 2;
		}
		std::string body = start == std::string::npos ? "" : ctx.raw->substr(start);
		std::vector<std::string> values {body};
		// :is against a whole body is near-useless, so a body test with no
		// explicit match type searches for a substring instead.
		std::string match_type = test.match_type == "is" ? "contains" : test.match_type;
		return MatchAny(values, test.keys, match_type, test.comparator);
	}
	}
	return false;
}

// Running state while walking the command list.
struct Runner {
	Context ctx;
	std::vector<Action> actions;
	bool cancel_implicit_keep = false;
	bool stopped = false;

	void AddAction(Action a) {
		actions.push_back(std::move(a));
	}

	void Run(const std::vector<CommandPtr> &block) {
		for (auto &cmd : block) {
			if (stopped) {
				return;
			}
			switch (cmd->kind) {
			case Command::REQUIRE:
				break;
			case Command::STOP:
				stopped = true;
				return;
			case Command::IF: {
				for (auto &branch : cmd->branches) {
					// A null test is the `else` arm.
					if (!branch.test || EvalTest(*branch.test, ctx)) {
						Run(branch.body);
						break;
					}
				}
				break;
			}
			case Command::KEEP: {
				AddAction(Action(Action::KEEP));
				cancel_implicit_keep = true;
				break;
			}
			case Command::DISCARD:
				// §4.2: discard cancels the implicit keep and adds nothing.
				cancel_implicit_keep = true;
				break;
			case Command::FILEINTO: {
				Action a(Action::FILEINTO);
				a.folder = cmd->argument;
				a.create = cmd->create;
				AddAction(std::move(a));
				// :copy leaves the implicit keep in place (RFC 3894).
				if (!cmd->copy) {
					cancel_implicit_keep = true;
				}
				break;
			}
			case Command::REDIRECT: {
				Action a(Action::REDIRECT);
				a.address = cmd->argument;
				AddAction(std::move(a));
				if (!cmd->copy) {
					cancel_implicit_keep = true;
				}
				break;
			}
			case Command::REJECT: {
				Action a(Action::REJECT);
				a.reason = cmd->argument;
				AddAction(std::move(a));
				cancel_implicit_keep = true;
				stopped = true;
				return;
			}
			}
		}
	}
};

bool ParseInternal(const std::string &script, std::vector<CommandPtr> &out, std::string &err) {
	std::vector<Token> tokens;
	Lexer lexer(script);
	if (!lexer.Run(tokens, err)) {
		return false;
	}
	Parser parser(std::move(tokens));
	return parser.ParseScript(out, err);
}

} // namespace

Result Evaluate(const std::string &script, const mime::ParsedMessage &msg, const std::string &raw,
                const Envelope &env) {
	Result result;
	if (Trim(script).empty()) {
		result.actions.push_back(Action(Action::KEEP));
		return result;
	}

	std::vector<CommandPtr> program;
	std::string err;
	if (!ParseInternal(script, program, err)) {
		// A broken filter must never lose mail: fall back to plain delivery and
		// report the reason so an admin can see it.
		result.error = err;
		result.actions.push_back(Action(Action::KEEP));
		return result;
	}

	Runner runner;
	runner.ctx.msg = &msg;
	runner.ctx.raw = &raw;
	runner.ctx.env = &env;
	runner.Run(program);

	result.actions = std::move(runner.actions);

	// A reject stands alone — nothing is delivered.
	for (auto &a : result.actions) {
		if (a.type == Action::REJECT) {
			Action only = a;
			result.actions.clear();
			result.actions.push_back(only);
			return result;
		}
	}

	if (!runner.cancel_implicit_keep) {
		// §2.10.2: with no delivering action taken, the message is still kept.
		result.actions.push_back(Action(Action::KEEP));
	} else if (result.actions.empty()) {
		// Everything cancelled and nothing added: an explicit discard.
		result.actions.push_back(Action(Action::DISCARD));
	}
	return result;
}

bool Check(const std::string &script, std::string &err) {
	err.clear();
	if (Trim(script).empty()) {
		return true; // an empty script is legal and does nothing
	}
	std::vector<CommandPtr> program;
	return ParseInternal(script, program, err);
}

std::string LoadActiveScript(Connection &con, const std::string &username) {
	auto stmt = con.Prepare("SELECT script FROM quackmail_sieve_scripts "
	                        "WHERE username = $1 AND active = true LIMIT 1");
	if (stmt->HasError()) {
		return "";
	}
	duckdb::vector<Value> params = {Value(username)};
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		return "";
	}
	auto &mat = r->Cast<duckdb::MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return "";
	}
	Value v = mat.GetValue(0, 0);
	if (v.IsNull()) {
		return "";
	}
	return v.ToString();
}

std::string Capabilities() {
	// Only what the evaluator above actually implements.
	return "fileinto reject envelope body copy";
}

} // namespace sieve
} // namespace quackmail
