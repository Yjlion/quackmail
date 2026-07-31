#include "quackmail/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace quackmail {
namespace json {

namespace {

const Value &NullValue() {
	static const Value null_value;
	return null_value;
}

bool IsSpace(char c) {
	// RFC 8259 whitespace, and only that: a vertical tab or a form feed between
	// tokens is not JSON however forgiving it would be to accept one.
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Append a code point as UTF-8.
void AppendUtf8(std::string &out, uint32_t cp) {
	if (cp < 0x80) {
		out += (char)cp;
	} else if (cp < 0x800) {
		out += (char)(0xC0 | (cp >> 6));
		out += (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += (char)(0xE0 | (cp >> 12));
		out += (char)(0x80 | ((cp >> 6) & 0x3F));
		out += (char)(0x80 | (cp & 0x3F));
	} else {
		out += (char)(0xF0 | (cp >> 18));
		out += (char)(0x80 | ((cp >> 12) & 0x3F));
		out += (char)(0x80 | ((cp >> 6) & 0x3F));
		out += (char)(0x80 | (cp & 0x3F));
	}
}

int HexVal(unsigned char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

// A recursive-descent parser over the whole buffer at once. JSON documents here
// are request bodies that have already been read in full, so there is no
// incremental mode to justify — unlike xmlstream, which needs one for XMPP.
class Parser {
public:
	Parser(const std::string &in, const Limits &limits) : in_(in), limits_(limits) {
	}

	bool Run(Value &out) {
		SkipSpace();
		if (!ParseValue(out, 0)) {
			return false;
		}
		SkipSpace();
		if (pos_ != in_.size()) {
			// Trailing content. Two concatenated documents are not one document,
			// and accepting the first would let a request smuggle a second past
			// anything that logged only what it parsed.
			return Fail("trailing content after the top-level value");
		}
		return true;
	}

	const std::string &Error() const {
		return err_;
	}

private:
	bool Fail(const char *why) {
		if (err_.empty()) {
			err_ = why;
		}
		return false;
	}

	void SkipSpace() {
		while (pos_ < in_.size() && IsSpace(in_[pos_])) {
			pos_++;
		}
	}

	bool Budget() {
		if (++nodes_ > limits_.max_nodes) {
			return Fail("too many values");
		}
		return true;
	}

	bool ParseValue(Value &out, size_t depth) {
		if (depth > limits_.max_depth) {
			return Fail("nested too deeply");
		}
		if (!Budget()) {
			return false;
		}
		if (pos_ >= in_.size()) {
			return Fail("unexpected end of input");
		}
		switch (in_[pos_]) {
		case '{':
			return ParseObject(out, depth);
		case '[':
			return ParseArray(out, depth);
		case '"': {
			out = Value();
			out.type = Value::String;
			return ParseString(out.str);
		}
		case 't':
			return Literal("true", Value::MakeBool(true), out);
		case 'f':
			return Literal("false", Value::MakeBool(false), out);
		case 'n':
			return Literal("null", Value(), out);
		default:
			return ParseNumber(out);
		}
	}

	bool Literal(const char *word, const Value &v, Value &out) {
		size_t n = std::char_traits<char>::length(word);
		if (in_.compare(pos_, n, word) != 0) {
			return Fail("not a JSON value");
		}
		pos_ += n;
		out = v;
		return true;
	}

	bool ParseObject(Value &out, size_t depth) {
		out = Value::MakeObject();
		pos_++; // '{'
		SkipSpace();
		if (pos_ < in_.size() && in_[pos_] == '}') {
			pos_++;
			return true;
		}
		for (;;) {
			SkipSpace();
			if (pos_ >= in_.size() || in_[pos_] != '"') {
				return Fail("expected a member name");
			}
			std::string name;
			if (!ParseString(name)) {
				return false;
			}
			SkipSpace();
			if (pos_ >= in_.size() || in_[pos_] != ':') {
				return Fail("expected ':' after a member name");
			}
			pos_++;
			SkipSpace();
			Value member;
			if (!ParseValue(member, depth + 1)) {
				return false;
			}
			// A duplicate name is kept rather than merged. Get() returns the
			// first, which is the only interpretation that cannot be steered by
			// appending a second copy of a field a validator already read.
			out.members.push_back(std::make_pair(name, member));
			SkipSpace();
			if (pos_ < in_.size() && in_[pos_] == ',') {
				pos_++;
				continue;
			}
			if (pos_ < in_.size() && in_[pos_] == '}') {
				pos_++;
				return true;
			}
			return Fail("expected ',' or '}'");
		}
	}

	bool ParseArray(Value &out, size_t depth) {
		out = Value::MakeArray();
		pos_++; // '['
		SkipSpace();
		if (pos_ < in_.size() && in_[pos_] == ']') {
			pos_++;
			return true;
		}
		for (;;) {
			SkipSpace();
			Value item;
			if (!ParseValue(item, depth + 1)) {
				return false;
			}
			out.items.push_back(item);
			SkipSpace();
			if (pos_ < in_.size() && in_[pos_] == ',') {
				pos_++;
				continue;
			}
			if (pos_ < in_.size() && in_[pos_] == ']') {
				pos_++;
				return true;
			}
			return Fail("expected ',' or ']'");
		}
	}

	bool ParseString(std::string &out) {
		pos_++; // opening quote
		out.clear();
		while (pos_ < in_.size()) {
			unsigned char c = (unsigned char)in_[pos_];
			if (c == '"') {
				pos_++;
				if (!ValidUtf8(out)) {
					return Fail("a string is not valid UTF-8");
				}
				return true;
			}
			if (out.size() > limits_.max_string) {
				return Fail("string too long");
			}
			if (c < 0x20) {
				// A literal control character inside a string is invalid JSON.
				// Accepting one would let a raw newline through into anything
				// that later treated the value as a header or a log line.
				return Fail("unescaped control character in a string");
			}
			if (c != '\\') {
				out += (char)c;
				pos_++;
				continue;
			}
			pos_++;
			if (pos_ >= in_.size()) {
				return Fail("unterminated escape");
			}
			char esc = in_[pos_++];
			switch (esc) {
			case '"':
				out += '"';
				break;
			case '\\':
				out += '\\';
				break;
			case '/':
				out += '/';
				break;
			case 'b':
				out += '\b';
				break;
			case 'f':
				out += '\f';
				break;
			case 'n':
				out += '\n';
				break;
			case 'r':
				out += '\r';
				break;
			case 't':
				out += '\t';
				break;
			case 'u': {
				uint32_t cp = 0;
				if (!Hex4(cp)) {
					return false;
				}
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					// A high surrogate must be followed by its low half. An
					// unpaired one is not a code point, and encoding it as
					// UTF-8 anyway is exactly the CESU-8 that makes two systems
					// disagree about what a string is.
					if (pos_ + 1 >= in_.size() || in_[pos_] != '\\' || in_[pos_ + 1] != 'u') {
						return Fail("unpaired surrogate");
					}
					pos_ += 2;
					uint32_t lo = 0;
					if (!Hex4(lo)) {
						return false;
					}
					if (lo < 0xDC00 || lo > 0xDFFF) {
						return Fail("unpaired surrogate");
					}
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
					return Fail("unpaired surrogate");
				}
				AppendUtf8(out, cp);
				break;
			}
			default:
				return Fail("unknown escape");
			}
		}
		return Fail("unterminated string");
	}

	bool Hex4(uint32_t &out) {
		if (pos_ + 4 > in_.size()) {
			return Fail("truncated \\u escape");
		}
		out = 0;
		for (int i = 0; i < 4; i++) {
			int v = HexVal((unsigned char)in_[pos_ + i]);
			if (v < 0) {
				return Fail("bad \\u escape");
			}
			out = (out << 4) | (uint32_t)v;
		}
		pos_ += 4;
		return true;
	}

	bool ParseNumber(Value &out) {
		size_t start = pos_;
		if (pos_ < in_.size() && in_[pos_] == '-') {
			pos_++;
		}
		// JSON forbids a leading zero followed by more digits, and forbids the
		// leading '+' and the bare '.5' that strtod would happily accept — so
		// the grammar is walked here rather than delegated.
		if (pos_ >= in_.size()) {
			return Fail("truncated number");
		}
		if (in_[pos_] == '0') {
			pos_++;
		} else if (in_[pos_] >= '1' && in_[pos_] <= '9') {
			while (pos_ < in_.size() && in_[pos_] >= '0' && in_[pos_] <= '9') {
				pos_++;
			}
		} else {
			return Fail("not a JSON value");
		}

		bool integral = true;
		if (pos_ < in_.size() && in_[pos_] == '.') {
			integral = false;
			pos_++;
			size_t digits = 0;
			while (pos_ < in_.size() && in_[pos_] >= '0' && in_[pos_] <= '9') {
				pos_++;
				digits++;
			}
			if (digits == 0) {
				return Fail("a fraction needs a digit");
			}
		}
		if (pos_ < in_.size() && (in_[pos_] == 'e' || in_[pos_] == 'E')) {
			integral = false;
			pos_++;
			if (pos_ < in_.size() && (in_[pos_] == '+' || in_[pos_] == '-')) {
				pos_++;
			}
			size_t digits = 0;
			while (pos_ < in_.size() && in_[pos_] >= '0' && in_[pos_] <= '9') {
				pos_++;
				digits++;
			}
			if (digits == 0) {
				return Fail("an exponent needs a digit");
			}
		}

		std::string text = in_.substr(start, pos_ - start);
		out = Value();
		out.type = Value::Number;
		out.number = std::strtod(text.c_str(), nullptr);
		out.integral = integral;
		return true;
	}

	const std::string &in_;
	const Limits &limits_;
	size_t pos_ = 0;
	size_t nodes_ = 0;
	std::string err_;
};

} // namespace

Value::Value() {
}

Limits::Limits() {
}

bool ValidUtf8(const std::string &s) {
	size_t i = 0;
	while (i < s.size()) {
		unsigned char c = (unsigned char)s[i];
		size_t extra = 0;
		uint32_t cp = 0;
		if (c < 0x80) {
			i++;
			continue;
		} else if ((c & 0xE0) == 0xC0) {
			extra = 1;
			cp = c & 0x1F;
		} else if ((c & 0xF0) == 0xE0) {
			extra = 2;
			cp = c & 0x0F;
		} else if ((c & 0xF8) == 0xF0) {
			extra = 3;
			cp = c & 0x07;
		} else {
			return false; // a continuation byte or an F8..FF lead
		}
		if (i + extra >= s.size()) {
			return false;
		}
		for (size_t k = 1; k <= extra; k++) {
			unsigned char cont = (unsigned char)s[i + k];
			if ((cont & 0xC0) != 0x80) {
				return false;
			}
			cp = (cp << 6) | (cont & 0x3F);
		}
		// Overlong encodings, the surrogate range and anything past U+10FFFF are
		// all well-formed-looking byte sequences that do not denote a character.
		// Letting them through is how two systems come to disagree about whether
		// two strings are equal.
		if (extra == 1 && cp < 0x80) {
			return false;
		}
		if (extra == 2 && cp < 0x800) {
			return false;
		}
		if (extra == 3 && cp < 0x10000) {
			return false;
		}
		if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
			return false;
		}
		i += extra + 1;
	}
	return true;
}

// ---- reading -------------------------------------------------------------

bool Value::AsBool(bool dflt) const {
	return type == Boolean ? boolean : dflt;
}

double Value::AsDouble(double dflt) const {
	return type == Number ? number : dflt;
}

int64_t Value::AsInt(int64_t dflt) const {
	if (type != Number) {
		return dflt;
	}
	// Out of range rather than wrapping: a client that sends 1e308 for a limit
	// gets the default, not undefined behaviour from the conversion.
	if (!(number >= -9.2233720368547758e18 && number <= 9.2233720368547758e18)) {
		return dflt;
	}
	return (int64_t)number;
}

std::string Value::AsString(const std::string &dflt) const {
	return type == String ? str : dflt;
}

const Value *Value::Get(const std::string &name) const {
	if (type != Object) {
		return nullptr;
	}
	for (const auto &m : members) {
		if (m.first == name) {
			return &m.second;
		}
	}
	return nullptr;
}

bool Value::Has(const std::string &name) const {
	return Get(name) != nullptr;
}

const Value &Value::operator[](const std::string &name) const {
	const Value *v = Get(name);
	return v ? *v : NullValue();
}

size_t Value::Size() const {
	return type == Array ? items.size() : 0;
}

const Value &Value::At(size_t i) const {
	if (type != Array || i >= items.size()) {
		return NullValue();
	}
	return items[i];
}

// ---- building ------------------------------------------------------------

Value Value::MakeString(const std::string &s) {
	Value v;
	v.type = String;
	v.str = s;
	return v;
}

Value Value::MakeInt(int64_t n) {
	Value v;
	v.type = Number;
	v.number = (double)n;
	v.integral = true;
	return v;
}

Value Value::MakeDouble(double n) {
	Value v;
	v.type = Number;
	v.number = n;
	v.integral = false;
	return v;
}

Value Value::MakeBool(bool b) {
	Value v;
	v.type = Boolean;
	v.boolean = b;
	return v;
}

Value Value::MakeArray() {
	Value v;
	v.type = Array;
	return v;
}

Value Value::MakeObject() {
	Value v;
	v.type = Object;
	return v;
}

void Value::Set(const std::string &name, const Value &v) {
	if (type != Object) {
		type = Object;
	}
	for (auto &m : members) {
		if (m.first == name) {
			m.second = v;
			return;
		}
	}
	members.push_back(std::make_pair(name, v));
}

void Value::Push(const Value &v) {
	if (type != Array) {
		type = Array;
	}
	items.push_back(v);
}

// ---- parsing -------------------------------------------------------------

bool Parse(const std::string &in, Value &out, std::string &err, const Limits &limits) {
	Parser p(in, limits);
	if (!p.Run(out)) {
		err = p.Error();
		out = Value();
		return false;
	}
	err.clear();
	return true;
}

bool Parse(const std::string &in, Value &out, std::string &err) {
	Limits limits;
	return Parse(in, out, err, limits);
}

bool Parse(const std::string &in, Value &out) {
	std::string ignored;
	return Parse(in, out, ignored);
}

// ---- serializing ---------------------------------------------------------

namespace {

void WriteString(std::string &out, const std::string &s) {
	out += '"';
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04X", c);
				out += buf;
			} else {
				// Everything else, UTF-8 included, goes out as itself. The
				// response is served as application/json with an explicit
				// charset, so there is nothing for \u escaping to protect.
				out += (char)c;
			}
		}
	}
	out += '"';
}

