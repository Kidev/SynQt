// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

/* The home page's "What it looks like" project.
 *
 * The section draws one small system as a diagram, with a file behind every part of it:
 * the configuration behind the cog, the contract behind the link the browser and the edge
 * share, and one QML file behind each entity. Hovering shows a file; this script adds the
 * two things hovering alone cannot give it.
 *
 * The first is pinning. A hovered file closes the moment the pointer leaves the node, which
 * is no use for reading twenty lines of QML and impossible on a touch screen. A click pins
 * the file open (one at a time, Escape or a click outside closes it), and a line under each
 * file says which of the two states it is in.
 *
 * The second is the glossary. Each file in docs/index.md is followed by a hidden list whose
 * entries name a fragment of it and say what that line does. This wraps every line of the
 * highlighted code in its own element, hands each gloss to the first line that contains its
 * fragment, and shows the text under the file while that line is hovered. It reads the
 * fragments rather than line numbers so that editing a snippet does not silently shift
 * every explanation in it by one. A gloss that also carries a `data-href` turns its line
 * into a link to the page that documents it, which is where the section hands the reader
 * on: to the class in the C++ reference for a runtime accessor, or to the framework page
 * that covers the idea for everything else.
 *
 * Everything is rebuilt on each page change through Material's `document$` observable, and
 * the document-level listeners are installed once, since the document survives instant
 * navigation.
 */
