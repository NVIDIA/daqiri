(function () {
  "use strict";

  var overlayState = {
    overlay: null,
    closeButton: null,
    previousFocus: null,
  };
  var keydownBound = false;

  function resetGraphicOverlay() {
    var overlay = overlayState.overlay;
    if (overlay && overlay.isConnected) {
      overlay.classList.remove("is-open");
      overlay.setAttribute("aria-hidden", "true");
    }
    document.body.classList.remove("graphic-overlay-open");
    overlayState.overlay = null;
    overlayState.closeButton = null;
    overlayState.previousFocus = null;
  }

  function openGraphicOverlay() {
    var overlay = overlayState.overlay;
    var closeButton = overlayState.closeButton;
    if (!overlay) return;
    overlayState.previousFocus = document.activeElement;
    overlay.classList.add("is-open");
    overlay.setAttribute("aria-hidden", "false");
    document.body.classList.add("graphic-overlay-open");
    if (closeButton) closeButton.focus();
  }

  function closeGraphicOverlay() {
    var overlay = overlayState.overlay;
    if (!overlay) {
      resetGraphicOverlay();
      return;
    }
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
    var prev = overlayState.previousFocus;
    overlayState.previousFocus = null;
    if (prev && typeof prev.focus === "function") {
      prev.focus();
    }
  }

  function bindOnce(el, event, handler) {
    if (!el || el.dataset.landingBound === "1") return;
    el.dataset.landingBound = "1";
    el.addEventListener(event, handler);
  }

  function maybeOpenFromHash() {
    if (window.location.hash !== "#graphic") return;
    window.requestAnimationFrame(openGraphicOverlay);
  }

  function initLandingGraphic() {
    var root = document.querySelector(".daqiri-landing");
    if (!root) {
      resetGraphicOverlay();
      return;
    }

    var graphicOpen = root.querySelector("[data-graphic-open]");
    var graphicOverlay = document.getElementById("daqiri-graphic-overlay");
    if (!graphicOpen || !graphicOverlay) {
      resetGraphicOverlay();
      return;
    }

    overlayState.overlay = graphicOverlay;
    overlayState.closeButton = root.querySelector(".graphic-overlay-close");

    bindOnce(graphicOpen, "click", openGraphicOverlay);

    root.querySelectorAll("[data-graphic-close]").forEach(function (control) {
      bindOnce(control, "click", closeGraphicOverlay);
    });

    if (!keydownBound) {
      document.addEventListener("keydown", function (event) {
        if (event.key !== "Escape") return;
        var overlay = overlayState.overlay;
        if (!overlay || !overlay.isConnected) return;
        if (!overlay.classList.contains("is-open")) return;
        closeGraphicOverlay();
      });
      keydownBound = true;
    }

    maybeOpenFromHash();
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
