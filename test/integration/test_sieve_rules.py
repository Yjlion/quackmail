#!/usr/bin/env python3
"""End-to-end test for the Sieve rule builder at /prefs/sieve.

The design claim under test is that the **script text is the single source of
truth**. Rules are derived from it on every page load and never stored beside it,
because `quackmail_sieve_scripts` is also written by ManageSieve and by the admin
console. A rules table would mean either regenerating destroys an out-of-band
edit, or trusting the table makes the UI describe filtering that is not what
runs — both silent.

So the assertions are:
  * a rule added in the browser produces a script the delivery parser accepts;
  * a script written over ManageSieve is what the browser then shows — this is
    the case that killed the alternative designs;
  * a script the rule view cannot represent shows a banner and is *not* rewritten
    by a stray click;
  * order is editable, because in Sieve order is semantics: a rule with `stop`
    prevents everything after it.

Requires: pip install duckdb==1.5.4
Run after `make` so the loadable extensions exist under build/release/extension.
"""
import http.cookiejar
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 18094
SIEVE_PORT = 42090
BASE = f"http://{HOST}:{PORT}"

USER = "sieveuser"
PASSWORD = "secret"


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


class Client:
    def __init__(self):
        self.jar = http.cookiejar.CookieJar()
        self.op = urllib.request.build_opener(
            NoRedirect(), urllib.request.HTTPCookieProcessor(self.jar))

    def get(self, path):
        return self._go(path, None)

    def post(self, path, fields):
        return self._go(path, urllib.parse.urlencode(fields).encode())

    def _go(self, path, body):
        try:
            r = self.op.open(BASE + path, body, timeout=20)
            return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")


