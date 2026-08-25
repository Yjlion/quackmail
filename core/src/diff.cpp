#include "quackmail/diff.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace quackmail {
namespace diff {

namespace {

// A file as lines. `final_newline` is tracked separately because a unified diff
// has to say "\ No newline at end of file" for the last line when it is absent,
// and because losing it would silently rewrite a page on every round trip.
struct Lines {
	std::vector<std::string> v;
	bool final_newline = true;
};

Lines Split(const std::string &s) {
	Lines out;
	if (s.empty()) {
		out.final_newline = true;
		return out;
	}
	size_t start = 0;
	while (start <= s.size()) {
		size_t nl = s.find('\n', start);
		if (nl == std::string::npos) {
			if (start < s.size()) {
				out.v.push_back(s.substr(start));
				out.final_newline = false;
			}
			break;
		}
		out.v.push_back(s.substr(start, nl - start));
		start = nl + 1;
		if (start == s.size()) {
			out.final_newline = true;
			break;
		}
	}
	return out;
}

// Comparison keys. A file that ends without a newline differs from one that
// ends with it even when every line is identical, and the difference has to be
// visible to the trimming and to the diff itself or the edit vanishes. A real
// line never contains a newline, so this suffix cannot collide with content.
std::vector<std::string> Keys(const Lines &l) {
	std::vector<std::string> k = l.v;
	if (!k.empty() && !l.final_newline) {
		k.back() += "\n\x01no-final-newline";
	}
	return k;
}

std::string Join(const Lines &l) {
	std::string out;
	for (size_t i = 0; i < l.v.size(); i++) {
		out += l.v[i];
		if (i + 1 < l.v.size() || l.final_newline) {
			out += '\n';
		}
	}
	return out;
}

// One step of the edit script.
enum class Op { Keep, Del, Ins };
struct Edit {
	Op op;
	size_t index; // into `from` for Keep/Del, into `to` for Ins
};

// Myers' O((N+M)D) diff, with the trace kept so the script can be recovered.
//
// `budget` is the largest edit distance explored. Returning false means the two
// texts are further apart than that, and the caller falls back to replacing the
// whole file — which is what the algorithm would converge on anyway.
bool MyersScript(const std::vector<std::string> &a, const std::vector<std::string> &b,
                 size_t budget, std::vector<Edit> &out) {
	const int n = (int)a.size();
	const int m = (int)b.size();
	const int max_d = (int)std::min<size_t>(budget, (size_t)n + (size_t)m);

	std::vector<std::vector<int>> trace;
	std::vector<int> v(2 * (size_t)max_d + 3, 0);
	const int off = max_d + 1;

	int found_d = -1;
	for (int d = 0; d <= max_d; d++) {
		trace.push_back(v);
		for (int k = -d; k <= d; k += 2) {
			int x;
			if (k == -d || (k != d && v[off + k - 1] < v[off + k + 1])) {
				x = v[off + k + 1]; // down: an insertion from b
			} else {
				x = v[off + k - 1] + 1; // right: a deletion from a
			}
			int y = x - k;
			while (x < n && y < m && a[(size_t)x] == b[(size_t)y]) {
				x++;
				y++;
			}
			v[off + k] = x;
			if (x >= n && y >= m) {
				found_d = d;
				break;
			}
		}
		if (found_d >= 0) {
			break;
		}
	}
	if (found_d < 0) {
		return false; // over budget
	}

	// Walk the trace back to the origin, emitting the script in reverse.
	std::vector<Edit> rev;
	int x = n;
	int y = m;
	for (int d = found_d; d > 0; d--) {
		// trace[d] is V as it stood *before* step d ran, which is exactly the
		// d-1 frontier the predecessor of (x, y) lies on. Reading trace[d-1]
		// here instead is the classic off-by-one in this algorithm.
		const std::vector<int> &vp = trace[(size_t)d];
		int k = x - y;
		int prev_k;
		if (k == -d || (k != d && vp[off + k - 1] < vp[off + k + 1])) {
			prev_k = k + 1;
		} else {
			prev_k = k - 1;
		}
		int prev_x = vp[off + prev_k];
		int prev_y = prev_x - prev_k;
		while (x > prev_x && y > prev_y) {
			x--;
			y--;
			rev.push_back({Op::Keep, (size_t)x});
		}
		if (prev_k == k + 1) {
			y--;
			rev.push_back({Op::Ins, (size_t)y});
		} else {
			x--;
			rev.push_back({Op::Del, (size_t)x});
		}
		x = prev_x;
		y = prev_y;
	}
	while (x > 0 && y > 0) {
		x--;
		y--;
		rev.push_back({Op::Keep, (size_t)x});
	}

	out.assign(rev.rbegin(), rev.rend());
	return true;
}

std::string Num(size_t n) {
	return std::to_string(n);
}

// "@@ -start,count +start,count @@". A zero-length side is numbered from the
// line *before* the change, which is what every unified-diff emitter does and
// what patchers rely on to place an insertion.
std::string HunkHeader(size_t a_start, size_t a_count, size_t b_start, size_t b_count) {
	std::string h = "@@ -";
	h += Num(a_count == 0 ? a_start : a_start + 1);
	if (a_count != 1) {
		h += "," + Num(a_count);
	}
	h += " +";
	h += Num(b_count == 0 ? b_start : b_start + 1);
	if (b_count != 1) {
		h += "," + Num(b_count);
	}
	h += " @@\n";
	return h;
}

const char *kNoNewlineText = "\\ No newline at end of file";
const char *kNoNewline = "\\ No newline at end of file\n";

// Emit one line of a hunk, appending the no-newline marker when this is the
// last line of a side that does not end in one.
void EmitLine(std::string &out, char prefix, const std::string &text, bool last_of_side,
              bool side_final_newline) {
	out += prefix;
	out += text;
	out += '\n';
	if (last_of_side && !side_final_newline) {
		out += kNoNewline;
	}
}

std::string WholeFileDiff(const Lines &a, const Lines &b) {
	std::string out = HunkHeader(0, a.v.size(), 0, b.v.size());
	for (size_t i = 0; i < a.v.size(); i++) {
		EmitLine(out, '-', a.v[i], i + 1 == a.v.size(), a.final_newline);
	}
	for (size_t i = 0; i < b.v.size(); i++) {
		EmitLine(out, '+', b.v[i], i + 1 == b.v.size(), b.final_newline);
	}
	return out;
}

} // namespace

std::string Unified(const std::string &from, const std::string &to, int ctxlen, size_t max_lines,
                    size_t max_edits) {
	if (from == to) {
		return "";
	}
	if (ctxlen < 0) {
		ctxlen = 0;
	}
	Lines a = Split(from);
	Lines b = Split(to);

	if (a.v.size() > max_lines || b.v.size() > max_lines) {
		return WholeFileDiff(a, b);
	}

	// Trimming the common head and tail first is what keeps the usual case —
	// one paragraph edited in a long page — cheap enough that the budget below
	// is never approached.
	const std::vector<std::string> ka = Keys(a);
	const std::vector<std::string> kb = Keys(b);

	size_t head = 0;
	while (head < ka.size() && head < kb.size() && ka[head] == kb[head]) {
		head++;
	}
	size_t tail = 0;
	while (tail < ka.size() - head && tail < kb.size() - head &&
	       ka[ka.size() - 1 - tail] == kb[kb.size() - 1 - tail]) {
		tail++;
	}

	std::vector<std::string> mid_a(ka.begin() + (long)head, ka.end() - (long)tail);
	std::vector<std::string> mid_b(kb.begin() + (long)head, kb.end() - (long)tail);

	std::vector<Edit> script;
	if (!MyersScript(mid_a, mid_b, max_edits, script)) {
		return WholeFileDiff(a, b);
	}

	// Re-base the script onto the untrimmed line numbers, with the trimmed
	// head and tail as ordinary context.
	std::vector<Edit> full;
	full.reserve(script.size() + head + tail);
	for (size_t i = 0; i < head; i++) {
		full.push_back({Op::Keep, i});
	}
	for (const Edit &e : script) {
		full.push_back({e.op, e.index + head});
	}
	for (size_t i = 0; i < tail; i++) {
		full.push_back({Op::Keep, a.v.size() - tail + i});
	}

	// Group the script into hunks: every run of changes, plus ctxlen lines of
	// context either side, merged when two runs are closer than 2*ctxlen.
	std::vector<size_t> changed;
	for (size_t i = 0; i < full.size(); i++) {
		if (full[i].op != Op::Keep) {
			changed.push_back(i);
		}
	}
	if (changed.empty()) {
		return "";
	}

	std::string out;
	size_t i = 0;
	while (i < changed.size()) {
		size_t first = changed[i];
		size_t last = first;
		size_t j = i;
		while (j + 1 < changed.size() &&
		       changed[j + 1] <= changed[j] + (size_t)(2 * ctxlen) + 1) {
			j++;
			last = changed[j];
		}
		size_t begin = first > (size_t)ctxlen ? first - (size_t)ctxlen : 0;
		size_t end = std::min(full.size(), last + (size_t)ctxlen + 1);

		// Line numbers at the start of the hunk.
		size_t a_at = 0;
		size_t b_at = 0;
		for (size_t k = 0; k < begin; k++) {
			if (full[k].op != Op::Ins) {
				a_at++;
			}
			if (full[k].op != Op::Del) {
				b_at++;
			}
		}
		size_t a_count = 0;
		size_t b_count = 0;
		for (size_t k = begin; k < end; k++) {
			if (full[k].op != Op::Ins) {
				a_count++;
			}
			if (full[k].op != Op::Del) {
				b_count++;
			}
		}

		out += HunkHeader(a_at, a_count, b_at, b_count);
		for (size_t k = begin; k < end; k++) {
			const Edit &e = full[k];
			switch (e.op) {
			case Op::Keep:
				EmitLine(out, ' ', a.v[e.index], e.index + 1 == a.v.size(),
				         a.final_newline && b.final_newline);
				break;
			case Op::Del:
				EmitLine(out, '-', a.v[e.index], e.index + 1 == a.v.size(), a.final_newline);
				break;
			case Op::Ins:
				EmitLine(out, '+', b.v[e.index], e.index + 1 == b.v.size(), b.final_newline);
				break;
			}
		}
		i = j + 1;
	}
	return out;
}

namespace {

// Parse "@@ -a,b +c,d @@". Returns false on anything that is not one.
bool ParseHunk(const std::string &line, size_t &a_start, size_t &a_count, size_t &b_start,
               size_t &b_count) {
	if (line.compare(0, 4, "@@ -") != 0) {
		return false;
	}
	size_t p = 4;
	auto number = [&](size_t &into) {
		if (p >= line.size() || !isdigit((unsigned char)line[p])) {
			return false;
		}
		size_t val = 0;
		while (p < line.size() && isdigit((unsigned char)line[p])) {
			if (val > (size_t)1 << 40) {
				return false; // a line number nobody has
			}
			val = val * 10 + (size_t)(line[p] - '0');
			p++;
		}
		into = val;
		return true;
	};
	if (!number(a_start)) {
		return false;
	}
	a_count = 1;
	if (p < line.size() && line[p] == ',') {
		p++;
		if (!number(a_count)) {
			return false;
		}
	}
	if (p + 1 >= line.size() || line[p] != ' ' || line[p + 1] != '+') {
		return false;
	}
	p += 2;
	if (!number(b_start)) {
		return false;
	}
	b_count = 1;
	if (p < line.size() && line[p] == ',') {
		p++;
		if (!number(b_count)) {
			return false;
		}
	}
	return line.find(" @@", p) == p;
}

} // namespace

bool Apply(const std::string &src, const std::string &patch, std::string &out, std::string &err) {
	err.clear();
	Lines s = Split(src);
	Lines patch_lines = Split(patch);

	Lines result;
	size_t src_at = 0; // next unconsumed line of src
	// Whether the line most recently appended to the result ends without a
	// newline. Only its value for the *final* line survives, which is why it is
	// tracked as each line is appended rather than reconstructed afterwards.
	bool last_no_newline = !s.final_newline;

	auto marker_at = [&](size_t idx) {
		return idx < patch_lines.v.size() && patch_lines.v[idx] == kNoNewlineText;
	};
	auto take_src = [&]() {
		last_no_newline = (src_at + 1 == s.v.size()) && !s.final_newline;
		result.v.push_back(s.v[src_at++]);
	};

	size_t i = 0;
	bool saw_hunk = false;
	while (i < patch_lines.v.size()) {
		const std::string &line = patch_lines.v[i];
		if (line.empty() || line == kNoNewlineText) {
			i++;
			continue;
		}
		size_t a_start = 0;
		size_t a_count = 0;
		size_t b_start = 0;
		size_t b_count = 0;
		if (!ParseHunk(line, a_start, a_count, b_start, b_count)) {
			// Tolerate the "---"/"+++" file header some emitters add, and
			// anything before the first hunk. Reject stray content after one,
			// which would mean a truncated or corrupt patch.
			if (!saw_hunk && (line.compare(0, 3, "---") == 0 || line.compare(0, 3, "+++") == 0 ||
			                  line.compare(0, 5, "diff ") == 0 || line.compare(0, 6, "index ") == 0)) {
				i++;
				continue;
			}
			if (!saw_hunk) {
				i++;
				continue;
			}
			err = "unexpected line in patch: " + line.substr(0, 60);
			return false;
		}
		saw_hunk = true;
		i++;

		// A hunk header numbers from 1, except for a zero-length side which
		// numbers from the line before.
		size_t want = a_count == 0 ? a_start : (a_start > 0 ? a_start - 1 : 0);
		if (want < src_at) {
			err = "patch hunks are out of order";
			return false;
		}
		if (want > s.v.size()) {
			err = "patch applies past the end of the text";
			return false;
		}
		while (src_at < want) {
			take_src();
		}

		size_t consumed = 0;
		size_t produced = 0;
		while (i < patch_lines.v.size() && (consumed < a_count || produced < b_count)) {
			const std::string &pl = patch_lines.v[i];
			if (pl == kNoNewlineText) {
				i++;
				continue;
			}
			if (pl.empty()) {
				// An empty patch line is a context line whose content is empty
				// and whose leading space was stripped by something on the way.
				if (consumed >= a_count || src_at >= s.v.size() || !s.v[src_at].empty()) {
					err = "context does not match at line " + Num(src_at + 1);
					return false;
				}
				take_src();
				last_no_newline = marker_at(i + 1);
				consumed++;
				produced++;
				i++;
				continue;
			}
			const char tag = pl[0];
			const std::string text = pl.substr(1);
			if (tag == ' ') {
				if (src_at >= s.v.size() || s.v[src_at] != text) {
					err = "context does not match at line " + Num(src_at + 1);
					return false;
				}
				take_src();
				last_no_newline = marker_at(i + 1);
				consumed++;
				produced++;
			} else if (tag == '-') {
				if (src_at >= s.v.size() || s.v[src_at] != text) {
					err = "removed line does not match at line " + Num(src_at + 1);
					return false;
				}
				src_at++;
				consumed++;
			} else if (tag == '+') {
				result.v.push_back(text);
				last_no_newline = marker_at(i + 1);
				produced++;
			} else {
				err = "unrecognised patch line: " + pl.substr(0, 60);
				return false;
			}
			i++;
		}
		if (consumed != a_count || produced != b_count) {
			err = "hunk is truncated";
			return false;
		}
	}
	while (src_at < s.v.size()) {
		take_src();
	}

	result.final_newline = result.v.empty() ? true : !last_no_newline;
	out = Join(result);
	return true;
}

} // namespace diff
} // namespace quackmail
