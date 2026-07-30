#!/usr/bin/env python3
"""Regenerate core/src/tzdata.cpp from a compiled IANA time zone database.

Run this by hand when the database should be refreshed, and commit the result.
The build never fetches anything: release.yml has no network step, and an
extension that needed one could not be built offline. The generated file is
therefore part of the source tree, exactly like core/src/psl_data.cpp.

    python3 tools/gen_tzdata.py                      # from /usr/share/zoneinfo
    python3 tools/gen_tzdata.py --from DIR           # from another compiled tree
    python3 tools/gen_tzdata.py --from DIR --links backward
    python3 tools/gen_tzdata.py --check              # CI: is the file current?

`--links` takes IANA's `backward` file, whose `Link` lines are the only
authoritative statement of which names are renames of which. Without it the
zone data is still complete and every name still resolves; only Canonical()
loses the ability to map a legacy name onto its modern spelling.

Two deliberate reductions, both stated in tz.hpp:

  * Transitions before 1970 are dropped. Mail and calendars do not schedule
    anything in 1883, and they are more than a third of the data.
  * Everything after the last stored transition comes from the zone's POSIX TZ
    footer rather than from a table, which is how the format is meant to be
    read and why a date in 2050 is still right.

If no compiled tree is available (Windows, a minimal container), the pip
`tzdata` package ships one:

    python3 -m pip install --target /tmp/tzpkg tzdata
    python3 tools/gen_tzdata.py --from /tmp/tzpkg/tzdata/zoneinfo
"""
import argparse
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "core", "src", "tzdata.cpp")

# Everything from 1970 onward. -1 rather than 0 so a transition exactly at the
# epoch survives the comparison.
EPOCH_FLOOR = -1

# Legacy top-level names. Every one of these is a link in modern tzdata, so they
# are kept resolvable but kept out of List(): a picker offering both
# "US/Eastern" and "America/New_York" is just noisier for no gain.
LEGACY_PREFIXES = ("US/", "Canada/", "Brazil/", "Mexico/", "Chile/", "Etc/",
                   "Australia/ACT", "Australia/LHI", "Australia/NSW",
                   "Australia/North", "Australia/Queensland", "Australia/South",
                   "Australia/Tasmania", "Australia/Victoria", "Australia/West")
# Bare single-word names (EST, Japan, GMT+0, ...) are legacy for the same reason.
# Anything with no "/" at all falls in here.


def parse_tzif(data):
    """Return (transitions, type_at, types, abbrs, posix, ok).

    Reads the 64-bit block of a v2+ file, which is where modern (slim) tzdata
    puts everything; the leading 32-bit block is left empty on purpose and is
    only there so a pre-2004 reader sees something.
    """
    if data[:4] != b"TZif":
        return None
    version = data[4:5]

    def block(off, width):
        isutcnt, isstdcnt, leapcnt, timecnt, typecnt, charcnt = struct.unpack(
            ">6I", data[off + 20:off + 44])
        p = off + 44
        fmt = "q" if width == 8 else "i"
        trans = list(struct.unpack(">%d%s" % (timecnt, fmt), data[p:p + timecnt * width]))
        p += timecnt * width
        type_at = list(data[p:p + timecnt])
        p += timecnt
        types = []
        for _ in range(typecnt):
            utoff, isdst, idx = struct.unpack(">iBB", data[p:p + 6])
            p += 6
            types.append((utoff, isdst, idx))
        abbrs = data[p:p + charcnt]
        p += charcnt
        p += leapcnt * (width + 4)
        p += isstdcnt + isutcnt
        return trans, type_at, types, abbrs, p

    trans, type_at, types, abbrs, end = block(0, 4)
    posix = b""
    if version in (b"2", b"3", b"4"):
        trans, type_at, types, abbrs, end = block(end, 8)
        # The footer is a newline-wrapped POSIX TZ string.
        nl = data.index(b"\n", end)
        nl2 = data.index(b"\n", nl + 1)
        posix = data[nl + 1:nl2]
    if not types:
        return None
    return trans, type_at, types, abbrs, posix


