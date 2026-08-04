// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

/* The Download modal.
 *
 * The "Download" entry in the top nav (mkdocs.yml) points at
 * https://get.synqt.org/. get.synqt.org now serves the installer script itself
 * (so `curl https://get.synqt.org | sh` works), not a user-facing page, so the
 * download UI that used to live at get.synqt.org/index.html lives here: this
 * script intercepts clicks on that nav link and opens a modal instead. The link
 * stays a working fallback when JavaScript is off (it just serves the script).
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

  var API_LATEST = "https://api.github.com/repos/" + OWNER + "/" + REPO + "/releases/latest";
  // Written by source-facts.js from the header's repository facts (see that file).
  var FACTS_KEY = "synqt-source-facts";

  var modal = null;
  var lastFocused = null;
  var version = null;

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

  function setDownload(os, arch) {
    var label = { linux: "Linux", macos: "macOS", windows: "Windows" }[os] || os;
    modal.querySelector("#synqt-dl-platform").textContent = label + " (" + arch + ")";
    var a = modal.querySelector("#synqt-dl-download");
    a.href = DL + "/" + assetFor(os, arch);
    a.textContent = "Download for " + label + " (" + arch + ")";
  }

  /* Which release the buttons above actually resolve to. The header already carries
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
    // No version rather than a wrong one: the row simply drops the release it could
    // not name, and the buttons still point at whatever "latest" is when clicked.
    node.parentNode.hidden = !version;
    node.textContent = version || "";
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

  function detectAndSet() {
    var os = detectOs();
    // Architecture is not reliably exposed to JavaScript. Ask for high entropy
    // values where supported (Chromium), otherwise default to x86_64.
    if (navigator.userAgentData && navigator.userAgentData.getHighEntropyValues) {
      navigator.userAgentData
        .getHighEntropyValues(["architecture"])
        .then(function (v) {
          setDownload(os, v.architecture === "arm" ? "arm64" : "x86_64");
        })
        .catch(function () {
          setDownload(os, "x86_64");
        });
    } else {
      setDownload(os, "x86_64");
    }
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
      '    <span class="synqt-dl__fact" hidden>Release: <strong id="synqt-dl-version"></strong></span>' +
      '    <span class="synqt-dl__fact">Detected platform: <strong id="synqt-dl-platform">checking&hellip;</strong></span>' +
      '  </p>' +
      '  <div class="synqt-dl__row">' +
      '    <a class="synqt-dl__btn" id="synqt-dl-download" href="#" rel="noopener">Download latest</a>' +
      '    <a class="synqt-dl__btn synqt-dl__btn--secondary" id="synqt-dl-releases" href="' + LATEST + '" rel="noopener" target="_blank">All releases and platforms</a>' +
      '  </div>' +
      '  <p class="synqt-dl__label">Or install from your terminal.</p>' +
      '  <p class="synqt-dl__sublabel">Linux and macOS:</p>' +
      '  <pre class="synqt-dl__pre"><button class="synqt-dl__copy" type="button">copy</button><code>' + ONELINER_SH + "</code></pre>" +
      '  <p class="synqt-dl__sublabel">Windows (PowerShell):</p>' +
      '  <pre class="synqt-dl__pre"><button class="synqt-dl__copy" type="button">copy</button><code>' + ONELINER_PS + "</code></pre>" +
      '  <p class="synqt-dl__warn"><strong>Read a script before you pipe it to a shell.</strong> Either of these downloads a release, extracts it, and copies one binary into a bin directory, and nothing else. Read <a href="' + INSTALL_SH_URL + '" target="_blank" rel="noopener">install.sh</a> or <a href="' + INSTALL_PS_URL + '" target="_blank" rel="noopener">install.ps1</a> yourself before you run it.</p>' +
      '  <p class="synqt-dl__label">Or, if you already have Python, from PyPI.</p>' +
      '  <pre class="synqt-dl__pre"><button class="synqt-dl__copy" type="button">copy</button><code>' + ONELINER_PIP + "</code></pre>" +
      '  <p class="synqt-dl__sublabel synqt-dl__last">Any platform, and the same CLI: the wheel is cut from the same tag as the downloads above. <code>pip install synqt</code> works too; pipx is the suggestion only because this is an application rather than a library. See <a href="' + PYPI_URL + '" target="_blank" rel="noopener">synqt on PyPI</a>.</p>' +
      "</div>";

    document.body.appendChild(modal);

    modal.querySelector("#synqt-dl-close").addEventListener("click", close);

    // Click on the backdrop (outside the card) closes the modal.
    modal.addEventListener("click", function (e) {
      if (e.target === modal) close();
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
      showVersion(version); // already resolved once this visit; do not ask again
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

  // Capture phase so we win before Material's own link handling / navigation.
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
