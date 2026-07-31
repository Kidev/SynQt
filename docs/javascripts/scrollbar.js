// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

/* The site's scrollbar, drawn by the page rather than by the platform.
 *
 * The window's own bar runs the full height of the window, so it starts up
 * beside the header, while the one on /api/ starts under it: what scrolls
 * there is the frame, and the frame begins below the header. This draws that
 * same bar on every other page, in the same place and the same colours, from
 * under the header to the bottom of the window.
 *
 * What it deliberately does not change is what scrolls. The document still
 * does, so everything the theme reads off the window's scroll position keeps
 * working: the table of contents following the reading position, anchor
 * links, the back to top button. Making an element inside the page the
 * scroller instead would have been one line of CSS and would have quietly
 * broken all of it, because the theme would then be watching a window that
 * never scrolls.
 *
 * It runs only where the platform reserves room for a bar. Where scrollbars
 * are overlays already (macOS, iOS, Android) there is nothing to replace:
 * that bar takes no space and fades out by itself, and it is drawn over the
 * page rather than beside it, so it never looked wrong.
 *
 * With scripting off nothing here runs and the page keeps the platform's bar,
 * thinned and coloured by the same stylesheet. That is the fallback, and it is
 * why the native bar is hidden from here rather than from CSS.
 */
(function () {
  "use strict";

  var ON = "synqt-bar-on";
  var DRAGGING = "synqt-bar-dragging";
  // Short of this the thumb is a dot that cannot be grabbed, so past a certain
  // document length the bar stops being a to-scale drawing of it.
  var MIN_THUMB = 28;

  // Does this platform draw scrollbars that take space? Asked of a box of our
  // own rather than of the window, because the window cannot answer it on a
  // page that does not scroll, which is exactly the page (/api/) where getting
  // the answer wrong shifts the header by the width of a bar.
  function classicBars() {
    var probe = document.createElement("div");
    var width;
    probe.style.cssText =
      "position:absolute;top:-9999px;width:100px;height:100px;overflow:scroll";
    document.body.appendChild(probe);
    width = probe.offsetWidth - probe.clientWidth;
    document.body.removeChild(probe);
    return width > 0;
  }

  var root = document.documentElement;
  var bar = document.createElement("div");
  var thumb = document.createElement("div");
  var header = null;
  var frame = 0;
  var grab = 0;

  bar.className = "synqt-scrollbar";
  thumb.className = "synqt-scrollbar__thumb";
  bar.appendChild(thumb);

  function height() {
    return Math.max(
      document.documentElement.scrollHeight,
      document.body ? document.body.scrollHeight : 0
    );
  }

  function draw() {
    frame = 0;
    if (!header || !header.isConnected) {
      header = document.querySelector(".md-header");
    }
    // The header is sticky at the top of the window, so its own box is where
    // the bar has to start, measured rather than assumed: it is one row here
    // and the theme sizes it from the wordmark.
    var top = header ? header.getBoundingClientRect().bottom : 0;
    var track = window.innerHeight - top;
    var page = height();
    var scrollable = page - window.innerHeight;
    if (scrollable < 1 || track < MIN_THUMB * 2) {
      bar.hidden = true;
      return;
    }
    bar.hidden = false;
    bar.style.top = top + "px";
    var size = Math.max(MIN_THUMB, Math.round(track * (window.innerHeight / page)));
    var travel = track - size;
    var at = Math.min(1, Math.max(0, window.scrollY / scrollable));
    thumb.style.height = size + "px";
    thumb.style.transform = "translateY(" + Math.round(at * travel) + "px)";
  }

  function schedule() {
    if (frame === 0) {
      frame = window.requestAnimationFrame(draw);
    }
  }

  function scrollTo(clientY) {
    var track = bar.getBoundingClientRect();
    var travel = track.height - thumb.offsetHeight;
    if (travel <= 0) {
      return;
    }
    var at = (clientY - track.top - grab) / travel;
    var scrollable = height() - window.innerHeight;
    window.scrollTo(0, Math.min(1, Math.max(0, at)) * scrollable);
  }

  thumb.addEventListener("pointerdown", function (event) {
    grab = event.clientY - thumb.getBoundingClientRect().top;
    thumb.setPointerCapture(event.pointerId);
    root.classList.add(DRAGGING);
    // Otherwise the drag selects the text it passes over.
    event.preventDefault();
  });

  thumb.addEventListener("pointermove", function (event) {
    if (thumb.hasPointerCapture(event.pointerId)) {
      scrollTo(event.clientY);
    }
  });

  function release(event) {
    if (thumb.hasPointerCapture(event.pointerId)) {
      thumb.releasePointerCapture(event.pointerId);
    }
    root.classList.remove(DRAGGING);
  }

  thumb.addEventListener("pointerup", release);
  thumb.addEventListener("pointercancel", release);

  window.addEventListener("scroll", schedule, { passive: true });
  window.addEventListener("resize", schedule);

  function start() {
    if (!classicBars()) {
      return;
    }
    document.body.appendChild(bar);
    // Only now, so a browser that got this far without the element (an error
    // in the lines above) is left with the bar it already had. The class also
    // takes the reserved width away, which is why it is set on a page with
    // nothing to scroll as well: leave it on there and that page alone would
    // be ten pixels narrower than every other, which is visible in the header.
    root.classList.add(ON);
    // The page grows and shrinks under a reader who never scrolls: instant
    // navigation swaps one page for another, images arrive, a details block
    // opens. All of it is a change of document height and nothing else, so
    // one observer covers the lot.
    if (window.ResizeObserver) {
      new window.ResizeObserver(schedule).observe(document.body);
    }
    draw();
  }

  if (document.body) {
    start();
  } else {
    document.addEventListener("DOMContentLoaded", start);
  }
})();
