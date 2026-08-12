#include "quackmail/sieve.hpp"

#include "quackmail/wildmat.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
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
		ALWAYS,
		NEVER,
		NOT,
		ANYOF,
		ALLOF,
		HEADER,
		ADDRESS,
		ENVELOPE,
		EXISTS,
		SIZE,
		BODY,
		STRING,  // RFC 5229 §5 — match against a source string, not the message
		HASFLAG, // RFC 5232 §5 — match against the internal flag set
	};
	Kind kind = ALWAYS;
	std::vector<TestPtr> children; // NOT/ANYOF/ALLOF
	std::vector<std::string> names;  // header names / envelope parts / STRING sources
	std::vector<std::string> keys;   // values to match against
	std::string match_type = "is";   // is | contains | matches
	std::string comparator = "i;ascii-casemap";
	std::string address_part = "all"; // all | localpart | domain | user | detail
	std::string size_relation;        // over | under
	long long size_limit = 0;
};

struct Command;
using CommandPtr = std::unique_ptr<Command>;

struct Command {
	enum Kind {
		REQUIRE,
		IF,
		STOP,
		KEEP,
		DISCARD,
		FILEINTO,
		REDIRECT,
		REJECT,
		SET,        // RFC 5229 §4
		SETFLAG,    // RFC 5232 §5
		ADDFLAG,    //
		REMOVEFLAG, //
		VACATION,   // RFC 5230
	};
	Kind kind = STOP;
	// IF: a chain of (test, block) branches plus an optional trailing else.
	struct Branch {
		TestPtr test; // null for the trailing `else`
		std::vector<CommandPtr> body;
	};
	std::vector<Branch> branches;
	std::string argument; // fileinto folder / redirect address / reject or vacation reason
	bool create = false;  // fileinto :create
	bool copy = false;    // :copy — act without cancelling the implicit keep

	// `set` (name, value) and its modifiers, in the order they were written:
	// RFC 5229 §4.1 orders them by precedence, not by appearance, so they are
	// sorted at evaluation rather than kept as flags.
	std::string var_name;
	std::vector<std::string> modifiers;

	// imap4flags: the list argument of setflag/addflag/removeflag, and the
	// `:flags` tag on keep/fileinto. `has_flags` distinguishes ":flags []"
	// (file it with no flags) from no tag at all (use the internal set).
	std::vector<std::string> flags;
	bool has_flags = false;

	// REQUIRE: the extensions the script asked for. Kept rather than discarded
	// for exactly one of them — see the note on `variables` in Evaluate.
	std::vector<std::string> capabilities;

