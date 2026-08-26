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
    prevents everything after it;
  * a rule can grow a second condition and a second action, which the page has
    told users to do since it was written and had no route for;
  * `fileinto :create` survives an edit to a *different* part of the page, which
    it did not: Decompose read it and Compose never wrote it back;
  * the same builder on /admin/sieve actually does something. Every control
    there posted to a route nobody had registered, so the page looked identical
    to the working one and quietly did nothing.

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
AIDE = "sieveaide"
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
    # An aide, for the admin console's copy of the same builder, and the user
    # whose filters they edit. Access level 6 is what /admin/* gates on.
    con.execute(f"CALL qm_user_add('{AIDE}', '{PASSWORD}')")
    con.execute(
        "INSERT INTO citadel_users (username, usernum, axlevel) "
        f"VALUES ('{AIDE}', nextval('citadel_user_seq'), 6)")
    con.execute("CALL qm_user_add('victim', 'pw')")
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    # The admin console is opt-in and HTTPS-only by default; the aide half of
    # this test is about it, and a dev box has no certificate.
    con.execute("CALL qm_config_set('qm_web_admin_enabled', '1')")
    con.execute("CALL qm_config_set('qm_web_admin_require_tls', '0')")
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
        assert 'id="rule-builder"' in page, (
            "the rule builder has no #rule-builder wrapper for qc-sieve.js to target"
        )
        assert "qc-sieve" in page, "the page does not load the rule-builder's JS enhancement"
        # The builder offers exactly the actions the evaluator implements — no
        # more, so a control cannot promise something the delivery path ignores,
        # and no fewer, so an implemented extension is not left unreachable.
        # This used to assert that vacation was *absent*, which was right while
        # the engine did not have it; the property being checked is the same one.
        assert "out-of-office" in page.lower(), \
            "the builder does not offer vacation, which this engine implements"
        assert "mark as read" in page.lower(), \
            "the builder does not offer imap4flags, which this engine implements"
        # Variables are deliberately not offered: a variable's value depends on
        # what ran before it, and a flat list of rules has no "before".
        assert "${" not in page, "the builder offers variables, which it cannot represent"

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

        # ---- a rule with more than one condition ----------------------------
        # The page has told users to build one this way since the builder was
        # written, and the route it names did not exist: /rule/test/add was
        # never registered, so the instruction was a dead end.
        status, _ = c.post("/prefs/sieve/rule/add", {
            "_csrf": token, "user": "", "name": "rules", "rule_name": "Two things",
            "field": "from", "op": "contains", "value": "boss@example.org",
            "action": "fileinto", "argument": "Urgent",
        })
        assert status == 303, f"adding the base rule returned {status}"

        _, page = c.get("/prefs/sieve?name=rules")
        assert "Add a condition" in page, "a rule offers no way to add a condition to it"
        status, _ = c.post("/prefs/sieve/rule/test/add", {
            "_csrf": csrf(page), "user": "", "name": "rules", "rule": "0",
            "field": "subject", "op": "contains", "value": "urgent",
        })
        assert status == 303, f"adding a condition returned {status}"
        text = script_text()
        assert "allof" in text, f"a second condition did not produce an allof:\n{text}"
        assert 'header :contains "subject" "urgent"' in text, text
        assert 'header :contains "from" "boss@example.org"' in text, text

        # all/any is a real change to what the rule matches, so it is a control
        # rather than something the builder decides for you.
        _, page = c.get("/prefs/sieve?name=rules")
        assert "Match any condition instead" in page, "no way to switch allof to anyof"
        status, _ = c.post("/prefs/sieve/rule/match", {
            "_csrf": csrf(page), "user": "", "name": "rules", "rule": "0", "all": "0",
        })
        assert status == 303, f"switching to anyof returned {status}"
        assert "anyof" in script_text(), f"anyof was not written:\n{script_text()}"

        # Removing one condition leaves the other, and the last one cannot go:
        # a rule with no conditions means "every message", which is a different
        # rule from the one being edited.
        _, page = c.get("/prefs/sieve?name=rules")
        status, _ = c.post("/prefs/sieve/rule/test/delete", {
            "_csrf": csrf(page), "user": "", "name": "rules", "rule": "0", "test": "1",
        })
        assert status == 303, f"removing a condition returned {status}"
        text = script_text()
        assert "urgent" not in text and "boss@example.org" in text, \
            f"the wrong condition was removed:\n{text}"
        _, page = c.get("/prefs/sieve?name=rules")
        status, _ = c.post("/prefs/sieve/rule/test/delete", {
            "_csrf": csrf(page), "user": "", "name": "rules", "rule": "0", "test": "0",
        })
        assert status == 400, "the last condition of a rule was allowed to go"

        # ---- a second action on one rule ------------------------------------
        _, page = c.get("/prefs/sieve?name=rules")
        assert "Add an action" in page, "a rule offers no way to add an action to it"
        status, _ = c.post("/prefs/sieve/rule/action/add", {
            "_csrf": csrf(page), "user": "", "name": "rules", "rule": "0",
            "action": "redirect", "argument": "phone@example.org",
        })
        assert status == 303, f"adding an action returned {status}"
        text = script_text()
        assert 'fileinto "Urgent"' in text and 'redirect "phone@example.org"' in text, \
            f"a rule did not keep both actions:\n{text}"
        _, page = c.get("/prefs/sieve?name=rules")
        status, _ = c.post("/prefs/sieve/rule/action/delete", {
            "_csrf": csrf(page), "user": "", "name": "rules", "rule": "0", "action": "1",
        })
        assert status == 303, f"removing an action returned {status}"
        assert "phone@example.org" not in script_text(), "the action was not removed"

        # ---- imap4flags, a custom header, and :create ------------------------
        con.execute(
            "UPDATE quackmail_sieve_scripts SET script = '' WHERE username = ? AND name = ?",
            [USER, "rules"])
        _, page = c.get("/prefs/sieve?name=rules")
        status, _ = c.post("/prefs/sieve/rule/add", {
            "_csrf": csrf(page), "user": "", "name": "rules", "rule_name": "Receipts",
            "field": "header", "header_name": "X-Receipt", "op": "is", "value": "yes",
            "action": "fileinto", "argument": "Receipts/2026", "create": "1",
            "flag_\\Seen": "1", "keyword": "$Receipt",
        })
        assert status == 303, f"a header:/flags/:create rule returned {status}"
        text = script_text()
        assert 'header :is "X-Receipt" "yes"' in text, f"the custom header was lost:\n{text}"
        assert ":create" in text, f":create was not written:\n{text}"
        assert ':flags ["\\\\Seen", "$Receipt"]' in text or '\\\\Seen' in text, \
            f"the flags were not written:\n{text}"
        assert con.execute("SELECT qm_sieve_valid(?)", [text]).fetchone()[0], \
            f"the builder produced a script the parser rejects:\n{text}"

        # **:create survives touching a different part of the page.** It used to
        # be read by Decompose and never written back, so any click anywhere
        # rewrote a script that asked for it.
        _, page = c.get("/prefs/sieve?name=rules")
        status, _ = c.post("/prefs/sieve/rule/stop", {
            "_csrf": csrf(page), "user": "", "name": "rules", "rule": "0", "stop": "1",
        })
        assert status == 303, f"toggling stop returned {status}"
        text = script_text()
        assert ":create" in text, f":create was dropped by an unrelated edit:\n{text}"
        assert "stop;" in text, f"stop was not written:\n{text}"

        # A keyword with a space in it would silently become two flags.
        _, page = c.get("/prefs/sieve?name=rules")
        status, _ = c.post("/prefs/sieve/rule/action/add", {
            "_csrf": csrf(page), "user": "", "name": "rules", "rule": "0",
            "action": "keep", "argument": "", "keyword": "two words",
        })
        assert status == 400, "a keyword containing a space was accepted"

        # Flags on an action that stores nothing would be written and then
        # ignored at delivery, which is worse than refusing them.
        _, page = c.get("/prefs/sieve?name=rules")
        status, _ = c.post("/prefs/sieve/rule/action/add", {
            "_csrf": csrf(page), "user": "", "name": "rules", "rule": "0",
            "action": "discard", "argument": "", "flag_\\Seen": "1",
        })
        assert status == 400, "flags were accepted on a discard"

        # ---- an unconditional rule: the out-of-office ------------------------
        con.execute(
            "UPDATE quackmail_sieve_scripts SET script = '' WHERE username = ? AND name = ?",
            [USER, "rules"])
        _, page = c.get("/prefs/sieve?name=rules")
        status, _ = c.post("/prefs/sieve/rule/add", {
            "_csrf": csrf(page), "user": "", "name": "rules", "rule_name": "Out of office",
            "always": "1", "action": "vacation",
            "argument": "I am away until Monday.",
        })
        assert status == 303, f"an unconditional vacation rule returned {status}"
        text = script_text()
        assert "if true" in text, f"an unconditional rule was not written as `if true`:\n{text}"
        assert "vacation" in text and "away until Monday" in text, text
        assert con.execute("SELECT qm_sieve_valid(?)", [text]).fetchone()[0], \
            f"the builder produced a script the parser rejects:\n{text}"
        _, page = c.get("/prefs/sieve?name=rules")
        assert "Applies to every message" in page, \
            f"an unconditional rule is not described as one: {page[:600]}"
        # It has no conditions, so it is not offered a way to add one — that
        # would turn it into a different rule behind the user's back.
        assert "Add a condition" not in page, \
            "an unconditional rule offered to grow a condition"

        # ---- the same builder on the admin console ---------------------------
        # Every control here posted to a route that was never registered, so the
        # page looked identical to the working one and did nothing.
        con.execute(
            "INSERT INTO quackmail_sieve_scripts (username, name, active, script) "
            "VALUES ('victim', 'theirs', false, '')")
        a = Client()
        _, page = a.get("/login")
        status, _ = a.post("/login", {"username": AIDE, "password": PASSWORD,
                                      "_csrf": csrf(page)})
        assert status == 303, f"the aide could not sign in: {status}"

        _, page = a.get("/admin/sieve?user=victim&name=theirs")
        assert "Add a rule" in page, f"the admin console does not offer the builder: {page[:400]}"
        status, _ = a.post("/admin/sieve/rule/add", {
            "_csrf": csrf(page), "user": "victim", "name": "theirs", "rule_name": "Filed",
            "field": "from", "op": "contains", "value": "noreply@example.org",
            "action": "fileinto", "argument": "Robots",
        })
        assert status == 303, f"adding a rule on /admin/sieve returned {status}"
        theirs = con.execute(
            "SELECT script FROM quackmail_sieve_scripts WHERE username = 'victim' AND name = 'theirs'"
        ).fetchone()[0]
        assert 'fileinto "Robots"' in theirs, \
            f"the admin console's rule builder did not write anything:\n{theirs}"
        # And the aide's own script was not the one that changed.
        assert "noreply@example.org" not in script_text(), \
            "an admin edit landed on the operator's own script"

    finally:
        con.execute("CALL qm_http_stop()")

    print("PASS: Sieve rule builder (derived rules, out-of-band edits, refusal to rewrite, "
          "multi-condition and multi-action rules, flags, :create, and the admin console)")


if __name__ == "__main__":
    sys.exit(main())
