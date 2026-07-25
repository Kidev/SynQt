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
 * every explanation in it by one.
 *
 * Everything is rebuilt on each page change through Material's `document$` observable, and
 * the document-level listeners are installed once, since the document survives instant
 * navigation.
 */
(function () {
  "use strict";

  var PIN = "synqt-pinned";
  var HINT_UNPINNED = "Click to pin";
  var HINT_PINNED = "Pinned. Hover a line to see what it does, click again to unpin.";

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
   * marked. The highlighter emits a flat run of token spans separated by plain newline
   * text, and no token of these snippets spans a line break; a snippet that ever did (a
   * block comment, a multi-line string) is left alone rather than re-flowed wrongly. */
  function wrapLines(code) {
    var children = Array.prototype.slice.call(code.childNodes);
    var lines = [];
    var line = document.createElement("span");
    line.className = "synqt-code__line";
    lines.push(line);
    for (var index = 0; index < children.length; index++) {
      var node = children[index];
      if (node.nodeType === 3) {
        var parts = node.data.split("\n");
        for (var part = 0; part < parts.length; part++) {
          if (part > 0) {
            line = document.createElement("span");
            line.className = "synqt-code__line";
            lines.push(line);
          }
          if (parts[part]) {
            line.appendChild(document.createTextNode(parts[part]));
          }
        }
      } else {
        if (node.textContent.indexOf("\n") !== -1) {
          return null;
        }
        line.appendChild(node);
      }
    }
    while (lines.length > 0 && lines[lines.length - 1].textContent === "") {
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
      for (var line = 0; line < lines.length; line++) {
        if (lines[line].getAttribute("data-gloss")) {
          continue;
        }
        if (lines[line].textContent.indexOf(fragment) !== -1) {
          lines[line].setAttribute("data-gloss", entries[entry].textContent.trim());
          lines[line].className = "synqt-code__line synqt-code__line--gloss";
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