def csrf(page):
    m = re.search(r'name="_csrf" value="([^"]+)"', page)
    assert m, "no CSRF token on that page"
    return m.group(1)


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_http"):
        con.execute(f"LOAD '{ext(name)}'")
    con.execute("SELECT count(*) FROM qm_status()").fetchall()

    con.execute(f"CALL qm_user_add('{USER}', '{PASSWORD}')")
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    con.execute(f"CALL qm_http_start('{HOST}', {PORT})")

    def script_text(name="rules"):
        row = con.execute(
            "SELECT script FROM quackmail_sieve_scripts WHERE username = ? AND name = ?",
            [USER, name]).fetchone()
        return row[0] if row else None

    try:
        c = Client()
        _, page = c.get("/login")
        status, _ = c.post("/login", {"username": USER, "password": PASSWORD,
                                      "_csrf": csrf(page)})
        assert status == 303, f"sign-in returned {status}"

        # ---- an empty script, then a rule built in the browser ---------------
        _, page = c.get("/prefs/sieve")
        assert status == 303 or "Mail filters" in page or "Script" in page
        # Create the script by saving empty text, which is how a user starts.
        status, _ = c.post("/prefs/sieve/save",
                           {"_csrf": csrf(page), "user": "", "name": "rules", "script": ""})
        assert status == 303, f"creating a script returned {status}"

        _, page = c.get("/prefs/sieve?name=rules")
        assert "Add a rule" in page, "the rule builder is not offered"
        assert "This script has no rules yet" in page, "an empty script did not say so"
        # Only actions the evaluator implements are offered. Vacation is not one.
        assert "vacation" not in page.lower(), \
            "the builder offers vacation, which this engine does not implement"

        status, _ = c.post("/prefs/sieve/rule/add", {
            "_csrf": csrf(page), "user": "", "name": "rules", "rule_name": "Newsletters",
            "field": "from", "op": "contains", "value": "news@example.org",
            "action": "fileinto", "argument": "Lists",
        })
        assert status == 303, f"adding a rule returned {status}"

        text = script_text()
        assert text, "the rule was not written to the script"
        assert "# rule: Newsletters" in text, f"the rule name was not written:\n{text}"
        assert 'header :contains "from" "news@example.org"' in text, text
        assert 'fileinto "Lists"' in text, text
        # The generated script has to be one the delivery path accepts, not merely
        # one the builder is happy with.
        assert con.execute("SELECT qm_sieve_valid(?)", [text]).fetchone()[0], \
            f"the builder produced a script the parser rejects:\n{text}"

        _, page = c.get("/prefs/sieve?name=rules")
        assert "Newsletters" in page, "the rule is not shown"
        assert "From contains news@example.org" in page, f"the condition reads wrong: {page[:400]}"
        assert "file into Lists" in page, "the action reads wrong"

        # ---- a second rule, and ordering ------------------------------------
        status, _ = c.post("/prefs/sieve/rule/add", {
            "_csrf": csrf(page), "user": "", "name": "rules", "rule_name": "Loud",
            "field": "subject", "op": "is", "value": "SALE",
            "action": "discard", "argument": "", "stop": "1",
        })
        assert status == 303, f"adding a second rule returned {status}"
        text = script_text()
        assert text.index("Newsletters") < text.index("Loud"), "rules are out of order"
        assert "stop;" in text, "the stop flag was not written"

        _, page = c.get("/prefs/sieve?name=rules")
        assert "Move down" in page and "Move up" in page, "rules cannot be reordered"

        # Order is semantics in Sieve — a rule with `stop` prevents everything
        # after it — so moving one must actually change the script.
        status, _ = c.post("/prefs/sieve/rule/move",
                           {"_csrf": csrf(page), "user": "", "name": "rules",
                            "rule": "1", "dir": "up"})
        assert status == 303, f"moving a rule returned {status}"
        text = script_text()
        assert text.index("Loud") < text.index("Newsletters"), \
            f"the move did not reorder the script:\n{text}"

        # ---- delete ---------------------------------------------------------
        _, page = c.get("/prefs/sieve?name=rules")
        status, _ = c.post("/prefs/sieve/rule/delete",
                           {"_csrf": csrf(page), "user": "", "name": "rules", "rule": "0"})
        assert status == 303, f"deleting a rule returned {status}"
        text = script_text()
        assert "Loud" not in text, "the deleted rule is still in the script"
        assert "Newsletters" in text, "deleting one rule removed the other"

        # ---- the case that decided the design -------------------------------
        # A script written out of band — as ManageSieve or the admin console does
        # — is what the browser then shows. Nothing structured is cached, so there
        # is no second copy to go stale.
        con.execute(
            "UPDATE quackmail_sieve_scripts SET script = ? WHERE username = ? AND name = ?",
            ['# rule: Written elsewhere\nif header :is "to" "someone@example.org" {\n'
             '  discard;\n}\n', USER, "rules"])
        _, page = c.get("/prefs/sieve?name=rules")
        assert "Written elsewhere" in page, \
            "an out-of-band edit is not reflected — the UI is showing a cached copy"
        assert "Newsletters" not in page, \
            "the replaced rule is still shown — the UI is showing a cached copy"

        # ---- a script the rule view cannot show -----------------------------
        # It must say so and offer only the text editor. A stray click must not
        # rewrite it into an approximation.
        nested = ('if header :is "from" "a@example.org" {\n'
                  '  if header :is "to" "b@example.org" {\n    discard;\n  }\n}\n')
        con.execute(
            "UPDATE quackmail_sieve_scripts SET script = ? WHERE username = ? AND name = ?",
            [nested, USER, "rules"])
        _, page = c.get("/prefs/sieve?name=rules")
        assert "nests one rule inside another" in page, \
            f"an unrepresentable script did not explain itself: {page[:500]}"
        assert "Edit it as text below" in page, "no fallback to the text editor was offered"
        # The source is still there to edit.
        assert "b@example.org" in page, "the script text is not shown for editing"

        status, body = c.post("/prefs/sieve/rule/add", {
            "_csrf": csrf(page), "user": "", "name": "rules", "rule_name": "Sneaky",
            "field": "from", "op": "is", "value": "x@example.org",
            "action": "keep", "argument": "",
        })
        assert status == 400, f"adding a rule to an unrepresentable script returned {status}"
        assert script_text() == nested, \
            "a rule-add rewrote a script the rule view cannot represent"

        # ---- validation -----------------------------------------------------
        con.execute(
            "UPDATE quackmail_sieve_scripts SET script = '' WHERE username = ? AND name = ?",
            [USER, "rules"])
        _, page = c.get("/prefs/sieve?name=rules")
        token = csrf(page)
        for fields, why in [
            ({"field": "from", "op": "is", "value": "", "action": "keep"},
             "a rule with no value"),
            ({"field": "size", "op": "over", "value": "lots", "action": "keep"},
             "a size that is not a number"),
            ({"field": "from", "op": "is", "value": "x", "action": "fileinto", "argument": ""},
             "fileinto with no folder"),
            ({"field": "from", "op": "is", "value": "x", "action": "redirect",
              "argument": "not-an-address"},
             "redirect to something that is not an address"),
        ]:
            payload = {"_csrf": token, "user": "", "name": "rules", "rule_name": ""}
            payload.update(fields)
            status, _ = c.post("/prefs/sieve/rule/add", payload)
            assert status == 400, f"{why} was accepted ({status})"
            _, page = c.get("/prefs/sieve?name=rules")
            token = csrf(page)

    finally:
        con.execute("CALL qm_http_stop()")

    print("PASS: Sieve rule builder (derived rules, out-of-band edits, refusal to rewrite)")


if __name__ == "__main__":
    sys.exit(main())
