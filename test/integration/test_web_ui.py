#!/usr/bin/env python3
"""Browser-driven test for the web interface (quackmail_http).

test_http.py drives the same server with urllib, which is the right tool for
almost everything: every route returns a complete page, so status codes, CSRF,
IDOR, escaping and headers are all assertable without a browser. This file is
for the handful of things that are only true once a browser has run the page —
the htmx reading pane, the keyboard shortcuts, thread disclosure, and the
card layout the mail listing collapses to on a phone.

Requires: pip install duckdb==1.5.4 playwright
Playwright drives the *system* Chromium (as tools/screenshots.py does), so
`playwright install` is not needed. Skips itself when either is missing.

Run after `make release`.
"""
import os
import sys
import time

try:
    import duckdb
except ImportError:  # pragma: no cover - environment guard
    print("SKIP test_web_ui.py: duckdb module not installed")
    sys.exit(0)

try:
    from playwright.sync_api import sync_playwright
except ImportError:  # pragma: no cover - environment guard
    print("SKIP test_web_ui.py: playwright not installed")
    sys.exit(0)

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
CHROMIUM = "/usr/bin/chromium"

HOST = "127.0.0.1"
PORT = 18081
BASE = f"http://{HOST}:{PORT}"

USER, PASSWORD = "webui", "correct-horse"

DESKTOP = {"width": 1440, "height": 900}
MOBILE = {"width": 390, "height": 844}


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def start_server():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_citadel", "quackmail_http"):
        con.execute(f"LOAD '{ext(name)}'")
    # store::EnsureSchema runs from a table function's init, not from LOAD, so
    # the citadel_* tables do not exist until some qm_* function has run once.
    con.execute("SELECT count(*) FROM qm_status()")

    con.execute(f"CALL qm_user_add('{USER}', '{PASSWORD}')")
    con.execute(
        "INSERT INTO citadel_users (username, usernum, axlevel) "
        f"VALUES ('{USER}', nextval('citadel_user_seq'), 4)"
    )
    # A dev box has no certificate, so the HTTPS redirect has to be off or the
    # plaintext listener serves nothing but a 301.
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    con.execute("CALL qm_config_set('c_fqdn', 'quackmail.test')")

    note = con.execute(f"SELECT note FROM qm_http_start('{HOST}', {PORT})").fetchone()[0]
    assert note == "started", f"http did not start: {note}"
    return con


def seed_mail(con):
    """A thread of three and two unrelated messages, in the inbox."""
    # EnsureUserRooms provisions the folders at first login, so this runs after
    # the browser has signed in once.
    room = con.execute(
        "SELECT room_num FROM citadel_rooms WHERE display_name = 'Mail' "
        f"AND mailbox_owner = (SELECT usernum FROM citadel_users WHERE username = '{USER}')"
    ).fetchone()
    assert room, "the inbox was not provisioned"
    room = room[0]

    def post(subject, refs, body):
        """One message in the inbox. Inserted directly: this is a fixture, and
        routing it through SMTP would couple this file to the delivery path."""
        con.execute(
            "INSERT INTO citadel_messages "
            "(msgnum, author, recipient, msgtime, subject, format_type, refs, raw) "
            "VALUES (nextval('citadel_msg_seq'), ?, ?, epoch(now())::BIGINT, ?, 0, ?, ?)",
            ["Ada Lovelace <ada@example.test>", USER, subject, refs, body.encode()],
        )
        msgnum = con.execute("SELECT max(msgnum) FROM citadel_messages").fetchone()[0]
        con.execute(
            "INSERT INTO citadel_room_msgs (room_num, msgnum) VALUES (?, ?)", [room, msgnum]
        )
        return msgnum

    # A reply's References holds the root's Message-ID, and MessageId() mints a
    # local one as <%08lX-msgnum@node> from the message's own time and number.
    def message_id(msgnum):
        when, node = con.execute(
            "SELECT msgtime, (SELECT value FROM citadel_config WHERE name = 'c_nodename') "
            "FROM citadel_messages WHERE msgnum = ?", [msgnum]).fetchone()
        return "<%08X-%d@%s>" % (when, msgnum, node or "quackcit")

    root = post("The difference engine", "", "It computes.")
    rid = message_id(root)
    post("Re: The difference engine", rid, "Indeed it does.")
    post("Re: The difference engine", rid, "Twice over.")
    post("Nanoseconds", "", "A nanosecond is about a foot.")
    post("Full-text search", "", "Across every room you can read.")
    return room


