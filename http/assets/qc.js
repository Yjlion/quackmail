// QuackCit web interface — shared behaviour.
//
// Loaded with `defer` from /static/qc.<hash>.js and cached forever. It may not
// assume any particular page is present: it looks for markers and does nothing
// when they are absent, which is what lets one file serve thirty screens.
//
// Pages are still rendered whole by the server — htmx swaps fragments into an
// already-complete document rather than assembling one — so everything here
// improves a flow that already works by ordinary navigation.

(function () {
  "use strict";

  document.documentElement.setAttribute("data-js", "1");

  // htmx evaluates nothing. Its only eval() and new Function() sit behind this
  // flag, so turning it off is what lets the page's CSP keep refusing
  // 'unsafe-eval'. The cost is hx-on: attributes and event-filter expressions,
  // which this interface does not use.
  function configureHtmx() {
    if (!window.htmx) return;
    window.htmx.config.allowEval = false;
    window.htmx.config.includeIndicatorStyles = false;
  }
  configureHtmx();
  document.addEventListener("DOMContentLoaded", configureHtmx);

  // ---- the reading pane ---------------------------------------------------
  // The server sets .panes.open when it renders a page with ?open=; an htmx
  // swap replaces the reader alone, so the container has to be told here. It
  // matters beyond styling: below the sidebar breakpoint `open` is what hides
  // the list and reveals the reader's way back.

  document.addEventListener("htmx:afterSwap", function (ev) {
    if (!ev.target || ev.target.id !== "reader") return;
    var panes = ev.target.closest(".panes");
    if (panes) panes.classList.add("open");
    ev.target.scrollTop = 0;
  });

  // Going back through the pushed history must undo it again.
  document.addEventListener("htmx:historyRestore", function () {
    var panes = document.querySelector(".panes");
    var reader = document.getElementById("reader");
    if (!panes || !reader) return;
    panes.classList.toggle("open", reader.innerHTML.trim() !== "");
  });

  // ---- confirmations ------------------------------------------------------
  // A safety net on the destructive buttons. Without it the form simply
  // submits, which is the behaviour every one of these had before.

  document.addEventListener("submit", function (ev) {
    var form = ev.target;
    if (!(form instanceof HTMLFormElement)) return;
    var ask = form.getAttribute("data-confirm");
    if (ask && !window.confirm(ask)) ev.preventDefault();
  });

  // ---- select-all ---------------------------------------------------------
  // Delegated, because the listing is one of the things htmx swaps.

  document.addEventListener("change", function (ev) {
    var t = ev.target;
    if (!t.classList || !t.classList.contains("pickall")) return;
    var boxes = document.querySelectorAll('input[type="checkbox"][name="msgnum"]');
    for (var i = 0; i < boxes.length; i++) boxes[i].checked = t.checked;
  });

  // ---- mobile sidebar -----------------------------------------------------
  // The checkbox is the real state; this just spares a second tap.

  var toggle = document.getElementById("navtoggle");
  var sidebar = document.querySelector(".sidebar");
  if (toggle && sidebar) {
    sidebar.addEventListener("click", function (ev) {
      if (ev.target.closest("a")) toggle.checked = false;
    });
  }

  // ---- the message list ---------------------------------------------------

  function list() { return document.querySelector(".msglist"); }
  function rows() { return document.querySelectorAll(".msglist > li"); }

  function currentIndex() {
    var all = rows();
    for (var i = 0; i < all.length; i++) {
      if (all[i].classList.contains("cursor")) return i;
      if (all[i].getAttribute("aria-current") === "true") return i;
    }
    return -1;
  }

  function focusRow(i) {
    var all = rows();
    if (!all.length) return;
    if (i < 0) i = 0;
    if (i >= all.length) i = all.length - 1;
    for (var j = 0; j < all.length; j++) all[j].classList.remove("cursor");
    all[i].classList.add("cursor");
    all[i].scrollIntoView({ block: "nearest" });
  }

  function openRow(i) {
    var all = rows();
    if (i < 0 || i >= all.length) return;
    var a = all[i].querySelector(".subject a");
    if (a) a.click();
  }

  // A whole row is a click target, not just the subject link. The link is still
  // there and still what keyboard and screen-reader users follow — this only
  // widens the hit area for a pointer.
  document.addEventListener("click", function (ev) {
    if (!ev.target.closest) return;
    var row = ev.target.closest(".msglist .row");
    if (!row) return;
    if (ev.target.closest("a, button, input, label, summary")) return;
    var a = row.querySelector(".subject a");
    if (a) a.click();
  });

  // ---- keyboard shortcuts -------------------------------------------------

  var pending = "";   // the first half of a two-key sequence, e.g. "g"
  var pendingAt = 0;

  function typing(el) {
    if (!el) return false;
    var tag = (el.tagName || "").toLowerCase();
    return tag === "input" || tag === "textarea" || tag === "select" ||
           el.isContentEditable;
  }

  function clickSelector(sel) {
    var el = document.querySelector(sel);
    if (el) { el.click(); return true; }
    return false;
  }

  document.addEventListener("keydown", function (ev) {
    if (ev.ctrlKey || ev.metaKey || ev.altKey) return;
    if (typing(ev.target)) {
      if (ev.key === "Escape") ev.target.blur();
      return;
    }

    var k = ev.key;

    // Two-key sequences: g then a destination, the convention every mail
    // client with shortcuts already uses.
    if (pending === "g" && Date.now() - pendingAt < 1500) {
      pending = "";
      var dest = { i: "/mail/", c: "/mail/compose", r: "/bbs/",
                   s: "/search", p: "/prefs" }[k];
      if (dest) { ev.preventDefault(); window.location.href = dest; }
      return;
    }
    pending = "";

    if (k === "g") { pending = "g"; pendingAt = Date.now(); return; }

    if (k === "?") { ev.preventDefault(); showHelp(); return; }
    if (k === "/") {
      var q = document.getElementById("topq");
      if (q) { ev.preventDefault(); q.focus(); q.select(); }
      return;
    }
    if (k === "c") { ev.preventDefault(); window.location.href = "/mail/compose"; return; }

    // Everything below acts on a listing, so it stays inert on the other
    // twenty-odd screens this file is also loaded into.
    if (!list()) return;

    switch (k) {
      case "j": ev.preventDefault(); focusRow(currentIndex() + 1); break;
      case "k": ev.preventDefault(); focusRow(currentIndex() - 1); break;
      case "Enter":
      case "o": ev.preventDefault(); openRow(currentIndex()); break;
      case "u": if (clickSelector(".backtolist")) ev.preventDefault(); break;
      case "r": if (clickSelector("[data-key='reply']")) ev.preventDefault(); break;
      case "a": if (clickSelector("[data-key='replyall']")) ev.preventDefault(); break;
      case "f": if (clickSelector("[data-key='forward']")) ev.preventDefault(); break;
      case "e": if (clickSelector("[data-key='archive']")) ev.preventDefault(); break;
      case "#": if (clickSelector("[data-key='trash']")) ev.preventDefault(); break;
      case "s": if (clickSelector("[data-key='flag']")) ev.preventDefault(); break;
      case "x": {
        var all = rows(), i = currentIndex();
        if (i >= 0) {
          var box = all[i].querySelector('input[type="checkbox"][name="msgnum"]');
          if (box) { ev.preventDefault(); box.checked = !box.checked; }
        }
        break;
      }
    }
  });

  // ---- the help overlay ---------------------------------------------------
  // Built here rather than shipped with every page: it is the one piece of the
  // interface that exists only because shortcuts do. The labels come from the
  // server so they go through the message catalog like everything else; the
  // fallbacks below are only reached if the attribute is missing.

  function showHelp() {
    var dlg = document.getElementById("keyshelp");
    if (dlg) { dlg.showModal(); return; }

    var spec = document.documentElement.getAttribute("data-keys");
    var pairs = [];
    if (spec) {
      // "keys|description" per entry, entries separated by "~" — a flat string
      // because it has to survive an HTML attribute.
      var items = spec.split("~");
      for (var i = 0; i < items.length; i++) {
        var bits = items[i].split("|");
        if (bits.length === 2) pairs.push([bits[0], bits[1]]);
      }
    }
    if (!pairs.length) {
      pairs = [
        ["j k", "Move down and up"],
        ["Enter", "Open the selected message"],
        ["u", "Back to the list"],
        ["x", "Select the message"],
        ["c", "Compose"],
        ["r a f", "Reply, reply all, forward"],
        ["e", "Archive"],
        ["#", "Move to Trash"],
        ["s", "Flag"],
        ["/", "Search"],
        ["g i", "Go to the inbox"],
        ["?", "This list"]
      ];
    }

    dlg = document.createElement("dialog");
    dlg.id = "keyshelp";
    dlg.className = "keyshelp";

    var art = document.createElement("article");
    var h = document.createElement("h3");
    h.textContent = document.documentElement.getAttribute("data-keys-title") ||
                    "Keyboard shortcuts";
    art.appendChild(h);

    var dl = document.createElement("dl");
    dl.className = "keys";
    for (var n = 0; n < pairs.length; n++) {
      var dt = document.createElement("dt");
      var keys = pairs[n][0].split(" ");
      for (var m = 0; m < keys.length; m++) {
        if (m) dt.appendChild(document.createTextNode(" "));
        var kbd = document.createElement("kbd");
        kbd.textContent = keys[m];
        dt.appendChild(kbd);
      }
      var dd = document.createElement("dd");
      dd.textContent = pairs[n][1];
      dl.appendChild(dt);
      dl.appendChild(dd);
    }
    art.appendChild(dl);

    var close = document.createElement("button");
    close.className = "secondary";
    close.textContent = document.documentElement.getAttribute("data-keys-close") || "Close";
    close.addEventListener("click", function () { dlg.close(); });
    art.appendChild(close);

    dlg.appendChild(art);
    document.body.appendChild(dlg);
    dlg.showModal();
  }
})();

