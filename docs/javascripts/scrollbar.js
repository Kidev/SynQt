// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

/* The site's scrollbar, drawn by the page rather than by the platform.
 *
 * This script runs in two documents, and draws the same bar in both: on a page of the
 * documentation site, where the window scrolls, and in the C++ reference inside the
 * frame on /api/, where the content pane scrolls instead of the window. They used to be
 * two different things, this element on the site and the platform's own in the
 * reference, which is a difference that shows the moment the platform draws its bar
 * differently, and which put the reference's bar three hundred pixels short of the right
 * edge of the window, since the pane it belongs to is not the rightmost column of that
 * layout. One bar, at the right of the window, over the full height of whatever is
 * scrolling.
 *
 * What it deliberately does not change is what scrolls. The window still scrolls on the
 * site, so everything the theme reads off the window's scroll position keeps working:
 * the table of contents following the reading position, anchor links, the back to top
 * button. Making an element inside the page the scroller instead would have been one
 * line of CSS and would have quietly broken all of it, because the theme would then be
 * watching a window that never scrolls.
 *
 * It runs only where the platform reserves room for a bar. Where scrollbars are overlays
 * already (macOS, iOS, Android, a GTK desktop set that way) there is nothing to replace:
 * that bar takes no space and fades out by itself, and it is drawn over the page rather
 * than beside it, so it never looked wrong.
 *
 * With scripting off nothing here runs and the page keeps the platform's bar, thinned
 * and coloured by the stylesheet of whichever document this is. That is the fallback,
 * and it is why the native bar is hidden from here rather than from CSS.
 */
