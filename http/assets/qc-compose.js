// The rich-text composer.
//
// The editing engine is **Squire** (`http/assets/squire.js`), Fastmail's
// contenteditable editor, MIT, vendored from `dist/squire.js` at upstream
// c07e8d9f1afdf025a4c1558bf65c269c1e29056f. This file is the glue: a toolbar,
// the mail-safe allow-list, recipient chips, attachments and the draft
// autosave.
//
// This reverses what stood here before, and the reasoning is worth keeping.
// The argument against a dependency was "TinyMCE or Quill: 200 KB to 1 MB of
// third-party JavaScript whose CVEs we would then own, most of them needing a
// build step this repo does not have and should not gain". Squire is the case
// that argument did not consider: 60 KB minified, and its `dist/` is committed
// upstream, so vendoring it is a download and a `tools/gen_assets.py` run —
// exactly how `htmx.min.js` and `pico.css` already arrive. No build step was
// gained. The CVE point stands and is now true of three vendored files rather
// than two; `docs/web.md` is the record.
//
// What the hand-rolled version could not do was survive its own foundation.
// `document.execCommand` is deprecated in every browser that implements it, it
// cannot report caret state reliably enough to light a toolbar button, and its
// paste path had to flatten everything to bare text because there was nothing
// to sanitize markup with. All three are fixed here.
//
// **One thing Squire needs that is easy to miss:** its default
// `sanitizeToDOMFragment` calls a global `DOMPurify`, which this tree does not
// vendor. Supplying that hook is therefore mandatory, not an improvement — and
// since it has to exist, it implements the *server's* allow-list
// (`SanitizeForCompose`, core/src/html_sanitize.cpp) so the editor and the
// message agree about what survives. They must be changed together.
//
// **This file is an enhancement, never a requirement.** The form ships with a
// working <textarea>; if this never runs — or if squire.js does not load —
// composing still works and sends plain text. That is why every hook below
// bails quietly when its element is absent, and why `initEditor` returns early
// when `window.Squire` is undefined.
//
// Everything here is re-entrant. Compose docks into the mail reading pane as an
// htmx swap, so the form can arrive long after this file ran, and can arrive
// again; `boot` is idempotent and marks the form it has wired.