	// vacation's tagged arguments.
	std::string vac_subject;
	std::string vac_from;
	std::string vac_handle;
	std::vector<std::string> vac_addresses;
	long long vac_days = 0; // 0 = not given
	bool vac_mime = false;
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
		// `[]` is a legal, empty list. `fileinto :flags [] "X"` is how RFC 5232
		// says "file it carrying no flags at all", which is a different thing
		// from not writing the tag.
		if (Peek().kind == Token::RBRACKET) {
			return Expect(Token::RBRACKET, "']'", err);
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
			} else if (tag == "all" || tag == "localpart" || tag == "domain" || tag == "user" ||
			           tag == "detail") {
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
			test->kind = Test::ALWAYS;
			out = std::move(test);
			return true;
		}
		if (name == "false") {
			test->kind = Test::NEVER;
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
		if (name == "string") {
			// RFC 5229 §5. Two string lists: the sources (usually variable
			// references) and the keys. Unlike `header` there is no message
			// involved at all, which is what makes it useful after `set`.
			test->kind = Test::STRING;
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
		if (name == "hasflag") {
			// RFC 5232 §5. The optional first list names variables to read the
			// flags from; we only implement the internal flag set, so a leading
			// list is the variable-name form and the *last* list is always the
			// keys. Parsing both and keeping the last is what makes both forms
			// work without implementing external flag variables.
			test->kind = Test::HASFLAG;
			ParseMatchTags(*test);
			if (!ParseStringList(test->keys, err)) {
				return false;
			}
			ParseMatchTags(*test);
			if (Peek().kind == Token::STRING || Peek().kind == Token::LBRACKET) {
				test->names = test->keys;
				test->keys.clear();
				if (!ParseStringList(test->keys, err)) {
					return false;
				}
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
			if (!ParseStringList(cmd->capabilities, err)) {
				return false;
			}
			// The capability list stays advisory for everything but `variables`:
			// an unimplemented extension simply has no effect, which is safer
			// than refusing to filter. `variables` is the exception because
			// without it `${foo}` is *literal text* (RFC 5229 §3), so honouring
			// the require is what stops a script that never asked for it from
			// changing meaning underneath its author.
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
		} else if (name == "set") {
			cmd->kind = Command::SET;
		} else if (name == "setflag") {
			cmd->kind = Command::SETFLAG;
		} else if (name == "addflag") {
			cmd->kind = Command::ADDFLAG;
		} else if (name == "removeflag") {
			cmd->kind = Command::REMOVEFLAG;
		} else if (name == "vacation") {
			cmd->kind = Command::VACATION;
		} else {
			err = "unknown command '" + name + "' on line " + std::to_string(Peek().line);
			return false;
		}

		// Tagged arguments, then the positional ones.
		while (Peek().kind == Token::TAG) {
			std::string tag = Next().text;
			if (tag == "create") {
				cmd->create = true;
			} else if (tag == "copy") {
				cmd->copy = true;
			} else if (tag == "flags") {
				if (!ParseStringList(cmd->flags, err)) {
					return false;
				}
				cmd->has_flags = true;
			} else if (tag == "mime") {
				cmd->vac_mime = true;
			} else if (tag == "days" || tag == "seconds") {
				if (Peek().kind != Token::NUMBER) {
					err = "':" + tag + "' needs a number on line " + std::to_string(Peek().line);
					return false;
				}
				long long n = Next().number;
				// RFC 6131's :seconds and RFC 5230's :days measure the same
				// window; keep one field and round a sub-day :seconds up to the
				// shortest window we honour rather than to zero, which would
				// mean "reply every time".
				cmd->vac_days = tag == "days" ? n : (n + 86399) / 86400;
			} else if (tag == "subject" || tag == "from" || tag == "handle") {
				if (Peek().kind != Token::STRING) {
					err = "':" + tag + "' needs a string on line " + std::to_string(Peek().line);
					return false;
				}
				std::string v = Next().text;
				if (tag == "subject") {
					cmd->vac_subject = v;
				} else if (tag == "from") {
					cmd->vac_from = v;
				} else {
					cmd->vac_handle = v;
				}
			} else if (tag == "addresses") {
				if (!ParseStringList(cmd->vac_addresses, err)) {
					return false;
				}
			} else if (tag == "lower" || tag == "upper" || tag == "lowerfirst" ||
			           tag == "upperfirst" || tag == "length" || tag == "quotewildcard") {
				cmd->modifiers.push_back(tag);
			}
			// Anything else is an unrecognised tag with no argument; skip it
			// rather than failing the whole script.
		}

		if (cmd->kind == Command::SET) {
			// `set <name> <value>`, after the modifiers.
			if (Peek().kind != Token::STRING) {
				err = "'set' needs a variable name on line " + std::to_string(Peek().line);
				return false;
			}
			cmd->var_name = Next().text;
			if (Peek().kind != Token::STRING) {
				err = "'set' needs a value on line " + std::to_string(Peek().line);
				return false;
			}
			cmd->argument = Next().text;
		} else if (cmd->kind == Command::SETFLAG || cmd->kind == Command::ADDFLAG ||
		           cmd->kind == Command::REMOVEFLAG) {
			// `setflag [<variablename>] <list-of-flags>`. Only the internal flag
			// set is implemented, so when two lists are given the first names a
			// variable we do not keep and the flags are the second — the same
			// shape `hasflag` is parsed with.
			if (!ParseStringList(cmd->flags, err)) {
				return false;
			}
			if (Peek().kind == Token::STRING || Peek().kind == Token::LBRACKET) {
				cmd->flags.clear();
				if (!ParseStringList(cmd->flags, err)) {
					return false;
				}
			}
			cmd->has_flags = true;
		} else if (Peek().kind == Token::STRING) {
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

// ---- variables (RFC 5229) -------------------------------------------------

// The variable store. Bounded on purpose: this runs on the delivery path for
// every inbound message, and `set` inside a rule a sender can trigger is
// otherwise a memory amplifier pointed at the server by whoever chooses the
// message. Over the limit the assignment is dropped rather than truncated —
// half a value is a filter quietly doing something else.
struct Variables {
	static const size_t kMaxNames = 128;
	static const size_t kMaxValue = 4096;

	std::map<std::string, std::string> named; // keys lowercased: names fold case
	std::vector<std::string> captures;        // ${0}..${9} from the last :matches
	bool enabled = false;                     // require "variables" was present

	void Set(const std::string &name, const std::string &value) {
		if (value.size() > kMaxValue) {
			return;
		}
		std::string key = ToLower(Trim(name));
		if (named.find(key) == named.end() && named.size() >= kMaxNames) {
			return;
		}
		named[key] = value;
	}

	// Unset variables expand to the empty string (RFC 5229 §3).
	std::string Get(const std::string &name) const {
		std::string key = ToLower(Trim(name));
		if (key.size() == 1 && key[0] >= '0' && key[0] <= '9') {
			size_t i = (size_t)(key[0] - '0');
			return i < captures.size() ? captures[i] : std::string();
		}
		auto it = named.find(key);
		return it == named.end() ? std::string() : it->second;
	}
};

// A `${...}` reference is only a reference when what it wraps is a variable
// name: one or more digits, or an identifier optionally qualified by a
// namespace. Anything else — `${}`, `${a b}`, an unclosed brace — is literal
// text and RFC 5229 §3 requires it be left exactly as written.
bool ValidVariableName(const std::string &name) {
	if (name.empty() || name.size() > 128) {
		return false;
	}
	bool all_digits = true;
	for (char c : name) {
		if (c < '0' || c > '9') {
			all_digits = false;
			break;
		}
	}
	if (all_digits) {
		return name.size() == 1; // only ${0}..${9} exist
	}
	bool start = true;
	for (char c : name) {
		if (start) {
			if (!std::isalpha((unsigned char)c) && c != '_') {
				return false;
			}
			start = false;
			continue;
		}
		if (c == '.') {
			start = true; // a namespace separator; a name must follow
			continue;
		}
		if (!std::isalnum((unsigned char)c) && c != '_') {
			return false;
		}
	}
	return !start;
}

// Substitute `${name}` throughout. Not recursive: a value that itself contains
// `${...}` is data, not a further reference (RFC 5229 §3), which is what stops
// a message's own text from being read back as script.
std::string Expand(const std::string &in, const Variables &vars) {
	if (!vars.enabled || in.find("${") == std::string::npos) {
		return in;
	}
	std::string out;
	out.reserve(in.size());
	size_t i = 0;
	while (i < in.size()) {
		if (in[i] == '$' && i + 1 < in.size() && in[i + 1] == '{') {
			size_t close = in.find('}', i + 2);
			if (close != std::string::npos) {
				std::string name = in.substr(i + 2, close - i - 2);
				if (ValidVariableName(name)) {
					out += vars.Get(name);
					i = close + 1;
					continue;
				}
			}
		}
		out += in[i++];
	}
	return out;
}

std::vector<std::string> ExpandAll(const std::vector<std::string> &in, const Variables &vars) {
	if (!vars.enabled) {
		return in;
	}
	std::vector<std::string> out;
	out.reserve(in.size());
	for (auto &s : in) {
		out.push_back(Expand(s, vars));
	}
	return out;
}

// ---- `:matches`, with captures --------------------------------------------

// Sieve's `:matches` (RFC 5228 §2.7.1) with the capture RFC 5229 §4 needs for
// `${1}`..`${9}`.
//
// This is deliberately not `WildmatMatch`. That answers yes/no for NNTP's
// comma-separated, `!`-negated, `[class]`-bearing patterns — syntax Sieve does
// not have, which is why the caller used to escape it back out — and it cannot
// report which characters a wildcard consumed. It is also unbounded: its `*`
// tries every split recursively, so `*a*a*a*a*b` against a long value is
// exponential, and the value here is text a sender chooses. Hence the step
// budget: a pattern that cannot be decided within it fails rather than holding
// a delivery thread. Nothing anybody writes on purpose comes near.
struct Globber {
	const std::string &value;
	const std::string &pattern;
	bool fold;
	std::vector<std::string> *caps; // by wildcard ordinal, or null
	std::vector<size_t> ordinal;    // pattern index -> wildcard number
	long long steps = 0;

	static const long long kMaxSteps = 2000000;

	Globber(const std::string &v, const std::string &p, bool f, std::vector<std::string> *c)
	    : value(v), pattern(p), fold(f), caps(c) {
		ordinal.assign(pattern.size(), 0);
		size_t n = 0;
		for (size_t i = 0; i < pattern.size(); i++) {
			if (pattern[i] == '\\' && i + 1 < pattern.size()) {
				i++;
				continue;
			}
			if (pattern[i] == '*' || pattern[i] == '?') {
				ordinal[i] = ++n; // 1-based: ${0} is the whole match
			}
		}
		if (caps) {
			caps->assign(n + 1, std::string());
		}
	}

	bool Same(char a, char b) const {
		return fold ? std::tolower((unsigned char)a) == std::tolower((unsigned char)b) : a == b;
	}

	void Record(size_t which, size_t from, size_t to) {
		if (caps && which < caps->size()) {
			(*caps)[which] = value.substr(from, to - from);
		}
	}

	bool Match(size_t vi, size_t pi) {
		if (++steps > kMaxSteps) {
			return false;
		}
		while (pi < pattern.size()) {
			char pc = pattern[pi];
			if (pc == '\\' && pi + 1 < pattern.size()) {
				if (vi >= value.size() || !Same(value[vi], pattern[pi + 1])) {
					return false;
				}
				vi++;
				pi += 2;
				continue;
			}
			if (pc == '*') {
				size_t which = ordinal[pi];
				// Shortest first, so the leftmost wildcard takes as little as it
				// can — `*` against "a.b.c" with pattern "*.*" captures "a" and
				// "b.c" rather than "a.b" and "c".
				for (size_t k = vi; k <= value.size(); k++) {
					if (Match(k, pi + 1)) {
						Record(which, vi, k);
						return true;
					}
					if (steps > kMaxSteps) {
						return false;
					}
				}
				return false;
			}
			if (pc == '?') {
				if (vi >= value.size()) {
					return false;
				}
				Record(ordinal[pi], vi, vi + 1);
				vi++;
				pi++;
				continue;
			}
			if (vi >= value.size() || !Same(value[vi], pc)) {
				return false;
			}
			vi++;
			pi++;
		}
		return vi == value.size();
	}
};

bool GlobMatch(const std::string &value, const std::string &pattern, bool fold,
               std::vector<std::string> *caps) {
	Globber g(value, pattern, fold, caps);
	if (!g.Match(0, 0)) {
		return false;
	}
	if (caps && !caps->empty()) {
		(*caps)[0] = value; // ${0} is everything the pattern matched
	}
	return true;
}

struct Context {
	const mime::ParsedMessage *msg = nullptr;
	const std::string *raw = nullptr;
	const Envelope *env = nullptr;
	// Mutable during evaluation: a successful `:matches` sets ${1}..${9}, so a
	// test writes as well as reads. Both are owned by the Runner.
	Variables *vars = nullptr;
	const std::vector<std::string> *flags = nullptr; // imap4flags' internal set
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

// `caps`, when given, receives the wildcard captures of a successful `:matches`
// so the caller can publish them as ${1}..${9}.
bool MatchOne(const std::string &value, const std::string &key, const std::string &match_type,
              const std::string &comparator, std::vector<std::string> *caps = nullptr) {
	// i;octet is case sensitive; every other comparator we support folds case.
	bool fold = comparator != "i;octet";

	if (match_type == "matches") {
		return GlobMatch(value, key, fold, caps);
	}
	std::string v = fold ? ToLower(value) : value;
	std::string k = fold ? ToLower(key) : key;
	if (match_type == "contains") {
		return v.find(k) != std::string::npos;
	}
	return v == k; // :is
}

// Apply the address-part selector to an addr-spec. Returns false when the part
// does not exist, which RFC 5233 defines as NIL rather than the empty string:
// `envelope :detail "to" ""` must not match mail sent to a plain address. The
// caller expresses that by contributing no value at all to the match.
bool AddressPart(const std::string &addr, const std::string &part, const std::string &sep,
                 std::string &out) {
	auto at = addr.rfind('@');
	std::string local = at == std::string::npos ? addr : addr.substr(0, at);
	if (part == "localpart") {
		out = local;
		return true;
	}
	if (part == "domain") {
		out = at == std::string::npos ? "" : addr.substr(at + 1);
		return true;
	}
	// RFC 5233 subaddressing: ":user" is the local-part up to the separator,
	// ":detail" everything after it.
	if (part == "user" || part == "detail") {
		auto cut = sep.empty() ? std::string::npos : local.find(sep);
		if (part == "user") {
			out = cut == std::string::npos ? local : local.substr(0, cut);
			return true;
		}
		if (cut == std::string::npos) {
			return false; // not a subaddress: NIL
		}
		out = local.substr(cut + sep.size());
		return true;
	}
	out = addr;
	return true;
}

bool EvalTest(const Test &test, const Context &ctx);

// The keys are expanded before matching, and a successful `:matches` publishes
// its captures. The captures of a *failed* match are dropped: RFC 5229 §4 leaves
// them holding whatever the last successful match set, and overwriting them
// from a comparison that did not match would let a later rule read values from
// a test that never fired.
bool MatchAny(const std::vector<std::string> &values, const std::vector<std::string> &keys,
              const std::string &match_type, const std::string &comparator, const Context &ctx) {
	bool want_caps = ctx.vars && ctx.vars->enabled && match_type == "matches";
	std::vector<std::string> caps;
	for (auto &v : values) {
		for (auto &raw_key : keys) {
			std::string k = ctx.vars ? Expand(raw_key, *ctx.vars) : raw_key;
			if (MatchOne(v, k, match_type, comparator, want_caps ? &caps : nullptr)) {
				if (want_caps) {
					ctx.vars->captures = caps;
				}
				return true;
			}
		}
	}
	return false;
}

// Test carries unique_ptr children, so it is move-only; pass its fields rather
// than copying it to tweak the match type.
bool MatchAny(const std::vector<std::string> &values, const Test &test, const Context &ctx) {
	return MatchAny(values, test.keys, test.match_type, test.comparator, ctx);
}

bool EvalTest(const Test &test, const Context &ctx) {
	switch (test.kind) {
	case Test::ALWAYS:
		return true;
	case Test::NEVER:
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
			if (HeaderValues(*ctx.msg, ctx.vars ? Expand(n, *ctx.vars) : n).empty()) {
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
		for (auto &raw_name : test.names) {
			auto vs = HeaderValues(*ctx.msg, ctx.vars ? Expand(raw_name, *ctx.vars) : raw_name);
			values.insert(values.end(), vs.begin(), vs.end());
		}
		return MatchAny(values, test, ctx);
	}

	case Test::ADDRESS: {
		std::vector<std::string> values;
		for (auto &raw_name : test.names) {
			std::string n = ctx.vars ? Expand(raw_name, *ctx.vars) : raw_name;
			for (auto &raw_value : HeaderValues(*ctx.msg, n)) {
				for (auto &a : mime::ParseAddressList(raw_value)) {
					if (!a.addr.empty()) {
						std::string part;
						if (AddressPart(a.addr, test.address_part, "+", part)) {
							values.push_back(part);
						}
					}
				}
			}
		}
		return MatchAny(values, test, ctx);
	}

	case Test::ENVELOPE: {
		std::vector<std::string> values;
		for (auto &n : test.names) {
			std::string part = ToLower(Trim(n));
			// The envelope test names transaction parts, not header names.
			std::string value;
			if (part == "from") {
				if (AddressPart(ctx.env->mail_from, test.address_part, ctx.env->separator, value)) {
					values.push_back(value);
				}
			} else if (part == "to") {
				if (AddressPart(ctx.env->rcpt_to, test.address_part, ctx.env->separator, value)) {
					values.push_back(value);
				}
			}
		}
		return MatchAny(values, test, ctx);
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
		return MatchAny(values, test.keys, match_type, test.comparator, ctx);
	}

	case Test::STRING: {
		// RFC 5229 §5. The sources are ordinary strings, which is only useful
		// once they hold `${...}` — so they are expanded like any other
		// argument and matched with no message involved.
		std::vector<std::string> values;
		for (auto &raw_name : test.names) {
			values.push_back(ctx.vars ? Expand(raw_name, *ctx.vars) : raw_name);
		}
		return MatchAny(values, test, ctx);
	}

	case Test::HASFLAG: {
		// RFC 5232 §5, against the internal flag set as it stands right now —
		// which is the only flag set that exists during delivery. The message
		// has not been stored yet, so there is nothing else to ask.
		if (!ctx.flags) {
			return false;
		}
		return MatchAny(*ctx.flags, test, ctx);
	}
	}
	return false;
}

// ---- imap4flags bookkeeping (RFC 5232) ------------------------------------

// `setflag "\\Seen \\Answered"` is one string holding two flags (§4), so every
// list argument is re-split before it is used.
std::vector<std::string> SplitFlags(const std::vector<std::string> &in, const Variables &vars) {
	std::vector<std::string> out;
	for (auto &raw : in) {
		std::string s = Expand(raw, vars);
		size_t i = 0;
		while (i < s.size()) {
			while (i < s.size() && std::isspace((unsigned char)s[i])) {
				i++;
			}
			size_t start = i;
			while (i < s.size() && !std::isspace((unsigned char)s[i])) {
				i++;
			}
			if (i > start) {
				out.push_back(s.substr(start, i - start));
			}
		}
	}
	return out;
}

// The rule view refuses any script that requires `variables`, so splitting a
// flag list for it needs no expansion.
std::vector<std::string> SplitFlags(const std::vector<std::string> &in) {
	Variables none;
	return SplitFlags(in, none);
}

// IMAP's system flags fold case on the wire, so `\seen` and `\Seen` are one
// flag; a keyword is passed through as written. Canonicalizing here is what
// stops a script from filling citadel_msg_flags with spellings IMAP will not
// recognise as \Seen.
std::string CanonicalFlag(const std::string &flag) {
	static const char *kSystem[] = {"\\Seen", "\\Answered", "\\Flagged", "\\Deleted", "\\Draft"};
	for (auto *s : kSystem) {
		if (ToLower(flag) == ToLower(std::string(s))) {
			return s;
		}
	}
	return flag;
}

void AddFlags(std::vector<std::string> &set, const std::vector<std::string> &add) {
	for (auto &raw : add) {
		std::string f = CanonicalFlag(raw);
		if (std::find(set.begin(), set.end(), f) == set.end()) {
			set.push_back(f);
		}
	}
}

void RemoveFlags(std::vector<std::string> &set, const std::vector<std::string> &drop) {
	for (auto &raw : drop) {
		std::string f = CanonicalFlag(raw);
		set.erase(std::remove(set.begin(), set.end(), f), set.end());
	}
}

// ---- vacation's message-only rules (RFC 5230 §4.5, §4.6) ------------------

// Every address in a header, lowercased.
void CollectAddrs(const mime::ParsedMessage &msg, const char *header,
                  std::vector<std::string> &out) {
	for (auto &value : HeaderValues(msg, header)) {
		for (auto &a : mime::ParseAddressList(value)) {
			if (!a.addr.empty()) {
				out.push_back(ToLower(a.addr));
			}
		}
	}
}

// Why an auto-reply must not be sent, or nullptr when one is warranted.
//
// These are exactly the rules that are a pure function of the message, so they
// belong here rather than in the caller: every one of them fails *silently*
// against a real correspondent — an auto-reply to a mailing list is not an
// error anybody sees until it has gone to four hundred people — and here they
// can be pinned by a test with no mail server anywhere near.
const char *VacationRefusal(const Context &ctx, const std::vector<std::string> &addresses) {
	// An empty return-path is a bounce or a notification. Replying to one is
	// how two servers talk to each other forever.
	if (Trim(ctx.env->mail_from).empty()) {
		return "the message has no envelope sender";
	}
	// §4.6: never reply to something that was itself generated.
	for (auto &v : HeaderValues(*ctx.msg, "Auto-Submitted")) {
		std::string first = ToLower(Trim(v.substr(0, v.find(';'))));
		if (first != "no") {
			return "the message is Auto-Submitted";
		}
	}
	for (auto &v : HeaderValues(*ctx.msg, "Precedence")) {
		std::string p = ToLower(Trim(v));
		if (p == "bulk" || p == "list" || p == "junk") {
			return "the message is bulk or list mail";
		}
	}
	static const char *kListHeaders[] = {"List-Id",   "List-Help",      "List-Unsubscribe",
	                                     "List-Post", "List-Subscribe", "List-Owner",
	                                     "List-Archive"};
	for (auto *h : kListHeaders) {
		if (!HeaderValues(*ctx.msg, h).empty()) {
			return "the message came from a mailing list";
		}
	}

	// Who counts as "me": the envelope recipient plus every :addresses entry.
	std::vector<std::string> mine {ToLower(Trim(ctx.env->rcpt_to))};
	for (auto &a : addresses) {
		mine.push_back(ToLower(Trim(a)));
	}

	std::string sender = ToLower(Trim(ctx.env->mail_from));
	for (auto &m : mine) {
		if (!m.empty() && m == sender) {
			return "the message is from the user themselves";
		}
	}

	// §4.5: the user must actually be a recipient. Without this, being bcc'd on
	// a thread auto-replies to it, and so does every message a rule happened to
	// route here.
	std::vector<std::string> recipients;
	for (auto *h : {"To", "Cc", "Bcc", "Resent-To", "Resent-Cc"}) {
		CollectAddrs(*ctx.msg, h, recipients);
	}
	for (auto &m : mine) {
		if (m.empty()) {
			continue;
		}
		if (std::find(recipients.begin(), recipients.end(), m) != recipients.end()) {
			return nullptr;
		}
	}
	return "the user is not a visible recipient";
}

// Running state while walking the command list.
struct Runner {
	Context ctx;
	Variables vars;
	std::vector<std::string> flags; // imap4flags' internal set
	std::vector<Action> actions;
	bool cancel_implicit_keep = false;
	bool stopped = false;

	void AddAction(Action a) {
		actions.push_back(std::move(a));
	}

	// What a keep/fileinto carries: its own `:flags` when it has one, otherwise
	// the internal set as it stands at this instant.
	std::vector<std::string> FlagsFor(const Command &cmd) {
		std::vector<std::string> chosen = cmd.has_flags ? SplitFlags(cmd.flags, vars) : flags;
		std::vector<std::string> out;
		AddFlags(out, chosen);
		return out;
	}

	// RFC 5229 §4.1: modifiers apply by precedence, not in the order written.
	std::string ApplyModifiers(std::string value, const std::vector<std::string> &modifiers) {
		auto has = [&](const char *m) {
			return std::find(modifiers.begin(), modifiers.end(), m) != modifiers.end();
		};
		// 10 — :quotewildcard
		if (has("quotewildcard")) {
			std::string q;
			for (char c : value) {
				if (c == '*' || c == '?' || c == '\\') {
					q += '\\';
				}
				q += c;
			}
			value = q;
		}
		// 20 — :length wins over the case modifiers, and is a number, so
		// nothing after it would mean anything anyway.
		if (has("length")) {
			return std::to_string(value.size());
		}
		// 30/40 — case folding.
		if (has("lower")) {
			value = ToLower(value);
		} else if (has("upper")) {
			std::transform(value.begin(), value.end(), value.begin(),
			               [](unsigned char c) { return (char)std::toupper(c); });
		}
		if (!value.empty()) {
			if (has("lowerfirst")) {
				value[0] = (char)std::tolower((unsigned char)value[0]);
			} else if (has("upperfirst")) {
				value[0] = (char)std::toupper((unsigned char)value[0]);
			}
		}
		return value;
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
				Action a(Action::KEEP);
				a.flags = FlagsFor(*cmd);
				AddAction(std::move(a));
				cancel_implicit_keep = true;
				break;
			}
			case Command::DISCARD:
				// §4.2: discard cancels the implicit keep and adds nothing.
				cancel_implicit_keep = true;
				break;
			case Command::FILEINTO: {
				Action a(Action::FILEINTO);
				a.folder = Expand(cmd->argument, vars);
				a.create = cmd->create;
				a.flags = FlagsFor(*cmd);
				AddAction(std::move(a));
				// :copy leaves the implicit keep in place (RFC 3894).
				if (!cmd->copy) {
					cancel_implicit_keep = true;
				}
				break;
			}
			case Command::REDIRECT: {
				Action a(Action::REDIRECT);
				a.address = Expand(cmd->argument, vars);
				AddAction(std::move(a));
				if (!cmd->copy) {
					cancel_implicit_keep = true;
				}
				break;
			}
			case Command::REJECT: {
				Action a(Action::REJECT);
				a.reason = Expand(cmd->argument, vars);
				AddAction(std::move(a));
				cancel_implicit_keep = true;
				stopped = true;
				return;
			}
			case Command::SET:
				vars.Set(cmd->var_name, ApplyModifiers(Expand(cmd->argument, vars), cmd->modifiers));
				break;
			case Command::SETFLAG:
				flags.clear();
				AddFlags(flags, SplitFlags(cmd->flags, vars));
				break;
			case Command::ADDFLAG:
				AddFlags(flags, SplitFlags(cmd->flags, vars));
				break;
			case Command::REMOVEFLAG:
				RemoveFlags(flags, SplitFlags(cmd->flags, vars));
				break;
			case Command::VACATION: {
				// RFC 5230 §4.7: at most one reply per message, however many
				// times the command is reached.
				bool already = false;
				for (auto &existing : actions) {
					already = already || existing.type == Action::VACATION;
				}
				if (already) {
					break;
				}
				Action a(Action::VACATION);
				a.reason = Expand(cmd->argument, vars);
				a.vacation.subject = Expand(cmd->vac_subject, vars);
				a.vacation.from = Expand(cmd->vac_from, vars);
				a.vacation.handle = Expand(cmd->vac_handle, vars);
				a.vacation.addresses = ExpandAll(cmd->vac_addresses, vars);
				a.vacation.mime = cmd->vac_mime;
				// §4.1: the default window is 7 days, and a shorter one than a
				// day is refused rather than honoured — an auto-replier with no
				// floor answers a persistent correspondent every time.
				a.vacation.days = cmd->vac_days > 0 ? (int)std::min<long long>(cmd->vac_days, 365) : 7;
				if (a.vacation.days < 1) {
					a.vacation.days = 1;
				}
				// Everything the message alone can settle is settled here, so
				// what reaches the caller is "reply to this, subject to when you
				// last did" and nothing more.
				if (VacationRefusal(ctx, a.vacation.addresses) != nullptr) {
					break;
				}
				AddAction(std::move(a));
				// A vacation reply is not a delivery: the implicit keep stands,
				// which is why an out-of-office script is one line and still
				// puts the message in the inbox.
				break;
			}
			}
		}
	}
};

// Whether the script asked for an extension by name. Only the top level is
// looked at: RFC 5228 §3 requires `require` there, before any other command.
bool Requires(const std::vector<CommandPtr> &program, const std::string &capability) {
	for (auto &cmd : program) {
		if (!cmd || cmd->kind != Command::REQUIRE) {
			continue;
		}
		for (auto &c : cmd->capabilities) {
			if (ToLower(Trim(c)) == capability) {
				return true;
			}
		}
	}
	return false;
}

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
	runner.ctx.vars = &runner.vars;
	runner.ctx.flags = &runner.flags;
	// The one capability that is not advisory. Without `require "variables"` a
	// `${...}` in a string is text, and a script written before this extension
	// existed has to keep meaning what it meant then.
	runner.vars.enabled = Requires(program, "variables");
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
	// Only what the evaluator above actually implements. `mailbox` is here for
	// `fileinto :create` alone — RFC 5490's mailboxexists test is not
	// implemented — but the create half is what clients use it for, and a
	// fileinto has always created the room it names.
	return "fileinto reject envelope body copy subaddress mailbox variables imap4flags vacation";
}

// ---------------------------------------------------------------------------
// The rule view
// ---------------------------------------------------------------------------
//
// Decompose walks the same AST Evaluate runs, so what the UI shows and what the
// server does cannot drift. Anything the flat Rule shape cannot express is
// reported rather than approximated: a builder that quietly simplifies a rule is
// worse than one that admits it cannot show it.

namespace {

// The `# rule: <name>` comment above each `if`, in order of appearance. The lexer
// discards comments — reasonably, since Sieve has none that are semantic — so
// names are recovered from the raw text instead.
std::vector<std::string> RuleNames(const std::string &script) {
	std::vector<std::string> out;
	std::string pending;
	size_t i = 0;
	while (i <= script.size()) {
		size_t nl = script.find('\n', i);
		std::string text = script.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
		size_t b = text.find_first_not_of(" \t\r");
		std::string trimmed = (b == std::string::npos) ? std::string() : text.substr(b);
		while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == ' ')) {
			trimmed.pop_back();
		}

		if (trimmed.rfind("# rule:", 0) == 0) {
			pending = trimmed.substr(7);
			size_t nb = pending.find_first_not_of(" \t");
			pending = (nb == std::string::npos) ? std::string() : pending.substr(nb);
		} else if (trimmed.rfind("if ", 0) == 0 || trimmed == "if") {
			// One entry per `if`, named or not, so the nth name lines up with the
			// nth rule the parser produced.
			out.push_back(pending);
			pending.clear();
		} else if (!trimmed.empty() && trimmed[0] != '#') {
			// Any other statement between the comment and the `if` means the
			// comment was not a rule name after all.
			pending.clear();
		}

		if (nl == std::string::npos) {
			break;
		}
		i = nl + 1;
	}
	return out;
}

// A single header/size/body test -> RuleTest. False when it is a shape the flat
// view cannot show.
bool FlattenTest(const Test &t, bool negate, RuleTest &out, std::string &why) {
	switch (t.kind) {
	case Test::HEADER:
	case Test::ADDRESS: {
		if (t.names.size() != 1 || t.keys.size() != 1) {
			why = "a condition tests several headers or values at once";
			return false;
		}
		std::string name = ToLower(t.names[0]);
		// The four the UI names directly; anything else keeps its header name so
		// nothing is lost.
		if (name == "from" || name == "to" || name == "cc" || name == "subject") {
			out.field = name;
		} else {
			out.field = "header:" + t.names[0];
		}
		out.op = t.match_type.empty() ? "is" : t.match_type;
		out.value = t.keys[0];
		out.negate = negate;
		return true;
	}
	case Test::BODY: {
		if (t.keys.size() != 1) {
			why = "a body condition tests several values at once";
			return false;
		}
		out.field = "body";
		out.op = t.match_type.empty() ? "contains" : t.match_type;
		out.value = t.keys[0];
		out.negate = negate;
		return true;
	}
	case Test::SIZE: {
		out.field = "size";
		out.op = t.size_relation.empty() ? "over" : t.size_relation;
		out.value = std::to_string(t.size_limit);
		out.negate = negate;
		return true;
	}
	case Test::ENVELOPE:
		why = "a condition tests the SMTP envelope";
		return false;
	case Test::EXISTS:
		why = "a condition tests whether a header exists";
		return false;
	case Test::NEVER:
		why = "a rule can never run";
		return false;
	default:
		why = "a condition uses a form the rule editor does not model";
		return false;
	}
}

// The top-level test of an `if` -> (all, tests).
bool FlattenCondition(const Test &t, Rule &rule, std::string &why) {
	// `if true` is how the rule view spells an unconditional rule, and how
	// Compose writes one back. No tests is the representation; see Rule.
	if (t.kind == Test::ALWAYS) {
		rule.all = true;
		return true;
	}
	// `not` around one simple test is a negated condition; around anything else
	// it is not something the flat view can show.
	if (t.kind == Test::NOT) {
		if (t.children.size() != 1) {
			why = "a negated condition wraps more than one test";
			return false;
		}
		RuleTest rt;
		if (!FlattenTest(*t.children[0], true, rt, why)) {
			return false;
		}
		rule.all = true;
		rule.tests.push_back(rt);
		return true;
	}
	if (t.kind == Test::ALLOF || t.kind == Test::ANYOF) {
		rule.all = (t.kind == Test::ALLOF);
		for (auto &child : t.children) {
			RuleTest rt;
			bool negate = false;
			const Test *inner = child.get();
			if (inner->kind == Test::NOT) {
				if (inner->children.size() != 1) {
					why = "a negated condition wraps more than one test";
					return false;
				}
				negate = true;
				inner = inner->children[0].get();
			}
			if (!FlattenTest(*inner, negate, rt, why)) {
				return false;
			}
			rule.tests.push_back(rt);
		}
		return !rule.tests.empty();
	}
	RuleTest rt;
	if (!FlattenTest(t, false, rt, why)) {
		return false;
	}
	rule.all = true;
	rule.tests.push_back(rt);
	return true;
}

std::string QuoteSieve(const std::string &in) {
	std::string out = "\"";
	for (char c : in) {
		if (c == '\r' || c == '\n') {
			// A literal newline cannot appear in a quoted string, and a rule
			// value containing one is not something the builder produces.
			continue;
		}
		if (c == '"' || c == '\\') {
			out += '\\';
		}
		out += c;
	}
	return out + "\"";
}

// A `text:` block, for the one value that is genuinely multi-line: an
// out-of-office message. Quoting it would silently run every paragraph
// together, which is not a message anybody meant to send. Dot-stuffed exactly
// the way the lexer unstuffs it.
std::string SieveText(const std::string &in_raw) {
	// The lexer terminates every line it reads, so a value that already ends in
	// a newline would otherwise gain a blank line on each save. Dropping one
	// here is what makes the round trip stable.
	std::string in = in_raw;
	if (!in.empty() && in.back() == '\n') {
		in.pop_back();
	}
	if (!in.empty() && in.back() == '\r') {
		in.pop_back();
	}
	std::string out = "text:\n";
	size_t i = 0;
	while (i <= in.size()) {
		size_t nl = in.find('\n', i);
		std::string line = in.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (!line.empty() && line[0] == '.') {
			line.insert(line.begin(), '.');
		}
		out += line + "\n";
		if (nl == std::string::npos) {
			break;
		}
		i = nl + 1;
	}
	return out + ".\n";
}

// Quoted when it fits on one line, a text: block when it does not.
std::string QuoteOrText(const std::string &in) {
	if (in.find('\n') == std::string::npos && in.find('\r') == std::string::npos) {
		return QuoteSieve(in);
	}
	return SieveText(in);
}

} // namespace

