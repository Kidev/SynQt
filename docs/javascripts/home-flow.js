// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

/* The home page's "What it looks like" project.
 *
 * The section is one small system, drawn three times: a project tree of its seven files,
 * a diagram of the mesh those files build, with a file behind every part of it (the
 * configuration behind the cog, the contract behind the link the browser and the edge
 * share, one QML file behind each entity), and a file view showing exactly one of those
 * files at a time. Pointing at a file in either the tree or the diagram opens it, and
 * lights it in the other, so the two are one set of triggers over the same seven files.
 * A file stays until another is pointed at, so the reader can move the pointer into the
 * file and read it, and it takes a moment's dwell to open, so a pointer crossing the
 * section on its way elsewhere does not leaf through every file behind it. The
 * configuration is shown to begin with, since it is what the rest is generated from.
 *
 * This script is the whole of that behavior, plus the glossary. Each file in
 * docs/index.md is followed by a hidden list whose entries name a fragment of it and say
 * what that line does. This wraps every line of the highlighted code in its own element,
 * hands each gloss to the first line that contains its fragment, and shows the text
 * under the file while that line is hovered. It reads the fragments rather than line
 * numbers so that editing a snippet does not silently shift every explanation in it by
 * one. A gloss that also carries a `data-href` turns its line into a link to the page
 * that documents it, which is where the section hands the reader on: to the class in the
 * C++ reference for a runtime accessor, or to the framework page that covers the idea
 * for everything else.
 *
 * Everything is rebuilt on each page change through Material's `document$` observable.
 */
(function () {
  "use strict";

  var CURRENT = "synqt-file--current";
  var STACKED = "synqt-file--stacked";
  var ON = "synqt-trigger--on";
  var SHOWN = "synqt-flow__hint--on";
  // Long enough that a pointer crossing the diagram on its way somewhere else does
  // not open three files behind it, and short enough to read as the file opening
  // where the pointer landed rather than a moment after it. Nothing is lit until
  // it elapses (the light follows the open file and nothing else, see
  // .synqt-tree__file in home.css), so a longer wait than this shows as a section
  // that lags the pointer.
  var DWELL = 100;

  /* Give every line of the highlighted block its own element, so a line can be hovered
   * and marked.
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

  function applyGlossary(file, lines) {
    var list = file.querySelector(".synqt-flow__glossary");
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

  function prepare(explorer) {
    if (explorer.getAttribute("data-synqt-flow") === "ready") {
      return;
    }
    explorer.setAttribute("data-synqt-flow", "ready");

    var view = explorer.querySelector(".synqt-explorer__view");
    var hint = explorer.querySelector(".synqt-flow__hint");
    var files = Array.prototype.slice.call(explorer.querySelectorAll(".synqt-file"));
    var triggers = Array.prototype.slice.call(explorer.querySelectorAll("[data-file]"))
      .filter(function (element) {
        return files.indexOf(element) === -1;
      });
    if (!view || !hint || files.length === 0) {
      return;
    }

    for (var index = 0; index < files.length; index++) {
      var code = files[index].querySelector(".highlight code");
      if (code) {
        applyGlossary(files[index], wrapLines(code));
      }
    }

    function explain(text) {
      hint.textContent = text || "";
      hint.classList.toggle(SHOWN, !!text);
    }

    // What a trigger names, as one string. Almost every trigger names one file, and the
    // database names two: the entity's QML and the schema.sql the query in it reads. They
    // are one thing to point at, so the pair is written on the diagram's database and on
    // both of its rows in the tree, and pointing at any of the three opens both files and
    // lights both rows. Nothing else about them is special: they are short enough to sit
    // one under the other in the panel without either of them scrolling.
    function nameOf(element) {
      return element.getAttribute("data-file").trim().split(/\s+/).join(" ");
    }

    function show(name) {
      var wanted = name.split(" ");
      var open = [];
      for (var at = 0; at < files.length; at++) {
        var current = wanted.indexOf(files[at].getAttribute("data-file")) !== -1;
        files[at].classList.toggle(CURRENT, current);
        files[at].classList.remove(STACKED);
        if (current) {
          open.push(files[at]);
        }
      }
      // With two files open, the first is as tall as it is and the second takes the rest
      // of the panel, which is what keeps the explanation and the note at the foot of it
      // rather than floating up under a short file.
      for (var index = 0; index + 1 < open.length; index++) {
        open[index].classList.add(STACKED);
      }
      for (var on = 0; on < triggers.length; on++) {
        var chosen = nameOf(triggers[on]) === name;
        triggers[on].classList.toggle(ON, chosen);
        triggers[on].setAttribute("aria-pressed", chosen ? "true" : "false");
      }
      explain("");
    }

    // A pointer on its way across the section passes over parts of the diagram it
    // has no interest in, and every one of them would otherwise swap the file being
    // read. So a part has to be pointed at rather than merely crossed: the file
    // opens once the pointer has stayed on it, and leaving before then cancels it.
    // Nothing is queued twice, so a pointer moving back and forth still ends on
    // whichever part it settled on.
    var pending = null;

    function cancel() {
      if (pending !== null) {
        window.clearTimeout(pending);
        pending = null;
      }
    }

    for (var wire = 0; wire < triggers.length; wire++) {
      (function (trigger) {
        var name = nameOf(trigger);
        trigger.addEventListener("mouseenter", function () {
          cancel();
          pending = window.setTimeout(function () {
            pending = null;
            show(name);
          }, DWELL);
        });
        trigger.addEventListener("mouseleave", cancel);
        // Keyboard and touch are deliberate already: there is nothing to cross by
        // accident, so they open the file with no wait.
        trigger.addEventListener("focus", function () {
          cancel();
          show(name);
        });
        trigger.addEventListener("click", function (event) {
          event.preventDefault();
          cancel();
          show(name);
        });
        trigger.addEventListener("keydown", function (event) {
          if (event.key === "Enter" || event.key === " " || event.key === "Spacebar") {
            event.preventDefault();
            cancel();
            show(name);
          }
        });
      })(triggers[wire]);
    }

    view.addEventListener("mouseover", function (event) {
      var line = event.target.closest ? event.target.closest(".synqt-code__line") : null;
      explain(line && line.getAttribute("data-gloss"));
    });

    view.addEventListener("mouseleave", function () {
      explain("");
    });

    // A line whose gloss names a page opens it, which is how the section hands the
    // reader on: the runtime accessors to their class in the C++ reference, the rest
    // to the page of the docs that covers them. Selecting text inside the file ends in
    // a click too, so a click that finished a selection is left alone.
    view.addEventListener("click", function (event) {
      if (!event.target.closest) {
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

    show(nameOf(files[0]));
  }

  function setup() {
    var all = document.querySelectorAll(".synqt-explorer");
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
