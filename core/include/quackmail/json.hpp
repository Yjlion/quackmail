#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace quackmail {
namespace json {

// RFC 8259 JSON, sized for JMAP.
//
// Written rather than vendored for the same reason everything else here is: the
// extension build has no package manager, and DuckDB's bundled yyjson is not
// exposed to extensions. It is deliberately small — a JMAP request is a few
// nested objects, not a document format.
//
// The input arrives from the network, so the parser refuses rather than
// recurses: depth, node count and string length are all bounded, and every
// reader below has a default, so a missing or wrong-typed field is a value
// rather than a crash. That posture is the whole point — a JMAP method call is
// attacker-shaped by definition.

struct Value {
	// Members are ordered, which JMAP does not require and which makes a
	// response reproducible enough to assert from a test.
	enum Type { Null, Boolean, Number, String, Array, Object };

	Type type = Null;
	bool boolean = false;
	double number = 0;
	// Whether `number` came in (or was built) as an integer. Serialize uses it
	// to write 3 rather than 3.0, which matters because JMAP ids are strings but
	// its sizes and counts are numbers a client may compare exactly.
	bool integral = false;
	std::string str;
	// A vector of the enclosing type, as mime::MimeEntity already does for its
	// children. Only one of these is ever populated.
	std::vector<Value> items;                           // Array
	std::vector<std::pair<std::string, Value>> members; // Object

	Value();

	// ---- reading ---------------------------------------------------------
	// None of these can fail. A field that is absent, null or of the wrong type
	// yields the default, because every one of them is reachable from a request
	// body and "the client sent a number where a string goes" is an ordinary
	// Tuesday rather than an exceptional condition.
	bool IsNull() const {
		return type == Null;
	}
	bool AsBool(bool dflt = false) const;
	double AsDouble(double dflt = 0) const;
	int64_t AsInt(int64_t dflt = 0) const;
	std::string AsString(const std::string &dflt = std::string()) const;

	// Object member lookup; nullptr when absent or when this is not an object.
	const Value *Get(const std::string &name) const;
	bool Has(const std::string &name) const;
	// The same, but never null: an absent member reads as a Null value, so a
	// chain like req["arguments"]["filter"]["inMailbox"] cannot fault.
	const Value &operator[](const std::string &name) const;

	// Array access. Size() is 0 for anything that is not an array, and At()
	// past the end returns Null rather than faulting.
	size_t Size() const;
	const Value &At(size_t i) const;

	// ---- building --------------------------------------------------------
	static Value MakeString(const std::string &s);
	static Value MakeInt(int64_t n);
	static Value MakeDouble(double n);
	static Value MakeBool(bool b);
	static Value MakeArray();
	static Value MakeObject();

	// Object: replace the member if it exists, append otherwise.
	void Set(const std::string &name, const Value &v);
	void Set(const std::string &name, const std::string &v) {
		Set(name, MakeString(v));
	}
	// Not merely a convenience: without it, Set("x", "hello") binds the string
	// literal to the bool overload, because pointer-to-bool is a standard
	// conversion and const char*-to-std::string is a user-defined one.
	void Set(const std::string &name, const char *v) {
		Set(name, MakeString(v ? std::string(v) : std::string()));
	}
	void Set(const std::string &name, int64_t v) {
		Set(name, MakeInt(v));
	}
	void Set(const std::string &name, bool v) {
		Set(name, MakeBool(v));
	}
	// Array: append.
	void Push(const Value &v);
};

struct Limits {
	size_t max_depth = 64;
	size_t max_nodes = 200000;
	size_t max_string = 16u * 1024 * 1024;

	Limits();
};

// Parse a complete document. False on malformed input, on trailing content, or
// on any limit above — `err` gets a short reason, which JMAP surfaces as its
// `notJSON` / `notRequest` problem types rather than swallowing.
//
// Strings are validated as UTF-8: overlong encodings, unpaired surrogates and
// anything past U+10FFFF are rejected on the way in, so nothing downstream has
// to wonder whether a std::string it was handed is well-formed.
bool Parse(const std::string &in, Value &out, std::string &err, const Limits &limits);
bool Parse(const std::string &in, Value &out, std::string &err);
bool Parse(const std::string &in, Value &out);

// Compact serialization: no spaces, no newlines. Control characters, '"' and
// '\\' are escaped; valid UTF-8 passes through as itself rather than as \u
// escapes, which is both smaller and what every JMAP implementation emits.
std::string Serialize(const Value &v);

// Whether a string is well-formed UTF-8. Exposed because the parser needs it
// and so does anything about to put a stored value into a JSON response.
bool ValidUtf8(const std::string &s);

} // namespace json
} // namespace quackmail