(function () {
  "use strict";

  // ---- small shared helpers ----------------------------------------------

  function msg(form, name, fallback) {
    return form.getAttribute("data-msg-" + name) || fallback;
  }

  // Split a comma-separated recipient list without splitting inside a quoted
  // display name or an angle-addr. "Smith, John <j@x>" is one recipient, and
  // the naive split is what turned it into two.
  function splitList(value) {
    var out = [];
    var cur = "";
    var quoted = false;
    var angled = false;
    for (var i = 0; i < value.length; i++) {
      var c = value.charAt(i);
      if (c === '"' && value.charAt(i - 1) !== "\\") quoted = !quoted;
      else if (c === "<" && !quoted) angled = true;
      else if (c === ">" && !quoted) angled = false;
      if (c === "," && !quoted && !angled) {
        out.push(cur);
        cur = "";
        continue;
      }
      cur += c;
    }
    out.push(cur);
    return out
      .map(function (s) { return s.trim(); })
      .filter(function (s) { return s.length > 0; });
  }

  // Addressable at all? A bare local name is a legal recipient on a Citadel
  // system, so this refuses only what cannot be delivered anywhere: whitespace
  // in the middle, or an @ with nothing on one side of it.
  function looksAddressable(entry) {
    var addr = entry;
    var lt = entry.lastIndexOf("<");
    if (lt >= 0 && entry.indexOf(">", lt) > lt) {
      addr = entry.slice(lt + 1, entry.indexOf(">", lt));
    }
    addr = addr.trim();
    if (!addr || /\s/.test(addr)) return false;
    if (addr.indexOf("@") < 0) return true;
    return /^[^@\s]+@[^@\s]+$/.test(addr);
  }

  function bytesLabel(n) {
    if (n < 1024) return n + " B";
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB";
    return (n / (1024 * 1024)).toFixed(1) + " MB";
  }

  // ---- the rich-text editor ----------------------------------------------
  //
  // Squire (http/assets/squire.js) does the contenteditable work. Everything
  // below is the glue: a toolbar, the mail-safe allow-list, and the sync seam.

  // The allow-list, kept deliberately in step with SanitizeForCompose in
  // core/src/html_sanitize.cpp. The server is the actual defence and will strip
  // anything not in its own list on the way into the message — so a mismatch
  // here is not a hole, it is a silent loss: formatting the editor shows and
  // the message never carries. That failure mode has bitten this tree before
  // (the style= attributes in web_notes.cpp), so these tables exist to be
  // compared against that file, not to be trusted on their own.
  var OK_TAGS = {
    P: 1, BR: 1, B: 1, I: 1, EM: 1, STRONG: 1, U: 1, S: 1, A: 1, UL: 1,
    OL: 1, LI: 1, BLOCKQUOTE: 1, PRE: 1, CODE: 1, SPAN: 1, DIV: 1, TABLE: 1,
    THEAD: 1, TBODY: 1, TR: 1, TD: 1, TH: 1, IMG: 1, H1: 1, H2: 1, H3: 1,
    H4: 1, HR: 1
  };
  // Not body text: the tag goes and takes its content with it. Dropping the tag
  // alone would spill a pasted document's <title> into the message as a stray
  // word.
  var KILL_TAGS = { SCRIPT: 1, STYLE: 1, HEAD: 1, TITLE: 1, TEMPLATE: 1, NOSCRIPT: 1 };
  var OK_STYLE = {
    "color": 1, "background-color": 1, "font-weight": 1, "font-style": 1,
    "font-size": 1, "font-family": 1, "text-align": 1, "text-decoration": 1,
    "margin": 1, "margin-left": 1, "margin-right": 1, "margin-top": 1,
    "margin-bottom": 1, "padding": 1, "padding-left": 1, "padding-right": 1,
    "padding-top": 1, "padding-bottom": 1, "border-left": 1, "line-height": 1
  };

  function safeStyle(value) {
    var out = [];
    (value || "").split(";").forEach(function (decl) {
      var colon = decl.indexOf(":");
      if (colon < 0) return;
      var prop = decl.slice(0, colon).trim().toLowerCase();
      var val = decl.slice(colon + 1).trim();
      if (!OK_STYLE[prop] || !val) return;
      // url(), expression() and escapes are what a hostile paste uses; the
      // server refuses any value containing them, so refuse them here too.
      if (/url\(|expression|\\|\(/i.test(val)) return;
      out.push(prop + ": " + val);
    });
    return out.join("; ");
  }

  function safeImgSrc(src) {
    var v = (src || "").trim();
    if (/^cid:/i.test(v)) return v;
    // Only real raster types: data:image/svg+xml is scriptable.
    if (/^data:image\/(png|jpeg|gif|webp)[;,]/i.test(v)) return v;
    // A remote image in a mail body is a tracking pixel. The reader opts in to
    // those on the display side; the composer never emits one.
    return "";
  }

  function scrubAttributes(el) {
    var tag = el.nodeName;
    var attrs = [];
    for (var i = 0; i < el.attributes.length; i++) attrs.push(el.attributes[i].name);
    attrs.forEach(function (name) {
      var lower = name.toLowerCase();
      var value = el.getAttribute(name);
      if (lower === "href" && tag === "A") {
        if (!/^(https?|mailto):/i.test((value || "").trim())) {
          el.removeAttribute(name);
          return;
        }
        el.setAttribute("rel", "noopener noreferrer");
        return;
      }
      if (lower === "src" && tag === "IMG") {
        var safe = safeImgSrc(value);
        if (safe) el.setAttribute(name, safe);
        else el.removeAttribute(name);
        return;
      }
      if (lower === "alt" || lower === "title") return;
      if ((lower === "width" || lower === "height") && /^\d+$/.test(value || "")) return;
      if (lower === "style") {
        var css = safeStyle(value);
        if (css) el.setAttribute(name, css);
        else el.removeAttribute(name);
        return;
      }
      if (lower === "rel" && tag === "A") return;
      // Everything else, including every on* handler, class and id — the server
      // strips those, so keeping them here would only mislead.
      el.removeAttribute(name);
    });
  }

  function scrubFragment(frag) {
    // Walk a snapshot: the tree is being modified underneath.
    var all = [];
    var walker = document.createTreeWalker(frag, NodeFilter.SHOW_ELEMENT, null, false);
    while (walker.nextNode()) all.push(walker.currentNode);
    all.forEach(function (el) {
      if (!el.parentNode) return; // already removed with an ancestor
      var tag = el.nodeName;
      if (KILL_TAGS[tag]) {
        el.parentNode.removeChild(el);
        return;
      }
      if (!OK_TAGS[tag]) {
        // Unwrap: <html>, <body> and anything unrecognised are containers whose
        // contents are still body text.
        var parent = el.parentNode;
        while (el.firstChild) parent.insertBefore(el.firstChild, el);
        parent.removeChild(el);
        return;
      }
      scrubAttributes(el);
    });
    return frag;
  }

  // Squire's own default for this hook calls a global DOMPurify, which this
  // tree does not vendor — so supplying it is required, not an improvement.
  // Parsing happens in an inert document: `innerHTML` on a detached <template>
  // builds nodes without running anything or fetching anything.
  function sanitizeToDOMFragment(html) {
    var tpl = document.createElement("template");
    tpl.innerHTML = html || "";
    return scrubFragment(tpl.content);
  }

  function initEditor(form) {
    var textarea = form.querySelector("textarea[name=body]");
    var htmlField = form.querySelector("input[name=html_body]");
    var richToggle = form.querySelector("input[name=rich]");
    if (!textarea || !htmlField || !richToggle) return;
    // The asset failed to load, or is blocked. The form is still a working
    // plain-text composer, the toggle stays hidden, and nothing below runs.
    if (!window.Squire) return;

    // ---- the editor surface -------------------------------------------------

    var root = document.createElement("div");
    root.className = "richbody";
    root.setAttribute("role", "textbox");
    root.setAttribute("aria-multiline", "true");
    // Named from the page's own label, so the accessible name is translated.
    // A hardcoded English string here was wrong in every other locale.
    if (document.getElementById("compose-body-label")) {
      root.setAttribute("aria-labelledby", "compose-body-label");
    } else {
      root.setAttribute("aria-label", "Message body");
    }

    var sq = new window.Squire(root, {
      // <div> rather than <p>: it is on the server's allow-list and it is what
      // the existing messages in the store already look like.
      blockTag: "DIV",
      sanitizeToDOMFragment: sanitizeToDOMFragment,
      // Typing a bare URL should not silently become a link the server then has
      // to vet; the Link button is the way to make one.
      addLinks: false
    });

    // Seed from whatever the server put in the textarea: a reply's quoted text,
    // a draft being resumed. Escaped, then newlines become breaks — the
    // textarea holds plain text, so treating it as markup would execute a
    // reply's quoted content.
    function seed(text) {
      var div = document.createElement("div");
      div.textContent = text;
      return div.innerHTML.replace(/\n/g, "<br>");
    }
    sq.setHTML(seed(textarea.value));

    // ---- the toolbar --------------------------------------------------------

    var toolbar = document.createElement("div");
    toolbar.className = "richtools";

    // Only what mail HTML renders reliably across clients. A colour picker and
    // a font menu would produce markup half of them ignore.
    //
    // `query` is what Squire's getPath()/hasFormat() reports when the caret is
    // inside this format, so a button can show whether it is on.
    var BUTTONS = [
      { label: "B", title: "Bold", key: "b", query: "B",
        on: function () { sq.bold(); }, off: function () { sq.removeBold(); } },
      { label: "I", title: "Italic", key: "i", query: "I",
        on: function () { sq.italic(); }, off: function () { sq.removeItalic(); } },
      { label: "U", title: "Underline", key: "u", query: "U",
        on: function () { sq.underline(); }, off: function () { sq.removeUnderline(); } },
      { label: "• List", title: "Bulleted list", query: "UL",
        on: function () { sq.makeUnorderedList(); }, off: function () { sq.removeList(); } },
      { label: "1. List", title: "Numbered list", query: "OL",
        on: function () { sq.makeOrderedList(); }, off: function () { sq.removeList(); } },
      { label: "“ Quote", title: "Quote", query: "BLOCKQUOTE",
        on: function () { sq.increaseQuoteLevel(); }, off: function () { sq.decreaseQuoteLevel(); } },
      { label: "Clear", title: "Remove formatting",
        on: function () { sq.removeAllFormatting(); } }
    ];

    var buttons = [];
    BUTTONS.forEach(function (spec) {
      var btn = document.createElement("button");
      btn.type = "button";
      btn.className = "btn sec";
      btn.title = spec.title + (spec.key ? " (Ctrl+" + spec.key.toUpperCase() + ")" : "");
      btn.textContent = spec.label;
      if (spec.query) btn.setAttribute("aria-pressed", "false");
      btn.addEventListener("mousedown", function (ev) {
        // mousedown, not click: clicking a button would move focus out of the
        // editor and collapse the selection before the command ran.
        ev.preventDefault();
        sq.focus();
        try {
          if (spec.query && spec.off && btn.getAttribute("aria-pressed") === "true") spec.off();
          else spec.on();
        } catch (e) {
          // One unsupported command is not worth breaking the composer over.
        }
      });
      toolbar.appendChild(btn);
      buttons.push({ btn: btn, spec: spec });
    });

    var linkBtn = document.createElement("button");
    linkBtn.type = "button";
    linkBtn.className = "btn sec";
    linkBtn.textContent = "Link";
    linkBtn.title = "Insert a link";
    linkBtn.addEventListener("mousedown", function (ev) {
      ev.preventDefault();
      sq.focus();
      var url = window.prompt("Link to which address?", "https://");
      if (!url) return;
      // Only the schemes the allow-list keeps, so the button cannot produce
      // something that silently disappears on save.
      if (!/^(https?:|mailto:)/i.test(url)) {
        window.alert("Links must start with http://, https:// or mailto:");
        return;
      }
      sq.makeLink(url, { rel: "noopener noreferrer" });
    });
    toolbar.appendChild(linkBtn);

    // The toolbar follows the caret. execCommand's queryCommandState could not
    // do this reliably; Squire reports the path it is in, so a button can say
    // whether it is on.
    function refreshButtons() {
      buttons.forEach(function (b) {
        if (!b.spec.query) return;
        var on = false;
        try {
          on = !!sq.hasFormat(b.spec.query);
        } catch (e) {
          on = false;
        }
        b.btn.setAttribute("aria-pressed", on ? "true" : "false");
      });
    }
    sq.addEventListener("pathChange", refreshButtons);
    sq.addEventListener("select", refreshButtons);

    // ---- paste --------------------------------------------------------------
    // A paste from a web page carries its whole stylesheet, its scripts and its
    // tracking pixels. Squire routes every paste through sanitizeToDOMFragment
    // above, which is the same allow-list the server applies — so unlike the
    // previous editor, a paste keeps its bold and its lists instead of being
    // flattened to bare text, and still cannot carry anything the message would
    // not have carried anyway.

    // ---- inline images ------------------------------------------------------
    // A dropped or pasted image becomes a data: URI in the editor. The server
    // turns those into `cid:` parts on send, so the message is self-contained
    // rather than pointing at anything remote. Object URLs are deliberately not
    // used: the page's CSP allows `img-src 'self' data:` and not `blob:`.
    function insertImage(file) {
      if (!/^image\/(png|jpeg|gif|webp)$/.test(file.type)) {
        window.alert("Only PNG, JPEG, GIF and WebP images can be inserted.");
        return;
      }
      if (file.size > 2 * 1024 * 1024) {
        window.alert("That image is larger than 2 MB. Attach it instead.");
        return;
      }
      var reader = new FileReader();
      reader.onload = function () {
        sq.focus();
        sq.insertImage(reader.result, { alt: file.name || "image" });
      };
      reader.readAsDataURL(file);
    }

    root.addEventListener("dragover", function (ev) { ev.preventDefault(); });
    root.addEventListener("drop", function (ev) {
      if (!ev.dataTransfer || !ev.dataTransfer.files || !ev.dataTransfer.files.length) return;
      ev.preventDefault();
      insertImage(ev.dataTransfer.files[0]);
    });

    // ---- wiring -------------------------------------------------------------

    textarea.parentNode.insertBefore(toolbar, textarea);
    textarea.parentNode.insertBefore(root, textarea);

    // The textarea stays in the DOM, hidden, and stays authoritative for the
    // plain-text half of the message. Removing it would mean a JS error mid-edit
    // loses the body entirely.
    textarea.classList.add("richhidden");

    var label = richToggle.closest("label") || richToggle.parentNode;

    function setRich(on) {
      root.style.display = on ? "" : "none";
      toolbar.style.display = on ? "" : "none";
      textarea.classList.toggle("richhidden", on);
    }

    // Whatever the server rendered it as: the preference is the user's, and
    // forcing it on here was the composer overruling them on every message.
    setRich(richToggle.checked);
    richToggle.addEventListener("change", function () { setRich(richToggle.checked); });
    if (label) label.classList.add("richavailable");

    // An edit inside the editor is an edit to the form. Squire's root is not a
    // form control, so its input events do not bubble as the form's own do, and
    // without this the autosave below would never see a body-only change.
    sq.addEventListener("input", function () {
      form.dispatchEvent(new Event("input", { bubbles: true }));
    });

    // Named, and hung on the form, because the autosave below has to produce
    // exactly the same two fields a real submit would. Two copies of this would
    // be two definitions of what a draft contains.
    form.qcSyncBody = function () {
      if (richToggle.checked) {
        htmlField.value = sq.getHTML();
        // The plain-text half comes from the editor's text, so a recipient with
        // no HTML gets what was actually written rather than the pre-edit seed.
        var el = sq.getRoot();
        // Squire parks a zero-width space in an inline node that has no text
        // yet — click Bold on an empty line and the DOM holds U+200B until you
        // type. getHTML() strips them; innerText does not, so the plain-text
        // half would carry an invisible character into every message the HTML
        // half does not. Strip them here for the same reason Squire does there.
        textarea.value = (el.innerText || el.textContent || "").replace(/\u200B/g, "");
      } else {
        htmlField.value = "";
      }
    };
    form.addEventListener("submit", form.qcSyncBody);
  }

  // ---- recipient chips ----------------------------------------------------
  //
  // The field the server reads stays a comma-separated string, so a browser
  // with no script — and the whole urllib test suite — submits exactly what it
  // always did. This only replaces the *visible* input with chips and keeps a
  // hidden field in step.

  function initRecipients(form) {
    ["to", "cc", "bcc"].forEach(function (name) {
      var input = form.querySelector("input[name=" + name + "]");
      if (!input) return;

      var hidden = document.createElement("input");
      hidden.type = "hidden";
      hidden.name = name;
      hidden.value = input.value;
      input.removeAttribute("name");
      input.value = "";
      input.parentNode.insertBefore(hidden, input);

      var box = document.createElement("span");
      box.className = "chips";
      input.parentNode.insertBefore(box, input);
      box.appendChild(input);

      var entries = splitList(hidden.value);

      function sync() {
        hidden.value = entries.join(", ");
        var chip = box.firstChild;
        while (chip && chip !== input) {
          var next = chip.nextSibling;
          box.removeChild(chip);
          chip = next;
        }
        entries.forEach(function (entry, i) {
          var el = document.createElement("span");
          el.className = "chip" + (looksAddressable(entry) ? "" : " bad");
          if (!looksAddressable(entry)) el.title = entry + " does not look like an address.";
          var label = document.createElement("span");
          label.textContent = entry;
          el.appendChild(label);
          var x = document.createElement("button");
          x.type = "button";
          x.className = "chipx";
          x.textContent = "×";
          x.setAttribute("aria-label", "Remove " + entry);
          // mousedown, not click, for the preventDefault: without it the click
          // blurs the input first, `commit` rebuilds the chips, and the button
          // being clicked is gone before the click lands on it.
          x.addEventListener("mousedown", function (ev) { ev.preventDefault(); });
          x.addEventListener("click", function () {
            entries.splice(i, 1);
            sync();
            input.focus();
          });
          el.appendChild(x);
          box.insertBefore(el, input);
        });
      }

      function commit() {
        var added = 0;
        splitList(input.value).forEach(function (entry) {
          if (entries.indexOf(entry) < 0) {
            entries.push(entry);
            added++;
          }
        });
        if (!added && input.value === "") {
          // Nothing to do — and rebuilding the chips anyway would destroy
          // whichever one the user is in the middle of clicking.
          return;
        }
        input.value = "";
        sync();
      }

      form.qcAddRecipient = form.qcAddRecipient || {};
      form.qcAddRecipient[name] = function (addr) {
        if (entries.indexOf(addr) < 0) entries.push(addr);
        sync();
      };

      input.addEventListener("keydown", function (ev) {
        if (ev.key === "Enter" || ev.key === ",") {
          // Enter in a recipient field commits the address. It must never
          // submit the message: half-typed recipients are how mail goes to the
          // wrong person.
          ev.preventDefault();
          commit();
          return;
        }
        if (ev.key === "Backspace" && input.value === "" && entries.length) {
          entries.pop();
          sync();
        }
      });
      input.addEventListener("blur", commit);
      input.addEventListener("paste", function () {
        window.setTimeout(commit, 0);
      });
      form.addEventListener("submit", commit);
      sync();
    });
  }

  // ---- attachments --------------------------------------------------------
  //
  // A <input type=file multiple> has no way to drop one file from its
  // selection, so the list is rebuilt through a DataTransfer and assigned back.
  // The size check matters more than it looks: the ceiling is on the whole
  // request body, and exceeding it is a connection-level rejection that loses
  // everything typed.

  function initAttachments(form) {
    var input = form.querySelector("input[type=file][name=attachment]");
    var list = form.querySelector("#attachlist");
    var note = form.querySelector("[data-maxbody]");
    if (!input || !list) return;
    if (typeof DataTransfer === "undefined") return;

    var max = note ? parseInt(note.getAttribute("data-maxbody"), 10) : 0;
    var chosen = [];

    function render() {
      list.textContent = "";
      list.hidden = chosen.length === 0;
      chosen.forEach(function (file, i) {
        var li = document.createElement("li");
        var name = document.createElement("span");
        name.textContent = file.name + " (" + bytesLabel(file.size) + ")";
        li.appendChild(name);
        var x = document.createElement("button");
        x.type = "button";
        x.className = "btn sec";
        x.textContent = msg(form, "remove", "Remove");
        x.addEventListener("click", function () {
          chosen.splice(i, 1);
          apply();
        });
        li.appendChild(x);
        list.appendChild(li);
      });
    }

    function apply() {
      var dt = new DataTransfer();
      chosen.forEach(function (f) { dt.items.add(f); });
      input.files = dt.files;
      render();
    }

    input.addEventListener("change", function () {
      for (var i = 0; i < input.files.length; i++) {
        var f = input.files[i];
        var already = chosen.some(function (c) {
          return c.name === f.name && c.size === f.size && c.lastModified === f.lastModified;
        });
        if (!already) chosen.push(f);
      }
      apply();
    });

    form.addEventListener("submit", function (ev) {
      if (!max) return;
      // Attachments travel base64-encoded, which is four bytes on the wire for
      // every three of file — so the check has to be against the encoded size,
      // not the file size, or a 9 MB attachment passes here and is rejected by
      // the server.
      var total = 0;
      chosen.forEach(function (f) { total += Math.ceil(f.size / 3) * 4; });
      var text = form.querySelector("textarea[name=body]");
      total += text ? text.value.length * 2 : 0;
      if (total > max) {
        ev.preventDefault();
        window.alert(msg(form, "toobig", "That is over the size limit."));
      }
    });

    render();
  }

  // ---- drafts: autosave, and the guard on leaving -------------------------
  //
  // The server answers /mail/draft with the draft's number, which goes back
  // into the form — so a message autosaved ten times leaves one draft, not ten.

  function initDrafts(form) {
    var status = form.querySelector("#compose-status");
    var draftOf = form.querySelector("input[name=draft_of]");
    var dirty = false;
    var saving = false;

    form.addEventListener("input", function () { dirty = true; });
    form.addEventListener("submit", function () { dirty = false; });

    function say(text) {
      if (status) status.textContent = text;
    }

    function save() {
      if (!dirty || saving || !window.fetch) return;
      if (form.qcSyncBody) form.qcSyncBody();
      var data = new FormData(form);
      // Files are not re-uploaded every minute; a draft keeps its text, and the
      // attachments ride along with the send.
      data.delete("attachment");
      data.delete("draft");
      saving = true;
      window
        .fetch("/mail/draft", { method: "POST", body: data, credentials: "same-origin" })
        .then(function (res) { return res.ok ? res.text() : null; })
        .then(function (num) {
          saving = false;
          if (!num || !(parseInt(num, 10) > 0)) return;
          if (draftOf) draftOf.value = num;
          dirty = false;
          say(msg(form, "saved", "Draft saved"));
        })
        .catch(function () { saving = false; });
    }

    var timer = window.setInterval(function () {
      if (!document.contains(form)) {
        window.clearInterval(timer);
        return;
      }
      save();
    }, 60000);

    // The guard. Browsers show their own wording; the string only has to be
    // non-empty for the prompt to appear at all.
    window.addEventListener("beforeunload", function (ev) {
      if (!dirty || !document.contains(form)) return;
      ev.preventDefault();
      ev.returnValue = msg(form, "unsaved", "This message has not been sent yet.");
      return ev.returnValue;
    });
  }

  // ---- the address-book picker --------------------------------------------
  //
  // The search half is htmx talking to /mail/addressbook, so the results are
  // swapped in and out; the click handler is therefore delegated to the panel
  // rather than bound to the buttons that happen to be there right now.

  function initAddressBook(form) {
    var scope = form.closest(".compose") || document;
    var toggle = scope.querySelector("#addressbook-toggle");
    var panel = scope.querySelector("#addressbook-panel");
    if (!toggle || !panel) return;

    var target = "to";
    ["to", "cc", "bcc"].forEach(function (name) {
      var el = scope.querySelector("#compose-" + name);
      if (el) el.addEventListener("focus", function () { target = name; });
    });

    toggle.addEventListener("click", function () {
      panel.hidden = !panel.hidden;
      if (!panel.hidden) {
        var q = panel.querySelector("#addressbook-q");
        if (q) q.focus();
      }
    });

    panel.addEventListener("click", function (ev) {
      var btn = ev.target.closest ? ev.target.closest("button[data-addr]") : null;
      if (!btn) return;
      var addr = btn.getAttribute("data-addr") || "";
      var field = scope.querySelector("#compose-" + target);
      if (form.qcAddRecipient && form.qcAddRecipient[target]) {
        form.qcAddRecipient[target](addr);
      } else if (field) {
        // No chips (the editor half failed to load): fall back to appending to
        // the raw field, which is what this did before chips existed.
        var existing = field.value.replace(/,\s*$/, "");
        field.value = existing ? existing + ", " + addr : addr;
      }
      panel.hidden = true;
    });
  }

  // ---- boot ---------------------------------------------------------------

  function boot(root) {
    var scope = root && root.querySelector ? root : document;
    var form = scope.querySelector("form[data-compose]");
    if (!form && scope !== document) {
      form = document.querySelector("form[data-compose]");
    }
    if (!form || form.hasAttribute("data-composed")) return;
    form.setAttribute("data-composed", "1");
    initEditor(form);
    initRecipients(form);
    initAttachments(form);
    initDrafts(form);
    initAddressBook(form);
  }

  boot(document);
  document.addEventListener("DOMContentLoaded", function () { boot(document); });
  // Compose can arrive as a pane swap, and can arrive again after one.
  document.addEventListener("htmx:afterSwap", function (ev) { boot(ev.target); });
})();