void WriteNumber(std::string &out, const Value &v) {
	if (!(v.number == v.number) || v.number == HUGE_VAL || v.number == -HUGE_VAL) {
		// JSON has no NaN or infinity. Emitting `null` is what every other
		// implementation does and keeps the document parseable, which matters
		// more here than signalling a bug in the producer.
		out += "null";
		return;
	}
	char buf[40];
	if (v.integral && v.number >= -9.007199254740992e15 && v.number <= 9.007199254740992e15) {
		std::snprintf(buf, sizeof(buf), "%lld", (long long)v.number);
	} else {
		// 17 significant digits round-trips an IEEE double exactly.
		std::snprintf(buf, sizeof(buf), "%.17g", v.number);
	}
	out += buf;
}

void Write(std::string &out, const Value &v) {
	switch (v.type) {
	case Value::Null:
		out += "null";
		return;
	case Value::Boolean:
		out += v.boolean ? "true" : "false";
		return;
	case Value::Number:
		WriteNumber(out, v);
		return;
	case Value::String:
		WriteString(out, v.str);
		return;
	case Value::Array:
		out += '[';
		for (size_t i = 0; i < v.items.size(); i++) {
			if (i) {
				out += ',';
			}
			Write(out, v.items[i]);
		}
		out += ']';
		return;
	case Value::Object:
		out += '{';
		for (size_t i = 0; i < v.members.size(); i++) {
			if (i) {
				out += ',';
			}
			WriteString(out, v.members[i].first);
			out += ':';
			Write(out, v.members[i].second);
		}
		out += '}';
		return;
	}
}

} // namespace

std::string Serialize(const Value &v) {
	std::string out;
	Write(out, v);
	return out;
}

} // namespace json
} // namespace quackmail
