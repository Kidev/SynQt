// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

/* The Download modal.
 *
 * The "Download" entry in the top nav (mkdocs.yml) points at
 * https://get.synqt.org/. get.synqt.org serves the installer script itself
 * (so `curl https://get.synqt.org | sh` works) rather than a user-facing page, so the
 * download UI lives here. This
 * script intercepts clicks on that nav link and opens a modal instead. The link
 * stays a working fallback when JavaScript is off (it serves the script).
 *
 * Everything is attached to `document`, which survives Material's instant
 * navigation (navigation.instant swaps page content but not the document node),
 * so the listeners are installed once and keep working across page changes. The
 * modal element is (re)built lazily and re-appended if a navigation dropped it.
 */
(function () {
  "use strict";

  var GET_URLS = ["https://get.synqt.org/", "https://get.synqt.org"];
  var OWNER = "Kidev";
  var REPO = "SynQt";
  var LATEST = "https://github.com/" + OWNER + "/" + REPO + "/releases/latest";
  // The /releases/latest/download/<asset> path always serves the asset of that
  // name from the most recent non prerelease, so this link self updates.
  var DL = LATEST + "/download";
  var INSTALL_SH_URL = "https://get.synqt.org/install.sh";
  var INSTALL_PS_URL = "https://get.synqt.org/install.ps1";
  var ONELINER_SH = "curl -fsSL https://get.synqt.org/install.sh | sh";
  var ONELINER_PS = "irm https://get.synqt.org/install.ps1 | iex";
  // The same CLI, published as a wheel from the same tag as the binaries above
  // (tools/synqt/pyproject.toml), for anyone who already manages tools with Python.
  var ONELINER_PIP = "pipx install synqt";
  var PYPI_URL = "https://pypi.org/project/synqt/";

  // The three commands, coloured. The home page prints the same lines through the site's
  // own `cli` Pygments lexer (tools/pygments-synqt/src/synqt_pygments/lexers.py) and this
  // modal is built in the browser, so the colouring has to be here or the same command
  // reads as syntax in one place and as a grey run in the other. The rules below are that
  // lexer's `root` state in the order it lists them, and the class on each run is the
  // class Pygments emits, so the two cannot mean different things by `synqt` or by a flag.
  //
  // Anchored and exhaustive. The last rule takes one character, so the scan can neither
  // stall nor skip, and every byte of the command comes back inside a run.
  var CLI_RULES = [
    [/^#.*/, "c1"],                   // a comment
    [/^\s+/, "w"],                    // whitespace, which nothing colours
    [/^\.\.\./, "o"],
    [/^\|/, "o"],                     // the pipe, which is what these lines are made of
    [/^[[\]]/, "p"],
    [/^<[^>]+>/, "nv"],               // <a-placeholder>
    [/^--?[A-Za-z][\w-]*/, "na"],     // a flag
    [/^synqt\b/, "nb"],               // the tool itself
    [/^[A-Za-z][\w-]*/, "k"],         // any other word: the command, a host, a path part
    [/^[\s\S]/, ""]                   // and one character of whatever is left
  ];

  function escaped(text) {
    return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  }

  // `command` as the HTML of one `<code>`: the runs, each in a span carrying its Pygments
  // class, with neighbours of one kind joined so a host name is one span rather than five.
  function coloured(command) {
    var rest = String(command);
    var out = "";
    var open = null;
    var held = "";
    while (rest.length) {
      var found = null;
      var kind = "";
      for (var rule = 0; rule < CLI_RULES.length && !found; rule++) {
        found = CLI_RULES[rule][0].exec(rest);
        kind = CLI_RULES[rule][1];
      }
      if (kind !== open) {
        out += written(open, held);
        open = kind;
        held = "";
      }
      held += found[0];
      rest = rest.slice(found[0].length);
    }
    return out + written(open, held);
  }

  function written(kind, text) {
    if (!text) {
      return "";
    }
    return kind ? '<span class="' + kind + '">' + escaped(text) + "</span>" : escaped(text);
  }

  var API_LATEST = "https://api.github.com/repos/" + OWNER + "/" + REPO + "/releases/latest";
  // Every published release, newest first, for the "More versions" panel. Capped: the
  // panel is for picking last week's build or the one before a regression, not for
  // browsing a project's whole history, and GitHub's own releases page is one click away
  // inside the panel for that.
  var API_RELEASES = "https://api.github.com/repos/" + OWNER + "/" + REPO
    + "/releases?per_page=20";

  // Every platform a release carries a binary for, in the order the panel lists them. The
  // label is what a reader recognises. The pair behind it is what names the asset.
  var TARGETS = [
    { key: "linux-x86_64", os: "linux", arch: "x86_64", label: "Linux (x86_64)" },
    { key: "linux-arm64", os: "linux", arch: "arm64", label: "Linux (arm64)" },
    { key: "macos-arm64", os: "macos", arch: "arm64", label: "macOS (Apple silicon)" },
    { key: "macos-x86_64", os: "macos", arch: "x86_64", label: "macOS (Intel)" },
    { key: "windows-x86_64", os: "windows", arch: "x86_64", label: "Windows (x86_64)" },
    { key: "windows-arm64", os: "windows", arch: "arm64", label: "Windows (arm64)" }
  ];
  // Written by source-facts.js from the header's repository facts (see that file).
  var FACTS_KEY = "synqt-source-facts";

  var modal = null;
  var lastFocused = null;
  var version = null;
  // What the buttons currently resolve to: a target key from TARGETS, and a release tag,
  // where the empty string means "whatever latest is when this is clicked". Both start as
  // what was detected and only move when somebody moves them.
  var target = "linux-x86_64";
  var release = "";
  var releasesAsked = false;
  // Whether somebody picked the platform themselves. Detection runs on every open, and it
  // must not undo a choice made a moment earlier in the panel.
  var chosen = false;

  function el(tag, attrs, html) {
    var node = document.createElement(tag);
    if (attrs) {
      Object.keys(attrs).forEach(function (k) {
        node.setAttribute(k, attrs[k]);
      });
    }
    if (html != null) node.innerHTML = html;
    return node;
  }

  function detectOs() {
    var ua = (navigator.userAgent || "").toLowerCase();
    var plat = ((navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || "").toLowerCase();
    if (plat.indexOf("win") !== -1 || ua.indexOf("windows") !== -1) return "windows";
    if (plat.indexOf("mac") !== -1 || ua.indexOf("mac os") !== -1) return "macos";
    if (plat.indexOf("linux") !== -1 || ua.indexOf("linux") !== -1) return "linux";
    return "linux";
  }

  function assetFor(os, arch) {
    var ext = os === "windows" ? "zip" : "tar.gz";
    return "synqt-" + os + "-" + arch + "." + ext;
  }

  function targetOptions() {
    return TARGETS.map(function (one) {
      return '<option value="' + one.key + '">' + one.label + "</option>";
    }).join("");
  }

  function targetNamed(key) {
    for (var i = 0; i < TARGETS.length; i++) {
      if (TARGETS[i].key === key) return TARGETS[i];
    }
    return TARGETS[0];
  }

  /* Everything the two choices decide, applied at once: which asset the button fetches,
   * what the button and the facts row say, and which of the two shell commands is the one
   * on screen. One function, because the choices are not independent of each other on the
   * page -- picking Windows in the panel and being left looking at the `curl` line was the
   * whole reason the old "On Windows instead?" link existed. */
  function apply() {
    var one = targetNamed(target);
    // A named release is fetched from its own tag; "latest" keeps the self-updating path,
    // so a bookmarked link to it goes on resolving to whatever the newest release is.
    var base = release ? "https://github.com/" + OWNER + "/" + REPO + "/releases/download/"
                         + encodeURIComponent(release)
                       : DL;
    var a = modal.querySelector("#synqt-dl-download");
    var picking = !modal.querySelector("#synqt-dl-versions").hidden;
    a.href = base + "/" + assetFor(one.os, one.arch);
    // With the drop-downs open the platform is written in one of them, and a button
    // repeating it is both the widest thing on the row and the second answer to a
    // question nobody asked twice.
    a.textContent = picking ? "Download" : "Download for " + one.label;
    modal.querySelector("#synqt-dl-platform").textContent = one.label;
    var windows = one.os === "windows";
    modal.querySelector("#synqt-dl-windows").hidden = !windows;
    modal.querySelector("#synqt-dl-posix").hidden = windows;
  }

  /* Which release the buttons above resolve to. The header already carries
   * it: Material draws the latest release tag as a repository fact on every page, and
   * source-facts.js keeps the last one this browser received. Read those two first, so
   * the common case costs no request at all, and only ask GitHub directly when neither
   * has it (a first visit whose header fetch was refused). */
  function versionFromHeader() {
    var fact = document.querySelector(".md-source__fact--version");
    return fact ? fact.textContent.trim() : "";
  }

  function versionFromCache() {
    var raw;
    try {
      raw = window.localStorage.getItem(FACTS_KEY);
    } catch (e) {
      return "";
    }
    var facts;
    try {
      facts = raw ? JSON.parse(raw) : null;
    } catch (e) {
      return "";
    }
    if (!(facts instanceof Array)) return "";
    for (var i = 0; i < facts.length; i++) {
      if (facts[i] && facts[i][0] === "version" && facts[i][1]) {
        return String(facts[i][1]).trim();
      }
    }
    return "";
  }

  function showVersion(text) {
    version = text || null;
    var node = modal && modal.querySelector("#synqt-dl-version");
    if (!node) return;
    // No version rather than a wrong one: the row drops the release it could
    // not name, and the buttons still point at whatever "latest" is when clicked.
    node.parentNode.hidden = !(version || release);
    node.textContent = release || version || "";
    // Named on the row, and reached from it. The notes and the checksums for the release
    // being fetched are behind the tag written here. A picked release has its own page;
    // "latest" keeps the self-updating link, which is the same release the button holds.
    node.href = release
      ? "https://github.com/" + OWNER + "/" + REPO + "/releases/tag/"
        + encodeURIComponent(release)
      : LATEST;
  }

  function resolveVersion() {
    var known = versionFromHeader() || versionFromCache();
    if (known) {
      showVersion(known);
      return;
    }
    if (!window.fetch) {
      showVersion("");
      return;
    }
    fetch(API_LATEST)
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (data) { showVersion(data && data.tag_name ? data.tag_name : ""); })
      .catch(function () { showVersion(""); });
  }

  /* What this browser is running on, as one of the keys in TARGETS. Only ever the starting
   * point. The panel is what settles it, and a detection that guesses wrong (a Mac
   * fetching for a Windows box) costs one drop-down rather than a wrong file. */
  function detectAndSet() {
    var os = detectOs();
    // Architecture is not reliably exposed to JavaScript. Ask for high entropy
    // values where supported (Chromium), otherwise default to x86_64.
    var settle = function (arch) {
      if (!chosen) {
        target = os + "-" + arch;
        var picker = modal.querySelector("#synqt-dl-target");
        if (picker) picker.value = target;
      }
      apply();
    };
    if (navigator.userAgentData && navigator.userAgentData.getHighEntropyValues) {
      navigator.userAgentData
        .getHighEntropyValues(["architecture"])
        .then(function (v) { settle(v.architecture === "arm" ? "arm64" : "x86_64"); })
        .catch(function () { settle("x86_64"); });
    } else {
      settle("x86_64");
    }
  }

  /* The releases, fetched the first time the panel is opened and not before: a modal that
   * asked GitHub for a list nobody had asked to see would spend somebody's rate limit on
   * the common case, which is downloading the newest build. Asked once per visit, and a
   * refusal leaves the drop-down holding the one entry it starts with, which is still the
   * right answer. */
  function loadReleases() {
    if (releasesAsked || !window.fetch) return;
    releasesAsked = true;
    fetch(API_RELEASES)
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (list) {
        if (!(list instanceof Array) || !list.length) return;
        var picker = modal.querySelector("#synqt-dl-release");
        if (!picker) return;
        list.forEach(function (one) {
          if (!one || !one.tag_name) return;
          var option = el("option", { value: one.tag_name });
          option.textContent = one.tag_name + (one.prerelease ? " (pre-release)" : "");
          picker.appendChild(option);
        });
      })
      .catch(function () { /* the one entry already in it is the honest fallback */ });
  }

  function build() {
    if (modal && document.body.contains(modal)) return modal;

    modal = el("div", { class: "synqt-dl", id: "synqt-dl", role: "dialog", "aria-modal": "true", "aria-labelledby": "synqt-dl-title", hidden: "" });
    modal.innerHTML =
      '<div class="synqt-dl__card">' +
      '  <button class="synqt-dl__close" id="synqt-dl-close" type="button" aria-label="Close">&times;</button>' +
      '  <h2 class="synqt-dl__title" id="synqt-dl-title">Get SynQt</h2>' +
      '  <p class="synqt-dl__sub">Install the latest release of the SynQt command line tool. It installs and pins the rest of the toolchain for you.</p>' +
      '  <p class="synqt-dl__platform">' +
      '    <span class="synqt-dl__fact" hidden>Release: <a class="synqt-dl__release" id="synqt-dl-version" href="' + LATEST + '" target="_blank" rel="noopener" title="Release notes and checksums"></a></span>' +
      '    <span class="synqt-dl__fact">Platform: <strong id="synqt-dl-platform">checking&hellip;</strong></span>' +
      '  </p>' +
      // One line, whichever it is showing. "More versions" gives up its own place on the
      // row to the two drop-downs, so opening them moves nothing. The card is the same
      // height and the same width open as shut, and the two choices sit where the button
      // that asked for them was. The download button shortens at the same moment, since
      // the platform it was naming is now written in the drop-down beside it.
      '  <div class="synqt-dl__row">' +
      '    <a class="synqt-dl__btn" id="synqt-dl-download" href="#" rel="noopener">Download latest</a>' +
      '    <button class="synqt-dl__btn synqt-dl__btn--secondary" id="synqt-dl-more" type="button" aria-expanded="false" aria-controls="synqt-dl-versions">More versions</button>' +
      '    <span class="synqt-dl__picks" id="synqt-dl-versions" hidden>' +
      '      <select class="synqt-dl__pick" id="synqt-dl-release" aria-label="Release"><option value="">Latest</option></select>' +
      '      <select class="synqt-dl__pick" id="synqt-dl-target" aria-label="Platform">' + targetOptions() + '</select>' +
      '    </span>' +
      '  </div>' +
      '  <p class="synqt-dl__label">Or install from your terminal.</p>' +
      // One shell line, the one for the platform this browser is on. The other is a
      // command the visitor cannot run, and printing both means everybody reads two
      // lines to find theirs. Detection can be wrong (a Mac browsing for a Windows box),
      // so the other one is a click away rather than gone.
      // The command for the platform above and no other. The warning sits against the
      // bottom of the block it is about rather than a paragraph away from it: it is about
      // that line, and a caution with air around it reads as a caution about the page.
      '  <div class="synqt-dl__install" id="synqt-dl-posix" hidden>' +
      '    <p class="synqt-dl__sublabel">Linux and macOS:</p>' +
      '    <pre class="synqt-dl__pre"><button class="synqt-dl__copy" type="button">copy</button><code>' + coloured(ONELINER_SH) + "</code></pre>" +
      '    <p class="synqt-dl__warn"><strong>Always read a script before you run it.</strong> This one downloads a release, unpacks it, and copies one binary into a bin directory. Nothing else: <a href="' + INSTALL_SH_URL + '" target="_blank" rel="noopener">install.sh</a>.</p>' +
      '  </div>' +
      '  <div class="synqt-dl__install" id="synqt-dl-windows" hidden>' +
      '    <p class="synqt-dl__sublabel">Windows (PowerShell):</p>' +
      '    <pre class="synqt-dl__pre"><button class="synqt-dl__copy" type="button">copy</button><code>' + coloured(ONELINER_PS) + "</code></pre>" +
      '    <p class="synqt-dl__warn"><strong>Always read a script before you run it.</strong> This one downloads a release, unpacks it, and copies one binary into a bin directory. Nothing else: <a href="' + INSTALL_PS_URL + '" target="_blank" rel="noopener">install.ps1</a>.</p>' +
      '  </div>' +
      '  <p class="synqt-dl__label">Or, if you already have Python, from PyPI.</p>' +
      '  <pre class="synqt-dl__pre"><button class="synqt-dl__copy" type="button">copy</button><code>' + coloured(ONELINER_PIP) + "</code></pre>" +
      '  <p class="synqt-dl__sublabel synqt-dl__last">Any platform, and the same CLI: the wheel is cut from the same tag as the downloads above. <code>pip install synqt</code> works too; pipx is the suggestion only because this is an application rather than a library. See <a href="' + PYPI_URL + '" target="_blank" rel="noopener">synqt on PyPI</a>.</p>' +
      "</div>";

    document.body.appendChild(modal);

    modal.querySelector("#synqt-dl-close").addEventListener("click", close);

    // Click on the backdrop (outside the card) closes the modal.
    modal.addEventListener("click", function (e) {
      if (e.target === modal) close();
    });

    // One way, not a toggle. The button is the question and the drop-downs are where it
    // is answered, so once they are on the row there is nothing left for it to ask. Going
    // back is picking Latest and the platform you are on, which is what it would have
    // restored anyway.
    var more = modal.querySelector("#synqt-dl-more");
    more.addEventListener("click", function () {
      var picks = modal.querySelector("#synqt-dl-versions");
      picks.hidden = false;
      more.hidden = true;
      more.setAttribute("aria-expanded", "true");
      loadReleases();
      apply();
      modal.querySelector("#synqt-dl-release").focus();
    });

    modal.querySelector("#synqt-dl-target").addEventListener("change", function (e) {
      chosen = true;
      target = e.target.value;
      apply();
    });

    modal.querySelector("#synqt-dl-release").addEventListener("change", function (e) {
      release = e.target.value;
      apply();
      showVersion(version);
    });

    // Each copy button copies the command in its own <pre>.
    Array.prototype.forEach.call(modal.querySelectorAll(".synqt-dl__copy"), function (btn) {
      btn.addEventListener("click", function () {
        var code = btn.parentNode.querySelector("code");
        copyText(code ? code.textContent : "").then(function (ok) {
          btn.textContent = ok ? "copied" : "copy failed";
          setTimeout(function () {
            btn.textContent = "copy";
          }, 1500);
        });
      });
    });

    return modal;
  }

  function copyText(text) {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      return navigator.clipboard.writeText(text).then(
        function () { return true; },
        function () { return false; }
      );
    }
    // Fallback for browsers without the async clipboard API.
    try {
      var ta = document.createElement("textarea");
      ta.value = text;
      ta.style.position = "fixed";
      ta.style.opacity = "0";
      document.body.appendChild(ta);
      ta.select();
      var ok = document.execCommand("copy");
      document.body.removeChild(ta);
      return Promise.resolve(ok);
    } catch (e) {
      return Promise.resolve(false);
    }
  }

  function open() {
    build();
    detectAndSet();
    if (version) {
      showVersion(version); // already resolved once this visit. Do not ask again
    } else {
      resolveVersion();
    }
    lastFocused = document.activeElement;
    modal.hidden = false;
    document.body.classList.add("synqt-dl-open");
    var closeBtn = modal.querySelector("#synqt-dl-close");
    if (closeBtn) closeBtn.focus();
  }

  function close() {
    if (!modal || modal.hidden) return;
    modal.hidden = true;
    document.body.classList.remove("synqt-dl-open");
    if (lastFocused && lastFocused.focus) lastFocused.focus();
    lastFocused = null;
  }

  function isGetLink(a) {
    if (!a || a.closest("#synqt-dl")) return false; // never intercept the modal's own links
    var href = a.getAttribute("href");
    return GET_URLS.indexOf(href) !== -1;
  }

  // Capture phase so this runs before Material's own link handling and navigation.
  document.addEventListener(
    "click",
    function (e) {
      var a = e.target.closest ? e.target.closest("a") : null;
      if (isGetLink(a)) {
        e.preventDefault();
        open();
      }
    },
    true
  );

  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape") close();
  });
})();