bool Decompose(const std::string &script, std::vector<Rule> &out, std::string &why) {
	out.clear();
	why.clear();

	std::vector<CommandPtr> program;
	std::string err;
	if (!ParseInternal(script, program, err)) {
		why = "This script does not parse: " + err;
		return false;
	}

	// The one extension the rule view cannot describe at all. A variable's value
	// depends on what ran before it, and a list of independent rules has no
	// "before"; showing the rules without it would describe filtering that is
	// not what happens.
	if (Requires(program, "variables")) {
		why = "This script uses variables, which the rule editor cannot show.";
		return false;
	}

	auto names = RuleNames(script);
	size_t seen_ifs = 0;

	for (auto &cmd : program) {
		if (!cmd) {
			continue;
		}
		if (cmd->kind == Command::REQUIRE) {
			// Compose emits its own require line from the actions it writes, so
			// the original does not need preserving.
			continue;
		}
		if (cmd->kind != Command::IF) {
			why = "This script has actions outside any rule, which the rule editor cannot show.";
			return false;
		}

		// An `else` branch (a second branch, or a first with no test) has no
		// equivalent in a flat list of independent rules.
		if (cmd->branches.size() != 1 || !cmd->branches[0].test) {
			why = "This script uses an `else` branch, which the rule editor cannot show.";
			return false;
		}

		Rule rule;
		if (!FlattenCondition(*cmd->branches[0].test, rule, why)) {
			why = "This script uses a condition the rule editor cannot show: " + why + ".";
			return false;
		}

		for (auto &inner : cmd->branches[0].body) {
			if (!inner) {
				continue;
			}
			switch (inner->kind) {
			case Command::IF:
				why = "This script nests one rule inside another, which the rule editor cannot show.";
				return false;
			case Command::STOP:
				rule.stop = true;
				break;
			case Command::KEEP: {
				Action a(Action::KEEP);
				a.flags = SplitFlags(inner->flags);
				rule.actions.push_back(a);
				break;
			}
			case Command::DISCARD:
				rule.actions.push_back(Action(Action::DISCARD));
				break;
			case Command::FILEINTO: {
				Action a(Action::FILEINTO);
				a.folder = inner->argument;
				a.create = inner->create;
				a.flags = SplitFlags(inner->flags);
				rule.actions.push_back(a);
				break;
			}
			case Command::VACATION: {
				Action a(Action::VACATION);
				a.reason = inner->argument;
				a.vacation.subject = inner->vac_subject;
				a.vacation.from = inner->vac_from;
				a.vacation.handle = inner->vac_handle;
				a.vacation.addresses = inner->vac_addresses;
				a.vacation.mime = inner->vac_mime;
				a.vacation.days = inner->vac_days > 0 ? (int)inner->vac_days : 7;
				rule.actions.push_back(a);
				break;
			}
			case Command::REDIRECT: {
				Action a(Action::REDIRECT);
				a.address = inner->argument;
				rule.actions.push_back(a);
				break;
			}
			case Command::REJECT: {
				Action a(Action::REJECT);
				a.reason = inner->argument;
				rule.actions.push_back(a);
				break;
			}
			case Command::SET:
			case Command::SETFLAG:
			case Command::ADDFLAG:
			case Command::REMOVEFLAG:
				// These change state for whatever runs *after* them, and a flat
				// list of independent rules has no "after". The builder writes
				// flags as `:flags` on the action they belong to instead, which
				// says the same thing without the ordering.
				why = "This script sets flags or variables as separate commands, which the rule "
				      "editor cannot show.";
				return false;
			default:
				why = "This script uses an action the rule editor cannot show.";
				return false;
			}
		}

		// Names come from the text in the same order the parser produced the
		// `if`s, which is what lets a name survive a round trip.
		if (seen_ifs < names.size()) {
			rule.name = names[seen_ifs];
		}
		seen_ifs++;
		out.push_back(rule);
	}

	return true;
}

