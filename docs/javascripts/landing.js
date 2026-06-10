(function () {
  "use strict";

  var bound = false;

  function openGraphicOverlay(overlay, closeButton, previousFocusRef) {
    if (!overlay) return;
    previousFocusRef.current = document.activeElement;
    overlay.classList.add("is-open");
    overlay.setAttribute("aria-hidden", "false");
    document.body.classList.add("graphic-overlay-open");
    if (closeButton) closeButton.focus();
  }

  function closeGraphicOverlay(overlay, previousFocusRef) {
    if (!overlay) return;
    overlay.classList.remove("is-open");
    overlay.setAttribute("aria-hidden", "true");
    document.body.classList.remove("graphic-overlay-open");
    if (window.location.hash === "#graphic") {
      history.replaceState(
        null,
        "",
        `${window.location.pathname}${window.location.search}`
      );
    }
    var prev = previousFocusRef.current;
    if (prev && typeof prev.focus === "function") {
      prev.focus();
    }
  }

  function initLandingGraphic() {
    var root = document.querySelector(".daqiri-landing");
    if (!root) return;

    var graphicOpen = root.querySelector("[data-graphic-open]");
    var graphicOverlay = document.getElementById("daqiri-graphic-overlay");
    if (!graphicOpen || !graphicOverlay) return;

    if (graphicOpen.dataset.landingBound === "1") return;
    graphicOpen.dataset.landingBound = "1";

    var graphicCloseControls = Array.from(
      root.querySelectorAll("[data-graphic-close]")
    );
    var graphicCloseButton = root.querySelector(".graphic-overlay-close");
    var previousFocusRef = { current: null };

    graphicOpen.addEventListener("click", function () {
      openGraphicOverlay(graphicOverlay, graphicCloseButton, previousFocusRef);
    });

    graphicCloseControls.forEach(function (control) {
      control.addEventListener("click", function () {
        closeGraphicOverlay(graphicOverlay, previousFocusRef);
      });
    });

    if (!bound) {
      document.addEventListener("keydown", function (event) {
        if (
          event.key === "Escape" &&
          graphicOverlay.classList.contains("is-open")
        ) {
          closeGraphicOverlay(graphicOverlay, previousFocusRef);
        }
      });
      bound = true;
    }

    if (window.location.hash === "#graphic") {
      window.requestAnimationFrame(function () {
        openGraphicOverlay(
          graphicOverlay,
          graphicCloseButton,
          previousFocusRef
        );
      });
    }
  }

  function setup() {
    initLandingGraphic();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", setup);
  } else {
    setup();
  }

  if (typeof document$ !== "undefined" && document$.subscribe) {
    document$.subscribe(setup);
  }
})();