def sign_in(page):
    page.goto(BASE + "/login", wait_until="networkidle")
    page.fill('input[name="username"]', USER)
    page.fill('input[name="password"]', PASSWORD)
    page.click('button[type="submit"], button:not([type])')
    page.wait_for_load_state("networkidle")


def main():
    con = start_server()
    failures = []

    def check(name, cond, detail=""):
        if cond:
            print(f"  ok   {name}")
        else:
            print(f"  FAIL {name} {detail}")
            failures.append(name)

    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(executable_path=CHROMIUM, args=["--no-sandbox"])
            ctx = browser.new_context(viewport=DESKTOP)
            page = ctx.new_page()

            # Signing in is what provisions the folders, so seeding follows it.
            sign_in(page)
            room = seed_mail(con)
            page.goto(f"{BASE}/bbs/room/{room}", wait_until="networkidle")

            # ---- the assets actually load -------------------------------
            print("assets")
            sheets = page.eval_on_selector_all(
                "link[rel=stylesheet]", "els => els.map(e => e.getAttribute('href'))")
            check("pico is linked", any("pico." in h for h in sheets), sheets)
            check("qc.css is linked", any("/static/qc." in h for h in sheets), sheets)
            check("htmx loaded", page.evaluate("() => typeof window.htmx === 'object'"))
            check("htmx eval is disabled",
                  page.evaluate("() => window.htmx && window.htmx.config.allowEval === false"))
            # Pico only styles what it has actually parsed, so a stylesheet that
            # 404'd shows up as an unstyled button rather than as a broken link.
            check("pico is in force", page.evaluate(
                "() => getComputedStyle(document.body)"
                ".getPropertyValue('--pico-background-color').trim() !== ''"))

            # ---- the reading pane ---------------------------------------
            print("reading pane")
            check("the listing rendered", page.locator(".msglist > li").count() == 5,
                  page.locator(".msglist > li").count())
            check("no reader before opening one",
                  page.locator(".panes.open").count() == 0)

            before = page.url
            nav_count = page.evaluate("() => performance.getEntriesByType('navigation').length")
            page.click(".msglist > li:first-child .subject a")
            page.wait_for_selector("#reader .msghead", timeout=5000)
            check("the reader filled in", page.locator("#reader .msghead").count() == 1)
            check("the URL was pushed", "open=" in page.url and page.url != before, page.url)
            check("the list is still there", page.locator(".msglist > li").count() == 5)
            check("it was a swap, not a navigation",
                  page.evaluate("() => performance.getEntriesByType('navigation').length")
                  == nav_count)

            # The reader carries the actions, since it renders with no page
            # around it inside the pane.
            check("the reader has actions", page.locator("#reader [data-key='reply']").count() == 1)

            # Back must undo the swap, which is the part hx-push-url buys.
            page.go_back()
            page.wait_for_load_state("networkidle")
            check("back leaves the listing", page.locator(".msglist > li").count() == 5)

            # ---- keyboard shortcuts -------------------------------------
            print("keyboard")
            page.goto(f"{BASE}/bbs/room/{room}", wait_until="networkidle")
            page.keyboard.press("j")
            check("j moves the cursor", page.locator(".msglist > li.cursor").count() == 1)
            page.keyboard.press("j")
            check("j again moves it on",
                  page.eval_on_selector_all(
                      ".msglist > li",
                      "els => els.findIndex(e => e.classList.contains('cursor'))") == 1)
            page.keyboard.press("k")
            check("k moves it back",
                  page.eval_on_selector_all(
                      ".msglist > li",
                      "els => els.findIndex(e => e.classList.contains('cursor'))") == 0)
            page.keyboard.press("x")
            check("x ticks the row",
                  page.locator('.msglist > li:first-child input[name="msgnum"]').is_checked())
            page.keyboard.press("Enter")
            page.wait_for_selector("#reader .msghead", timeout=5000)
            check("Enter opens the message", page.locator("#reader .msghead").count() == 1)

            page.goto(f"{BASE}/bbs/room/{room}", wait_until="networkidle")
            page.keyboard.press("?")
            check("? opens the help overlay", page.locator("dialog#keyshelp[open]").count() == 1)
            check("the overlay is translated from the catalog",
                  page.locator("dialog#keyshelp kbd").count() >= 10)
            page.keyboard.press("Escape")

            page.keyboard.press("/")
            check("/ focuses the search box",
                  page.evaluate("() => document.activeElement.id") == "topq")
            # A shortcut must not fire while the caret is in a field.
            page.keyboard.type("j")
            check("shortcuts are inert while typing",
                  page.evaluate("() => document.getElementById('topq').value") == "j")

            # ---- threading ----------------------------------------------
            print("threading")
            con.execute(
                "INSERT INTO citadel_user_prefs (username, name, value) VALUES (?, ?, ?) "
                "ON CONFLICT DO UPDATE SET value = excluded.value",
                [USER, "web_mail_threaded", "1"],
            )
            page.goto(f"{BASE}/bbs/room/{room}", wait_until="networkidle")
            rows = page.locator(".msglist > li").count()
            check("three messages collapsed into one row", rows == 3, f"{rows} rows")
            check("the disclosure says how many more there are",
                  "2" in page.locator(".msglist details > summary").first.inner_text())
            page.locator(".msglist details > summary").first.click()
            check("the replies disclose",
                  page.locator(".msglist .thread > li").count() == 2)

            con.execute(
                "UPDATE citadel_user_prefs SET value = '' WHERE username = ? AND name = ?",
                [USER, "web_mail_threaded"],
            )

            # ---- themes -------------------------------------------------
            print("themes")
            page.goto(BASE + "/prefs", wait_until="networkidle")
            check("auto sets no data-theme",
                  page.evaluate("() => document.documentElement.getAttribute('data-theme')") is None)
            page.select_option('select[name="theme"]', "dark")
            page.click('form[action="/prefs/settings"] button')
            page.wait_for_load_state("networkidle")
            check("dark pins the attribute",
                  page.evaluate("() => document.documentElement.getAttribute('data-theme')") == "dark")
            page.select_option('select[name="theme"]', "auto")
            page.click('form[action="/prefs/settings"] button')
            page.wait_for_load_state("networkidle")

            # ---- the phone layout ---------------------------------------
            print("mobile")
            page.set_viewport_size(MOBILE)
            page.goto(f"{BASE}/mail/", wait_until="networkidle")
            # The folder table becomes cards, each cell labelled from data-label.
            check("the folder table is carded",
                  page.evaluate(
                      "() => getComputedStyle(document.querySelector('.wrap tr')).display")
                  == "block")
            check("cells label themselves",
                  page.evaluate(
                      "() => getComputedStyle("
                      "document.querySelector('.wrap tbody td'), '::before').content")
                  not in ("none", "normal", ""))
            # Nothing may overflow the viewport sideways.
            check("nothing overflows sideways",
                  page.evaluate("() => document.documentElement.scrollWidth <= window.innerWidth + 1"),
                  page.evaluate("() => document.documentElement.scrollWidth"))

            page.goto(f"{BASE}/bbs/room/{room}", wait_until="networkidle")
            check("the listing does not overflow either",
                  page.evaluate("() => document.documentElement.scrollWidth <= window.innerWidth + 1"),
                  page.evaluate("() => document.documentElement.scrollWidth"))
            check("the sidebar is collapsed",
                  page.evaluate(
                      "() => getComputedStyle(document.querySelector('.sidebar')).display")
                  == "none")

            # With the reader open the list gets out of the way rather than
            # squeezing into half a phone.
            page.click(".msglist > li:first-child .subject a")
            page.wait_for_selector("#reader .msghead", timeout=5000)
            check("the list yields to the reader",
                  page.evaluate(
                      "() => getComputedStyle(document.querySelector('.panes > .list')).display")
                  == "none")
            check("the reader offers a way back",
                  page.locator("#reader .backtolist").count() == 1)

            browser.close()
    finally:
        try:
            con.execute("SELECT note FROM qm_http_stop()")
        except Exception:
            pass
        con.close()

    if failures:
        print(f"\n{len(failures)} failure(s): " + ", ".join(failures))
        return 1
    print("\ntest_web_ui.py: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
