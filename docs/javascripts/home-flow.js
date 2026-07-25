// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

/* The home page's "What it looks like" project.
 *
 * The section is one small system, drawn twice: on the left a diagram of the mesh, with
 * a file behind every part of it (the configuration behind the cog, the contract behind
 * the link the browser and the edge share, one QML file behind each entity), and on the
 * right a file view showing exactly one of those files at a time. Pointing at a part of
 * the diagram opens its file, which then stays until another part is pointed at, so the
 * reader can move the pointer into the file and read it. The configuration is shown to
 * begin with, since it is the file the rest of the diagram is generated from.
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
  var ON = "synqt-trigger--on";
  var HINT = "Hover a line of the file for what it does. A line that ends in an arrow "
    + "opens the page covering it.";

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

    function show(name) {
      for (var at = 0; at < files.length; at++) {
        files[at].classList.toggle(CURRENT, files[at].getAttribute("data-file") === name);
      }
      for (var on = 0; on < triggers.length; on++) {
        var chosen = triggers[on].getAttribute("data-file") === name;
        triggers[on].classList.toggle(ON, chosen);
        triggers[on].setAttribute("aria-pressed", chosen ? "true" : "false");
      }
      hint.textContent = HINT;
    }

    for (var wire = 0; wire < triggers.length; wire++) {
      (function (trigger) {
        var name = trigger.getAttribute("data-file");
        // Pointer and keyboard reach a file the same way: there is nothing to open and
        // nothing to close, so moving onto a part of the diagram is the whole gesture.
        // The click is for a touch screen, which has no hover to give.
        trigger.addEventListener("mouseenter", function () {
          show(name);
        });
        trigger.addEventListener("focus", function () {
          show(name);
        });
        trigger.addEventListener("click", function (event) {
          event.preventDefault();
          show(name);
        });
        trigger.addEventListener("keydown", function (event) {
          if (event.key === "Enter" || event.key === " " || event.key === "Spacebar") {
            event.preventDefault();
            show(name);
          }
        });
      })(triggers[wire]);
    }

    view.addEventListener("mouseover", function (event) {
      var line = event.target.closest ? event.target.closest(".synqt-code__line") : null;
      var gloss = line && line.getAttribute("data-gloss");
      hint.textContent = gloss || HINT;
    });

    view.addEventListener("mouseleave", function () {
      hint.textContent = HINT;
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

    show(files[0].getAttribute("data-file"));
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