(function () {
  "use strict";

  var ON = "synqt-bar-on";
  var DRAGGING = "synqt-bar-dragging";
  // Short of this the thumb is a dot that cannot be grabbed, so past a certain document
  // length the bar stops being a to-scale drawing of it.
  var MIN_THUMB = 28;
  // A wheel notch in lines rather than pixels, which is what Firefox reports. The number
  // is the one Firefox itself uses when it has no line height to go on.
  var LINE_HEIGHT = 16;

  // Does this platform draw scrollbars that take space? Asked of a box of our own rather
  // than of the window, because the window cannot answer it on a page that does not
  // scroll, which is exactly the page (/api/) where getting the answer wrong shifts the
  // header by the width of a bar.
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
  // The reference's content pane, in the frame on /api/. Null on a page of the site,
  // where the window is what scrolls.
  var pane = null;
  var header = null;
  var frame = 0;
  var grab = 0;

  bar.className = "synqt-scrollbar";
  thumb.className = "synqt-scrollbar__thumb";
  bar.appendChild(thumb);

  // Where the bar goes and what it has to describe, for whichever of the two this is:
  // the box on screen that the bar runs beside, the height of one screenful of what is
  // in it, the height of the whole of it, and how far down it we are.
  function metrics() {
    if (pane) {
      var box = pane.getBoundingClientRect();
      return {
        top: box.top,
        bottom: box.bottom,
        view: pane.clientHeight,
        page: pane.scrollHeight,
        at: pane.scrollTop
      };
    }
    if (!header || !header.isConnected) {
      header = document.querySelector(".md-header");
    }
    // The header is sticky at the top of the window, so its own box is where the bar has
    // to start, measured rather than assumed: it is one row here and the theme sizes it
    // from the wordmark.
    return {
      top: header ? header.getBoundingClientRect().bottom : 0,
      bottom: window.innerHeight,
      view: window.innerHeight,
      page: Math.max(root.scrollHeight, document.body ? document.body.scrollHeight : 0),
      at: window.scrollY
    };
  }

  function scrollTo(to) {
    var limit = metrics();
    var at = Math.min(Math.max(0, to), limit.page - limit.view);
    if (pane) {
      pane.scrollTop = at;
      return;
    }
    window.scrollTo(0, at);
  }

  function draw() {
    frame = 0;
    var now = metrics();
    var track = now.bottom - now.top;
    var scrollable = now.page - now.view;
    if (scrollable < 1 || track < MIN_THUMB * 2) {
      bar.hidden = true;
      return;
    }
    bar.hidden = false;
    bar.style.top = now.top + "px";
    bar.style.height = track + "px";
    var size = Math.max(MIN_THUMB, Math.round(track * (now.view / now.page)));
    var travel = track - size;
    var at = Math.min(1, Math.max(0, now.at / scrollable));
    thumb.style.height = size + "px";
    thumb.style.transform = "translateY(" + Math.round(at * travel) + "px)";
  }

  function schedule() {
    if (frame === 0) {
      frame = window.requestAnimationFrame(draw);
    }
  }

  // Where the given point on the track puts the top of the thumb, as a scroll position.
  function positionFor(clientY, offset) {
    var track = bar.getBoundingClientRect();
    var travel = track.height - thumb.offsetHeight;
    if (travel <= 0) {
      return 0;
    }
    var now = metrics();
    var at = (clientY - track.top - offset) / travel;
    return Math.min(1, Math.max(0, at)) * (now.page - now.view);
  }

  thumb.addEventListener("pointerdown", function (event) {
    grab = event.clientY - thumb.getBoundingClientRect().top;
    thumb.setPointerCapture(event.pointerId);
    root.classList.add(DRAGGING);
    // Otherwise the drag selects the text it passes over.
    event.preventDefault();
    // And this click is the start of a drag, not a jump to where it landed.
    event.stopPropagation();
  });

  thumb.addEventListener("pointermove", function (event) {
    if (thumb.hasPointerCapture(event.pointerId)) {
      scrollTo(positionFor(event.clientY, grab));
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

  // Clicking the track jumps to that point, and the wheel over the strip scrolls what
  // the strip belongs to. Both are things the platform's bar does, and the second is
  // what stops the strip being a ten pixel dead band down the side of the page: the
  // element takes the pointer, so without this the wheel would reach whatever is under
  // it, which in the reference is the page outline rather than the page.
  bar.addEventListener("pointerdown", function (event) {
    scrollTo(positionFor(event.clientY, thumb.offsetHeight / 2));
    event.preventDefault();
  });

  bar.addEventListener("wheel", function (event) {
    var by = event.deltaY * (event.deltaMode === 1 ? LINE_HEIGHT : 1);
    scrollTo(metrics().at + by);
    event.preventDefault();
  }, { passive: false });

  window.addEventListener("resize", schedule);

  function start() {
    if (!classicBars()) {
      return;
    }
    pane = document.getElementById("doc-content");
    document.body.appendChild(bar);
    // Only now, so a browser that got this far without the element (an error in the
    // lines above) is left with the bar it already had. The class also takes the
    // reserved width away, which is why it is set on a page with nothing to scroll as
    // well: leave it on there and that page alone would be ten pixels narrower than
    // every other, which is visible in the header.
    root.classList.add(ON);
    (pane || window).addEventListener("scroll", schedule, { passive: true });
    // The page grows and shrinks under a reader who never scrolls: instant navigation
    // swaps one page for another, images arrive, a details block opens, the reference
    // expands an inheritance diagram. All of it is a change of height and nothing else,
    // so one observer covers the lot. What is watched is whatever holds that height: the
    // body on a page of the site, and in the reference the blocks inside the pane, since
    // the pane's own box is a fixed screenful and never reports a thing.
    if (window.ResizeObserver) {
      var observer = new window.ResizeObserver(schedule);
      var index;
      if (pane) {
        observer.observe(pane);
        for (index = 0; index < pane.children.length; index++) {
          observer.observe(pane.children[index]);
        }
      } else {
        observer.observe(document.body);
      }
    }
    draw();
  }

  if (document.body) {
    start();
  } else {
    document.addEventListener("DOMContentLoaded", start);
  }
})();