(function () {
  "use strict";

  var PIN = "synqt-pinned";
  var HINT_UNPINNED = "Click to pin";
  var HINT_PINNED = "Pinned. Hover a line for what it does, an arrow to open its page. "
    + "Click again to unpin.";

  // Every part of the diagram that has a file behind it: the five entity and contract
  // hotspots over the drawing, and the configuration chip above it.
  function panels() {
    return Array.prototype.slice.call(
      document.querySelectorAll(".synqt-flow__hotspot, .synqt-config"));
  }

  function tooltipOf(panel) {
    return panel.querySelector(".synqt-flow__tooltip, .synqt-config__tooltip");
  }

  /* What a reader clicks or focuses to open a file. For an entity it is the hotspot itself,
   * an invisible target the size of its node in the diagram. For the configuration it is
   * the chip, not the row the chip is centered in, which spans the width of the page and
   * would otherwise pin the file from a click in the empty space beside it. */
  function triggerOf(panel) {
    return panel.querySelector(".synqt-config__trigger") || panel;
  }

  /* Give every line of the highlighted block its own element, so a line can be hovered and
   * marked.
   *
   * The highlighter usually emits a flat run of token spans with plain newline text
   * between them, but it is not required to: a token that covers a blank line, and any
   * future lexer that emits one token per block comment, hands back a span with a line
   * break inside it. Such a span is reopened on the next line, the way a text editor
   * splits a styled run, so the colouring survives the split. The whole block is built
   * first and swapped in at the end, so a snippet this cannot handle is left exactly as
   * the highlighter wrote it instead of half rewritten. */
  function wrapLines(code) {
    var lines = [];
    var open = [];
    var line = null;

    function start() {
      line = document.createElement("span");
      line.className = "synqt-code__line";
      lines.push(line);
      var host = line;
      for (var depth = 0; depth < open.length; depth++) {
        var reopened = open[depth].source.cloneNode(false);
        host.appendChild(reopened);
        open[depth].live = reopened;
        host = reopened;
      }
    }

    function tip() {
      return open.length > 0 ? open[open.length - 1].live : line;
    }

    function add(node) {
      if (node.nodeType === 3) {
        var parts = node.data.split("\n");
        for (var part = 0; part < parts.length; part++) {
          if (part > 0) {
            start();
          }
          if (parts[part]) {
            tip().appendChild(document.createTextNode(parts[part]));
          }
        }
        return;
      }
      if (node.nodeType !== 1) {
        return;
      }
      if (node.textContent.indexOf("\n") === -1) {
        tip().appendChild(node.cloneNode(true));
        return;
      }
      var shell = node.cloneNode(false);
      tip().appendChild(shell);
      open.push({ source: node, live: shell });
      var children = Array.prototype.slice.call(node.childNodes);
      for (var index = 0; index < children.length; index++) {
        add(children[index]);
      }
      open.pop();
    }

    start();
    var top = Array.prototype.slice.call(code.childNodes);
    for (var at = 0; at < top.length; at++) {
      add(top[at]);
    }
    while (lines.length > 1 && lines[lines.length - 1].textContent === "") {
      lines.pop();
    }
    code.textContent = "";
    for (var written = 0; written < lines.length; written++) {
      code.appendChild(lines[written]);
    }
    return lines;
  }

  function applyGlossary(panel, lines) {
    var list = panel.querySelector(".synqt-flow__glossary");
    if (!list) {
      return;
    }
    var entries = list.querySelectorAll("li[data-code]");
    for (var entry = 0; entry < entries.length; entry++) {
      var fragment = entries[entry].getAttribute("data-code");
      var href = entries[entry].getAttribute("data-href");
      for (var line = 0; line < lines.length; line++) {
        if (lines[line].getAttribute("data-gloss")) {
          continue;
        }
        if (lines[line].textContent.indexOf(fragment) !== -1) {
          lines[line].setAttribute("data-gloss", entries[entry].textContent.trim());
          lines[line].className = "synqt-code__line synqt-code__line--gloss";
          if (href) {
            lines[line].setAttribute("data-href", href);
            lines[line].className += " synqt-code__line--linked";
          }
          break;
        }
      }
    }
    list.parentNode.removeChild(list);
  }

  function hintOf(panel) {
    var tooltip = tooltipOf(panel);
    var hint = tooltip.querySelector(".synqt-flow__hint");
    if (!hint) {
      hint = document.createElement("p");
      hint.className = "synqt-flow__hint";
      // The text changes under the pointer without the pointer moving to it, so a screen
      // reader has to be told it changed.
      hint.setAttribute("aria-live", "polite");
      tooltip.appendChild(hint);
    }
    return hint;
  }

  function say(panel, text) {
    hintOf(panel).textContent = text;
  }

  function unpinAll(except) {
    var all = panels();
    for (var index = 0; index < all.length; index++) {
      if (all[index] !== except && all[index].classList.contains(PIN)) {
        all[index].classList.remove(PIN);
        say(all[index], HINT_UNPINNED);
      }
    }
  }

  function togglePin(panel) {
    var pinned = panel.classList.toggle(PIN);
    unpinAll(panel);
    say(panel, pinned ? HINT_PINNED : HINT_UNPINNED);
    if (pinned && tooltipOf(panel).scrollIntoView) {
      // A file is twenty lines and the diagram sits mid page, so a card opened near the
      // fold can end below it. `nearest` scrolls the least that brings it fully in view,
      // and does nothing at all when it already is.
      tooltipOf(panel).scrollIntoView({ block: "nearest", behavior: "smooth" });
    }
  }

  function prepare(panel) {
    if (panel.getAttribute("data-synqt-flow") === "ready") {
      return;
    }
    panel.setAttribute("data-synqt-flow", "ready");

    var code = panel.querySelector(".highlight code");
    if (code) {
      var lines = wrapLines(code);
      if (lines) {
        applyGlossary(panel, lines);
      }
    }
    say(panel, HINT_UNPINNED);

    var trigger = triggerOf(panel);

    // A click on the file itself is a click on text (selecting a line, copying a name), not
    // a request to close what is being read.
    trigger.addEventListener("click", function (event) {
      if (tooltipOf(panel).contains(event.target)) {
        return;
      }
      event.preventDefault();
      togglePin(panel);
    });

    trigger.addEventListener("keydown", function (event) {
      if (event.key === "Enter" || event.key === " " || event.key === "Spacebar") {
        event.preventDefault();
        togglePin(panel);
      }
    });

    tooltipOf(panel).addEventListener("mouseover", function (event) {
      if (!panel.classList.contains(PIN)) {
        return;
      }
      var line = event.target.closest ? event.target.closest(".synqt-code__line") : null;
      var gloss = line && line.getAttribute("data-gloss");
      say(panel, gloss || HINT_PINNED);
    });

    tooltipOf(panel).addEventListener("mouseleave", function () {
      if (panel.classList.contains(PIN)) {
        say(panel, HINT_PINNED);
      }
    });

    // A line whose gloss names a page opens it, which is how the section hands the
    // reader on: the runtime accessors to their class in the C++ reference, the rest
    // to the page of the docs that covers them. Selecting text inside the file ends in
    // a click too, so a click that finished a selection is left alone.
    tooltipOf(panel).addEventListener("click", function (event) {
      if (!panel.classList.contains(PIN) || !event.target.closest) {
        return;
      }
      var line = event.target.closest(".synqt-code__line[data-href]");
      if (!line) {
        return;
      }
      var selection = window.getSelection && window.getSelection();
      if (selection && !selection.isCollapsed) {
        return;
      }
      event.preventDefault();
      window.location.href = line.getAttribute("data-href");
    });
  }

  document.addEventListener("keydown", function (event) {
    if (event.key === "Escape") {
      unpinAll(null);
    }
  });

  document.addEventListener("click", function (event) {
    var all = panels();
    for (var index = 0; index < all.length; index++) {
      if (all[index].contains(event.target)) {
        return;
      }
    }
    unpinAll(null);
  });

  function setup() {
    var all = panels();
    for (var index = 0; index < all.length; index++) {
      prepare(all[index]);
    }
  }

  if (window.document$ && typeof window.document$.subscribe === "function") {
    window.document$.subscribe(setup);
  } else if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", setup);
  } else {
    setup();
  }
})();
