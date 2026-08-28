// The rich-text composer.
//
// Deliberately hand-rolled, ~200 lines, no dependency. The alternative was
// vendoring TinyMCE or Quill: 200 KB to 1 MB of third-party JavaScript whose
// CVEs we would then own, most of them needing a build step this repo does not
// have and should not gain, and all of them emitting markup that has to be
// converted to mail-safe HTML anyway. `contenteditable` plus `execCommand`
// covers exactly the six things HTML mail actually supports.
//
// `execCommand` is formally deprecated and universally implemented. The
// replacement is to hand-roll Range surgery, which is considerably more code and
// more ways to corrupt a selection.
//
// **This file is an enhancement, never a requirement.** The form ships with a
// working <textarea>; if this never runs, composing still works and sends plain
// text. That is why every hook below bails quietly when its element is absent.
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

  function initEditor(form) {
    var textarea = form.querySelector("textarea[name=body]");
    var htmlField = form.querySelector("input[name=html_body]");
    var richToggle = form.querySelector("input[name=rich]");
    if (!textarea || !htmlField || !richToggle) return;

    // ---- the editor surface -------------------------------------------------

    var editor = document.createElement("div");
    editor.className = "richbody";
    editor.setAttribute("contenteditable", "true");
    editor.setAttribute("role", "textbox");
    editor.setAttribute("aria-multiline", "true");
    editor.setAttribute("aria-label", "Message body");

    // Seed from whatever the server put in the textarea: a reply's quoted text, a
    // draft being resumed. Escaped, then newlines become breaks — the textarea
    // holds plain text, so treating it as markup would execute a reply's quoted
    // content.
    function seed(text) {
      var div = document.createElement("div");
      div.textContent = text;
      return div.innerHTML.replace(/\n/g, "<br>");
    }
    editor.innerHTML = seed(textarea.value);

    var toolbar = document.createElement("div");
    toolbar.className = "richtools";

    // Only what mail HTML renders reliably across clients. A colour picker and a
    // font menu would produce markup half of them ignore.
    var BUTTONS = [
      ["bold", "B", "Bold", "b"],
      ["italic", "I", "Italic", "i"],
      ["underline", "U", "Underline", "u"],
      ["insertUnorderedList", "• List", "Bulleted list", null],
      ["insertOrderedList", "1. List", "Numbered list", null],
      ["formatBlock:blockquote", "“ Quote", "Quote", null],
      ["removeFormat", "Clear", "Remove formatting", null]
    ];

    BUTTONS.forEach(function (spec) {
      var btn = document.createElement("button");
      btn.type = "button";
      btn.className = "btn sec";
      btn.title = spec[2] + (spec[3] ? " (Ctrl+" + spec[3].toUpperCase() + ")" : "");
      btn.textContent = spec[1];
      btn.addEventListener("mousedown", function (ev) {
        // mousedown, not click: clicking a button would move focus out of the
        // editor and collapse the selection before the command ran.
        ev.preventDefault();
        exec(spec[0]);
      });
      toolbar.appendChild(btn);
    });

    var linkBtn = document.createElement("button");
    linkBtn.type = "button";
    linkBtn.className = "btn sec";
    linkBtn.textContent = "Link";
    linkBtn.title = "Insert a link";
    linkBtn.addEventListener("mousedown", function (ev) {
      ev.preventDefault();
      var url = window.prompt("Link to which address?", "https://");
      if (!url) return;
      // Only the schemes the server's allow-list keeps, so the button cannot
      // produce something that silently disappears on save.
      if (!/^(https?:|mailto:)/i.test(url)) {
        window.alert("Links must start with http://, https:// or mailto:");
        return;
      }
      exec("createLink", url);
    });
    toolbar.appendChild(linkBtn);

    function exec(command, value) {
      editor.focus();
      var parts = command.split(":");
      try {
        if (parts.length === 2) {
          document.execCommand(parts[0], false, parts[1]);
        } else {
          document.execCommand(command, false, value === undefined ? null : value);
        }
      } catch (e) {
        // An unsupported command is not worth breaking the composer over.
      }
    }

    // Ctrl/Cmd+B/I/U. The browser usually does these anyway inside a
    // contenteditable; binding them means the behaviour is the same everywhere.
    editor.addEventListener("keydown", function (ev) {
      if (!(ev.ctrlKey || ev.metaKey) || ev.altKey) return;
      var map = { b: "bold", i: "italic", u: "underline" };
      var cmd = map[ev.key.toLowerCase()];
      if (cmd) {
        ev.preventDefault();
        exec(cmd);
      }
    });

    // ---- paste --------------------------------------------------------------
    // A paste from a web page carries its whole stylesheet, its scripts and its
    // tracking pixels. The server's allow-list is the actual defence — this only
    // spares the user a body full of markup that is about to be stripped anyway.
    editor.addEventListener("paste", function (ev) {
      if (!ev.clipboardData) return;
      var html = ev.clipboardData.getData("text/html");
      var text = ev.clipboardData.getData("text/plain");
      ev.preventDefault();
      if (html) {
        // Reduce to text plus paragraph breaks. Deliberately crude: keeping some
        // of a hostile page's markup is worse than losing its formatting.
        var tmp = document.createElement("div");
        tmp.innerHTML = html;
        var stripped = tmp.textContent || "";
        document.execCommand("insertText", false, stripped);
        return;
      }
      document.execCommand("insertText", false, text || "");
    });

    // ---- inline images ------------------------------------------------------
    // A dropped or pasted image becomes a data: URI in the editor. The server
    // turns those into `cid:` parts on send, so the message is self-contained
    // rather than pointing at anything remote.
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
        editor.focus();
        document.execCommand("insertHTML", false,
          '<img src="' + reader.result + '" alt="' + (file.name || "image") + '">');
      };
      reader.readAsDataURL(file);
    }

    editor.addEventListener("dragover", function (ev) { ev.preventDefault(); });
    editor.addEventListener("drop", function (ev) {
      if (!ev.dataTransfer || !ev.dataTransfer.files || !ev.dataTransfer.files.length) return;
      ev.preventDefault();
      insertImage(ev.dataTransfer.files[0]);
    });

    // ---- wiring -------------------------------------------------------------

    textarea.parentNode.insertBefore(toolbar, textarea);
    textarea.parentNode.insertBefore(editor, textarea);

    // The textarea stays in the DOM, hidden, and stays authoritative for the
    // plain-text half of the message. Removing it would mean a JS error mid-edit
    // loses the body entirely.
    textarea.classList.add("richhidden");

    var label = richToggle.closest("label") || richToggle.parentNode;

    function setRich(on) {
      editor.style.display = on ? "" : "none";
      toolbar.style.display = on ? "" : "none";
      textarea.classList.toggle("richhidden", on);
    }

    // Whatever the server rendered it as: the preference is the user's, and
    // forcing it on here was the composer overruling them on every message.
    setRich(richToggle.checked);
    richToggle.addEventListener("change", function () { setRich(richToggle.checked); });
    if (label) label.classList.add("richavailable");

    // Named, and hung on the form, because the autosave below has to produce
    // exactly the same two fields a real submit would. Two copies of this would
    // be two definitions of what a draft contains.
    form.qcSyncBody = function () {
      if (richToggle.checked) {
        htmlField.value = editor.innerHTML;
        // The plain-text half comes from the editor's text, so a recipient with no
        // HTML gets what was actually written rather than the pre-edit seed.
        textarea.value = editor.innerText || editor.textContent || "";
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
