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

  var modal = null;
  var lastFocused = null;

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
      '  <p class="synqt-dl__platform">Detected platform: <strong id="synqt-dl-platform">checking&hellip;</strong></p>' +
      '  <div class="synqt-dl__row">' +
      '    <a class="synqt-dl__btn" id="synqt-dl-download" href="#" rel="noopener">Download latest</a>' +
      '    <a class="synqt-dl__btn synqt-dl__btn--secondary" id="synqt-dl-releases" href="' + LATEST + '" rel="noopener" target="_blank">All releases and platforms</a>' +
      '  </div>' +
      '  <p class="synqt-dl__label">Or install from your terminal.</p>' +
      '  <p class="synqt-dl__sublabel">Linux and macOS:</p>' +
      '  <pre class="synqt-dl__pre"><button class="synqt-dl__copy" type="button">copy</button><code>' + ONELINER_SH + "</code></pre>" +
      '  <p class="synqt-dl__sublabel">Windows (PowerShell):</p>' +
      '  <pre class="synqt-dl__pre"><button class="synqt-dl__copy" type="button">copy</button><code>' + ONELINER_PS + "</code></pre>" +
      '  <p class="synqt-dl__muted">Reading a script before piping it to a shell is strongly recommended. Each only downloads, extracts, and copies a single binary into a bin directory. Read <a href="' + INSTALL_SH_URL + '" target="_blank" rel="noopener">install.sh</a> or <a href="' + INSTALL_PS_URL + '" target="_blank" rel="noopener">install.ps1</a> yourself before running it.</p>' +
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