def trim(trans, type_at, types):
    """Drop pre-1970 transitions, keeping the type in force at the cut.

    The starting type is not type_at[0]: that is what applies *after* the first
    transition. RFC 8536 says the type before any transition is the first
    non-DST one (or type 0 if every type is DST) — which matters for zones whose
    first transition is already after 1970, such as Antarctica/Rothera, where
    using type_at[0] would report the post-1976 offset for all of 1971.
    """
    first_type = 0
    for idx, (_, isdst, _) in enumerate(types):
        if not isdst:
            first_type = idx
            break

    keep_t, keep_i = [], []
    for t, i in zip(trans, type_at):
        if t >= EPOCH_FLOOR:
            keep_t.append(t)
            keep_i.append(i)
        else:
            # Anything discarded was in force up to the cut, so the last one
            # discarded is what the epoch starts in.
            first_type = i
    return keep_t, keep_i, first_type


def collect(root, links_file):
    """Every zone in `root`, plus the Link table if one was supplied."""
    zones = {}
    for dirpath, dirnames, filenames in os.walk(root):
        # posix/ and right/ are alternative compilations of the same zones;
        # right/ counts leap seconds, which is not what anything here wants.
        dirnames[:] = [d for d in dirnames if d not in ("posix", "right")]
        for fn in filenames:
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            if rel in ("tzdata.zi", "leapseconds", "leap-seconds.list", "+VERSION"):
                continue
            try:
                with open(path, "rb") as f:
                    data = f.read()
            except OSError:
                continue
            parsed = parse_tzif(data)
            if parsed:
                zones[rel] = parsed
    if not zones:
        raise SystemExit(f"no TZif files under {root}")

    links = []
    if links_file:
        with open(links_file, encoding="utf-8") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                parts = line.split()
                # "Link  TARGET  LINK-NAME"
                if len(parts) >= 3 and parts[0] == "Link":
                    if parts[2] in zones and parts[1] in zones:
                        links.append((parts[2], parts[1]))
    return zones, sorted(set(links))


def selectable(name):
    """Should this name appear in the zone picker?"""
    if "/" not in name:
        return False
    return not name.startswith(LEGACY_PREFIXES)


def version_of(root, override):
    if override:
        return override
    for candidate in ("+VERSION", "tzdata.zi"):
        path = os.path.join(root, candidate)
        if os.path.exists(path):
            with open(path, encoding="utf-8", errors="replace") as f:
                head = f.read(4096)
            m = re.search(r"\b(20\d\d[a-z])\b", head)
            if m:
                return m.group(1)
    # The pip package records it, and is the documented fallback source.
    try:
        sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(root))))
        import tzdata  # noqa: PLC0415
        return tzdata.IANA_VERSION
    except Exception:  # noqa: BLE001 - a missing version is not fatal
        return "unknown"


def clines(values, per_line, indent="    "):
    out = []
    for i in range(0, len(values), per_line):
        out.append(indent + ", ".join(str(v) for v in values[i:i + per_line]) + ",")
    return out


def cstr(b):
    """A C string literal for a bytes value, escaping what has to be."""
    out = []
    for ch in b:
        c = chr(ch)
        if c == '"':
            out.append('\\"')
        elif c == "\\":
            out.append("\\\\")
        elif 32 <= ch < 127:
            out.append(c)
        else:
            out.append("\\%03o" % ch)
    return '"' + "".join(out) + '"'