std::string Compose(const std::vector<Rule> &rules) {
	// Require only what is used. A script claiming to need `reject` when it does
	// not is misleading, and some servers refuse an unused require.
	std::vector<std::string> needed;
	auto need = [&](const char *cap) {
		if (std::find(needed.begin(), needed.end(), cap) == needed.end()) {
			needed.push_back(cap);
		}
	};
	for (auto &r : rules) {
		for (auto &a : r.actions) {
			if (a.type == Action::FILEINTO) {
				need("fileinto");
				if (a.create) {
					need("mailbox");
				}
			}
			if (a.type == Action::REJECT) {
				need("reject");
			}
			if (a.type == Action::VACATION) {
				need("vacation");
			}
			if (!a.flags.empty()) {
				need("imap4flags");
			}
		}
	}

	std::string out = "# Generated by the QuackCit rule editor.\n";
	out += "# Editing this text by hand is fine: the rules are read back from it.\n";
	if (!needed.empty()) {
		out += "require [";
		for (size_t i = 0; i < needed.size(); i++) {
			out += (i ? ", " : "") + QuoteSieve(needed[i]);
		}
		out += "];\n";
	}

	for (auto &r : rules) {
		if (r.actions.empty()) {
			continue; // an incomplete rule from a half-filled form
		}
		out += "\n";
		if (!r.name.empty()) {
			std::string name = r.name;
			for (auto &c : name) {
				if (c == '\r' || c == '\n') {
					c = ' ';
				}
			}
			out += "# rule: " + name + "\n";
		}

		std::vector<std::string> conditions;
		for (auto &t : r.tests) {
			std::string cond;
			if (t.field == "size") {
				cond = "size :" + std::string(t.op == "under" ? "under" : "over") + " " + t.value;
			} else if (t.field == "body") {
				cond = "body :text :" + t.op + " " + QuoteSieve(t.value);
			} else {
				std::string header = t.field;
				if (header.rfind("header:", 0) == 0) {
					header = header.substr(7);
				}
				cond = "header :" + t.op + " " + QuoteSieve(header) + " " + QuoteSieve(t.value);
			}
			if (t.negate) {
				cond = "not " + cond;
			}
			conditions.push_back(cond);
		}

		std::string test;
		if (conditions.empty()) {
			// An unconditional rule. `if true` rather than bare commands at the
			// top level so that a rule is always one `if`, which is what lets
			// `# rule:` names and rule ordering keep working unchanged.
			test = "true";
		} else if (conditions.size() == 1) {
			test = conditions[0];
		} else {
			test = std::string(r.all ? "allof" : "anyof") + " (";
			for (size_t i = 0; i < conditions.size(); i++) {
				test += (i ? ", " : "") + conditions[i];
			}
			test += ")";
		}
		out += "if " + test + " {\n";

		// `:flags ["\\Seen", "\\Flagged"]`, or "" when the action carries none.
		auto flag_tag = [](const Action &a) {
			if (a.flags.empty()) {
				return std::string();
			}
			std::string s = " :flags [";
			for (size_t i = 0; i < a.flags.size(); i++) {
				s += (i ? ", " : "") + QuoteSieve(a.flags[i]);
			}
			return s + "]";
		};

		for (auto &a : r.actions) {
			switch (a.type) {
			case Action::FILEINTO:
				// `:create` is read back by Decompose, so it has to be written
				// back too: dropping it here would silently rewrite a script
				// that asked for it the first time any other rule was touched.
				out += "    fileinto" + std::string(a.create ? " :create" : "") + flag_tag(a) + " " +
				       QuoteSieve(a.folder) + ";\n";
				break;
			case Action::REDIRECT:
				out += "    redirect " + QuoteSieve(a.address) + ";\n";
				break;
			case Action::REJECT:
				out += "    reject " + QuoteOrText(a.reason) + ";\n";
				break;
			case Action::DISCARD:
				out += "    discard;\n";
				break;
			case Action::VACATION: {
				out += "    vacation";
				if (a.vacation.days > 0 && a.vacation.days != 7) {
					out += " :days " + std::to_string(a.vacation.days);
				}
				if (!a.vacation.subject.empty()) {
					out += " :subject " + QuoteSieve(a.vacation.subject);
				}
				if (!a.vacation.from.empty()) {
					out += " :from " + QuoteSieve(a.vacation.from);
				}
				if (!a.vacation.handle.empty()) {
					out += " :handle " + QuoteSieve(a.vacation.handle);
				}
				if (!a.vacation.addresses.empty()) {
					out += " :addresses [";
					for (size_t i = 0; i < a.vacation.addresses.size(); i++) {
						out += (i ? ", " : "") + QuoteSieve(a.vacation.addresses[i]);
					}
					out += "]";
				}
				out += " " + QuoteOrText(a.reason) + ";\n";
				break;
			}
			case Action::KEEP:
			default:
				out += "    keep" + flag_tag(a) + ";\n";
				break;
			}
		}
		if (r.stop) {
			out += "    stop;\n";
		}
		out += "}\n";
	}
	return out;
}

} // namespace sieve
} // namespace quackmail