// ---- data tables: movable and resizable columns ---------------------------
//
// Sorting is the server's (`?sort=`/`?dir=`) because the listings that need it
// paginate before they load their rows. Order and width are the reader's, and
// belong to this browser rather than to the account — so they live in
// localStorage keyed by the table's own id, and a browser that has never
// touched a table renders exactly what the server sent.
//
// Off below the card breakpoint: at that width every row is a card and a
// reordered column means nothing.

(function () {
  "use strict";

  var WIDE = "(min-width: 52.0625rem)";

  function load(id) {
    try {
      return JSON.parse(window.localStorage.getItem("qc.cols." + id)) || {};
    } catch (e) {
      // Private browsing, a full quota, or somebody's hand-edited JSON. None of
      // those is worth losing the table over.
      return {};
    }
  }

  function save(id, state) {
    try {
      window.localStorage.setItem("qc.cols." + id, JSON.stringify(state));
    } catch (e) {
      /* nothing to do: the layout is a convenience, not data */
    }
  }

  function keysOf(table) {
    return Array.prototype.map.call(table.querySelectorAll("thead th"), function (th) {
      return th.getAttribute("data-col") || "";
    });
  }

  function colFor(table, key) {
    var cols = table.querySelectorAll("colgroup col");
    for (var i = 0; i < cols.length; i++) {
      if (cols[i].getAttribute("data-col") === key) return cols[i];
    }
    return null;
  }

  function reorderChildren(parent, from) {
    if (!parent) return;
    var kids = Array.prototype.slice.call(parent.children);
    from.forEach(function (i) { parent.appendChild(kids[i]); });
  }

  function applyOrder(table, order) {
    if (!order || !order.length) return;
    var current = keysOf(table);
    var target = order.filter(function (k) { return current.indexOf(k) >= 0; });
    // A column the stored order has never seen — one added by a later release —
    // keeps its place at the end rather than disappearing.
    current.forEach(function (k) { if (target.indexOf(k) < 0) target.push(k); });
    if (target.join(" ") === current.join(" ")) return;
    var from = target.map(function (k) { return current.indexOf(k); });
    reorderChildren(table.querySelector("colgroup"), from);
    reorderChildren(table.querySelector("thead tr"), from);
    var rows = table.querySelectorAll("tbody tr");
    for (var i = 0; i < rows.length; i++) reorderChildren(rows[i], from);
  }

  function setWidth(table, key, px) {
    var col = colFor(table, key);
    if (!col) return;
    // A <col> width is only binding under a fixed layout; without this the
    // browser is free to ignore it whenever the content is wider.
    table.style.tableLayout = "fixed";
    table.style.width = "100%";
    col.style.width = px + "px";
  }

  function applyWidths(table, widths) {
    if (!widths) return;
    Object.keys(widths).forEach(function (key) { setWidth(table, key, widths[key]); });
  }

  function wire(table, id, state) {
    var ths = table.querySelectorAll("thead th");

    Array.prototype.forEach.call(ths, function (th) {
      // draggable is set here rather than in the markup: with no script the
      // header is a heading and a sort link, nothing else.
      th.draggable = true;
      var link = th.querySelector("a");
      // An <a> is natively draggable, and dragging it would drag the URL
      // instead of the column.
      if (link) link.draggable = false;

      th.addEventListener("dragstart", function (ev) {
        if (!ev.dataTransfer) return;
        ev.dataTransfer.setData("text/plain", th.getAttribute("data-col") || "");
        ev.dataTransfer.effectAllowed = "move";
        table.classList.add("dragging");
      });
      th.addEventListener("dragend", function () { table.classList.remove("dragging"); });
      th.addEventListener("dragover", function (ev) {
        ev.preventDefault();
        if (ev.dataTransfer) ev.dataTransfer.dropEffect = "move";
        th.classList.add("dropzone");
      });
      th.addEventListener("dragleave", function () { th.classList.remove("dropzone"); });
      th.addEventListener("drop", function (ev) {
        ev.preventDefault();
        th.classList.remove("dropzone");
        var from = ev.dataTransfer ? ev.dataTransfer.getData("text/plain") : "";
        var to = th.getAttribute("data-col") || "";
        if (!from || from === to) return;
        var order = keysOf(table);
        var fromIdx = order.indexOf(from);
        var toIdx = order.indexOf(to);
        if (fromIdx < 0 || toIdx < 0) return;
        order.splice(fromIdx, 1);
        // Dropped to the right of where it started, it lands after the target;
        // to the left, before it. That is what a drop looks like it should do,
        // and always inserting before makes a rightward drag move nothing.
        var target = order.indexOf(to);
        order.splice(fromIdx < toIdx ? target + 1 : target, 0, from);
        applyOrder(table, order);
        state.order = keysOf(table);
        save(id, state);
      });

      var grip = th.querySelector(".colgrip");
      if (!grip) return;
      // Dragging the grip resizes; it must never start a column drag.
      grip.addEventListener("dragstart", function (ev) { ev.preventDefault(); });
      grip.addEventListener("mousedown", function (ev) {
        ev.preventDefault();
        ev.stopPropagation();
        var key = th.getAttribute("data-col") || "";
        var startX = ev.clientX;
        var startW = th.getBoundingClientRect().width;
        document.body.classList.add("colresizing");

        function move(e) {
          setWidth(table, key, Math.max(48, Math.round(startW + (e.clientX - startX))));
        }
        function up() {
          document.removeEventListener("mousemove", move);
          document.removeEventListener("mouseup", up);
          document.body.classList.remove("colresizing");
          state.widths = state.widths || {};
          state.widths[key] = Math.round(th.getBoundingClientRect().width);
          save(id, state);
        }
        document.addEventListener("mousemove", move);
        document.addEventListener("mouseup", up);
      });
      // Double-click a grip to hand the column back to the browser.
      grip.addEventListener("dblclick", function () {
        var key = th.getAttribute("data-col") || "";
        if (state.widths) delete state.widths[key];
        save(id, state);
        var col = colFor(table, key);
        if (col) col.style.width = "";
      });
    });
  }

  function initTables() {
    if (!window.matchMedia || !window.matchMedia(WIDE).matches) return;
    var tables = document.querySelectorAll("table.datatable[data-table]");
    Array.prototype.forEach.call(tables, function (table) {
      if (table.hasAttribute("data-cols-ready")) return;
      table.setAttribute("data-cols-ready", "1");
      var id = table.getAttribute("data-table");
      var state = load(id);
      applyOrder(table, state.order);
      applyWidths(table, state.widths);
      wire(table, id, state);
    });
  }

  initTables();
  document.addEventListener("DOMContentLoaded", initTables);
  // Listings are one of the things htmx swaps.
  document.addEventListener("htmx:afterSwap", initTables);
})();
