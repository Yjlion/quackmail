// The Sieve rule builder's live-update enhancement.
//
// Every add/delete/move/match/stop control inside #rule-builder is a plain
// form that posts and is redirected back to this same page — that full round
// trip is what has always made the builder work with no script at all, and
// it still works exactly that way if anything below fails. All this adds is
// swapping the response's own #rule-builder into the current page instead of
// navigating to it, so a click updates the cards in place rather than
// reloading the whole page.
//
// **This file is an enhancement, never a requirement.** Every form it touches
// already has a working action and method; if this never runs, or a fetch
// fails for any reason, the form is resubmitted for real rather than left
// looking like the click did nothing.
(function () {
  "use strict";

  var container = document.getElementById("rule-builder");
  if (!container) return;

  function swap(html) {
    var doc = new DOMParser().parseFromString(html, "text/html");
    var next = doc.getElementById("rule-builder");
    if (!next) return false;
    container.replaceWith(document.importNode(next, true));
    container = document.getElementById("rule-builder");
    return true;
  }

  document.addEventListener("submit", function (e) {
    var form = e.target;
    if (!(form instanceof HTMLFormElement) || !container.contains(form)) {
      return;
    }
    e.preventDefault();
    fetch(form.action, {
      method: "POST",
      body: new URLSearchParams(new FormData(form)),
      credentials: "same-origin",
    })
      .then(function (r) {
        if (!r.ok) {
          throw new Error("sieve rule-builder request failed: " + r.status);
        }
        return r.text();
      })
      .then(function (html) {
        if (!swap(html)) {
          throw new Error("no #rule-builder in the response");
        }
      })
      .catch(function () {
        form.submit();
      });
  });
})();
