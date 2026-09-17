// Tag right-hand TOC links with the platform tab that owns their heading,
// then mirror the active tab onto <body data-active-tab="..."> so CSS can
// fade out the inactive tab's TOC entries. Used by docs/tutorials/system_configuration.md.

(function () {
  function apply() {
    var wrapper = document.querySelector(".platform-tabs > .tabbed-set");
    if (!wrapper) return;

    var blocks = wrapper.querySelectorAll(
      ":scope > .tabbed-content > .tabbed-block"
    );
    if (blocks.length < 2) return;

    var tabKeys = ["igx-orin", "dgx-spark"];

    var anchorTab = {};
    blocks.forEach(function (block, i) {
      block.querySelectorAll("[id]").forEach(function (el) {
        anchorTab["#" + el.id] = tabKeys[i];
      });
    });

    document
      .querySelectorAll(".md-nav--secondary a[href^='#']")
      .forEach(function (a) {
        var key = anchorTab[a.getAttribute("href")];
        if (key) a.setAttribute("data-tab", key);
      });

    var radios = wrapper.querySelectorAll(":scope > input[type='radio']");
    function syncActive() {
      for (var i = 0; i < radios.length; i++) {
        if (radios[i].checked) {
          document.body.dataset.activeTab = tabKeys[i];
          return;
        }
      }
    }
    radios.forEach(function (r) {
      r.addEventListener("change", syncActive);
      r.addEventListener("input", syncActive);
    });
    syncActive();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", apply);
  } else {
    apply();
  }

  if (window.document$ && typeof window.document$.subscribe === "function") {
    window.document$.subscribe(apply);
  }
})();
