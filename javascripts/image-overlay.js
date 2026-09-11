(function () {
  "use strict";

  var keydownBound = false;

  function initImageOverlays() {
    var thumb = document.getElementById("dt-thumb");
    var overlay = document.getElementById("dt-overlay");
    if (!thumb || !overlay) return;

    // Avoid double-binding click/overlay handlers across navigations
    if (thumb.dataset.overlayBound === "1") return;
    thumb.dataset.overlayBound = "1";

    var overlayImg = document.getElementById("dt-overlay-img");

    thumb.addEventListener("click", function () {
      // Copy the browser-resolved src so the overlay uses the correct URL
      if (overlayImg && !overlayImg.src) overlayImg.src = thumb.src;
      overlay.style.display = "flex";
      document.body.style.overflow = "hidden";
    });
    overlay.addEventListener("click", function () {
      overlay.style.display = "none";
      document.body.style.overflow = "";
    });

    // Register the Escape handler once for the lifetime of the page.
    // Re-query the overlay each time so the handler always operates on the
    // current DOM element rather than a stale reference from a prior navigation.
    if (!keydownBound) {
      document.addEventListener("keydown", function (e) {
        if (e.key !== "Escape") return;
        var o = document.getElementById("dt-overlay");
        if (!o || o.style.display !== "flex") return;
        o.style.display = "none";
        document.body.style.overflow = "";
      });
      keydownBound = true;
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", initImageOverlays);
  } else {
    initImageOverlays();
  }

  // MkDocs Material instant navigation
  if (typeof document$ !== "undefined" && document$.subscribe) {
    document$.subscribe(initImageOverlays);
  }
})();
