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

(function () {
  "use strict";

  var form = document.querySelector("form[data-compose]");
  if (!form) return;

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

  richToggle.checked = true;
  setRich(true);
  richToggle.addEventListener("change", function () { setRich(richToggle.checked); });
  if (label) label.classList.add("richavailable");

  form.addEventListener("submit", function () {
    if (richToggle.checked) {
      htmlField.value = editor.innerHTML;
      // The plain-text half comes from the editor's text, so a recipient with no
      // HTML gets what was actually written rather than the pre-edit seed.
      htmlField.setAttribute("data-provided", "1");
      textarea.value = editor.innerText || editor.textContent || "";
    } else {
      htmlField.value = "";
    }
  });
})();
