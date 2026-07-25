// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

/* The repository facts in the site header (version, stars, forks).
 *
 * Material fetches them from the GitHub API on the first page of a visit and draws
 * them under the repository link. That API allows sixty unauthenticated requests an
 * hour per address, and a visit spends two of them, so a reader who moves through the
 * documentation long enough runs out: the fetch fails, Material draws nothing, and the
 * facts stay missing for the rest of that tab even after the allowance resets, because
 * nothing retries. The header then reads as though the project has no releases.
 *
 * So keep the last facts this browser actually received, and draw those when a fetch
 * comes back with nothing. They are a moment old rather than current, which is the
 * right trade for a star count: the alternative on screen is not fresher numbers, it is
 * no numbers at all. A later visit that does reach the API overwrites them, and the
 * drawn copy is dropped the moment Material's own arrives, so the two never stack.
 */
(function () {
  "use strict";

  var KEY = "synqt-source-facts";
  var MINE = "data-synqt-cached";
  // Long enough for the two API calls to have answered on a working connection, so a
  // reader on a slow link does not see cached numbers replaced by live ones.
  var WAIT = 2000;

  function readCache() {
    var raw;
    try {
      raw = window.localStorage.getItem(KEY);
    } catch (error) {
      return null; // Storage disabled: there is nothing kept, and nothing to keep.
    }
    if (!raw) {
      return null;
    }
    var facts;
    try {
      facts = JSON.parse(raw);
    } catch (error) {
      return null;
    }
    return facts instanceof Array && facts.length > 0 ? facts : null;
  }

  /* The values, not the markup: rebuilding the list from text through the DOM means
   * nothing that ends up in the header was ever parsed as HTML out of storage. */
  function writeCache(list) {
    var facts = [];
    var items = list.querySelectorAll(".md-source__fact");
    for (var index = 0; index < items.length; index++) {
      var kind = /md-source__fact--([\w-]+)/.exec(items[index].className);
      facts.push([kind ? kind[1] : "", items[index].textContent]);
    }
    if (facts.length === 0) {
      return;
    }
    try {
      window.localStorage.setItem(KEY, JSON.stringify(facts));
    } catch (error) {
      // A full or disabled store costs the fallback, nothing else.
    }
  }

  function draw(repository, facts) {
    var list = document.createElement("ul");
    list.className = "md-source__facts";
    list.setAttribute(MINE, "");
    for (var index = 0; index < facts.length; index++) {
      var fact = document.createElement("li");
      fact.className = "md-source__fact"
        + (facts[index][0] ? " md-source__fact--" + facts[index][0] : "");
      fact.textContent = facts[index][1];
      list.appendChild(fact);
    }
    repository.appendChild(list);
    repository.classList.add("md-source__repository--active");
  }

  /* Two of these are drawn on a page: the one in the header, and the one Material
   * puts at the top of the navigation drawer for a narrow screen. Both are filled
   * from the same fetch, so both go blank together and both are restored here. */
  function prepare() {
    var all = document.querySelectorAll(".md-source__repository");
    for (var index = 0; index < all.length; index++) {
      prepareOne(all[index]);
    }
  }

  function prepareOne(repository) {
    if (repository.getAttribute("data-synqt-facts") === "ready") {
      return;
    }
    repository.setAttribute("data-synqt-facts", "ready");

    var live = repository.querySelector(".md-source__facts:not([" + MINE + "])");
    if (live) {
      writeCache(live);
      return;
    }

    // Material appends its own list when the fetch resolves, which is after this runs.
    // Take whichever arrives: its list if the API answered, the kept one if it did not.
    var observer = new MutationObserver(function () {
      var arrived = repository.querySelector(".md-source__facts:not([" + MINE + "])");
      if (!arrived) {
        return;
      }
      var kept = repository.querySelector(".md-source__facts[" + MINE + "]");
      if (kept) {
        kept.parentNode.removeChild(kept);
      }
      writeCache(arrived);
      observer.disconnect();
    });
    observer.observe(repository, { childList: true });

    window.setTimeout(function () {
      if (repository.querySelector(".md-source__facts")) {
        return;
      }
      var facts = readCache();
      if (facts) {
        draw(repository, facts);
      }
    }, WAIT);
  }

  if (window.document$ && typeof window.document$.subscribe === "function") {
    window.document$.subscribe(prepare);
  } else if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", prepare);
  } else {
    prepare();
  }
})();
