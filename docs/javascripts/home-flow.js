// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

/* The home page's "What it looks like" project.
 *
 * The section is one small system, seen the way the design editor sees it: the mesh drawn
 * across the top, a project tree of its five files under it, and beside the tree the one
 * file being read. Every part of the drawing has a file behind it. One QML file per
 * entity, and the configuration behind each contract mark, since what crosses a link is
 * written there. Pointing at a file in either the tree or the drawing opens it, and lights
 * it in the other, so the two are one set of triggers over the same five files.
 * A file stays until another is pointed at, so the reader can move the pointer into the
 * file and read it. The configuration is shown to begin with, since it is what the rest is
 * generated from.
 *
 * This script is the whole of that behavior, plus the glossary. Each file in
 * docs/index.md is followed by a hidden list whose entries name a fragment of it and say
 * what that line does. This wraps every line of the highlighted code in its own element,
 * hands each gloss to the first line that contains its fragment, and shows the text
 * under the file while that line is hovered. It reads the fragments rather than line
 * numbers so that editing a snippet does not silently shift every explanation in it by
 * one. A gloss that also carries a `data-href` turns its line into a link to the page
 * that documents it, which is where the section hands the reader on. To the class in the
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
  // The editor's own class for the thing that is open, put on the parts of the drawing so
  // they take the editor's own selected look rather than a copy of it written here.
  var CHOSEN = "is-selected";
  var SHOWN = "synqt-flow__hint--on";

  /* Give every line of the highlighted block its own element, so a line can be hovered
   * and marked.
   *
   * The highlighter usually emits a flat run of token spans with plain newline text
   * between them, but it is not required to. A token that covers a blank line, and any
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

  /* The drawing.
   *
   * Not a picture of the system. The system, drawn by the design editor's own `draw` from
   * the same document the editor opens at /designer/#example=demo. This page adds two
   * things and no more. Where the drawing goes, and which file each part of it opens.
   *
   * It lives in a shadow root because the editor's stylesheet is the editor's whole look,
   * `html`, `body` and `*` included, and this page has a look of its own. Inside a shadow
   * root none of those match, so the file can be adopted whole rather than picked over: the
   * one edit is `:root` to `:host`, since `:root` is the document element and there is no
   * document element in here. It has to be adopted whole. A hand-picked subset of it is a
   * second answer to what an entity looks like, and the drawing this replaced was exactly
   * that, kept by hand, and it had already drifted.
   */
  var DESIGNER = "/designer/";

  /* What this page puts on top of the editor's stylesheet. The drawing is a picture here
   * rather than a canvas somebody is dragging on, and the trigger states are this section's
   * (`show` below puts the class on) rather than the editor's.
   *
   * Nothing here says what a hovered part of the drawing looks like, and nothing here says
   * what an open one looks like either. Both are the editor's own rules on the editor's own
   * classes, adopted with the rest of its stylesheet. `is-hover` and the two role classes
   * are put on by the editor's own `highlight` (light.js), and the file being read wears
   * `is-selected`, because the file being read is what a selection is on a page with no
   * panel to select into. Four rules here (two copied out of the editor's
   * selected look and two answering `:hover` with a disc that lit and said nothing about
   * what the line it was on connected to) would be second answers to questions the editor
   * has already answered, and a copied pair drifts. */
  var MESH_CSS = [
    ":host { display: block; }",
    ".canvas { width: 100%; height: auto; background: none; cursor: default;",
    "          touch-action: auto; }",
    ".node, .link__doc { cursor: pointer; }",
    // The ring of handles a link is pulled out of. The editor shows them on whatever is
    // selected. Nothing is pulled out of anything here, so a selected entity would wear a
    // ring of dots that answer no gesture this page has.
    ".node__slot, .node__slot-grab { display: none; }",
    ".node:focus-visible, .link__doc:focus-visible { outline: 2px solid var(--accent); }",
    // The card the editor opens over whatever the pointer is on, opening here over the same
    // drawing. It is `position: fixed` in the editor's own stylesheet, which is the viewport
    // either way, so all this page owes it is a place in the stack. Above the section, below
    // the header it can never reach from down here.
    ".tip { z-index: 60; }"
  ].join("\n");

  /* How much room is left round the drawing, in the units it is drawn in. The same margin
   * the editor's own picture export leaves, and for the same reason. A name written under a
   * node reaches past the node. */
  var MESH_MARGIN = 28;

  function svgNode(tag, attrs) {
    var node = document.createElementNS("http://www.w3.org/2000/svg", tag);
    Object.keys(attrs || {}).forEach(function (key) {
      node.setAttribute(key, attrs[key]);
    });
    return node;
  }

  /* Draw the stage, and hand back every part of it that opens a file.
   *
   * Nothing here throws outward. A refused fetch or a browser with no shadow DOM leaves the
   * stage empty and the rest of the section working, because the project tree beside it
   * opens the same files and is the reason this is not the only way in.
   */
  function drawMesh(stage) {
    // Already drawn: Material's instant navigation reuses the document, and a second
    // attachShadow on one element throws. Nothing to redo either way, since what is in
    // there is a function of a document that does not change.
    if (!stage || !stage.attachShadow || !window.fetch || stage.shadowRoot) {
      return Promise.resolve([]);
    }
    var wanted = stage.getAttribute("data-example") || "demo";
    var opens = {};
    try {
      opens = JSON.parse(stage.getAttribute("data-files") || "{}");
    } catch (e) {
      return Promise.resolve([]);
    }
    return Promise.all([
      import(DESIGNER + "canvas.js"),
      import(DESIGNER + "project.js"),
      fetch(DESIGNER + "examples.json").then(function (r) { return r.json(); }),
      fetch(DESIGNER + "design.css").then(function (r) { return r.text(); }),
      import(DESIGNER + "tip.js"),
      import(DESIGNER + "light.js")
    ]).then(function (parts) {
      var canvas = parts[0];
      var project = parts[1];
      var design = (parts[2].examples || {})[wanted];
      if (!design) {
        return [];
      }
      var shadow = stage.attachShadow({ mode: "open" });
      var style = document.createElement("style");
      style.textContent = parts[3].split(":root").join(":host") + "\n" + MESH_CSS;
      shadow.appendChild(style);

      var svg = svgNode("svg", { "class": "canvas", "aria-hidden": "true" });
      var viewport = svgNode("g", { id: "viewport" });
      var layers = {
        zones: svgNode("g", { id: "zones" }),
        links: svgNode("g", { id: "links" }),
        nodes: svgNode("g", { id: "nodes" })
      };
      viewport.appendChild(layers.zones);
      viewport.appendChild(layers.links);
      viewport.appendChild(layers.nodes);
      svg.appendChild(viewport);
      shadow.appendChild(svg);

      canvas.draw(layers, design, {
        problems: { entities: new Map(), links: new Map() },
        selected: null,
        // The caption under each node is the file somebody opens next, which is the same
        // question this section is about. Asked of the editor's own reader, so the drawing
        // and the tree name one set of files.
        filesOf: function (entity) { return project.entityFiles(design, entity); }
      });

      // Measured after it is drawn, so the box is what is on screen rather than what the
      // entity coordinates alone would suggest. A name under a node and a contract written
      // beside a line both reach past the shapes.
      var box = viewport.getBBox();
      var wide = box.width + (MESH_MARGIN * 2);
      var tall = box.height + (MESH_MARGIN * 2);
      svg.setAttribute("viewBox", [box.x - MESH_MARGIN, box.y - MESH_MARGIN,
                                   wide, tall].join(" "));
      /* And the box is that shape. The stylesheet gives the stage a ratio to hold before
         the drawing arrives, so the section does not jump when it does. From here on the
         drawing is what knows, and a stage still holding the guess is a stage with a strip
         of empty page under the picture. Measured rather than written down, so the next
         arrangement of the example does not need this file edited too. */
      stage.style.aspectRatio = wide + " / " + tall;

      answer(shadow, svg, design, parts[4], parts[5]);

      var found = [];
      ["entity", "contract"].forEach(function (kind) {
        var parts = shadow.querySelectorAll("[data-" + kind + "]");
        for (var at = 0; at < parts.length; at++) {
          var name = parts[at].getAttribute("data-" + kind);
          var file = opens[kind + ":" + name];
          if (!file) {
            continue;
          }
          parts[at].setAttribute("data-file", file);
          parts[at].setAttribute("data-drawn", "");
          parts[at].setAttribute("tabindex", "0");
          parts[at].setAttribute("role", "button");
          found.push(parts[at]);
        }
      });
      return found;
    }).catch(function () {
      return [];
    });
  }

  /* What the drawing answers a pointer with. The editor's own card, and the editor's own
   * lighting, over the editor's own drawing.
   *
   * Both are functions of the design document, so both are the editor's own (`tipFor` for
   * the words and `highlight` for the marks) rather than shorter ones written for this page.
   * A second answer to what an entity is, or to which end of a line it is, would drift from
   * the first the week either changed, and this section exists to show the real thing. What
   * is this page's is where the card goes. In the shadow root, where the editor's stylesheet
   * is, so it is painted without a line of CSS here.
   *
   * One listener for the two, because both ask the same question of the same pointer. There
   * is no selection on this drawing, so `highlight` is passed none and hovering says what it
   * says in the editor with nothing selected: the line, the contract on it, and OWNER and
   * CONSUMER on the two entities it runs between.
   *
   * Pointer only. A card that opened on focus would fight the file this page opens on focus,
   * and the file is the better answer for somebody moving through by keyboard. It is the same
   * facts, in a panel that stays.
   */
  function answer(shadow, svg, design, tip, light) {
    var card = document.createElement("div");
    var showing = null;
    var lit = "";
    card.className = "tip";
    card.hidden = true;
    shadow.appendChild(card);

    function hide() {
      showing = null;
      card.hidden = true;
      card.replaceChildren();
    }

    // Lit and unlit through the same door, so there is one place that knows what the
    // drawing is currently showing and one key guarding the work: a pointer travelling
    // across a line it is already lighting rewrites nothing.
    function mark(what) {
      var key = light.hoverKey(what);
      if (key === lit) {
        return;
      }
      lit = key;
      if (what) {
        light.highlight(svg, design, what, null);
      } else {
        light.clearHighlight(svg);
      }
    }

    // Instant navigation replaces the page, and with it this drawing. The window it was
    // listening to survives. Each drawing takes its own listener off the moment its card
    // is no longer in a document, so leaving the home page and coming back does not leave
    // a scroll handler behind per visit.
    function hideWhileHere() {
      if (!card.isConnected) {
        window.removeEventListener("scroll", hideWhileHere);
        return;
      }
      hide();
    }

    svg.addEventListener("pointermove", function (event) {
      var what = tip.whatIsUnder(event.target);
      var key = what ? [what.kind, what.name, what.link, what.consumer].join("\n") : "";
      mark(what);
      if (!what) {
        hide();
        return;
      }
      // Rebuilt only when the answer changes, so the card does not flicker while the
      // pointer travels across the thing it is already describing.
      if (key !== showing) {
        var body = tip.tipFor(design, what);
        if (!body) {
          hide();
          return;
        }
        showing = key;
        card.replaceChildren(body);
        card.hidden = false;
      }
      // Measured after it is shown, so what is flipped is the size it has, and it
      // opens on the other side of the pointer rather than off the edge of the window.
      var box = card.getBoundingClientRect();
      var x = event.clientX + 18 + box.width > window.innerWidth
        ? event.clientX - 18 - box.width : event.clientX + 18;
      var y = Math.min(event.clientY + 12, window.innerHeight - box.height - 8);
      card.style.left = Math.max(8, x) + "px";
      card.style.top = Math.max(8, y) + "px";
    });

    svg.addEventListener("pointerleave", function () {
      mark(null);
      hide();
    });
    // A pointer that leaves by scrolling never fires `pointerleave`, and a card left
    // hanging over the page after the drawing has gone past is the one way this can be
    // worse than no card at all. What is lit on the drawing stays lit. It is on the drawing
    // rather than over the page, and it is the answer to where the pointer still is.
    window.addEventListener("scroll", hideWhileHere, { passive: true });
  }

  function prepare(explorer, drawn) {
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
      })
      .concat(drawn || []);
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

    // What a trigger names, as one string. Most name one file. The database names two,
    // its QML and the schema.sql the query in it reads, because they are one thing to
    // point at. The pair is written on the diagram's database and on both of its rows in
    // the tree. A directory row names everything under it, which is what makes shared/
    // open its three contracts while each contract still opens on its own.
    function nameOf(element) {
      return element.getAttribute("data-file").trim().split(/\s+/).join(" ");
    }

    // A trigger lights when every file it names is open, rather than when it names
    // exactly what is open. For all but the directories those are the same thing. Where
    // they differ. Shared/ opens its three contracts, and each of the three rows under it
    // and each of the three glyphs on the drawing is showing one of the files now open,
    // so all of them light with it. Pointing at one of those instead opens that one
    // contract and leaves the directory dark, since two of the files it names are not
    // open. One reading, both directions. What is lit is exactly what is on screen.
    function lit(open, named) {
      for (var at = 0; at < named.length; at++) {
        if (open.indexOf(named[at]) === -1) {
          return false;
        }
      }
      return true;
    }

    var showing = "";

    function show(name) {
      showing = name;
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
        var chosen = lit(wanted, nameOf(triggers[on]).split(" "));
        triggers[on].classList.toggle(ON, chosen);
        // A part of the drawing whose file is open is that drawing's selection, so it wears
        // the class the editor puts on a selection and takes the editor's own look: the
        // node glows in its own colour, the contract badge draws a size up in the accent.
        // The tree beside it is this page's own and keeps this page's class.
        if (triggers[on].hasAttribute("data-drawn")) {
          triggers[on].classList.toggle(CHOSEN, chosen);
        }
        triggers[on].setAttribute("aria-pressed", chosen ? "true" : "false");
      }
      explain("");
    }

    // The file opens the moment the pointer arrives. A tenth of a second
    // between the two, meant to stop a pointer crossing the diagram on its way somewhere else
    // from leafing through every file behind it, buys a section that
    // lags the pointer, since nothing at all happens during the wait and the entity under
    // the pointer is already lit by then. A file that opens when it is not wanted costs a
    // reader nothing. It is one panel, and the next one they point at replaces it.
    for (var wire = 0; wire < triggers.length; wire++) {
      (function (trigger) {
        var name = nameOf(trigger);
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
      explain(line && line.getAttribute("data-gloss"));
    });

    view.addEventListener("mouseleave", function () {
      explain("");
    });

    // A line whose gloss names a page opens it, which is how the section hands the
    // reader on. The runtime accessors to their class in the C++ reference, the rest
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

    /* The panel is as tall as the tallest thing it opens, so every file reads whole with
     * no scrollbar of its own. It stays one height for all of them, because a panel that
     * resized per file would move the page under a pointer still travelling over the tree.
     *
     * Measured rather than written into the stylesheet. Below the width where lines wrap,
     * the tallest file is a different height at every width, and the files themselves are
     * edited in docs/index.md, where nothing would tell the author that a number in
     * home.css had just gone stale. Each set of files a trigger opens is shown once and
     * asked how far past the panel its pages run, and the panel grows by the most any of
     * them needed. The stylesheet's height is where every measurement starts from, so the
     * panel can shrink back when the window widens. */
    var opened = [];
    triggers.concat(files).forEach(function (element) {
      var name = nameOf(element);
      if (opened.indexOf(name) === -1) {
        opened.push(name);
      }
    });

    function overrun() {
      var pages = view.querySelectorAll("." + CURRENT + " .highlight, ."
                                        + CURRENT + " pre > code");
      var total = 0;
      for (var at = 0; at < pages.length; at++) {
        total += Math.max(0, pages[at].scrollHeight - pages[at].clientHeight);
      }
      return total;
    }

    function fit() {
      var was = showing;
      view.style.height = "";
      var base = parseFloat(window.getComputedStyle(view).height);
      var tallest = base;
      for (var at = 0; at < opened.length; at++) {
        show(opened[at]);
        tallest = Math.max(tallest, base + overrun());
      }
      view.style.height = Math.ceil(tallest) + "px";
      // A page that stopped scrolling down can start scrolling across by a pixel, whose bar
      // then costs a row of height again. One more pass settles it.
      for (var again = 0; again < opened.length; again++) {
        show(opened[again]);
        tallest = Math.max(tallest, parseFloat(view.style.height) + overrun());
      }
      view.style.height = Math.ceil(tallest) + "px";
      show(was);
    }

    // Again when the width changes, which is what moves a wrapped file's height, and once
    // the code font has arrived, which a first measurement in the fallback face gets wrong.
    var measuredAt = -1;
    function refit() {
      if (!view.isConnected) {
        window.removeEventListener("resize", refit);
        return;
      }
      if (window.innerWidth !== measuredAt) {
        measuredAt = window.innerWidth;
        fit();
      }
    }
    window.addEventListener("resize", refit, { passive: true });
    refit();
    if (document.fonts && document.fonts.ready) {
      document.fonts.ready.then(function () {
        measuredAt = -1;
        refit();
      });
    }
  }

  function setup() {
    var all = document.querySelectorAll(".synqt-explorer");
    for (var index = 0; index < all.length; index++) {
      (function (explorer) {
        // The drawing is fetched, so the section is wired once it is there: the parts of
        // the drawing are triggers like the rows of the tree, and a list of triggers
        // gathered before it exists is a drawing nothing answers.
        drawMesh(explorer.querySelector(".synqt-flow__stage")).then(function (drawn) {
          prepare(explorer, drawn);
        });
      })(all[index]);
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