def emit(zones, links, version):
    # Deduplicate the data: a link is a second name for the same bytes, and
    # there are hundreds of them. Names stay separate (so every zone is still
    # selectable); only the arrays are shared.
    shared = {}
    order = []
    for name in sorted(zones):
        trans, type_at, types, abbrs, posix = zones[name]
        t, i, first = trim(trans, type_at, types)
        key = (tuple(t), tuple(i), tuple(types), abbrs, posix, first)
        if key not in shared:
            shared[key] = len(order)
            order.append(key)

    body = [
        "// Generated by tools/gen_tzdata.py. Do not edit.",
        "//",
        "// The IANA time zone database, compiled in. Public domain, as the",
        "// database itself is. Transitions before 1970 are dropped and everything",
        "// after the last transition comes from the zone's POSIX rule — see",
        "// tools/gen_tzdata.py for why.",
        "",
        '#include "quackmail/tz.hpp"',
        "",
        "namespace quackmail {",
        "namespace tz {",
        "",
        f'const char *const kTzVersion = "{version}";',
        "",
        "namespace {",
        "",
    ]

    for idx, (t, i, types, abbrs, posix, first) in enumerate(order):
        if t:
            body.append(f"const int64_t kT{idx}[] = {{")
            body += clines(t, 8)
            body.append("};")
            body.append(f"const uint8_t kI{idx}[] = {{")
            body += clines(i, 24)
            body.append("};")
        body.append(f"const ZoneType kY{idx}[] = {{")
        for utoff, isdst, ai in types:
            body.append(f"    {{{utoff}, {isdst}, {ai}}},")
        body.append("};")
        # The abbreviation blob is NUL-separated and indexed directly, so it has
        # to survive verbatim rather than being split.
        body.append(f"const char kA{idx}[] = {cstr(abbrs)};")
        body.append("")

    # The tables themselves are not in the anonymous namespace: tz.hpp declares
    # them extern so tz.cpp can walk them.
    body += ["} // namespace", ""]

    body.append("const Zone kZones[] = {")
    for name in sorted(zones):
        trans, type_at, types, abbrs, posix = zones[name]
        t, i, first = trim(trans, type_at, types)
        idx = shared[(tuple(t), tuple(i), tuple(types), abbrs, posix, first)]
        tp = f"kT{idx}" if t else "nullptr"
        ip = f"kI{idx}" if t else "nullptr"
        body.append(
            f'    {{"{name}", {tp}, {ip}, {len(t)}, kY{idx}, {len(types)}, '
            f"kA{idx}, {cstr(posix)}, {first}, {1 if selectable(name) else 0}}},")
    body.append("};")
    body.append(f"const size_t kZoneCount = {len(zones)};")
    body.append("")

    body.append("const Link kLinks[] = {")
    for frm, to in links:
        body.append(f'    {{"{frm}", "{to}"}},')
    if not links:
        body.append("    {nullptr, nullptr}, // no `backward` file was supplied")
    body.append("};")
    body.append(f"const size_t kLinkCount = {len(links)};")
    body += [
        "",
        "} // namespace tz",
        "} // namespace quackmail",
        "",
    ]
    return "\n".join(body), len(order)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="source", default="/usr/share/zoneinfo",
                    help="a compiled zoneinfo tree (default /usr/share/zoneinfo)")
    ap.add_argument("--links", help="IANA's `backward` file, for Canonical()")
    ap.add_argument("--version", help="override the recorded IANA release")
    ap.add_argument("--check", action="store_true",
                    help="verify the generated file is current; do not write")
    args = ap.parse_args()

    if not os.path.isdir(args.source):
        raise SystemExit(f"{args.source} is not a directory — see --from in the docstring")

    zones, links = collect(args.source, args.links)
    version = version_of(args.source, args.version)
    text, unique = emit(zones, links, version)

    if len(zones) < 300:
        raise SystemExit(f"only {len(zones)} zones — that tree looks truncated")

    if args.check:
        try:
            with open(OUT, encoding="utf-8") as f:
                current = f.read()
        except FileNotFoundError:
            raise SystemExit(f"{OUT} does not exist — run tools/gen_tzdata.py")
        if current != text:
            raise SystemExit(f"{OUT} is out of date — run tools/gen_tzdata.py and commit")
        print(f"{OUT} is up to date ({len(zones)} zones, IANA {version})")
        return

    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    pickable = sum(1 for z in zones if selectable(z))
    print(f"wrote {OUT}: IANA {version}, {len(zones)} zones "
          f"({unique} distinct, {pickable} selectable), {len(links)} links")


if __name__ == "__main__":
    main()
