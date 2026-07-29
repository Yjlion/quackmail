// QuackCit web interface — shared progressive enhancement.
//
// Every page works without this file. It is loaded with `defer` from
// /static/qc.<hash>.js, cached forever, and it may not assume any particular
// page is present: it looks for markers and does nothing when they are absent.
//
// The rule for this whole module: never make script the only way to do
// something. Anything here must be an improvement on a flow that already works
// as plain HTML.

(function () {
  "use strict";

  // Lets CSS style the enhanced state without guessing whether script ran.
  document.documentElement.setAttribute("data-js", "1");

  // A confirmation step for the destructive buttons. Without script the form
  // simply submits, which is today's behaviour, so this only ever adds a
  // safety net — it never gates an action that would otherwise be available.
  document.addEventListener("submit", function (ev) {
    var form = ev.target;
    if (!(form instanceof HTMLFormElement)) return;
    var ask = form.getAttribute("data-confirm");
    if (!ask) return;
    if (!window.confirm(ask)) ev.preventDefault();
  });

  // Close the mobile sidebar after following a link in it. The checkbox is the
  // real state; this just spares a second tap.
  var toggle = document.getElementById("navtoggle");
  var sidebar = document.querySelector(".sidebar");
  if (toggle && sidebar) {
    sidebar.addEventListener("click", function (ev) {
      if (ev.target.closest("a")) toggle.checked = false;
    });
  }
})();
