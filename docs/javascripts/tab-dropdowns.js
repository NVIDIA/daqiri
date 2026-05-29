// Inject hover/focus dropdowns under top-nav tabs that map to a
// multi-page section in mkdocs.yml. The primary sidebar is hidden via
// `hide: navigation` on those section's sub-pages (see frontmatter), so
// without this script there's no way to hop directly between sibling
// pages without bouncing through the section's index.
//
// Sub-page list is mirrored from `mkdocs.yml` nav. Keep them in sync when
// adding/removing entries.

(function () {
  "use strict";

  const SECTIONS = {
    "API Reference": [
      { label: "API Guide",                    path: "api-reference/" },
      { label: "Configuration YAML Reference", path: "api-reference/configuration/" },
      { label: "C++ API Usage",                path: "api-reference/cpp/" }
    ],
    "Tutorials": [
      { label: "System Configuration",          path: "tutorials/system_configuration/" },
      { label: "Configuration YAML Walkthrough", path: "tutorials/configuration-walkthrough/" }
    ]
  };

  function getSiteBase() {
    // Material's site logo link in the header always points to the site
    // root, which is the most reliable cross-environment anchor (works in
    // `mkdocs serve` locally and on the deployed gh-pages site).
    const logo = document.querySelector("a.md-header__button.md-logo");
    if (logo && logo.href) {
      const u = new URL(logo.href, window.location.href);
      return u.pathname.endsWith("/") ? u.pathname : u.pathname + "/";
    }
    return "/";
  }

  function buildDropdowns() {
    const base = getSiteBase();
    const tabs = document.querySelectorAll(".md-tabs__item");
    if (!tabs.length) return;

    tabs.forEach((tab) => {
      const link = tab.querySelector(".md-tabs__link");
      if (!link) return;

      const label = link.textContent.trim();
      const subpages = SECTIONS[label];
      if (!subpages) return;

      // Skip if we've already attached (Material instant-loading re-runs us).
      if (tab.classList.contains("nv-has-dropdown")) return;

      const dd = document.createElement("ul");
      dd.className = "nv-tab-dropdown";
      subpages.forEach((sub) => {
        const li = document.createElement("li");
        const a  = document.createElement("a");
        a.href = base + sub.path;
        a.textContent = sub.label;
        li.appendChild(a);
        dd.appendChild(li);
      });

      tab.classList.add("nv-has-dropdown");
      tab.appendChild(dd);
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", buildDropdowns);
  } else {
    buildDropdowns();
  }

  // Material's instant-loading feature swaps page content without a full
  // reload; re-run on the document$ event when it fires.
  if (typeof document$ !== "undefined" && document$.subscribe) {
    document$.subscribe(buildDropdowns);
  }
})();
