(function () {
  "use strict";

  function initImageOverlays() {
    var thumb = document.getElementById("dt-thumb");
    var overlay = document.getElementById("dt-overlay");
    if (!thumb || !overlay) return;

    // Avoid double-binding across navigations
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
    document.addEventListener("keydown", function (e) {
      if (e.key === "Escape" && overlay.style.display === "flex") {
        overlay.style.display = "none";
        document.body.style.overflow = "";
      }
    });
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
